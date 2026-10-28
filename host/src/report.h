#ifndef REPORT_H
#define REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "event_queue.h"

/* Report of a run (F13). The report process feeds every HostEvent it takes
   from the shared queue to ReportRecord, so it gets everything from the
   queue: the node events, the tick reports and the records of the planner
   (configuration, calibration, accepted and rejected tasks, nodes that went
   down, link counters). ReportPrint writes, per node: the Gantt chart, the
   releases, executions, deadlines and failures of each task, the theoretical
   and measured utilization, the task switches, the accepted and rejected
   tasks with their reason, the latency of the sporadic tasks and the skew
   between the nodes measured from the tick reports. */

enum {
    ReportMaxTasks = 128,     /* (node, task) pairs; a relocated task counts twice */
    ReportMaxNodes = 8,
    ReportMaxDecisions = 192, /* Accepted and rejected tasks, nodes down */
    ReportListMax = 20,       /* Task switches and misses listed per node */
    ReportGanttWidth = 100,   /* Ticks per Gantt window */
    ReportSkewWindow = 10,    /* Tick reports per skew estimate */
    ReportSkewMaxPoints = 2048, /* Skew estimates kept per node */
    ReportMaxLogged = 1u << 20 /* Node events kept for the Gantt chart */
};

/* Counters for one task of one node */
typedef struct {
    uint32_t nodeId;
    uint32_t taskId;
    bool sporadic;
    uint32_t releases;         /* Periodic releases */
    uint32_t sporadicReleases; /* Accepted sporadic releases */
    uint32_t sporadicIgnored;  /* Sporadic releases before the minimum interval */
    uint32_t starts;
    uint32_t completions;
    uint32_t deadlineMisses;
    uint32_t aborts;
    uint32_t preemptions;      /* Times this task lost the processor */
    uint32_t executedTicks;    /* Ticks this task ran */
    uint32_t minResponse;      /* Shortest response time, in ticks */
    uint32_t maxResponse;      /* Longest response time, in ticks */
    uint64_t totalResponse;    /* Sum of response times, in ticks */
    bool running;              /* Has the processor since runningSince */
    uint32_t runningSince;
} TaskStats;

typedef struct {
    int64_t zeroUs;  /* Estimated host time of tick zero */
    uint64_t hostUs; /* Arrival of the last report of the window */
} ReportSkewPoint;

typedef struct {
    uint32_t tick;
    uint32_t from; /* Task that had the processor, 0 = idle */
    uint32_t to;
} ReportSwitch;

typedef struct {
    uint32_t tick;
    uint32_t taskId;
    uint32_t left; /* Ticks of work the job still needed */
} ReportMiss;

/* Counters for one node */
typedef struct {
    uint32_t nodeId;
    uint32_t taskSwitches; /* Start events: a task takes the processor */
    uint32_t preemptions;  /* Preempt events */
    uint32_t current;      /* Task running now, 0 = idle */
    uint32_t lastTask;     /* Task that finished or was aborted last */
    uint32_t lastEnd;      /* First tick after it held the processor */
    uint32_t observedTicks; /* Ticks since Start seen in events and tick reports */

    bool calibrated;
    uint32_t unitsPerTick;
    char port[HostEventTextMax];
    bool started;
    uint64_t startedUs;    /* Ack of Start */
    bool down;
    uint32_t downTick;
    uint32_t downCause;
    uint64_t downUs;
    bool haveLink;
    HostLinkStats link;

    ReportSwitch switches[ReportListMax];
    uint32_t switchCount;  /* Stored in switches */
    ReportMiss misses[ReportListMax];
    uint32_t missCount;    /* Stored in misses */

    /* Skew: estimate of the host time of tick zero, hostUs - tick * tickUs,
       keeping the smallest of each window because delays only add. One
       point per complete window, so two nodes can be compared at the same
       time even when one of them went down earlier */
    uint32_t tickReports;
    ReportSkewPoint skew[ReportSkewMaxPoints];
    uint32_t skewCount;    /* When full, the last point keeps being replaced */
    int64_t windowZeroUs;  /* Window being filled */
    uint32_t windowCount;
} NodeStats;

typedef enum {
    ReportDecisionAccepted = 0,
    ReportDecisionRejected,
    ReportDecisionNodeDown
} ReportDecisionKind;

/* What the planner decided, in arrival order */
typedef struct {
    ReportDecisionKind kind;
    Task task;
    uint32_t nodeId;
    uint32_t cost;
    uint32_t origin;
    uint32_t reason;
    uint64_t hostUs;
} ReportDecision;

typedef struct {
    uint32_t nodeId;
    Event event;
} ReportLogged;

typedef struct {
    TaskStats tasks[ReportMaxTasks];
    NodeStats nodes[ReportMaxNodes];
    uint32_t taskCount;
    uint32_t nodeCount;
    ReportDecision decisions[ReportMaxDecisions];
    uint32_t decisionCount;
    bool haveRun;
    HostRunInfo run;
    uint64_t startUs;       /* First Ack of Start */
    uint32_t totalEvents;   /* Every node event received */
    uint32_t ignoredEvents; /* Events and records that could not be counted */

    ReportLogged *log;      /* Node events in arrival order, for the Gantt chart */
    size_t logCount;
    size_t logCapacity;
    bool logTruncated;
} Report;

/* Clears every counter */
void ReportInit(Report *report);

/* Frees the event log; the report can be initialized again */
void ReportFree(Report *report);

/* Counts one event or record; returns false if it could not be counted */
bool ReportRecord(Report *report, const HostEvent *hostEvent);

/* Writes the report as text */
void ReportPrint(const Report *report, FILE *out);

/* Text for a HostRejectReason, shared with the planner messages */
const char *ReportRejectName(uint32_t reason);

/* Text for a HostOrigin */
const char *ReportOriginName(uint32_t origin);

#endif /* REPORT_H */
