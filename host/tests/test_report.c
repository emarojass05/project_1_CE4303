#include <stdio.h>
#include <string.h>

#include "report.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static HostEvent MakeEvent(uint32_t nodeId, EventType type, uint32_t taskId,
                           uint32_t value)
{
    HostEvent hostEvent;

    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventNode;
    hostEvent.nodeId = nodeId;
    hostEvent.event.type = type;
    hostEvent.event.tick = 0;
    hostEvent.event.taskId = taskId;
    hostEvent.event.value = value;
    return hostEvent;
}

static void Record(Report *report, uint32_t nodeId, EventType type,
                   uint32_t taskId, uint32_t value)
{
    HostEvent hostEvent = MakeEvent(nodeId, type, taskId, value);
    CHECK(ReportRecord(report, &hostEvent));
}

static void TestEmptyReport(void)
{
    static Report report;

    ReportInit(&report);
    CHECK(report.totalEvents == 0);
    CHECK(report.ignoredEvents == 0);
    CHECK(report.taskCount == 0);
    CHECK(report.nodeCount == 0);
}

static void TestCountersPerTask(void)
{
    static Report report;
    const TaskStats *task;

    ReportInit(&report);
    Record(&report, 1, EventTypeRelease, 5, 0);
    Record(&report, 1, EventTypeRelease, 5, 0);
    Record(&report, 1, EventTypeStart, 5, 0);
    Record(&report, 1, EventTypeComplete, 5, 4);
    Record(&report, 1, EventTypeComplete, 5, 10);
    Record(&report, 1, EventTypeDeadlineMiss, 5, 2);
    Record(&report, 1, EventTypeAbort, 5, 0);
    Record(&report, 1, EventTypeSporadicRelease, 5, 0);
    Record(&report, 1, EventTypeSporadicIgnored, 5, 0);
    Record(&report, 1, EventTypeSporadicIgnored, 5, 0);

    CHECK(report.taskCount == 1);
    CHECK(report.totalEvents == 10);
    task = &report.tasks[0];
    CHECK(task->nodeId == 1 && task->taskId == 5);
    CHECK(task->releases == 2);
    CHECK(task->starts == 1);
    CHECK(task->completions == 2);
    CHECK(task->maxResponse == 10);
    CHECK(task->totalResponse == 14);
    CHECK(task->deadlineMisses == 1);
    CHECK(task->aborts == 1);
    CHECK(task->sporadicReleases == 1);
    CHECK(task->sporadicIgnored == 2);
    CHECK(task->preemptions == 0);
    ReportFree(&report);
}

static void TestPreemptionsCountOnTaskAndNode(void)
{
    static Report report;

    ReportInit(&report);
    Record(&report, 1, EventTypePreempt, 2, 1);
    Record(&report, 1, EventTypePreempt, 2, 1);
    Record(&report, 1, EventTypePreempt, 3, 1);
    Record(&report, 2, EventTypePreempt, 2, 1);

    CHECK(report.nodeCount == 2);
    CHECK(report.nodes[0].nodeId == 1 && report.nodes[0].preemptions == 3);
    CHECK(report.nodes[1].nodeId == 2 && report.nodes[1].preemptions == 1);
    CHECK(report.taskCount == 3);
    CHECK(report.tasks[0].preemptions == 2);
    CHECK(report.tasks[1].preemptions == 1);
    CHECK(report.tasks[2].nodeId == 2 && report.tasks[2].preemptions == 1);
    ReportFree(&report);
}

static void TestSameTaskIdOnTwoNodesIsSeparate(void)
{
    static Report report;

    ReportInit(&report);
    Record(&report, 1, EventTypeRelease, 7, 0);
    Record(&report, 2, EventTypeRelease, 7, 0);
    Record(&report, 2, EventTypeRelease, 7, 0);

    CHECK(report.taskCount == 2);
    CHECK(report.tasks[0].releases == 1);
    CHECK(report.tasks[1].releases == 2);
    ReportFree(&report);
}

static void TestFullTablesAndBadInput(void)
{
    static Report report;
    HostEvent hostEvent;
    uint32_t id;

    ReportInit(&report);
    for (id = 0; id < ReportMaxTasks; id++) {
        Record(&report, 1, EventTypeRelease, id, 0);
    }
    hostEvent = MakeEvent(1, EventTypeRelease, 999, 0);
    CHECK(!ReportRecord(&report, &hostEvent));
    CHECK(report.ignoredEvents == 1);
    CHECK(report.taskCount == ReportMaxTasks);

    hostEvent = MakeEvent(1, (EventType)99, 0, 0);
    CHECK(!ReportRecord(&report, &hostEvent));
    CHECK(report.ignoredEvents == 2);

    CHECK(!ReportRecord(NULL, &hostEvent));
    CHECK(!ReportRecord(&report, NULL));
    ReportInit(NULL);
    ReportPrint(NULL, stdout);
    ReportPrint(&report, NULL);
    ReportFree(&report);
}

/* Writes the report to a string */
static void PrintToText(const Report *report, char *text, size_t size)
{
    FILE *file = tmpfile();
    size_t length;

    text[0] = '\0';
    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    ReportPrint(report, file);
    rewind(file);
    length = fread(text, 1, size - 1, file);
    text[length] = '\0';
    fclose(file);
}

static void TestPrintedSummary(void)
{
    static Report report;
    static char text[32768];

    ReportInit(&report);
    Record(&report, 3, EventTypeRelease, 4, 0);
    Record(&report, 3, EventTypeComplete, 4, 2);
    Record(&report, 3, EventTypeComplete, 4, 3);
    Record(&report, 3, EventTypePreempt, 4, 1);
    PrintToText(&report, text, sizeof(text));

    CHECK(strstr(text, "Events received: 4") != NULL);
    CHECK(strstr(text, "Events ignored: 0") != NULL);
    CHECK(strstr(text, "Task switches: 0 (1 preemptions)") != NULL);
    CHECK(strstr(text, "2.5") != NULL);
    ReportFree(&report);
}

static void RecordAt(Report *report, uint32_t nodeId, EventType type, uint32_t tick,
                     uint32_t taskId, uint32_t value)
{
    HostEvent hostEvent = MakeEvent(nodeId, type, taskId, value);

    hostEvent.event.tick = tick;
    CHECK(ReportRecord(report, &hostEvent));
}

static void RecordPlanner(Report *report, HostEvent *hostEvent)
{
    CHECK(ReportRecord(report, hostEvent));
}

static HostEvent Accepted(uint32_t nodeId, uint32_t taskId, TaskType type, uint32_t cost,
                          uint32_t period, uint32_t origin)
{
    HostEvent hostEvent;

    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventTaskAccepted;
    hostEvent.nodeId = nodeId;
    hostEvent.task.id = taskId;
    hostEvent.task.type = type;
    hostEvent.task.work = cost * 10;
    hostEvent.task.period = period;
    hostEvent.task.deadline = period;
    hostEvent.task.criticality = 1;
    hostEvent.value = cost;
    hostEvent.origin = origin;
    return hostEvent;
}

/* Task 1 (C=2, T=5) and task 2 (C=1, T=10) on node 1 for 10 ticks, with a
   preemption, a miss, a sporadic task, a rejected task and a node down */
static void TestFullRun(void)
{
    static Report report;
    static char text[32768];
    HostEvent hostEvent;

    ReportInit(&report);
    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventRunInfo;
    hostEvent.run.algorithm = 0; /* EDF */
    hostEvent.run.tickMs = 10;
    hostEvent.run.nodeCount = 2;
    hostEvent.run.tickReportEvery = 5;
    hostEvent.run.missedReports = 3;
    RecordPlanner(&report, &hostEvent);

    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventCalibration;
    hostEvent.nodeId = 1;
    hostEvent.value = 60;
    snprintf(hostEvent.text, sizeof(hostEvent.text), "/dev/ttyUSB0");
    RecordPlanner(&report, &hostEvent);

    hostEvent = Accepted(1, 1, TaskTypePeriodic, 2, 5, HostOriginFile);
    RecordPlanner(&report, &hostEvent);
    hostEvent = Accepted(1, 2, TaskTypePeriodic, 1, 10, HostOriginFile);
    RecordPlanner(&report, &hostEvent);
    hostEvent = Accepted(2, 3, TaskTypePeriodic, 1, 4, HostOriginFile);
    RecordPlanner(&report, &hostEvent);
    hostEvent = Accepted(1, 6, TaskTypeSporadic, 1, 20, HostOriginManual);
    RecordPlanner(&report, &hostEvent);
    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventTaskRejected;
    hostEvent.task.id = 9;
    hostEvent.task.period = 10;
    hostEvent.reason = HostRejectNotSchedulable;
    RecordPlanner(&report, &hostEvent);

    /* Node 1: tick 0 releases 1 and 2, 2 starts... EDF picks 1 first */
    RecordAt(&report, 1, EventTypeRelease, 0, 1, 0);
    RecordAt(&report, 1, EventTypeRelease, 0, 2, 0);
    RecordAt(&report, 1, EventTypeStart, 0, 1, 0);
    RecordAt(&report, 1, EventTypeComplete, 1, 1, 2);
    RecordAt(&report, 1, EventTypeStart, 2, 2, 0);
    RecordAt(&report, 1, EventTypeComplete, 2, 2, 3);
    RecordAt(&report, 1, EventTypeSporadicRelease, 3, 6, 0);
    RecordAt(&report, 1, EventTypeStart, 3, 6, 0);
    RecordAt(&report, 1, EventTypeComplete, 3, 6, 1);
    RecordAt(&report, 1, EventTypeSporadicIgnored, 4, 6, 0);
    RecordAt(&report, 1, EventTypeRelease, 5, 1, 0);
    RecordAt(&report, 1, EventTypeStart, 5, 1, 0);
    RecordAt(&report, 1, EventTypePreempt, 6, 1, 2);
    RecordAt(&report, 1, EventTypeStart, 6, 2, 0);
    RecordAt(&report, 1, EventTypeDeadlineMiss, 9, 1, 1);
    RecordAt(&report, 1, EventTypeAbort, 9, 1, 0);

    /* Node 2 goes down after tick 4 */
    RecordAt(&report, 2, EventTypeRelease, 0, 3, 0);
    RecordAt(&report, 2, EventTypeStart, 0, 3, 0);
    RecordAt(&report, 2, EventTypeComplete, 0, 3, 1);
    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventNodeDown;
    hostEvent.nodeId = 2;
    hostEvent.event.tick = 4;
    hostEvent.value = HostDownByCommand;
    RecordPlanner(&report, &hostEvent);
    hostEvent = Accepted(1, 3, TaskTypePeriodic, 1, 4, HostOriginRelocated);
    RecordPlanner(&report, &hostEvent);

    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventLinkStats;
    hostEvent.nodeId = 1;
    hostEvent.link.badFrames = 2;
    RecordPlanner(&report, &hostEvent);

    CHECK(report.decisionCount == 7);
    CHECK(report.nodes[0].observedTicks == 10);
    CHECK(report.nodes[0].taskSwitches == 5);
    CHECK(report.nodes[0].switchCount == 5);
    CHECK(report.nodes[0].switches[0].from == 0 && report.nodes[0].switches[0].to == 1);
    CHECK(report.nodes[0].switches[4].from == 1 && report.nodes[0].switches[4].to == 2);
    CHECK(report.tasks[0].taskId == 1 && report.tasks[0].executedTicks == 3);
    CHECK(report.tasks[1].taskId == 2 && report.tasks[1].executedTicks == 1);

    PrintToText(&report, text, sizeof(text));
    if (failureCount != 0) {
        printf("%s", text);
    }
    CHECK(strstr(text, "Run: EDF") != NULL);
    CHECK(strstr(text, "Node 1 (/dev/ttyUSB0): sn 60") != NULL);
    CHECK(strstr(text, "task 9 (P") != NULL);
    CHECK(strstr(text, "rejected, fails the admission test on every node") != NULL);
    CHECK(strstr(text, "accepted on node 1, C = 1, U = 0.250") != NULL);
    CHECK(strstr(text, "relocated") != NULL);
    CHECK(strstr(text, "node 2 down") != NULL);
    CHECK(strstr(text, "DOWN at") != NULL && strstr(text, "k command") != NULL);
    /* Final set on node 1: 2/5 + 1/10 + 1/20 + 1/4 = 0.8; busy 3+1+1 of 10 ticks
       plus task 2 still running from tick 6 to 10 */
    CHECK(strstr(text, "theoretical 0.800") != NULL);
    CHECK(strstr(text, "measured 0.900 (9 busy of 10 ticks)") != NULL);
    CHECK(strstr(text, "latency from release to completion: min 1, avg 1.0, max 1 ticks") !=
          NULL);
    CHECK(strstr(text, "1 release(s) accepted, 1 ignored") != NULL);
    CHECK(strstr(text, "corrupt frames 2") != NULL);
    /* Gantt rows of node 1, ticks 0 to 9 */
    CHECK(strstr(text, "T1     ##...#---!") != NULL);
    CHECK(strstr(text, "T2     --#...####") != NULL);
    CHECK(strstr(text, "T6     ...#......") != NULL);
    CHECK(strstr(text, "0:idle>1 2:1>2 3:2>6 5:idle>1 6:1>2") != NULL);
    CHECK(strstr(text, "tick 9 task 1 (1 left)") != NULL);
    ReportFree(&report);
}

/* Node 2 reports 3 ms after node 1, and 1 ms later at the end */
static void TestSkewFromTickReports(void)
{
    static Report report;
    static char text[32768];
    HostEvent hostEvent;
    uint32_t i;

    ReportInit(&report);
    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventRunInfo;
    hostEvent.run.tickMs = 10;
    RecordPlanner(&report, &hostEvent);
    for (i = 1; i <= 2u * ReportSkewWindow; i++) {
        uint64_t base = 1000000u + (uint64_t)i * 100000u; /* Tick 10 i at 1 s + 100 ms i */
        uint64_t later = i > ReportSkewWindow ? 4000u : 3000u;

        memset(&hostEvent, 0, sizeof(hostEvent));
        hostEvent.kind = HostEventTickReport;
        hostEvent.nodeId = 1;
        hostEvent.event.tick = 10u * i;
        hostEvent.hostUs = base + (i % 3) * 500u; /* Jitter the minimum filters out */
        RecordPlanner(&report, &hostEvent);
        hostEvent.nodeId = 2;
        hostEvent.hostUs = base + later;
        RecordPlanner(&report, &hostEvent);
    }
    /* Node 2 goes down; node 1 keeps reporting with a shifted clock, which
       must not count because node 2 was no longer there to compare */
    for (i = 2u * ReportSkewWindow + 1; i <= 3u * ReportSkewWindow; i++) {
        memset(&hostEvent, 0, sizeof(hostEvent));
        hostEvent.kind = HostEventTickReport;
        hostEvent.nodeId = 1;
        hostEvent.event.tick = 10u * i;
        hostEvent.hostUs = 1000000u + (uint64_t)i * 100000u + 50000u;
        RecordPlanner(&report, &hostEvent);
    }
    PrintToText(&report, text, sizeof(text));
    if (strstr(text, "Node 2 vs node 1: +3.00 ms at the start, +4.00 ms 1.0 s later") == NULL) {
        printf("%s", text);
    }
    CHECK(strstr(text, "Node 2 vs node 1: +3.00 ms at the start, +4.00 ms 1.0 s later") != NULL);
    ReportFree(&report);
}

int main(void)
{
    TestEmptyReport();
    TestCountersPerTask();
    TestPreemptionsCountOnTaskAndNode();
    TestSameTaskIdOnTwoNodesIsSeparate();
    TestFullTablesAndBadInput();
    TestPrintedSummary();
    TestFullRun();
    TestSkewFromTickReports();

    if (failureCount != 0) {
        printf("test_report: %d failure(s)\n", failureCount);
        return 1;
    }
    printf("test_report: all checks passed\n");
    return 0;
}