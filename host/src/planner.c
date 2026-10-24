#include "planner.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "assign.h"
#include "driver.h"
#include "event_queue.h"
#include "protocol.h"
#include "report.h"

enum {
    HelloAttempts = 10,         /* A board that was just plugged in may still be booting */
    CalibrationTimeoutMs = 1000, /* The node times about one tick before answering */
    PlannerManualTasks = 32,    /* Tasks that can be typed during a run */
    PlannerMaxTasks = TaskFileMaxTasks + PlannerManualTasks,
    CommandLineMax = 128,
    DownSlackMinMs = 100,       /* Least extra wait before declaring a node down */
    CheckMinMs = 20,            /* Bounds of the sleep between node checks */
    CheckMaxMs = 500
};

/* A task known to the planner and where it runs */
typedef struct {
    Task task;
    bool accepted;      /* Deployed and running on nodeIndex */
    uint32_t nodeIndex;
    uint32_t origin;    /* HostOrigin */
} PlannerTask;

typedef struct {
    const HostConfig *config;
    ControlBlock *control;
    EventQueue *queue;
    uint32_t nodeCount;
    NodeInfo nodes[AssignMaxNodes];   /* unitsPerTick 0 = node left out */
    bool running[AssignMaxNodes];     /* Acknowledged Start */
    bool down[AssignMaxNodes];        /* Declared down (F11) */
    bool commanded[AssignMaxNodes];   /* Stopped with the k command */
    uint32_t seenReports[AssignMaxNodes];
    uint64_t seenUs[AssignMaxNodes];  /* When seenReports last changed */
    NodeState states[AssignMaxNodes]; /* Load of every node, for the admission */
    PlannerTask tasks[PlannerMaxTasks];
    size_t taskCount;
    FILE *out;

    /* Console: a command letter waiting for the rest of its line */
    char command;
    char line[CommandLineMax];
    size_t lineLength;
    bool echo; /* The terminal does not echo in key mode, so the planner does */

    /* Report and driver processes; one that already ended keeps its code */
    pid_t children[AssignMaxNodes + 1];
    bool reaped[AssignMaxNodes + 1];
    int exitCodes[AssignMaxNodes + 1];
    uint32_t childCount;
} Planner;

/* A node reads the port once per tick, so with a long tick a reply can take
   up to two ticks on top of the 200 ms of docs/protocolo.md, section 7 */
static uint32_t ReplyTimeout(const Planner *planner)
{
    return DriverReplyTimeoutMs + 2u * planner->config->tickMs;
}

/* Write end of the pipe that wakes the planner on SIGCHLD, SIGINT or SIGTERM */
static int wakeWriteFd = -1;

static void OnSignal(int signalNumber)
{
    int savedErrno = errno;
    char byte = (char)signalNumber;

    (void)write(wakeWriteFd, &byte, 1);
    errno = savedErrno;
}

static const char *ResultName(DriverResult result)
{
    switch (result) {
    case DriverResultOk:        return "ok";
    case DriverResultNoReply:   return "no reply";
    case DriverResultPortError: return "port error";
    }
    return "?";
}

static const char *AckName(AckStatus status)
{
    switch (status) {
    case AckStatusOk:         return "ok";
    case AckStatusBadPayload: return "bad payload";
    case AckStatusFull:       return "node full";
    case AckStatusBadState:   return "bad state";
    }
    return "?";
}

/* ---- Records for the report, through the event queue ---- */

static void Push(Planner *planner, HostEvent *hostEvent)
{
    if (hostEvent->hostUs == 0) {
        hostEvent->hostUs = HostClockUs();
    }
    (void)EventQueuePush(planner->queue, hostEvent);
}

static void PushTaskRecord(Planner *planner, const PlannerTask *entry, bool accepted,
                           uint32_t reason)
{
    HostEvent hostEvent;

    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = accepted ? HostEventTaskAccepted : HostEventTaskRejected;
    hostEvent.task = entry->task;
    hostEvent.origin = entry->origin;
    hostEvent.reason = reason;
    if (accepted) {
        hostEvent.nodeId = entry->nodeIndex + 1;
        (void)TaskCost(&entry->task, planner->nodes[entry->nodeIndex].unitsPerTick,
                       &hostEvent.value);
    }
    Push(planner, &hostEvent);
}

static void PushRunInfo(Planner *planner)
{
    HostEvent hostEvent;

    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventRunInfo;
    hostEvent.run.algorithm = planner->config->algorithm;
    hostEvent.run.heuristic = planner->config->heuristic;
    hostEvent.run.tickMs = planner->config->tickMs;
    hostEvent.run.nodeCount = planner->nodeCount;
    hostEvent.run.tickReportEvery = planner->config->tickReportEvery;
    hostEvent.run.missedReports = planner->config->missedReportsForFailure;
    Push(planner, &hostEvent);
}

static void PushNodeRecord(Planner *planner, uint32_t node, HostEventKind kind, uint32_t value,
                           uint64_t hostUs)
{
    HostEvent hostEvent;

    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = kind;
    hostEvent.nodeId = node + 1;
    hostEvent.value = value;
    hostEvent.hostUs = hostUs;
    snprintf(hostEvent.text, sizeof(hostEvent.text), "%s", planner->config->ports[node]);
    Push(planner, &hostEvent);
}

/* Link counters of every driver, once the drivers ended */
static void PushLinkStats(Planner *planner)
{
    HostEvent hostEvent;
    uint32_t i;

    for (i = 0; i < planner->nodeCount; i++) {
        DriverStats stats = DriverReadStats(&planner->control->channels[i]);

        memset(&hostEvent, 0, sizeof(hostEvent));
        hostEvent.kind = HostEventLinkStats;
        hostEvent.nodeId = i + 1;
        hostEvent.link.framesReceived = stats.framesReceived;
        hostEvent.link.badFrames = stats.badFrames + stats.badEvents;
        hostEvent.link.eventsReceived = stats.eventsReceived;
        hostEvent.link.tickReports = stats.tickReports;
        hostEvent.link.retransmissions = stats.retransmissions;
        hostEvent.link.staleFrames = stats.staleFrames;
        hostEvent.link.droppedEvents = stats.droppedEvents;
        hostEvent.link.missCount = stats.missCount;
        Push(planner, &hostEvent);
    }
}

/* ---- Requests to the nodes ---- */

/* Result of a request that expects an Ack: true when the node applied it */
static bool AckedOk(Planner *planner, uint32_t node, const char *what, DriverResult result,
                    const Frame *reply)
{
    AckMessage ack;

    if (result != DriverResultOk) {
        fprintf(planner->out, "Node %u: %s: %s\n", (unsigned)(node + 1), what, ResultName(result));
        return false;
    }
    if (ProtocolUnpackAck(reply, &ack) != ProtocolStatusOk) {
        fprintf(planner->out, "Node %u: %s: unexpected reply\n", (unsigned)(node + 1), what);
        return false;
    }
    if (ack.status != AckStatusOk) {
        fprintf(planner->out, "Node %u: %s: rejected (%s)\n", (unsigned)(node + 1), what,
                AckName(ack.status));
        return false;
    }
    return true;
}

static bool NodeUsable(const Planner *planner, uint32_t node)
{
    return planner->nodes[node].unitsPerTick > 0 && !planner->down[node] &&
           !planner->commanded[node];
}

/* Sends the same empty message (Start or Stop) to the given nodes at once */
static void BroadcastEmpty(Planner *planner, MessageType type, const bool *targets,
                           uint32_t attempts, bool *outOk)
{
    Frame request;
    Frame reply;
    uint32_t i;

    (void)ProtocolPackEmpty(type, &request);
    for (i = 0; i < planner->nodeCount; i++) {
        if (targets[i]) {
            DriverPost(&planner->control->channels[i], &request, ReplyTimeout(planner), attempts);
        }
    }
    for (i = 0; i < planner->nodeCount; i++) {
        outOk[i] = false;
        if (targets[i]) {
            DriverResult result = DriverWait(&planner->control->channels[i], &reply);
            outOk[i] = AckedOk(planner, i, type == MessageTypeStart ? "start" : "stop",
                               result, &reply);
        }
    }
}

static bool SendStop(Planner *planner, uint32_t node, uint32_t attempts)
{
    Frame request;
    Frame reply;

    (void)ProtocolPackEmpty(MessageTypeStop, &request);
    return AckedOk(planner, node, "stop",
                   DriverRequest(&planner->control->channels[node], &request,
                                 ReplyTimeout(planner), attempts, &reply),
                   &reply);
}

/* One DeployTask with the cost of the task on that node */
static bool DeployTaskTo(Planner *planner, const Task *task, uint32_t node)
{
    DeployTaskMessage message;
    Frame request;
    Frame reply;
    char what[48];

    message.id = task->id;
    message.type = task->type == TaskTypeSporadic ? ProtocolTaskSporadic : ProtocolTaskPeriodic;
    if (TaskCost(task, planner->nodes[node].unitsPerTick, &message.cost) != TaskStatusOk) {
        return false;
    }
    message.period = task->period;
    message.deadline = task->deadline;
    message.criticality = task->criticality;
    (void)ProtocolPackDeployTask(&message, &request);
    snprintf(what, sizeof(what), "task %u", (unsigned)task->id);
    return AckedOk(planner, node, what,
                   DriverRequest(&planner->control->channels[node], &request,
                                 ReplyTimeout(planner), DriverAttempts, &reply),
                   &reply);
}

/* Load of every usable node from the tasks running on it */
static void RebuildStates(Planner *planner)
{
    uint32_t i;
    size_t t;

    for (i = 0; i < planner->nodeCount; i++) {
        NodeInfo info = planner->nodes[i];

        if (!NodeUsable(planner, i)) {
            info.unitsPerTick = 0; /* AssignPlaceTask skips it */
        }
        AssignInitNode(&planner->states[i], info);
    }
    for (t = 0; t < planner->taskCount; t++) {
        const PlannerTask *entry = &planner->tasks[t];

        if (entry->accepted && NodeUsable(planner, entry->nodeIndex)) {
            (void)AssignCommitTask(&planner->states[entry->nodeIndex], &entry->task);
        }
    }
}

static void PrintPlaced(Planner *planner, const PlannerTask *entry, const char *verb)
{
    uint32_t cost = 0;

    (void)TaskCost(&entry->task, planner->nodes[entry->nodeIndex].unitsPerTick, &cost);
    fprintf(planner->out, "  task %u: %snode %u, C = %u ticks, U = %.3f\n",
            (unsigned)entry->task.id, verb, (unsigned)(entry->nodeIndex + 1), (unsigned)cost,
            (double)cost / (double)entry->task.period);
}

/* ---- Automatic mode: greeting, calibration, assignment, deployment ---- */

/* Steps 1 and 2: Stop to reset the node, then calibration with the tick */
static void GreetAndCalibrate(Planner *planner)
{
    bool all[AssignMaxNodes];
    bool greeted[AssignMaxNodes];
    CalibRequestMessage calib = { .tickUs = planner->config->tickMs * 1000u };
    CalibResponseMessage response;
    Frame request;
    Frame reply;
    uint32_t i;

    for (i = 0; i < planner->nodeCount; i++) {
        all[i] = true;
        planner->nodes[i].id = i + 1;
        planner->nodes[i].unitsPerTick = 0;
    }
    BroadcastEmpty(planner, MessageTypeStop, all, HelloAttempts, greeted);

    (void)ProtocolPackCalibRequest(&calib, &request);
    for (i = 0; i < planner->nodeCount; i++) {
        if (greeted[i]) {
            DriverPost(&planner->control->channels[i], &request,
                       CalibrationTimeoutMs + ReplyTimeout(planner), DriverAttempts);
        }
    }
    for (i = 0; i < planner->nodeCount; i++) {
        DriverResult result;

        if (!greeted[i]) {
            fprintf(planner->out, "Node %u (%s): not available, left out\n", (unsigned)(i + 1),
                    planner->config->ports[i]);
            continue;
        }
        result = DriverWait(&planner->control->channels[i], &reply);
        if (result != DriverResultOk || ProtocolUnpackCalibResponse(&reply, &response) !=
                                            ProtocolStatusOk || response.unitsPerTick == 0) {
            fprintf(planner->out, "Node %u (%s): calibration failed (%s), left out\n",
                    (unsigned)(i + 1), planner->config->ports[i],
                    result == DriverResultOk ? "unexpected reply" : ResultName(result));
            continue;
        }
        planner->nodes[i].unitsPerTick = response.unitsPerTick;
        fprintf(planner->out, "Node %u (%s): sn = %u work units per tick of %u ms\n",
                (unsigned)(i + 1), planner->config->ports[i], (unsigned)response.unitsPerTick,
                (unsigned)planner->config->tickMs);
        PushNodeRecord(planner, i, HostEventCalibration, response.unitsPerTick, 0);
    }
}

/* Step 3: cost, assignment and admission; prints one line per task.
   rejected receives the reason of every task that was not placed. */
static bool Assign(Planner *planner, uint32_t *rejected)
{
    static Task tasks[PlannerMaxTasks];
    static Assignment assignments[PlannerMaxTasks];
    size_t i;

    for (i = 0; i < planner->taskCount; i++) {
        tasks[i] = planner->tasks[i].task;
    }
    if (AssignTasks(tasks, planner->taskCount, planner->nodes, planner->nodeCount,
                    planner->config->algorithm, planner->config->heuristic,
                    assignments, NULL) != AssignStatusOk) {
        fprintf(planner->out, "Assignment failed\n");
        return false;
    }
    fprintf(planner->out, "Assignment (%s, %s):\n",
            planner->config->algorithm == AssignAlgorithmEdf ? "EDF" : "RMS",
            planner->config->heuristic == AssignHeuristicFirstFit ? "first-fit" : "worst-fit");
    for (i = 0; i < planner->taskCount; i++) {
        PlannerTask *entry = &planner->tasks[i];

        entry->accepted = assignments[i].accepted;
        entry->nodeIndex = (uint32_t)assignments[i].nodeIndex;
        rejected[i] = assignments[i].accepted ? 0 : (uint32_t)assignments[i].reason;
        if (!entry->accepted) {
            fprintf(planner->out, "  task %u: rejected, %s\n", (unsigned)entry->task.id,
                    ReportRejectName(rejected[i]));
            continue;
        }
        PrintPlaced(planner, entry, "");
    }
    return true;
}

/* Step 4, one node: DeployConfig and one DeployTask per assigned task */
static bool DeployNode(Planner *planner, uint32_t node, uint32_t *rejected)
{
    DeployConfigMessage config;
    Frame request;
    Frame reply;
    bool configured;
    size_t i;

    config.algorithm = planner->config->algorithm == AssignAlgorithmEdf ? ProtocolAlgorithmEdf :
                                                                          ProtocolAlgorithmRms;
    config.tickReportEvery = (uint16_t)planner->config->tickReportEvery;
    (void)ProtocolPackDeployConfig(&config, &request);
    configured = AckedOk(planner, node, "configuration",
                         DriverRequest(&planner->control->channels[node], &request,
                                       ReplyTimeout(planner), DriverAttempts, &reply),
                         &reply);

    for (i = 0; i < planner->taskCount; i++) {
        PlannerTask *entry = &planner->tasks[i];

        if (!entry->accepted || entry->nodeIndex != node) {
            continue;
        }
        if (!configured || !DeployTaskTo(planner, &entry->task, node)) {
            entry->accepted = false;
            rejected[i] = HostRejectNodeRefused;
        }
    }
    return configured;
}

/* Step 4: deploy to every calibrated node, start them together, and tell the
   report what was accepted and rejected */
static uint32_t DeployAndStart(Planner *planner, uint32_t *rejected)
{
    bool deployed[AssignMaxNodes];
    uint32_t running = 0;
    uint32_t i;
    size_t t;

    for (i = 0; i < planner->nodeCount; i++) {
        deployed[i] = NodeUsable(planner, i) && DeployNode(planner, i, rejected);
    }
    /* Start goes to every node back to back: it sets tick zero (spec 3.6) */
    BroadcastEmpty(planner, MessageTypeStart, deployed, DriverAttempts, planner->running);
    for (i = 0; i < planner->nodeCount; i++) {
        if (planner->running[i]) {
            running++;
            planner->seenUs[i] = planner->control->channels[i].replyUs;
            PushNodeRecord(planner, i, HostEventStarted, 0, planner->control->channels[i].replyUs);
        }
    }
    for (t = 0; t < planner->taskCount; t++) {
        PlannerTask *entry = &planner->tasks[t];

        if (entry->accepted && !planner->running[entry->nodeIndex]) {
            entry->accepted = false;
            rejected[t] = HostRejectNodeRefused;
        }
        PushTaskRecord(planner, entry, entry->accepted, rejected[t]);
    }
    RebuildStates(planner);
    return running;
}

/* ---- Node failure (F11) ---- */

/* Order for relocation: higher criticality first, so the lower ones are the
   first rejected; then higher utilization and lower id, as in the assignment */
static bool RelocatesFirst(const Planner *planner, const Task *a, const Task *b)
{
    uint32_t fastest = 0;
    uint32_t costA = 0;
    uint32_t costB = 0;
    double utilizationA;
    double utilizationB;
    uint32_t i;

    if (a->criticality != b->criticality) {
        return a->criticality > b->criticality;
    }
    for (i = 0; i < planner->nodeCount; i++) {
        if (NodeUsable(planner, i) && planner->nodes[i].unitsPerTick > fastest) {
            fastest = planner->nodes[i].unitsPerTick;
        }
    }
    if (fastest > 0) {
        (void)TaskCost(a, fastest, &costA);
        (void)TaskCost(b, fastest, &costB);
    }
    utilizationA = (double)costA / (double)a->period;
    utilizationB = (double)costB / (double)b->period;
    if (utilizationA != utilizationB) {
        return utilizationA > utilizationB;
    }
    return a->id < b->id;
}

/* Moves the tasks of a node that went down to the others, with the same
   heuristic and admission test; the tasks of the other nodes stay (spec 3.8) */
static void Relocate(Planner *planner, uint32_t node)
{
    size_t order[PlannerMaxTasks];
    size_t count = 0;
    size_t i;

    for (i = 0; i < planner->taskCount; i++) {
        PlannerTask *entry = &planner->tasks[i];
        size_t position;

        if (!entry->accepted || entry->nodeIndex != node) {
            continue;
        }
        entry->accepted = false;
        entry->origin = HostOriginRelocated;
        if (entry->task.type == TaskTypeSporadic) {
            fprintf(planner->out, "  task %u: rejected, %s\n", (unsigned)entry->task.id,
                    ReportRejectName(HostRejectSporadicOfDown));
            PushTaskRecord(planner, entry, false, HostRejectSporadicOfDown);
            continue;
        }
        position = count++;
        while (position > 0 &&
               RelocatesFirst(planner, &entry->task, &planner->tasks[order[position - 1]].task)) {
            order[position] = order[position - 1];
            position--;
        }
        order[position] = i;
    }

    RebuildStates(planner);
    for (i = 0; i < count; i++) {
        PlannerTask *entry = &planner->tasks[order[i]];
        Assignment assignment;

        if (AssignPlaceTask(planner->states, planner->nodeCount, &entry->task,
                            planner->config->algorithm, planner->config->heuristic,
                            &assignment) != AssignStatusOk || !assignment.accepted) {
            fprintf(planner->out, "  task %u (criticality %u): rejected, %s\n",
                    (unsigned)entry->task.id, (unsigned)entry->task.criticality,
                    ReportRejectName(HostRejectNoRoomAfterDown));
            PushTaskRecord(planner, entry, false, HostRejectNoRoomAfterDown);
            continue;
        }
        if (!DeployTaskTo(planner, &entry->task, (uint32_t)assignment.nodeIndex)) {
            PushTaskRecord(planner, entry, false, HostRejectNodeRefused);
            RebuildStates(planner);
            continue;
        }
        entry->accepted = true;
        entry->nodeIndex = (uint32_t)assignment.nodeIndex;
        PrintPlaced(planner, entry, "restarted on ");
        PushTaskRecord(planner, entry, true, 0);
    }
}

static void DeclareDown(Planner *planner, uint32_t node, const DriverStats *stats,
                        uint64_t silentUs)
{
    HostEvent hostEvent;
    uint32_t i;
    bool anyLeft = false;

    planner->down[node] = true;
    fprintf(planner->out, "Node %u DOWN: no tick report for %.0f ms (%u missed, last tick %u)\n",
            (unsigned)(node + 1), (double)silentUs / 1000.0,
            (unsigned)planner->config->missedReportsForFailure, (unsigned)stats->lastReportTick);
    /* A node that is alive but silent must not keep running the tasks that move */
    if (!planner->commanded[node] && stats->portOpen) {
        (void)SendStop(planner, node, 1);
    }

    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventNodeDown;
    hostEvent.nodeId = node + 1;
    hostEvent.event.tick = stats->lastReportTick;
    hostEvent.value = planner->commanded[node] ? HostDownByCommand : HostDownMissedReports;
    Push(planner, &hostEvent);

    fprintf(planner->out, "Replanning the tasks of node %u:\n", (unsigned)(node + 1));
    Relocate(planner, node);
    for (i = 0; i < planner->nodeCount; i++) {
        anyLeft = anyLeft || (planner->running[i] && !planner->down[i]);
    }
    if (!anyLeft) {
        fprintf(planner->out, "Every node is down. Press q for the report.\n");
    }
    fflush(planner->out);
}

static uint64_t ReportPeriodUs(const Planner *planner)
{
    return (uint64_t)planner->config->tickReportEvery * planner->config->tickMs * 1000u;
}

/* Declares down every running node whose tick reports stopped for
   missed_reports_for_failure periods, plus a margin for the USB jitter */
static void CheckNodes(Planner *planner)
{
    uint64_t period = ReportPeriodUs(planner);
    uint64_t slack = period / 2 > DownSlackMinMs * 1000u ? period / 2 : DownSlackMinMs * 1000u;
    uint64_t limit = (uint64_t)planner->config->missedReportsForFailure * period + slack;
    uint64_t now = HostClockUs();
    uint32_t i;

    for (i = 0; i < planner->nodeCount; i++) {
        DriverStats stats;

        if (!planner->running[i] || planner->down[i]) {
            continue;
        }
        stats = DriverReadStats(&planner->control->channels[i]);
        if (stats.tickReports != planner->seenReports[i]) {
            planner->seenReports[i] = stats.tickReports;
            planner->seenUs[i] = stats.lastReportUs;
            continue;
        }
        if (now > planner->seenUs[i] && now - planner->seenUs[i] > limit) {
            DeclareDown(planner, i, &stats, now - planner->seenUs[i]);
        }
    }
}

/* ---- Processes of the host ---- */

static int ExitCode(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* Waits for one of the processes of the planner and returns its exit code */
static int WaitForChild(Planner *planner, pid_t child)
{
    int status;
    uint32_t i;

    if (child <= 0) {
        return 1;
    }
    for (i = 0; i < planner->childCount; i++) {
        if (planner->children[i] == child && planner->reaped[i]) {
            return planner->exitCodes[i];
        }
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return 1;
        }
    }
    return ExitCode(status);
}

/* True when the report or a driver ended; other children of this process
   (for example simulated nodes in the tests) are not the planner's */
static bool OwnChildEnded(Planner *planner)
{
    bool ended = false;
    int status;
    uint32_t i;

    for (i = 0; i < planner->childCount; i++) {
        if (planner->children[i] > 0 && !planner->reaped[i] &&
            waitpid(planner->children[i], &status, WNOHANG) == planner->children[i]) {
            planner->reaped[i] = true;
            planner->exitCodes[i] = ExitCode(status);
            ended = true;
        }
    }
    return ended;
}

/* ---- Console: q, a (manual mode, F10), k, s, h ---- */

static void PrintHelp(Planner *planner)
{
    fprintf(planner->out,
            "Commands:\n"
            "  q                      stop the nodes and print the report\n"
            "  a id,type,W,T,D,crit   add a task now (manual mode), e.g. a 9,P,200,40,40,2\n"
            "  k node                 take a node down (it is stopped and detected as down)\n"
            "  s node                 release the sporadic task of a node, like its button\n"
            "  h                      this help\n");
}

/* Parses a node number from 1 to nodeCount */
static bool ParseNode(Planner *planner, const char *text, uint32_t *outNode)
{
    uint32_t value = 0;

    if (!ParseUint32(text, &value) || value == 0 || value > planner->nodeCount) {
        fprintf(planner->out, "Expected a node number from 1 to %u\n",
                (unsigned)planner->nodeCount);
        return false;
    }
    *outNode = value - 1;
    return true;
}

static bool NodeAlive(Planner *planner, uint32_t node)
{
    if (!planner->running[node] || planner->down[node] || planner->commanded[node]) {
        fprintf(planner->out, "Node %u is not running\n", (unsigned)(node + 1));
        return false;
    }
    return true;
}

/* Manual mode: admission with the current load and deployment without
   stopping the nodes; the node adds it at its current tick */
static void AddTask(Planner *planner, const char *text)
{
    PlannerTask *entry;
    ParseError error;
    Assignment assignment;
    Task task;
    size_t i;

    if (!TaskFileParseTask(text, &task, &error)) {
        fprintf(planner->out, "Task not added: %s\n", error.message);
        return;
    }
    if (planner->taskCount >= PlannerMaxTasks) {
        fprintf(planner->out, "Task not added: already %d tasks\n", PlannerMaxTasks);
        return;
    }
    entry = &planner->tasks[planner->taskCount];
    entry->task = task;
    entry->accepted = false;
    entry->nodeIndex = 0;
    entry->origin = HostOriginManual;
    for (i = 0; i < planner->taskCount; i++) {
        if (planner->tasks[i].task.id == task.id) {
            fprintf(planner->out, "  task %u: rejected, %s\n", (unsigned)task.id,
                    ReportRejectName(HostRejectDuplicateId));
            PushTaskRecord(planner, entry, false, HostRejectDuplicateId);
            return;
        }
    }
    planner->taskCount++;

    RebuildStates(planner);
    if (AssignPlaceTask(planner->states, planner->nodeCount, &task, planner->config->algorithm,
                        planner->config->heuristic, &assignment) != AssignStatusOk) {
        assignment.accepted = false;
        assignment.reason = AssignRejectInvalidTask;
    }
    if (!assignment.accepted) {
        fprintf(planner->out, "  task %u: rejected, %s\n", (unsigned)task.id,
                ReportRejectName((uint32_t)assignment.reason));
        PushTaskRecord(planner, entry, false, (uint32_t)assignment.reason);
        return;
    }
    if (!DeployTaskTo(planner, &task, (uint32_t)assignment.nodeIndex)) {
        PushTaskRecord(planner, entry, false, HostRejectNodeRefused);
        RebuildStates(planner);
        return;
    }
    entry->accepted = true;
    entry->nodeIndex = (uint32_t)assignment.nodeIndex;
    PrintPlaced(planner, entry, "added to ");
    PushTaskRecord(planner, entry, true, 0);
}

/* Stops the node; it stops sending tick reports, so it is detected as down
   the same way as a pulled cable */
static void TakeDown(Planner *planner, const char *text)
{
    uint32_t node;

    if (!ParseNode(planner, text, &node) || !NodeAlive(planner, node)) {
        return;
    }
    planner->commanded[node] = true;
    (void)SendStop(planner, node, DriverAttempts);
    fprintf(planner->out, "Node %u stopped; it is declared down after %u missed tick reports\n",
            (unsigned)(node + 1), (unsigned)planner->config->missedReportsForFailure);
}

static void TriggerSporadic(Planner *planner, const char *text)
{
    SporadicTriggerMessage message;
    Frame request;
    Frame reply;
    uint32_t node;
    size_t i;

    if (!ParseNode(planner, text, &node) || !NodeAlive(planner, node)) {
        return;
    }
    message.taskId = 0;
    for (i = 0; i < planner->taskCount; i++) {
        const PlannerTask *entry = &planner->tasks[i];

        if (entry->accepted && entry->nodeIndex == node && entry->task.type == TaskTypeSporadic) {
            message.taskId = entry->task.id;
        }
    }
    if (message.taskId == 0) {
        fprintf(planner->out, "Node %u has no sporadic task\n", (unsigned)(node + 1));
        return;
    }
    (void)ProtocolPackSporadicTrigger(&message, &request);
    if (AckedOk(planner, node, "sporadic release",
                DriverRequest(&planner->control->channels[node], &request, ReplyTimeout(planner),
                              DriverAttempts, &reply),
                &reply)) {
        fprintf(planner->out, "Node %u: sporadic task %u released (ignored if before its "
                "minimum interval)\n", (unsigned)(node + 1), (unsigned)message.taskId);
    }
}

static void RunCommand(Planner *planner, char command, char *argument)
{
    argument = ParseTrim(argument);
    if (*argument == '\0') {
        fprintf(planner->out, "Cancelled\n");
        return;
    }
    switch (command) {
    case 'a':
        AddTask(planner, argument);
        break;
    case 'k':
        TakeDown(planner, argument);
        break;
    case 's':
        TriggerSporadic(planner, argument);
        break;
    default:
        break;
    }
}

/* Handles typed bytes; returns true when q was typed */
static bool HandleInput(Planner *planner, const char *bytes, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        char c = bytes[i];

        if (planner->command != 0) {
            if (c == '\n' || c == '\r') {
                char command = planner->command;

                planner->line[planner->lineLength] = '\0';
                planner->command = 0;
                planner->lineLength = 0;
                if (planner->echo) {
                    fputc('\n', planner->out);
                }
                RunCommand(planner, command, planner->line);
            } else if ((c == 0x7f || c == '\b') && planner->lineLength > 0) {
                planner->lineLength--;
                if (planner->echo) {
                    fputs("\b \b", planner->out);
                }
            } else if (isprint((unsigned char)c) && planner->lineLength + 1 < CommandLineMax) {
                planner->line[planner->lineLength++] = c;
                if (planner->echo) {
                    fputc(c, planner->out);
                }
            }
            fflush(planner->out);
            continue;
        }
        switch (c) {
        case 'q':
        case 'Q':
            return true;
        case 'a':
            planner->command = c;
            fprintf(planner->out, "New task (id,type,W,T,D,criticality): ");
            break;
        case 'k':
            planner->command = c;
            fprintf(planner->out, "Node to take down (1-%u): ", (unsigned)planner->nodeCount);
            break;
        case 's':
            planner->command = c;
            fprintf(planner->out, "Node for the sporadic release (1-%u): ",
                    (unsigned)planner->nodeCount);
            break;
        case 'h':
        case '?':
            PrintHelp(planner);
            break;
        default:
            break; /* Spaces, newlines and unknown keys */
        }
        fflush(planner->out);
    }
    return false;
}

/* Sleeps in poll until q is typed, input ends, Ctrl+C or a child exits,
   waking every report period to look for nodes that went down */
static void RunConsole(Planner *planner, int inputFd, int wakeReadFd)
{
    uint64_t period = ReportPeriodUs(planner) / 1000u;
    int timeout = (int)(period < CheckMinMs ? CheckMinMs : period > CheckMaxMs ? CheckMaxMs :
                                                                                 period);
    struct pollfd fds[2];
    char buffer[64];
    ssize_t count;

    fds[0].fd = inputFd;
    fds[0].events = POLLIN;
    fds[1].fd = wakeReadFd;
    fds[1].events = POLLIN;

    for (;;) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        if (poll(fds, 2, timeout) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (fds[1].revents & POLLIN) {
            count = read(wakeReadFd, buffer, sizeof(buffer));
            if (count <= 0 || memchr(buffer, SIGINT, (size_t)count) != NULL ||
                memchr(buffer, SIGTERM, (size_t)count) != NULL || OwnChildEnded(planner)) {
                return;
            }
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            count = read(inputFd, buffer, sizeof(buffer));
            if (count <= 0 || HandleInput(planner, buffer, (size_t)count)) {
                return;
            }
        }
        CheckNodes(planner);
    }
}

static const char *EventName(EventType type)
{
    switch (type) {
    case EventTypeRelease:         return "release";
    case EventTypeStart:           return "start";
    case EventTypePreempt:         return "preempt";
    case EventTypeComplete:        return "complete";
    case EventTypeDeadlineMiss:    return "deadline-miss";
    case EventTypeAbort:           return "abort";
    case EventTypeSporadicRelease: return "sporadic-release";
    case EventTypeSporadicIgnored: return "sporadic-ignored";
    }
    return "unknown";
}

/* Report process: consumes the queue until it is closed and empty */
static int RunReporter(EventQueue *queue, FILE *out, bool verbose)
{
    static Report report;
    HostEvent hostEvent;

    ReportInit(&report);
    while (EventQueuePop(queue, &hostEvent)) {
        if (verbose && hostEvent.kind == HostEventNode) {
            fprintf(out, "node %u tick %u task %u %s %u\n", (unsigned)hostEvent.nodeId,
                    (unsigned)hostEvent.event.tick, (unsigned)hostEvent.event.taskId,
                    EventName(hostEvent.event.type), (unsigned)hostEvent.event.value);
            fflush(out);
        }
        (void)ReportRecord(&report, &hostEvent);
    }
    ReportPrint(&report, out);
    fflush(out);
    ReportFree(&report);
    return 0;
}

/* Children leave Ctrl+C to the planner, which stops everything in order */
static void PrepareChild(void)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

/* Puts the terminal in non-canonical mode so q works without Enter */
static bool EnterKeyMode(int fd, struct termios *saved)
{
    struct termios keys;

    if (!isatty(fd) || tcgetattr(fd, saved) != 0) {
        return false;
    }
    keys = *saved;
    keys.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    keys.c_cc[VMIN] = 1;
    keys.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSANOW, &keys) == 0;
}

/* Everything after the processes exist: steps 1 to 5 and the Stop of step 6 */
static int Operate(Planner *planner, int inputFd, int wakeReadFd)
{
    static uint32_t rejected[PlannerMaxTasks];
    bool targets[AssignMaxNodes];
    bool stopped[AssignMaxNodes];
    struct termios savedTerminal;
    bool keyMode;
    uint32_t calibrated = 0;
    uint32_t i;

    PushRunInfo(planner);
    GreetAndCalibrate(planner);
    for (i = 0; i < planner->nodeCount; i++) {
        planner->running[i] = false;
        if (NodeUsable(planner, i)) {
            calibrated++;
        }
    }
    if (calibrated == 0) {
        fprintf(planner->out, "No node answered; nothing to run\n");
        return 1;
    }
    if (!Assign(planner, rejected) || DeployAndStart(planner, rejected) == 0) {
        fprintf(planner->out, "No node could start\n");
        BroadcastEmpty(planner, MessageTypeStop, planner->running, DriverAttempts, stopped);
        return 1;
    }

    keyMode = EnterKeyMode(inputFd, &savedTerminal);
    planner->echo = keyMode;
    fprintf(planner->out, "Running. Press q to stop%s, h for the other commands.\n",
            keyMode ? "" : " (then Enter)");
    fflush(planner->out);
    RunConsole(planner, inputFd, wakeReadFd);
    if (keyMode) {
        (void)tcsetattr(inputFd, TCSANOW, &savedTerminal);
    }

    fprintf(planner->out, "\nStopping the nodes...\n");
    fflush(planner->out);
    for (i = 0; i < planner->nodeCount; i++) {
        targets[i] = planner->running[i] && !planner->down[i] && !planner->commanded[i];
    }
    BroadcastEmpty(planner, MessageTypeStop, targets, DriverAttempts, stopped);
    return 0;
}

int PlannerRun(const HostConfig *config, const TaskFile *tasks, int inputFd, FILE *out,
               bool verbose)
{
    static Planner planner;
    char queueName[64];
    char controlName[64];
    struct sigaction action;
    struct sigaction previous[3];
    EventQueue *queue;
    pid_t reporter;
    pid_t drivers[AssignMaxNodes];
    int wakePipe[2];
    int result = 0;
    uint32_t started = 0;
    uint32_t i;

    if (config == NULL || tasks == NULL || out == NULL || config->portCount == 0 ||
        config->portCount > AssignMaxNodes || tasks->taskCount > TaskFileMaxTasks) {
        return 1;
    }
    memset(&planner, 0, sizeof(planner));
    planner.config = config;
    planner.nodeCount = (uint32_t)config->portCount;
    planner.out = out;
    for (i = 0; i < tasks->taskCount; i++) {
        planner.tasks[i].task = tasks->tasks[i];
        planner.tasks[i].origin = HostOriginFile;
    }
    planner.taskCount = tasks->taskCount;

    snprintf(queueName, sizeof(queueName), "/rt_events_%d", (int)getpid());
    snprintf(controlName, sizeof(controlName), "/rt_control_%d", (int)getpid());
    queue = EventQueueCreate(queueName);
    if (queue == NULL) {
        fprintf(out, "Cannot create the event queue\n");
        return 1;
    }
    planner.queue = queue;
    planner.control = ControlCreate(controlName, config->ports, planner.nodeCount);
    if (planner.control == NULL) {
        fprintf(out, "Cannot create the control memory\n");
        EventQueueDestroy(queue, queueName);
        return 1;
    }
    if (pipe(wakePipe) != 0) {
        ControlDestroy(planner.control, controlName);
        EventQueueDestroy(queue, queueName);
        return 1;
    }
    fcntl(wakePipe[1], F_SETFL, O_NONBLOCK);
    wakeWriteFd = wakePipe[1];

    memset(&action, 0, sizeof(action));
    action.sa_handler = OnSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &action, &previous[0]);
    sigaction(SIGINT, &action, &previous[1]);
    sigaction(SIGTERM, &action, &previous[2]);

    fflush(out);
    fflush(stdout);
    reporter = fork();
    if (reporter == 0) {
        PrepareChild();
        _exit(RunReporter(queue, out, verbose));
    }
    planner.children[planner.childCount++] = reporter;
    for (i = 0; i < planner.nodeCount; i++) {
        drivers[i] = reporter > 0 ? fork() : -1;
        if (drivers[i] == 0) {
            PrepareChild();
            _exit(DriverRun(&planner.control->channels[i], queue));
        }
        if (drivers[i] > 0) {
            planner.children[planner.childCount++] = drivers[i];
            started++;
        }
    }

    if (reporter > 0 && started == planner.nodeCount) {
        result = Operate(&planner, inputFd, wakePipe[0]);
    } else {
        fprintf(out, "Cannot start the host processes\n");
        result = 1;
    }

    /* Drivers close their ports first; then the link counters go to the
       report, which drains the queue and prints */
    for (i = 0; i < planner.nodeCount; i++) {
        if (drivers[i] > 0) {
            DriverPostQuit(&planner.control->channels[i]);
        }
    }
    for (i = 0; i < planner.nodeCount; i++) {
        if (drivers[i] > 0) {
            result |= WaitForChild(&planner, drivers[i]);
        }
    }
    if (reporter > 0) {
        PushLinkStats(&planner);
    }
    EventQueueShutdown(queue);
    if (reporter > 0) {
        result |= WaitForChild(&planner, reporter);
    }
    fflush(out);

    sigaction(SIGCHLD, &previous[0], NULL);
    sigaction(SIGINT, &previous[1], NULL);
    sigaction(SIGTERM, &previous[2], NULL);
    wakeWriteFd = -1;
    close(wakePipe[0]);
    close(wakePipe[1]);
    ControlDestroy(planner.control, controlName);
    EventQueueDestroy(queue, queueName);
    return result;
}
