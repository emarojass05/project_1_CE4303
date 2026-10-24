#include "planner.h"

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
    HelloAttempts = 10,        /* A board that was just plugged in may still be booting */
    CalibrationTimeoutMs = 1000 /* The node times about one tick before answering */
};

typedef struct {
    const HostConfig *config;
    const TaskFile *tasks;
    ControlBlock *control;
    uint32_t nodeCount;
    NodeInfo nodes[AssignMaxNodes];   /* unitsPerTick 0 = node left out */
    Assignment assignments[TaskFileMaxTasks];
    FILE *out;
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

static const char *RejectName(AssignRejectReason reason)
{
    switch (reason) {
    case AssignRejectNone:            return "-";
    case AssignRejectInvalidTask:     return "invalid task";
    case AssignRejectNoUsableNode:    return "no calibrated node";
    case AssignRejectSporadicLimit:   return "every node already has a sporadic task";
    case AssignRejectNodeFull:        return "every node already has 8 tasks";
    case AssignRejectNotSchedulable:  return "fails the admission test on every node";
    }
    return "?";
}

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
    return planner->nodes[node].unitsPerTick > 0;
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
    }
}

/* Step 3: cost, assignment and admission; prints one line per task */
static bool Assign(Planner *planner)
{
    const TaskFile *tasks = planner->tasks;
    size_t i;

    if (AssignTasks(tasks->tasks, tasks->taskCount, planner->nodes, planner->nodeCount,
                    planner->config->algorithm, planner->config->heuristic,
                    planner->assignments, NULL) != AssignStatusOk) {
        fprintf(planner->out, "Assignment failed\n");
        return false;
    }
    fprintf(planner->out, "Assignment (%s, %s):\n",
            planner->config->algorithm == AssignAlgorithmEdf ? "EDF" : "RMS",
            planner->config->heuristic == AssignHeuristicFirstFit ? "first-fit" : "worst-fit");
    for (i = 0; i < tasks->taskCount; i++) {
        const Task *task = &tasks->tasks[i];
        const Assignment *assignment = &planner->assignments[i];
        uint32_t cost = 0;

        if (!assignment->accepted) {
            fprintf(planner->out, "  task %u: rejected, %s\n", (unsigned)task->id,
                    RejectName(assignment->reason));
            continue;
        }
        (void)TaskCost(task, planner->nodes[assignment->nodeIndex].unitsPerTick, &cost);
        fprintf(planner->out, "  task %u: node %u, C = %u ticks, U = %.3f\n",
                (unsigned)task->id, (unsigned)(assignment->nodeIndex + 1), (unsigned)cost,
                (double)cost / (double)task->period);
    }
    return true;
}

/* Step 4, one node: DeployConfig and one DeployTask per assigned task */
static bool DeployNode(Planner *planner, uint32_t node)
{
    DriverChannel *channel = &planner->control->channels[node];
    DeployConfigMessage config;
    DeployTaskMessage message;
    Frame request;
    Frame reply;
    char what[48];
    size_t i;

    config.algorithm = planner->config->algorithm == AssignAlgorithmEdf ? ProtocolAlgorithmEdf :
                                                                          ProtocolAlgorithmRms;
    config.tickReportEvery = (uint16_t)planner->config->tickReportEvery;
    (void)ProtocolPackDeployConfig(&config, &request);
    if (!AckedOk(planner, node, "configuration",
                 DriverRequest(channel, &request, ReplyTimeout(planner), DriverAttempts, &reply),
                 &reply)) {
        return false;
    }

    for (i = 0; i < planner->tasks->taskCount; i++) {
        const Task *task = &planner->tasks->tasks[i];

        if (!planner->assignments[i].accepted || planner->assignments[i].nodeIndex != node) {
            continue;
        }
        message.id = task->id;
        message.type = task->type == TaskTypeSporadic ? ProtocolTaskSporadic : ProtocolTaskPeriodic;
        (void)TaskCost(task, planner->nodes[node].unitsPerTick, &message.cost);
        message.period = task->period;
        message.deadline = task->deadline;
        message.criticality = task->criticality;
        (void)ProtocolPackDeployTask(&message, &request);
        snprintf(what, sizeof(what), "task %u", (unsigned)task->id);
        if (!AckedOk(planner, node, what,
                     DriverRequest(channel, &request, ReplyTimeout(planner), DriverAttempts,
                                   &reply),
                     &reply)) {
            planner->assignments[i].accepted = false;
        }
    }
    return true;
}

/* Step 4: deploy to every calibrated node and start them together */
static uint32_t DeployAndStart(Planner *planner, bool *outRunning)
{
    bool deployed[AssignMaxNodes];
    uint32_t running = 0;
    uint32_t i;

    for (i = 0; i < planner->nodeCount; i++) {
        deployed[i] = NodeUsable(planner, i) && DeployNode(planner, i);
    }
    /* Start goes to every node back to back: it sets tick zero (spec 3.6) */
    BroadcastEmpty(planner, MessageTypeStart, deployed, DriverAttempts, outRunning);
    for (i = 0; i < planner->nodeCount; i++) {
        if (outRunning[i]) {
            running++;
        }
    }
    return running;
}

/* Sleeps until q is typed, input ends, Ctrl+C, or a child exits */
static void WaitForStop(int inputFd, int wakeReadFd)
{
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
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (fds[1].revents & POLLIN) {
            return;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            count = read(inputFd, buffer, sizeof(buffer));
            if (count <= 0 || memchr(buffer, 'q', (size_t)count) != NULL ||
                memchr(buffer, 'Q', (size_t)count) != NULL) {
                return;
            }
        }
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
    Report report;
    HostEvent hostEvent;

    ReportInit(&report);
    while (EventQueuePop(queue, &hostEvent)) {
        if (verbose) {
            fprintf(out, "node %u tick %u task %u %s %u\n", (unsigned)hostEvent.nodeId,
                    (unsigned)hostEvent.event.tick, (unsigned)hostEvent.event.taskId,
                    EventName(hostEvent.event.type), (unsigned)hostEvent.event.value);
            fflush(out);
        }
        (void)ReportRecord(&report, &hostEvent);
    }
    ReportPrint(&report, out);
    fflush(out);
    return 0;
}

static int WaitForChild(pid_t child)
{
    int status;

    if (child <= 0) {
        return 1;
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return 1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* Children leave Ctrl+C to the planner, which stops everything in order */
static void PrepareChild(void)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

static void PrintLinkSummary(Planner *planner)
{
    uint32_t i;

    fprintf(planner->out, "===== Link =====\n");
    for (i = 0; i < planner->nodeCount; i++) {
        DriverStats stats = DriverReadStats(&planner->control->channels[i]);

        fprintf(planner->out,
                "Node %u (%s): sn %u, frames %u, events %u, corrupt frames %u, "
                "tick reports %u (last tick %u), misses %u, dropped events %u, "
                "retransmissions %u, leftovers of an earlier run %u\n",
                (unsigned)(i + 1), planner->config->ports[i],
                (unsigned)planner->nodes[i].unitsPerTick, (unsigned)stats.framesReceived,
                (unsigned)stats.eventsReceived, (unsigned)(stats.badFrames + stats.badEvents),
                (unsigned)stats.tickReports, (unsigned)stats.lastReportTick,
                (unsigned)stats.missCount, (unsigned)stats.droppedEvents,
                (unsigned)stats.retransmissions, (unsigned)stats.staleFrames);
    }
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
    bool running[AssignMaxNodes];
    bool stopped[AssignMaxNodes];
    struct termios savedTerminal;
    bool keyMode;
    uint32_t calibrated = 0;
    uint32_t i;

    GreetAndCalibrate(planner);
    for (i = 0; i < planner->nodeCount; i++) {
        running[i] = false;
        if (NodeUsable(planner, i)) {
            calibrated++;
        }
    }
    if (calibrated == 0) {
        fprintf(planner->out, "No node answered; nothing to run\n");
        return 1;
    }
    if (!Assign(planner) || DeployAndStart(planner, running) == 0) {
        fprintf(planner->out, "No node could start\n");
        BroadcastEmpty(planner, MessageTypeStop, running, DriverAttempts, stopped);
        return 1;
    }

    keyMode = EnterKeyMode(inputFd, &savedTerminal);
    fprintf(planner->out, "Running. Press q to stop%s.\n", keyMode ? "" : " (then Enter)");
    fflush(planner->out);
    WaitForStop(inputFd, wakeReadFd);
    if (keyMode) {
        (void)tcsetattr(inputFd, TCSANOW, &savedTerminal);
    }

    fprintf(planner->out, "Stopping the nodes...\n");
    fflush(planner->out);
    BroadcastEmpty(planner, MessageTypeStop, running, DriverAttempts, stopped);
    return 0;
}

int PlannerRun(const HostConfig *config, const TaskFile *tasks, int inputFd, FILE *out,
               bool verbose)
{
    char queueName[64];
    char controlName[64];
    struct sigaction action;
    struct sigaction previous[3];
    Planner planner;
    EventQueue *queue;
    pid_t reporter;
    pid_t drivers[AssignMaxNodes];
    int wakePipe[2];
    int result = 0;
    uint32_t started = 0;
    uint32_t i;

    if (config == NULL || tasks == NULL || out == NULL || config->portCount == 0 ||
        config->portCount > AssignMaxNodes) {
        return 1;
    }
    memset(&planner, 0, sizeof(planner));
    planner.config = config;
    planner.tasks = tasks;
    planner.nodeCount = (uint32_t)config->portCount;
    planner.out = out;

    snprintf(queueName, sizeof(queueName), "/rt_events_%d", (int)getpid());
    snprintf(controlName, sizeof(controlName), "/rt_control_%d", (int)getpid());
    queue = EventQueueCreate(queueName);
    if (queue == NULL) {
        fprintf(out, "Cannot create the event queue\n");
        return 1;
    }
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
    for (i = 0; i < planner.nodeCount; i++) {
        drivers[i] = reporter > 0 ? fork() : -1;
        if (drivers[i] == 0) {
            PrepareChild();
            _exit(DriverRun(&planner.control->channels[i], queue));
        }
        if (drivers[i] > 0) {
            started++;
        }
    }

    if (reporter > 0 && started == planner.nodeCount) {
        result = Operate(&planner, inputFd, wakePipe[0]);
    } else {
        fprintf(out, "Cannot start the host processes\n");
        result = 1;
    }

    /* Drivers close their ports first; then the report drains the queue */
    for (i = 0; i < planner.nodeCount; i++) {
        if (drivers[i] > 0) {
            DriverPostQuit(&planner.control->channels[i]);
        }
    }
    for (i = 0; i < planner.nodeCount; i++) {
        if (drivers[i] > 0) {
            result |= WaitForChild(drivers[i]);
        }
    }
    EventQueueShutdown(queue);
    if (reporter > 0) {
        result |= WaitForChild(reporter);
    }
    PrintLinkSummary(&planner);
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
