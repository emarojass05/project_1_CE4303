#include "scheduler.h"

/* Writes one event stamped with the current tick; a full buffer counts the drop */
static void Emit(Scheduler *scheduler, EventType type, uint32_t taskId, uint32_t value)
{
    Event event = { .type = type, .tick = scheduler->tick,
                    .taskId = taskId, .value = value };
    (void)EventBufferPush(scheduler->events, event);
}

/* Starts a new job of the task at the current tick */
static void StartJob(const Scheduler *scheduler, SchedulerTask *entry)
{
    entry->active = 1;
    entry->remaining = entry->cost;
    entry->releaseTick = scheduler->tick;
    entry->absoluteDeadline = scheduler->tick + entry->task.deadline;
}

/* Smaller key means higher priority */
static uint32_t PriorityKey(const Scheduler *scheduler, const SchedulerTask *entry)
{
    if (scheduler->algorithm == SchedulerAlgorithmEdf) {
        return entry->absoluteDeadline;
    }
    return entry->task.period;
}

/* True when the task at index a wins over the task at index b */
static int HasPriority(const Scheduler *scheduler, size_t a, size_t b)
{
    const SchedulerTask *first = &scheduler->tasks[a];
    const SchedulerTask *second = &scheduler->tasks[b];
    uint32_t firstKey = PriorityKey(scheduler, first);
    uint32_t secondKey = PriorityKey(scheduler, second);

    if (firstKey != secondKey) {
        return firstKey < secondKey;
    }
    if (scheduler->runningIndex == (int)a) {
        return 1;
    }
    if (scheduler->runningIndex == (int)b) {
        return 0;
    }
    if (first->task.criticality != second->task.criticality) {
        return first->task.criticality > second->task.criticality;
    }
    return first->task.id < second->task.id;
}

/* Aborts every pending job whose deadline has been reached */
static void AbortLateJobs(Scheduler *scheduler)
{
    size_t i;

    for (i = 0; i < scheduler->taskCount; i++) {
        SchedulerTask *entry = &scheduler->tasks[i];

        if (!entry->active || scheduler->tick < entry->absoluteDeadline) {
            continue;
        }
        Emit(scheduler, EventTypeDeadlineMiss, entry->task.id, entry->remaining);
        Emit(scheduler, EventTypeAbort, entry->task.id, 0);
        entry->active = 0;
        entry->remaining = 0;
        scheduler->missCount++;
        if (scheduler->runningIndex == (int)i) {
            scheduler->runningIndex = SchedulerNoTask;
        }
    }
}

/* Releases every periodic job that is due at the current tick */
static void ReleasePeriodicJobs(Scheduler *scheduler)
{
    size_t i;

    for (i = 0; i < scheduler->taskCount; i++) {
        SchedulerTask *entry = &scheduler->tasks[i];

        if (entry->task.type != TaskTypePeriodic || entry->nextRelease != scheduler->tick) {
            continue;
        }
        entry->nextRelease += entry->task.period;
        if (entry->active) {
            continue;
        }
        StartJob(scheduler, entry);
        Emit(scheduler, EventTypeRelease, entry->task.id, 0);
    }
}

/* Index of the active task with the highest priority, or SchedulerNoTask */
static int SelectTask(const Scheduler *scheduler)
{
    int best = SchedulerNoTask;
    size_t i;

    for (i = 0; i < scheduler->taskCount; i++) {
        if (!scheduler->tasks[i].active) {
            continue;
        }
        if (best == SchedulerNoTask || HasPriority(scheduler, i, (size_t)best)) {
            best = (int)i;
        }
    }
    return best;
}

SchedulerStatus SchedulerInit(Scheduler *scheduler, SchedulerAlgorithm algorithm,
                              EventBuffer *events)
{
    if (scheduler == NULL || events == NULL) {
        return SchedulerStatusInvalidArgument;
    }
    if (algorithm != SchedulerAlgorithmRms && algorithm != SchedulerAlgorithmEdf) {
        return SchedulerStatusInvalidArgument;
    }
    scheduler->taskCount = 0;
    scheduler->algorithm = algorithm;
    scheduler->tick = 0;
    scheduler->runningIndex = SchedulerNoTask;
    scheduler->missCount = 0;
    scheduler->events = events;
    return SchedulerStatusOk;
}

SchedulerStatus SchedulerAddTask(Scheduler *scheduler, const Task *task, uint32_t cost)
{
    SchedulerTask *entry;
    size_t i;

    if (scheduler == NULL || task == NULL) {
        return SchedulerStatusInvalidArgument;
    }
    if (cost == 0 || task->period == 0 || task->deadline == 0) {
        return SchedulerStatusInvalidTask;
    }
    for (i = 0; i < scheduler->taskCount; i++) {
        if (scheduler->tasks[i].task.id == task->id) {
            return SchedulerStatusInvalidTask;
        }
    }
    if (scheduler->taskCount >= (size_t)SchedulerMaxTasks) {
        return SchedulerStatusFull;
    }

    entry = &scheduler->tasks[scheduler->taskCount];
    entry->task = *task;
    entry->cost = cost;
    entry->active = 0;
    entry->remaining = 0;
    entry->releaseTick = 0;
    entry->absoluteDeadline = 0;
    entry->nextRelease = scheduler->tick;
    scheduler->taskCount++;
    return SchedulerStatusOk;
}

SchedulerStatus SchedulerReleaseSporadic(Scheduler *scheduler, uint32_t taskId)
{
    size_t i;

    if (scheduler == NULL) {
        return SchedulerStatusInvalidArgument;
    }
    for (i = 0; i < scheduler->taskCount; i++) {
        SchedulerTask *entry = &scheduler->tasks[i];

        if (entry->task.id != taskId) {
            continue;
        }
        if (entry->task.type != TaskTypeSporadic) {
            return SchedulerStatusInvalidArgument;
        }
        if (entry->active || scheduler->tick < entry->nextRelease) {
            Emit(scheduler, EventTypeSporadicIgnored, taskId, 0);
            return SchedulerStatusIgnored;
        }
        StartJob(scheduler, entry);
        entry->nextRelease = scheduler->tick + entry->task.period;
        Emit(scheduler, EventTypeSporadicRelease, taskId, 0);
        return SchedulerStatusOk;
    }
    return SchedulerStatusInvalidArgument;
}

SchedulerStatus SchedulerTick(Scheduler *scheduler)
{
    SchedulerTask *entry;
    int selected;

    if (scheduler == NULL || scheduler->events == NULL) {
        return SchedulerStatusInvalidArgument;
    }

    AbortLateJobs(scheduler);
    ReleasePeriodicJobs(scheduler);
    selected = SelectTask(scheduler);

    if (selected != SchedulerNoTask) {
        entry = &scheduler->tasks[selected];

        if (selected != scheduler->runningIndex) {
            if (scheduler->runningIndex != SchedulerNoTask) {
                Emit(scheduler, EventTypePreempt,
                     scheduler->tasks[scheduler->runningIndex].task.id, entry->task.id);
            }
            Emit(scheduler, EventTypeStart, entry->task.id, 0);
            scheduler->runningIndex = selected;
        }

        entry->remaining--;
        if (entry->remaining == 0) {
            entry->active = 0;
            Emit(scheduler, EventTypeComplete, entry->task.id,
                 scheduler->tick + 1u - entry->releaseTick);
            scheduler->runningIndex = SchedulerNoTask;
        }
    } else {
        scheduler->runningIndex = SchedulerNoTask;
    }

    scheduler->tick++;
    return SchedulerStatusOk;
}
