#ifndef TASK_H
#define TASK_H

#include <stdint.h>

/* Kind of task as written in the task file */
typedef enum {
    TaskTypePeriodic = 0,
    TaskTypeSporadic = 1
} TaskType;

/* Task record: id,type,W,T,D,criticality */
typedef struct {
    uint32_t id;
    TaskType type;
    uint32_t work;        /* Work units (W) */
    uint32_t period;      /* Period or minimum interarrival, in ticks (T) */
    uint32_t deadline;    /* Relative deadline, in ticks (D) */
    uint32_t criticality; /* Higher value means more critical */
} Task;

/* Node speed measured by calibration */
typedef struct {
    uint32_t id;
    uint32_t unitsPerTick; /* Work units executed per tick (sn) */
} NodeInfo;

/* Result codes for task functions */
typedef enum {
    TaskStatusOk = 0,
    TaskStatusInvalidArgument = -1, /* Null pointer or zero speed */
    TaskStatusInvalidTask = -2      /* Zero work, period or deadline */
} TaskStatus;

/* Checks that the task fields are usable */
TaskStatus TaskValidate(const Task *task);

/* Cost in ticks: ceil(work / unitsPerTick), written to outCost */
TaskStatus TaskCost(const Task *task, uint32_t unitsPerTick, uint32_t *outCost);

#endif /* TASK_H */