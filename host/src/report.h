#ifndef REPORT_H
#define REPORT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "event_queue.h"

/* Limits of the tables kept by the report */
enum {
    ReportMaxTasks = 64,
    ReportMaxNodes = 8
};

/* Counters for one task of one node */
typedef struct {
    uint32_t nodeId;
    uint32_t taskId;
    uint32_t releases;
    uint32_t starts;
    uint32_t completions;
    uint32_t deadlineMisses;
    uint32_t aborts;
    uint32_t preemptions;      /* Times this task lost the processor */
    uint32_t sporadicReleases;
    uint32_t sporadicIgnored;
    uint32_t maxResponse;      /* Longest response time, in ticks */
    uint64_t totalResponse;    /* Sum of response times, in ticks */
} TaskStats;

/* Counters for one node */
typedef struct {
    uint32_t nodeId;
    uint32_t taskSwitches; /* Preemptions seen on this node */
} NodeStats;

typedef struct {
    TaskStats tasks[ReportMaxTasks];
    NodeStats nodes[ReportMaxNodes];
    uint32_t taskCount;
    uint32_t nodeCount;
    uint32_t totalEvents;   /* Every event received */
    uint32_t ignoredEvents; /* Events that could not be counted */
} Report;

/* Clears every counter */
void ReportInit(Report *report);

/* Counts one event; returns false if it could not be counted */
bool ReportRecord(Report *report, const HostEvent *hostEvent);

/* Writes the summary as text */
void ReportPrint(const Report *report, FILE *out);

#endif /* REPORT_H */
