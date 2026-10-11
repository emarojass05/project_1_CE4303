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
    Report report;

    ReportInit(&report);
    CHECK(report.totalEvents == 0);
    CHECK(report.ignoredEvents == 0);
    CHECK(report.taskCount == 0);
    CHECK(report.nodeCount == 0);
}

static void TestCountersPerTask(void)
{
    Report report;
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
}

static void TestPreemptionsCountOnTaskAndNode(void)
{
    Report report;

    ReportInit(&report);
    Record(&report, 1, EventTypePreempt, 2, 1);
    Record(&report, 1, EventTypePreempt, 2, 1);
    Record(&report, 1, EventTypePreempt, 3, 1);
    Record(&report, 2, EventTypePreempt, 2, 1);

    CHECK(report.nodeCount == 2);
    CHECK(report.nodes[0].nodeId == 1 && report.nodes[0].taskSwitches == 3);
    CHECK(report.nodes[1].nodeId == 2 && report.nodes[1].taskSwitches == 1);
    CHECK(report.taskCount == 3);
    CHECK(report.tasks[0].preemptions == 2);
    CHECK(report.tasks[1].preemptions == 1);
    CHECK(report.tasks[2].nodeId == 2 && report.tasks[2].preemptions == 1);
}

static void TestSameTaskIdOnTwoNodesIsSeparate(void)
{
    Report report;

    ReportInit(&report);
    Record(&report, 1, EventTypeRelease, 7, 0);
    Record(&report, 2, EventTypeRelease, 7, 0);
    Record(&report, 2, EventTypeRelease, 7, 0);

    CHECK(report.taskCount == 2);
    CHECK(report.tasks[0].releases == 1);
    CHECK(report.tasks[1].releases == 2);
}

static void TestFullTablesAndBadInput(void)
{
    Report report;
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
}

static void TestPrintedSummary(void)
{
    Report report;
    FILE *file = tmpfile();
    char text[2048];
    size_t length;

    ReportInit(&report);
    Record(&report, 3, EventTypeRelease, 4, 0);
    Record(&report, 3, EventTypeComplete, 4, 2);
    Record(&report, 3, EventTypeComplete, 4, 3);
    Record(&report, 3, EventTypePreempt, 4, 1);

    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    ReportPrint(&report, file);
    rewind(file);
    length = fread(text, 1, sizeof(text) - 1, file);
    text[length] = '\0';
    fclose(file);

    CHECK(strstr(text, "Events received: 4") != NULL);
    CHECK(strstr(text, "Events ignored: 0") != NULL);
    CHECK(strstr(text, "Node 3: 1 task switches") != NULL);
    CHECK(strstr(text, "2.5") != NULL);
}

int main(void)
{
    TestEmptyReport();
    TestCountersPerTask();
    TestPreemptionsCountOnTaskAndNode();
    TestSameTaskIdOnTwoNodesIsSeparate();
    TestFullTablesAndBadInput();
    TestPrintedSummary();

    if (failureCount != 0) {
        printf("test_report: %d failure(s)\n", failureCount);
        return 1;
    }
    printf("test_report: all checks passed\n");
    return 0;
}