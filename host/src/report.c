#include "report.h"

#include <stdlib.h>
#include <string.h>

#include "admission.h"
#include "assign.h"

/* Report: finds or creates a task's counters */
static TaskStats *FindTask(Report *report, uint32_t nodeId, uint32_t taskId)
{
    TaskStats *entry;
    uint32_t i;

    for (i = 0; i < report->taskCount; i++) {
        entry = &report->tasks[i];
        if (entry->nodeId == nodeId && entry->taskId == taskId) {
            return entry;
        }
    }
    if (report->taskCount >= ReportMaxTasks) {
        return NULL;
    }
    entry = &report->tasks[report->taskCount];
    memset(entry, 0, sizeof(*entry));
    entry->nodeId = nodeId;
    entry->taskId = taskId;
    report->taskCount++;
    return entry;
}

/* Report: finds or creates a node's counters */
static NodeStats *FindNode(Report *report, uint32_t nodeId)
{
    NodeStats *entry;
    uint32_t i;

    for (i = 0; i < report->nodeCount; i++) {
        if (report->nodes[i].nodeId == nodeId) {
            return &report->nodes[i];
        }
    }
    if (nodeId == 0 || report->nodeCount >= ReportMaxNodes) {
        return NULL;
    }
    entry = &report->nodes[report->nodeCount];
    memset(entry, 0, sizeof(*entry));
    entry->nodeId = nodeId;
    report->nodeCount++;
    return entry;
}

/* Report: clears every counter */
void ReportInit(Report *report)
{
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
}

/* Report: frees the event log */
void ReportFree(Report *report)
{
    if (report != NULL) {
        free(report->log);
        report->log = NULL;
        report->logCount = 0;
        report->logCapacity = 0;
    }
}

/* Report: text for a reject reason */
const char *ReportRejectName(uint32_t reason)
{
    switch (reason) {
    case HostRejectInvalidTask:     return "invalid task";
    case HostRejectNoUsableNode:    return "no calibrated node";
    case HostRejectSporadicLimit:   return "every node already has a sporadic task";
    case HostRejectNodeFull:        return "every node already has 8 tasks";
    case HostRejectNotSchedulable:  return "fails the admission test on every node";
    case HostRejectNodeRefused:     return "the node refused the task";
    case HostRejectDuplicateId:     return "id already in use";
    case HostRejectSporadicOfDown:  return "sporadic task of a node that went down";
    case HostRejectNoRoomAfterDown: return "its node went down and no other node can take it";
    }
    return "?";
}

/* Report: text for a task origin */
const char *ReportOriginName(uint32_t origin)
{
    switch (origin) {
    case HostOriginFile:      return "task file";
    case HostOriginManual:    return "manual";
    case HostOriginRelocated: return "relocated";
    }
    return "?";
}

/* Report: stores a planner decision */
static void AddDecision(Report *report, ReportDecisionKind kind, const HostEvent *hostEvent)
{
    ReportDecision *decision;

    if (report->decisionCount >= ReportMaxDecisions) {
        report->ignoredEvents++;
        return;
    }
    decision = &report->decisions[report->decisionCount++];
    decision->kind = kind;
    decision->task = hostEvent->task;
    decision->nodeId = hostEvent->nodeId;
    decision->cost = hostEvent->value;
    decision->origin = hostEvent->origin;
    decision->reason = hostEvent->reason;
    decision->hostUs = hostEvent->hostUs;
}

/* Report: keeps an event for the Gantt chart */
static void LogEvent(Report *report, const HostEvent *hostEvent)
{
    ReportLogged *grown;
    size_t capacity;

    if (report->logCount >= report->logCapacity) {
        if (report->logCapacity >= ReportMaxLogged) {
            report->logTruncated = true;
            return;
        }
        capacity = report->logCapacity == 0 ? 4096 : report->logCapacity * 2;
        if (capacity > ReportMaxLogged) {
            capacity = ReportMaxLogged;
        }
        grown = realloc(report->log, capacity * sizeof(*grown));
        if (grown == NULL) {
            report->logTruncated = true;
            return;
        }
        report->log = grown;
        report->logCapacity = capacity;
    }
    report->log[report->logCount].nodeId = hostEvent->nodeId;
    report->log[report->logCount].event = hostEvent->event;
    report->logCount++;
}

/* Report: keeps the highest tick seen for a node */
static void SeeTick(NodeStats *node, uint32_t ticks)
{
    if (ticks > node->observedTicks) {
        node->observedTicks = ticks;
    }
}

/* Report: a task leaves the processor */
static void StopRunning(TaskStats *task, uint32_t end)
{
    if (task->running) {
        if (end > task->runningSince) {
            task->executedTicks += end - task->runningSince;
        }
        task->running = false;
    }
}

/* Report: counts one node event by type */
static bool RecordNodeEvent(Report *report, const HostEvent *hostEvent)
{
    const Event *event = &hostEvent->event;
    NodeStats *node;
    TaskStats *task;

    report->totalEvents++;
    node = FindNode(report, hostEvent->nodeId);
    task = FindTask(report, hostEvent->nodeId, event->taskId);
    if (node == NULL || task == NULL) {
        report->ignoredEvents++;
        return false;
    }

    switch (event->type) {
    case EventTypeRelease:
        task->releases++;
        break;
    case EventTypeStart:
        task->starts++;
        task->running = true;
        task->runningSince = event->tick;
        node->taskSwitches++;
        if (node->switchCount < ReportListMax) {
            ReportSwitch *entry = &node->switches[node->switchCount++];

            entry->tick = event->tick;
            /* Idle only if no task finished right before this tick */
            entry->from = node->current != 0 ? node->current :
                          node->lastEnd == event->tick ? node->lastTask : 0;
            entry->to = event->taskId;
        }
        node->current = event->taskId;
        break;
    case EventTypePreempt:
        task->preemptions++;
        node->preemptions++;
        StopRunning(task, event->tick);
        break;
    case EventTypeComplete:
        task->completions++;
        task->totalResponse += event->value;
        if (task->completions == 1 || event->value < task->minResponse) {
            task->minResponse = event->value;
        }
        if (event->value > task->maxResponse) {
            task->maxResponse = event->value;
        }
        StopRunning(task, event->tick + 1u);
        if (node->current == event->taskId) {
            node->current = 0;
            node->lastTask = event->taskId;
            node->lastEnd = event->tick + 1u;
        }
        break;
    case EventTypeDeadlineMiss:
        task->deadlineMisses++;
        if (node->missCount < ReportListMax) {
            ReportMiss *entry = &node->misses[node->missCount++];

            entry->tick = event->tick;
            entry->taskId = event->taskId;
            entry->left = event->value;
        }
        break;
    case EventTypeAbort:
        task->aborts++;
        StopRunning(task, event->tick);
        if (node->current == event->taskId) {
            node->current = 0;
            node->lastTask = event->taskId;
            node->lastEnd = event->tick;
        }
        break;
    case EventTypeSporadicRelease:
        task->sporadic = true;
        task->sporadicReleases++;
        break;
    case EventTypeSporadicIgnored:
        task->sporadic = true;
        task->sporadicIgnored++;
        break;
    default:
        report->ignoredEvents++;
        return false;
    }
    SeeTick(node, event->tick + 1u);
    LogEvent(report, hostEvent);
    return true;
}

/* Report: updates the skew estimate */
static void RecordTickReport(Report *report, NodeStats *node, const HostEvent *hostEvent)
{
    int64_t zero;

    SeeTick(node, hostEvent->event.tick);
    node->tickReports++;
    if (!report->haveRun || report->run.tickMs == 0) {
        return;
    }
    zero = (int64_t)hostEvent->hostUs -
           (int64_t)hostEvent->event.tick * (int64_t)report->run.tickMs * 1000;
    if (node->windowCount == 0 || zero < node->windowZeroUs) {
        node->windowZeroUs = zero;
    }
    node->windowCount++;
    if (node->windowCount < ReportSkewWindow) {
        return;
    }
    if (node->skewCount < ReportSkewMaxPoints) {
        node->skewCount++;
    }
    node->skew[node->skewCount - 1].zeroUs = node->windowZeroUs;
    node->skew[node->skewCount - 1].hostUs = hostEvent->hostUs;
    node->windowCount = 0;
}

/* Report: counts one event from the queue */
bool ReportRecord(Report *report, const HostEvent *hostEvent)
{
    NodeStats *node;

    if (report == NULL || hostEvent == NULL) {
        return false;
    }
    if (hostEvent->kind == HostEventNode) {
        return RecordNodeEvent(report, hostEvent);
    }
    if (hostEvent->kind == HostEventRunInfo) {
        report->run = hostEvent->run;
        report->haveRun = true;
        return true;
    }
    if (hostEvent->kind == HostEventTaskRejected) {
        AddDecision(report, ReportDecisionRejected, hostEvent);
        return true;
    }

    node = FindNode(report, hostEvent->nodeId);
    if (node == NULL) {
        report->ignoredEvents++;
        return false;
    }
    switch (hostEvent->kind) {
    case HostEventTickReport:
        RecordTickReport(report, node, hostEvent);
        break;
    case HostEventCalibration:
        node->calibrated = true;
        node->unitsPerTick = hostEvent->value;
        memcpy(node->port, hostEvent->text, sizeof(node->port));
        node->port[sizeof(node->port) - 1] = '\0';
        break;
    case HostEventStarted:
        node->started = true;
        node->startedUs = hostEvent->hostUs;
        if (report->startUs == 0 || hostEvent->hostUs < report->startUs) {
            report->startUs = hostEvent->hostUs;
        }
        break;
    case HostEventTaskAccepted:
        AddDecision(report, ReportDecisionAccepted, hostEvent);
        break;
    case HostEventNodeDown:
        node->down = true;
        node->downTick = hostEvent->event.tick;
        node->downCause = hostEvent->value;
        node->downUs = hostEvent->hostUs;
        AddDecision(report, ReportDecisionNodeDown, hostEvent);
        break;
    case HostEventLinkStats:
        node->haveLink = true;
        node->link = hostEvent->link;
        break;
    default:
        report->ignoredEvents++;
        return false;
    }
    return true;
}

/* ---- Printing ---- */

/* Placement of every task at the end of the run, replaying the decisions */
typedef struct {
    uint32_t taskId;
    uint32_t nodeId; /* 0 = on no node */
    uint32_t cost;
    uint32_t period;
    bool sporadic;
} FinalPlace;

/* Report: final node of each task */
static uint32_t FinalPlacement(const Report *report, FinalPlace *places)
{
    uint32_t count = 0;
    uint32_t d;
    uint32_t i;

    for (d = 0; d < report->decisionCount; d++) {
        const ReportDecision *decision = &report->decisions[d];
        FinalPlace *place = NULL;

        if (decision->kind == ReportDecisionNodeDown) {
            for (i = 0; i < count; i++) {
                if (places[i].nodeId == decision->nodeId) {
                    places[i].nodeId = 0;
                }
            }
            continue;
        }
        /* A rejection never moves a task that runs: a repeated id is another
           task, and a relocation left its node at the NodeDown before */
        if (decision->kind != ReportDecisionAccepted) {
            continue;
        }
        for (i = 0; i < count; i++) {
            if (places[i].taskId == decision->task.id) {
                place = &places[i];
            }
        }
        if (place == NULL) {
            if (count >= ReportMaxDecisions) {
                continue;
            }
            place = &places[count++];
            place->taskId = decision->task.id;
        }
        place->period = decision->task.period;
        place->sporadic = decision->task.type == TaskTypeSporadic;
        place->nodeId = decision->nodeId;
        place->cost = decision->cost;
    }
    return count;
}

/* Report: cost and period of a task on a node */
static bool TaskLoad(const Report *report, uint32_t nodeId, uint32_t taskId, uint32_t *cost,
                     uint32_t *period)
{
    bool found = false;
    uint32_t d;

    for (d = 0; d < report->decisionCount; d++) {
        const ReportDecision *decision = &report->decisions[d];

        if (decision->kind == ReportDecisionAccepted && decision->nodeId == nodeId &&
            decision->task.id == taskId) {
            *cost = decision->cost;
            *period = decision->task.period;
            found = true;
        }
    }
    return found;
}

/* Report: host seconds since the start */
static double Seconds(const Report *report, uint64_t hostUs)
{
    if (report->startUs == 0 || hostUs < report->startUs) {
        return 0.0;
    }
    return (double)(hostUs - report->startUs) / 1e6;
}

/* Report: prints the run settings */
static void PrintRun(const Report *report, FILE *out)
{
    uint32_t longest = 0;
    uint32_t n;

    for (n = 0; n < report->nodeCount; n++) {
        if (report->nodes[n].observedTicks > longest) {
            longest = report->nodes[n].observedTicks;
        }
    }
    if (report->haveRun) {
        fprintf(out, "Run: %s, %s, tick %u ms, %u node(s), tick report every %u ticks, "
                "down after %u missed reports\n",
                report->run.algorithm == AssignAlgorithmEdf ? "EDF" : "RMS",
                report->run.heuristic == AssignHeuristicFirstFit ? "first-fit" : "worst-fit",
                (unsigned)report->run.tickMs, (unsigned)report->run.nodeCount,
                (unsigned)report->run.tickReportEvery, (unsigned)report->run.missedReports);
        fprintf(out, "Length: %u ticks (%.2f s)\n", (unsigned)longest,
                (double)longest * (double)report->run.tickMs / 1000.0);
    }
    fprintf(out, "Events received: %u\n", (unsigned)report->totalEvents);
    fprintf(out, "Events ignored: %u\n", (unsigned)report->ignoredEvents);
}

/* Report: prints accepted and rejected tasks */
static void PrintDecisions(const Report *report, FILE *out)
{
    uint32_t accepted = 0;
    uint32_t rejected = 0;
    uint32_t d;

    fprintf(out, "\n--- Accepted and rejected tasks ---\n");
    for (d = 0; d < report->decisionCount; d++) {
        const ReportDecision *decision = &report->decisions[d];
        const Task *task = &decision->task;

        if (decision->kind == ReportDecisionNodeDown) {
            fprintf(out, "  [%7.2f s] node %u down: its tasks are relocated\n",
                    Seconds(report, decision->hostUs), (unsigned)decision->nodeId);
            continue;
        }
        fprintf(out, "  [%7.2f s] task %u (%c, W=%u, T=%u, D=%u, crit=%u, %s): ",
                Seconds(report, decision->hostUs), (unsigned)task->id,
                task->type == TaskTypeSporadic ? 'S' : 'P', (unsigned)task->work,
                (unsigned)task->period, (unsigned)task->deadline, (unsigned)task->criticality,
                ReportOriginName(decision->origin));
        if (decision->kind == ReportDecisionAccepted) {
            accepted++;
            fprintf(out, "accepted on node %u, C = %u, U = %.3f\n", (unsigned)decision->nodeId,
                    (unsigned)decision->cost, (double)decision->cost / (double)task->period);
        } else {
            rejected++;
            fprintf(out, "rejected, %s\n", ReportRejectName(decision->reason));
        }
    }
    fprintf(out, "  %u accepted, %u rejected\n", (unsigned)accepted, (unsigned)rejected);
}

/* Report: prints per-node totals */
static void PrintNodes(const Report *report, FILE *out)
{
    FinalPlace places[ReportMaxDecisions];
    uint32_t placeCount = FinalPlacement(report, places);
    uint32_t n;
    uint32_t i;

    fprintf(out, "\n--- Nodes ---\n");
    for (n = 0; n < report->nodeCount; n++) {
        const NodeStats *node = &report->nodes[n];
        LoadEntry entries[ReportMaxDecisions];
        size_t entryCount = 0;
        uint64_t busy = 0;
        double theoretical;
        double bound;

        for (i = 0; i < placeCount; i++) {
            if (places[i].nodeId == node->nodeId && places[i].period > 0) {
                entries[entryCount].cost = places[i].cost;
                entries[entryCount].period = places[i].period;
                entryCount++;
            }
        }
        for (i = 0; i < report->taskCount; i++) {
            const TaskStats *task = &report->tasks[i];

            if (task->nodeId == node->nodeId) {
                busy += task->executedTicks;
                if (task->running && node->observedTicks > task->runningSince) {
                    busy += node->observedTicks - task->runningSince;
                }
            }
        }
        theoretical = entryCount > 0 ? AdmissionUtilization(entries, entryCount) : 0.0;
        bound = (report->haveRun && report->run.algorithm == AssignAlgorithmRms) ?
                AdmissionRmsBound(entryCount) : 1.0;

        fprintf(out, "Node %u", (unsigned)node->nodeId);
        if (node->port[0] != '\0') {
            fprintf(out, " (%s)", node->port);
        }
        if (node->calibrated) {
            fprintf(out, ": sn %u", (unsigned)node->unitsPerTick);
        }
        if (node->down) {
            fprintf(out, ", DOWN at %.2f s (last reported tick %u, %s)",
                    Seconds(report, node->downUs), (unsigned)node->downTick,
                    node->downCause == HostDownByCommand ? "k command" :
                                                           "tick reports stopped");
        }
        fprintf(out, "\n");
        if (node->down) {
            fprintf(out, "  Utilization: theoretical - (no tasks left), measured %.3f "
                    "(%llu busy of %u ticks before going down)\n",
                    node->observedTicks > 0 ? (double)busy / (double)node->observedTicks : 0.0,
                    (unsigned long long)busy, (unsigned)node->observedTicks);
        } else {
            fprintf(out, "  Utilization: theoretical %.3f (bound %.3f, %u task(s) at the end), "
                    "measured %.3f (%llu busy of %u ticks)\n",
                    theoretical, bound, (unsigned)entryCount,
                    node->observedTicks > 0 ? (double)busy / (double)node->observedTicks : 0.0,
                    (unsigned long long)busy, (unsigned)node->observedTicks);
        }
        fprintf(out, "  Task switches: %u (%u preemptions)\n", (unsigned)node->taskSwitches,
                (unsigned)node->preemptions);
    }
}

/* Report: prints per-task counters */
static void PrintTaskTables(const Report *report, FILE *out)
{
    uint32_t n;
    uint32_t t;

    fprintf(out, "\n--- Tasks: releases, execution, deadlines and failures ---\n");
    fprintf(out, "Rel: jobs released (periodic, or sporadic accepted)  Ign: sporadic releases "
            "ignored (before the minimum interval)\n");
    fprintf(out, "Run: ticks executed  Resp: response time in ticks (release to end)\n");
    for (n = 0; n < report->nodeCount; n++) {
        const NodeStats *node = &report->nodes[n];

        fprintf(out, "Node %u:\n", (unsigned)node->nodeId);
        fprintf(out, "  %-5s %-4s %-6s %-6s %-6s %-5s %-5s %-5s %-4s %-7s %-6s %-6s %-6s %-6s "
                "%-6s\n",
                "Task", "Type", "Rel", "Start", "Done", "Miss", "Abort", "Preem", "Ign", "Run",
                "MinRsp", "AvgRsp", "MaxRsp", "U teo", "U med");
        for (t = 0; t < report->taskCount; t++) {
            const TaskStats *task = &report->tasks[t];
            uint32_t executed = task->executedTicks;
            uint32_t cost = 0;
            uint32_t period = 0;
            double average = 0.0;
            char theoretical[16];

            if (task->nodeId != node->nodeId) {
                continue;
            }
            if (task->running && node->observedTicks > task->runningSince) {
                executed += node->observedTicks - task->runningSince;
            }
            if (task->completions > 0) {
                average = (double)task->totalResponse / (double)task->completions;
            }
            if (TaskLoad(report, node->nodeId, task->taskId, &cost, &period) && period > 0) {
                snprintf(theoretical, sizeof(theoretical), "%.3f", (double)cost / (double)period);
            } else {
                snprintf(theoretical, sizeof(theoretical), "-");
            }
            fprintf(out, "  %-5u %-4s %-6u %-6u %-6u %-5u %-5u %-5u %-4u %-7u %-6u %-6.1f %-6u "
                    "%-6s %-6.3f\n",
                    (unsigned)task->taskId, task->sporadic ? "S" : "P",
                    (unsigned)(task->releases + task->sporadicReleases), (unsigned)task->starts,
                    (unsigned)task->completions, (unsigned)task->deadlineMisses,
                    (unsigned)task->aborts, (unsigned)task->preemptions,
                    (unsigned)task->sporadicIgnored, (unsigned)executed,
                    (unsigned)task->minResponse, average, (unsigned)task->maxResponse, theoretical,
                    node->observedTicks > 0 ? (double)executed / (double)node->observedTicks : 0.0);
        }
    }
}

/* Report: prints sporadic response latency */
static void PrintSporadic(const Report *report, FILE *out)
{
    double tickMs = report->haveRun ? (double)report->run.tickMs : 0.0;
    bool any = false;
    uint32_t t;

    fprintf(out, "\n--- Sporadic tasks: response latency ---\n");
    for (t = 0; t < report->taskCount; t++) {
        const TaskStats *task = &report->tasks[t];
        double average;

        if (!task->sporadic) {
            continue;
        }
        any = true;
        fprintf(out, "Node %u task %u: %u release(s) accepted, %u ignored, %u completed, "
                "%u missed\n",
                (unsigned)task->nodeId, (unsigned)task->taskId, (unsigned)task->sporadicReleases,
                (unsigned)task->sporadicIgnored, (unsigned)task->completions,
                (unsigned)task->deadlineMisses);
        if (task->completions == 0) {
            continue;
        }
        average = (double)task->totalResponse / (double)task->completions;
        fprintf(out, "  latency from release to completion: min %u, avg %.1f, max %u ticks "
                "(%.1f / %.1f / %.1f ms)\n",
                (unsigned)task->minResponse, average, (unsigned)task->maxResponse,
                task->minResponse * tickMs, average * tickMs, task->maxResponse * tickMs);
    }
    if (!any) {
        fprintf(out, "No sporadic task ran\n");
    }
}

/* Gantt state of one task row while replaying */
typedef struct {
    uint32_t taskId;
    bool pending;
    bool running;
    bool completed;
    bool missed;
} GanttRow;

/* Report: finds a Gantt row */
static GanttRow *FindRow(GanttRow *rows, uint32_t rowCount, uint32_t taskId)
{
    uint32_t i;

    for (i = 0; i < rowCount; i++) {
        if (rows[i].taskId == taskId) {
            return &rows[i];
        }
    }
    return NULL;
}

/* Report: applies an event to a Gantt row */
static void ApplyToRow(GanttRow *row, EventType type)
{
    switch (type) {
    case EventTypeRelease:
    case EventTypeSporadicRelease:
        row->pending = true;
        break;
    case EventTypeStart:
        row->running = true;
        break;
    case EventTypePreempt:
        row->running = false;
        break;
    case EventTypeComplete:
        row->completed = true;
        break;
    case EventTypeDeadlineMiss:
        row->missed = true;
        break;
    case EventTypeAbort:
        row->pending = false;
        row->running = false;
        break;
    default:
        break;
    }
}

/* Report: prints one Gantt window */
static void PrintGanttWindow(const Report *report, const NodeStats *node, uint32_t from,
                             uint32_t to, FILE *out)
{
    GanttRow rows[ReportMaxTasks];
    uint32_t rowCount = 0;
    uint32_t width = to - from;
    char *grid;
    char ruler[ReportGanttWidth + 16];
    size_t next = 0;
    uint32_t tick;
    uint32_t r;
    uint32_t t;

    /* One row per task seen on the node, by id */
    for (t = 0; t < report->taskCount; t++) {
        const TaskStats *task = &report->tasks[t];
        uint32_t position = rowCount;

        if (task->nodeId != node->nodeId) {
            continue;
        }
        while (position > 0 && rows[position - 1].taskId > task->taskId) {
            rows[position] = rows[position - 1];
            position--;
        }
        memset(&rows[position], 0, sizeof(rows[position]));
        rows[position].taskId = task->taskId;
        rowCount++;
    }
    if (rowCount == 0 || width == 0) {
        return;
    }
    grid = malloc((size_t)rowCount * width);
    if (grid == NULL) {
        return;
    }
    memset(grid, '.', (size_t)rowCount * width);

    for (tick = 0; tick < to; tick++) {
        while (next < report->logCount) {
            const ReportLogged *logged = &report->log[next];
            GanttRow *row;

            if (logged->nodeId != node->nodeId) {
                next++;
                continue;
            }
            if (logged->event.tick > tick) {
                break;
            }
            row = FindRow(rows, rowCount, logged->event.taskId);
            if (row != NULL) {
                ApplyToRow(row, logged->event.type);
            }
            next++;
        }
        for (r = 0; r < rowCount; r++) {
            GanttRow *row = &rows[r];

            if (tick >= from) {
                char mark = '.';

                if (row->missed) {
                    mark = '!';
                } else if (row->running || row->completed) {
                    mark = '#';
                } else if (row->pending) {
                    mark = '-';
                }
                grid[(size_t)r * width + (tick - from)] = mark;
            }
            if (row->completed) {
                row->running = false;
                row->pending = false;
            }
            row->completed = false;
            row->missed = false;
        }
    }

    /* Tick numbers every 10 ticks */
    memset(ruler, ' ', sizeof(ruler));
    for (tick = from; tick < to; tick++) {
        if (tick % 10 == 0) {
            char label[12];
            int length = snprintf(label, sizeof(label), "%u", (unsigned)tick);

            if (length > 0 && (size_t)(tick - from) + (size_t)length <= width) {
                memcpy(&ruler[tick - from], label, (size_t)length);
            }
        }
    }
    ruler[width < sizeof(ruler) - 1 ? width : sizeof(ruler) - 1] = '\0';
    fprintf(out, "  tick   %s\n", ruler);
    for (r = 0; r < rowCount; r++) {
        fprintf(out, "  T%-4u  %.*s\n", (unsigned)rows[r].taskId, (int)width,
                &grid[(size_t)r * width]);
    }
    free(grid);
}

/* Report: prints the Gantt chart */
static void PrintGantt(const Report *report, FILE *out)
{
    uint32_t n;

    fprintf(out, "\n--- Gantt chart per node ---\n");
    fprintf(out, "# running   - released, waiting   ! deadline missed (job aborted)   . no job\n");
    if (report->logTruncated) {
        fprintf(out, "(only the first %u events were kept for the chart)\n",
                (unsigned)report->logCount);
    }
    for (n = 0; n < report->nodeCount; n++) {
        const NodeStats *node = &report->nodes[n];
        uint32_t end = node->observedTicks;

        if (end == 0) {
            fprintf(out, "Node %u: no ticks\n", (unsigned)node->nodeId);
            continue;
        }
        fprintf(out, "Node %u, ticks 0 to %u:\n", (unsigned)node->nodeId,
                (unsigned)(end < ReportGanttWidth ? end : ReportGanttWidth) - 1u);
        PrintGanttWindow(report, node, 0, end < ReportGanttWidth ? end : ReportGanttWidth, out);
        if (end > 2u * ReportGanttWidth) {
            fprintf(out, "Node %u, last ticks %u to %u:\n", (unsigned)node->nodeId,
                    (unsigned)(end - ReportGanttWidth), (unsigned)(end - 1u));
            PrintGanttWindow(report, node, end - ReportGanttWidth, end, out);
        }
    }
}

/* Report: lists task switches and misses */
static void PrintSwitchesAndMisses(const Report *report, FILE *out)
{
    uint32_t n;
    uint32_t i;

    fprintf(out, "\n--- Task switches and deadline misses (first %d per node) ---\n",
            ReportListMax);
    for (n = 0; n < report->nodeCount; n++) {
        const NodeStats *node = &report->nodes[n];

        fprintf(out, "Node %u switches:", (unsigned)node->nodeId);
        if (node->switchCount == 0) {
            fprintf(out, " none");
        }
        for (i = 0; i < node->switchCount; i++) {
            const ReportSwitch *entry = &node->switches[i];

            if (entry->from == 0) {
                fprintf(out, " %u:idle>%u", (unsigned)entry->tick, (unsigned)entry->to);
            } else {
                fprintf(out, " %u:%u>%u", (unsigned)entry->tick, (unsigned)entry->from,
                        (unsigned)entry->to);
            }
        }
        fprintf(out, "\nNode %u misses:", (unsigned)node->nodeId);
        if (node->missCount == 0) {
            fprintf(out, " none");
        }
        for (i = 0; i < node->missCount; i++) {
            const ReportMiss *entry = &node->misses[i];

            fprintf(out, " tick %u task %u (%u left)", (unsigned)entry->tick,
                    (unsigned)entry->taskId, (unsigned)entry->left);
        }
        fprintf(out, "\n");
    }
}

/* Report: prints link counters */
static void PrintLink(const Report *report, FILE *out)
{
    uint32_t n;

    fprintf(out, "\n--- Link ---\n");
    for (n = 0; n < report->nodeCount; n++) {
        const NodeStats *node = &report->nodes[n];
        const HostLinkStats *link = &node->link;

        if (!node->haveLink) {
            continue;
        }
        fprintf(out, "Node %u: frames %u, events %u, corrupt frames %u, tick reports %u, "
                "misses %u, dropped events %u, retransmissions %u, leftovers of an earlier "
                "run %u\n",
                (unsigned)node->nodeId, (unsigned)link->framesReceived,
                (unsigned)link->eventsReceived, (unsigned)link->badFrames,
                (unsigned)link->tickReports, (unsigned)link->missCount,
                (unsigned)link->droppedEvents, (unsigned)link->retransmissions,
                (unsigned)link->staleFrames);
    }
}

/* Report: closest skew point in time */
static uint32_t NearestPoint(const NodeStats *node, uint64_t hostUs)
{
    uint32_t best = 0;
    uint64_t bestGap = UINT64_MAX;
    uint32_t i;

    for (i = 0; i < node->skewCount; i++) {
        uint64_t time = node->skew[i].hostUs;
        uint64_t gap = time > hostUs ? time - hostUs : hostUs - time;

        if (gap < bestGap) {
            bestGap = gap;
            best = i;
        }
    }
    return best;
}

/* Report: prints skew between nodes */
static void PrintSkew(const Report *report, FILE *out)
{
    const NodeStats *reference = NULL;
    uint32_t n;

    fprintf(out, "\n--- Skew between nodes (F12) ---\n");
    fprintf(out, "Tick zero of each node estimated by the host as arrival - tick x tick_ms, "
            "smallest value of each %d tick reports\n", ReportSkewWindow);
    for (n = 0; n < report->nodeCount; n++) {
        if (report->nodes[n].skewCount > 0) {
            reference = &report->nodes[n];
            break;
        }
    }
    if (reference == NULL) {
        fprintf(out, "Not enough tick reports\n");
        return;
    }
    for (n = 0; n < report->nodeCount; n++) {
        const NodeStats *node = &report->nodes[n];
        const ReportSkewPoint *nodeFirst;
        const ReportSkewPoint *referenceFirst;
        uint64_t start;
        uint64_t end;
        double early;
        double late;
        double span;

        if (node == reference) {
            continue;
        }
        if (node->skewCount == 0) {
            fprintf(out, "Node %u: not enough tick reports\n", (unsigned)node->nodeId);
            continue;
        }
        /* Both nodes are compared at the same moments: their first windows,
           and the last moment both were still sending reports */
        nodeFirst = &node->skew[0];
        referenceFirst = &reference->skew[0];
        start = nodeFirst->hostUs > referenceFirst->hostUs ? nodeFirst->hostUs :
                                                             referenceFirst->hostUs;
        end = node->skew[node->skewCount - 1].hostUs;
        if (reference->skew[reference->skewCount - 1].hostUs < end) {
            end = reference->skew[reference->skewCount - 1].hostUs;
        }
        early = (double)(nodeFirst->zeroUs - referenceFirst->zeroUs) / 1000.0;
        late = (double)(node->skew[NearestPoint(node, end)].zeroUs -
                        reference->skew[NearestPoint(reference, end)].zeroUs) / 1000.0;
        span = end > start ? (double)(end - start) / 1e6 : 0.0;
        if (span > 0.0) {
            fprintf(out, "Node %u vs node %u: %+.2f ms at the start, %+.2f ms %.1f s later "
                    "(while both ran), drift %+.0f ppm\n", (unsigned)node->nodeId,
                    (unsigned)reference->nodeId, early, late, span,
                    (late - early) * 1000.0 / span);
        } else {
            fprintf(out, "Node %u vs node %u: %+.2f ms (one window of reports)\n",
                    (unsigned)node->nodeId, (unsigned)reference->nodeId, early);
        }
        if (node->started && reference->started) {
            fprintf(out, "  Ack of Start: node %u %+.2f ms after node %u\n",
                    (unsigned)node->nodeId,
                    ((double)node->startedUs - (double)reference->startedUs) / 1000.0,
                    (unsigned)reference->nodeId);
        }
    }
}

/* Report: prints the full summary */
void ReportPrint(const Report *report, FILE *out)
{
    if (report == NULL || out == NULL) {
        return;
    }
    fprintf(out, "===== Report =====\n");
    PrintRun(report, out);
    PrintDecisions(report, out);
    PrintNodes(report, out);
    PrintTaskTables(report, out);
    PrintSporadic(report, out);
    PrintGantt(report, out);
    PrintSwitchesAndMisses(report, out);
    PrintLink(report, out);
    PrintSkew(report, out);
}
