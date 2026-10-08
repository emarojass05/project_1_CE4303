#include "task.h"

#include <stddef.h>

TaskStatus TaskValidate(const Task *task)
{
    if (task == NULL) {
        return TaskStatusInvalidArgument;
    }
    if (task->type != TaskTypePeriodic && task->type != TaskTypeSporadic) {
        return TaskStatusInvalidTask;
    }
    if (task->work == 0 || task->period == 0 || task->deadline == 0) {
        return TaskStatusInvalidTask;
    }
    return TaskStatusOk;
}

TaskStatus TaskCost(const Task *task, uint32_t unitsPerTick, uint32_t *outCost)
{
    if (task == NULL || outCost == NULL || unitsPerTick == 0) {
        return TaskStatusInvalidArgument;
    }
    if (task->work == 0) {
        return TaskStatusInvalidTask;
    }

    /* Ceiling division */
    uint32_t cost = task->work / unitsPerTick;
    if (task->work % unitsPerTick != 0) {
        cost += 1;
    }

    *outCost = cost;
    return TaskStatusOk;
}