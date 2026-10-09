#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stddef.h>
#include <stdint.h>

#include "event_buffer.h"
#include "task.h"

/* Maximum number of tasks one node can schedule */
enum {
    SchedulerMaxTasks = 8
};

/* Index value meaning that no task is running */
enum {
    SchedulerNoTask = -1
};

typedef enum {
    SchedulerAlgorithmRms = 0,
    SchedulerAlgorithmEdf
} SchedulerAlgorithm;

/* Result codes for scheduler functions */
typedef enum {
    SchedulerStatusOk = 0,
    SchedulerStatusIgnored = 1,         /* Sporadic release was not accepted */
    SchedulerStatusInvalidArgument = -1, /* Null pointer or unknown task */
    SchedulerStatusInvalidTask = -2,     /* Zero cost, period or deadline, or repeated id */
    SchedulerStatusFull = -3             /* No room for another task */
} SchedulerStatus;

/* A task together with the state of its current job */
typedef struct {
    Task task;
    uint32_t cost;             /* Ticks of work per job */
    int active;                /* Nonzero while a job is pending */
    uint32_t remaining;        /* Ticks of work left in the current job */
    uint32_t releaseTick;      /* Tick at which the current job was released */
    uint32_t absoluteDeadline; /* Tick by which the current job must finish */
    uint32_t nextRelease;      /* Next tick a periodic release is due, or the
                                  earliest tick a sporadic one is accepted */
} SchedulerTask;

typedef struct {
    SchedulerTask tasks[SchedulerMaxTasks];
    size_t taskCount;
    SchedulerAlgorithm algorithm;
    uint32_t tick;      /* Tick the next call to SchedulerTick will process */
    int runningIndex;   /* Index in tasks, or SchedulerNoTask */
    uint32_t missCount; /* Jobs aborted for missing their deadline */
    EventBuffer *events; /* Where the scheduler reports what it does */
} Scheduler;

/* Prepares an empty scheduler that reports into events */
SchedulerStatus SchedulerInit(Scheduler *scheduler, SchedulerAlgorithm algorithm,
                              EventBuffer *events);

/* Adds a task whose job costs cost ticks; periodic tasks release at the current tick */
SchedulerStatus SchedulerAddTask(Scheduler *scheduler, const Task *task, uint32_t cost);

/* Requests a job of a sporadic task at the current tick */
SchedulerStatus SchedulerReleaseSporadic(Scheduler *scheduler, uint32_t taskId);

/* Advances one tick: aborts late jobs, releases jobs, picks a task and runs it */
SchedulerStatus SchedulerTick(Scheduler *scheduler);

#endif /* SCHEDULER_H */
