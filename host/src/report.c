#include "report.h"

#include <string.h>

/* Entry for the task, created on first use; NULL when the table is full */
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

/* Entry for the node, created on first use; NULL when the table is full */
static NodeStats *FindNode(Report *report, uint32_t nodeId)
{
    NodeStats *entry;
    uint32_t i;

    for (i = 0; i < report->nodeCount; i++) {
        if (report->nodes[i].nodeId == nodeId) {
            return &report->nodes[i];
        }
    }
    if (report->nodeCount >= ReportMaxNodes) {
        return NULL;
    }
    entry = &report->nodes[report->nodeCount];
    memset(entry, 0, sizeof(*entry));
    entry->nodeId = nodeId;
    report->nodeCount++;
    return entry;
}

void ReportInit(Report *report)
{
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
}

bool ReportRecord(Report *report, const HostEvent *hostEvent)
{
    NodeStats *node;
    TaskStats *task;

    if (report == NULL || hostEvent == NULL) {
        return false;
    }
    report->totalEvents++;

    node = FindNode(report, hostEvent->nodeId);
    task = FindTask(report, hostEvent->nodeId, hostEvent->event.taskId);
    if (node == NULL || task == NULL) {
        report->ignoredEvents++;
        return false;
    }

    switch (hostEvent->event.type) {
    case EventTypeRelease:
        task->releases++;
        break;
    case EventTypeStart:
        task->starts++;
        break;
    case EventTypePreempt:
        task->preemptions++;
        node->taskSwitches++;
        break;
    case EventTypeComplete:
        task->completions++;
        task->totalResponse += hostEvent->event.value;
        if (hostEvent->event.value > task->maxResponse) {
            task->maxResponse = hostEvent->event.value;
        }
        break;
    case EventTypeDeadlineMiss:
        task->deadlineMisses++;
        break;
    case EventTypeAbort:
        task->aborts++;
        break;
    case EventTypeSporadicRelease:
        task->sporadicReleases++;
        break;
    case EventTypeSporadicIgnored:
        task->sporadicIgnored++;
        break;
    default:
        report->ignoredEvents++;
        return false;
    }
    return true;
}

void ReportPrint(const Report *report, FILE *out)
{
    uint32_t n;
    uint32_t t;

    if (report == NULL || out == NULL) {
        return;
    }
    fprintf(out, "===== Report =====\n");
    fprintf(out, "Events received: %u\n", (unsigned int)report->totalEvents);
    fprintf(out, "Events ignored: %u\n", (unsigned int)report->ignoredEvents);

    for (n = 0; n < report->nodeCount; n++) {
        const NodeStats *node = &report->nodes[n];

        fprintf(out, "Node %u: %u task switches\n", (unsigned int)node->nodeId,
                (unsigned int)node->taskSwitches);
        fprintf(out, "  %-5s %-5s %-5s %-5s %-5s %-5s %-5s %-8s %-8s %-5s %-5s\n",
                "Task", "Rel", "Start", "Done", "Miss", "Abort", "Preem",
                "MaxResp", "AvgResp", "SpRel", "SpIgn");
        for (t = 0; t < report->taskCount; t++) {
            const TaskStats *task = &report->tasks[t];
            double average = 0.0;

            if (task->nodeId != node->nodeId) {
                continue;
            }
            if (task->completions > 0) {
                average = (double)task->totalResponse / (double)task->completions;
            }
            fprintf(out, "  %-5u %-5u %-5u %-5u %-5u %-5u %-5u %-8u %-8.1f %-5u %-5u\n",
                    (unsigned int)task->taskId, (unsigned int)task->releases,
                    (unsigned int)task->starts, (unsigned int)task->completions,
                    (unsigned int)task->deadlineMisses, (unsigned int)task->aborts,
                    (unsigned int)task->preemptions, (unsigned int)task->maxResponse,
                    average, (unsigned int)task->sporadicReleases,
                    (unsigned int)task->sporadicIgnored);
        }
    }
}
