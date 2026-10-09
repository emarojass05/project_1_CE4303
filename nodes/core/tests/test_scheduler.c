#include <stdio.h>

#include "scheduler.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

/* Next event must match all four fields */
static int NextEventIs(EventBuffer *buffer, EventType type, uint32_t tick,
                       uint32_t taskId, uint32_t value)
{
    Event event;

    if (!EventBufferPop(buffer, &event)) {
        return 0;
    }
    return event.type == type && event.tick == tick &&
           event.taskId == taskId && event.value == value;
}

static Task MakeTask(uint32_t id, TaskType type, uint32_t period, uint32_t deadline,
                     uint32_t criticality)
{
    Task task = { .id = id, .type = type, .work = 1, .period = period,
                  .deadline = deadline, .criticality = criticality };
    return task;
}

static void RunTicks(Scheduler *scheduler, unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; i++) {
        CHECK(SchedulerTick(scheduler) == SchedulerStatusOk);
    }
}

/* A: cost 2, period 4. B: cost 3, period 8. */
static void LoadTwoTasks(Scheduler *scheduler)
{
    Task taskA = MakeTask(1, TaskTypePeriodic, 4, 4, 1);
    Task taskB = MakeTask(2, TaskTypePeriodic, 8, 8, 1);

    CHECK(SchedulerAddTask(scheduler, &taskA, 2) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(scheduler, &taskB, 3) == SchedulerStatusOk);
}

static void TestRmsPreemption(void)
{
    EventBuffer buffer;
    Scheduler scheduler;

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    LoadTwoTasks(&scheduler);
    RunTicks(&scheduler, 8);

    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 2, 0));
    CHECK(NextEventIs(&buffer, EventTypeStart, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeComplete, 1, 1, 2));
    CHECK(NextEventIs(&buffer, EventTypeStart, 2, 2, 0));
    CHECK(NextEventIs(&buffer, EventTypeRelease, 4, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypePreempt, 4, 2, 1));
    CHECK(NextEventIs(&buffer, EventTypeStart, 4, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeComplete, 5, 1, 2));
    CHECK(NextEventIs(&buffer, EventTypeStart, 6, 2, 0));
    CHECK(NextEventIs(&buffer, EventTypeComplete, 6, 2, 7));
    CHECK(EventBufferCount(&buffer) == 0);
}

static void TestEdfTieKeepsRunningTask(void)
{
    EventBuffer buffer;
    Scheduler scheduler;

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmEdf, &buffer) == SchedulerStatusOk);
    LoadTwoTasks(&scheduler);
    RunTicks(&scheduler, 8);

    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 2, 0));
    CHECK(NextEventIs(&buffer, EventTypeStart, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeComplete, 1, 1, 2));
    CHECK(NextEventIs(&buffer, EventTypeStart, 2, 2, 0));
    CHECK(NextEventIs(&buffer, EventTypeRelease, 4, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeComplete, 4, 2, 5));
    CHECK(NextEventIs(&buffer, EventTypeStart, 5, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeComplete, 6, 1, 3));
    CHECK(EventBufferCount(&buffer) == 0);
}

/* Identifier of the task that starts at tick 0, or 0 when none does */
static uint32_t FirstStartedId(const Task *first, const Task *second)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Event event;

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, first, 2) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, second, 2) == SchedulerStatusOk);
    RunTicks(&scheduler, 1);

    while (EventBufferPop(&buffer, &event)) {
        if (event.type == EventTypeStart) {
            return event.taskId;
        }
    }
    return 0;
}

static void TestTieBreakByCriticalityThenId(void)
{
    Task lowCriticality = MakeTask(3, TaskTypePeriodic, 10, 10, 1);
    Task highCriticality = MakeTask(5, TaskTypePeriodic, 10, 10, 2);
    Task smallId = MakeTask(3, TaskTypePeriodic, 10, 10, 1);
    Task bigId = MakeTask(5, TaskTypePeriodic, 10, 10, 1);

    CHECK(FirstStartedId(&lowCriticality, &highCriticality) == 5);
    CHECK(FirstStartedId(&highCriticality, &lowCriticality) == 5);
    CHECK(FirstStartedId(&smallId, &bigId) == 3);
    CHECK(FirstStartedId(&bigId, &smallId) == 3);
}

static void TestEdfFullLoadMeetsDeadlines(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Task taskA = MakeTask(1, TaskTypePeriodic, 2, 2, 1);
    Task taskB = MakeTask(2, TaskTypePeriodic, 4, 4, 1);
    Event event;
    unsigned int completeA = 0;
    unsigned int completeB = 0;

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmEdf, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &taskA, 1) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &taskB, 2) == SchedulerStatusOk);
    RunTicks(&scheduler, 8);

    while (EventBufferPop(&buffer, &event)) {
        if (event.type == EventTypeComplete && event.taskId == 1) {
            CHECK(event.value <= 2);
            completeA++;
        } else if (event.type == EventTypeComplete && event.taskId == 2) {
            CHECK(event.value <= 4);
            completeB++;
        }
        CHECK(event.type != EventTypeDeadlineMiss);
    }
    CHECK(completeA == 4);
    CHECK(completeB == 2);
}

static void TestEightTasksRunInIdOrder(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Task extra = MakeTask(99, TaskTypePeriodic, 16, 16, 1);
    uint32_t id;

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    for (id = 8; id >= 1; id--) {
        Task task = MakeTask(id, TaskTypePeriodic, 16, 16, 1);
        CHECK(SchedulerAddTask(&scheduler, &task, 1) == SchedulerStatusOk);
    }
    CHECK(SchedulerAddTask(&scheduler, &extra, 1) == SchedulerStatusFull);
    RunTicks(&scheduler, 8);

    for (id = 1; id <= 8; id++) {
        Event event;
        int found = 0;

        while (EventBufferPop(&buffer, &event)) {
            if (event.type == EventTypeComplete) {
                CHECK(event.taskId == id && event.tick == id - 1);
                found = 1;
                break;
            }
        }
        CHECK(found);
    }
}

static void TestSporadicRelease(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Task sporadic = MakeTask(9, TaskTypeSporadic, 5, 5, 1);
    Task periodic = MakeTask(1, TaskTypePeriodic, 5, 5, 1);

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &sporadic, 1) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &periodic, 1) == SchedulerStatusOk);

    CHECK(SchedulerReleaseSporadic(&scheduler, 9) == SchedulerStatusOk);
    CHECK(SchedulerReleaseSporadic(&scheduler, 9) == SchedulerStatusIgnored);
    RunTicks(&scheduler, 3);
    CHECK(SchedulerReleaseSporadic(&scheduler, 9) == SchedulerStatusIgnored);
    RunTicks(&scheduler, 2);
    CHECK(SchedulerReleaseSporadic(&scheduler, 9) == SchedulerStatusOk);
    RunTicks(&scheduler, 1);

    CHECK(NextEventIs(&buffer, EventTypeSporadicRelease, 0, 9, 0));
    CHECK(NextEventIs(&buffer, EventTypeSporadicIgnored, 0, 9, 0));
    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeStart, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeComplete, 0, 1, 1));
    CHECK(NextEventIs(&buffer, EventTypeStart, 1, 9, 0));
    CHECK(NextEventIs(&buffer, EventTypeComplete, 1, 9, 2));
    CHECK(NextEventIs(&buffer, EventTypeSporadicIgnored, 3, 9, 0));
    CHECK(NextEventIs(&buffer, EventTypeSporadicRelease, 5, 9, 0));

    CHECK(SchedulerReleaseSporadic(&scheduler, 1) == SchedulerStatusInvalidArgument);
    CHECK(SchedulerReleaseSporadic(&scheduler, 77) == SchedulerStatusInvalidArgument);
}

static void TestOverrunAbortsAndReleasesAgain(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Task heavy = MakeTask(1, TaskTypePeriodic, 2, 2, 1);

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &heavy, 3) == SchedulerStatusOk);
    RunTicks(&scheduler, 4);

    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeStart, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeDeadlineMiss, 2, 1, 1));
    CHECK(NextEventIs(&buffer, EventTypeAbort, 2, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeRelease, 2, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeStart, 2, 1, 0));
    CHECK(EventBufferCount(&buffer) == 0);
    CHECK(scheduler.missCount == 1);
}

static void TestJobFinishingBeforeDeadlineMeetsIt(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Task task = MakeTask(1, TaskTypePeriodic, 8, 4, 1);

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &task, 4) == SchedulerStatusOk);
    RunTicks(&scheduler, 8);

    CHECK(scheduler.missCount == 0);
}

static void TestJobLateByOneTickIsAborted(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Task task = MakeTask(1, TaskTypePeriodic, 8, 4, 1);

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &task, 5) == SchedulerStatusOk);
    RunTicks(&scheduler, 5);

    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeStart, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeDeadlineMiss, 4, 1, 1));
    CHECK(NextEventIs(&buffer, EventTypeAbort, 4, 1, 0));
    CHECK(EventBufferCount(&buffer) == 0);
    CHECK(scheduler.missCount == 1);
}

static void TestAbortOfWaitingJobKeepsRunningTask(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Task runner = MakeTask(1, TaskTypePeriodic, 5, 5, 1);
    Task waiter = MakeTask(2, TaskTypePeriodic, 10, 3, 1);

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &runner, 4) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &waiter, 2) == SchedulerStatusOk);
    RunTicks(&scheduler, 4);

    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 2, 0));
    CHECK(NextEventIs(&buffer, EventTypeStart, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeDeadlineMiss, 3, 2, 2));
    CHECK(NextEventIs(&buffer, EventTypeAbort, 3, 2, 0));
    CHECK(NextEventIs(&buffer, EventTypeComplete, 3, 1, 4));
    CHECK(EventBufferCount(&buffer) == 0);
    CHECK(scheduler.missCount == 1);
}

static void TestSeveralAbortsInOneTick(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    uint32_t id;

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    for (id = 1; id <= 3; id++) {
        Task task = MakeTask(id, TaskTypePeriodic, 10, 1, 1);
        CHECK(SchedulerAddTask(&scheduler, &task, 2) == SchedulerStatusOk);
    }
    RunTicks(&scheduler, 2);

    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 2, 0));
    CHECK(NextEventIs(&buffer, EventTypeRelease, 0, 3, 0));
    CHECK(NextEventIs(&buffer, EventTypeStart, 0, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeDeadlineMiss, 1, 1, 1));
    CHECK(NextEventIs(&buffer, EventTypeAbort, 1, 1, 0));
    CHECK(NextEventIs(&buffer, EventTypeDeadlineMiss, 1, 2, 2));
    CHECK(NextEventIs(&buffer, EventTypeAbort, 1, 2, 0));
    CHECK(NextEventIs(&buffer, EventTypeDeadlineMiss, 1, 3, 2));
    CHECK(NextEventIs(&buffer, EventTypeAbort, 1, 3, 0));
    CHECK(EventBufferCount(&buffer) == 0);
    CHECK(scheduler.missCount == 3);
}

static void TestOverloadedSetKeepsRunning(void)
{
    SchedulerAlgorithm algorithms[2] = { SchedulerAlgorithmRms, SchedulerAlgorithmEdf };
    unsigned int a;

    for (a = 0; a < 2; a++) {
        EventBuffer buffer;
        Scheduler scheduler;
        Event event;
        Task first = MakeTask(1, TaskTypePeriodic, 4, 4, 1);
        Task second = MakeTask(2, TaskTypePeriodic, 5, 5, 2);
        Task third = MakeTask(3, TaskTypePeriodic, 6, 6, 3);

        EventBufferInit(&buffer);
        CHECK(SchedulerInit(&scheduler, algorithms[a], &buffer) == SchedulerStatusOk);
        CHECK(SchedulerAddTask(&scheduler, &first, 3) == SchedulerStatusOk);
        CHECK(SchedulerAddTask(&scheduler, &second, 3) == SchedulerStatusOk);
        CHECK(SchedulerAddTask(&scheduler, &third, 3) == SchedulerStatusOk);

        for (unsigned int tick = 0; tick < 100; tick++) {
            CHECK(SchedulerTick(&scheduler) == SchedulerStatusOk);
            while (EventBufferPop(&buffer, &event)) {
            }
        }
        CHECK(scheduler.tick == 100);
        CHECK(scheduler.missCount > 0);
    }
}

static void TestInvalidTasks(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Task good = MakeTask(1, TaskTypePeriodic, 4, 4, 1);
    Task zeroPeriod = MakeTask(2, TaskTypePeriodic, 0, 4, 1);
    Task zeroDeadline = MakeTask(3, TaskTypePeriodic, 4, 0, 1);
    Task repeated = MakeTask(1, TaskTypePeriodic, 8, 8, 1);

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &good, 1) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &good, 0) == SchedulerStatusInvalidTask);
    CHECK(SchedulerAddTask(&scheduler, &zeroPeriod, 1) == SchedulerStatusInvalidTask);
    CHECK(SchedulerAddTask(&scheduler, &zeroDeadline, 1) == SchedulerStatusInvalidTask);
    CHECK(SchedulerAddTask(&scheduler, &repeated, 1) == SchedulerStatusInvalidTask);
    CHECK(scheduler.taskCount == 1);
}

static void TestIdleTicksAndNullArguments(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Task good = MakeTask(1, TaskTypePeriodic, 4, 4, 1);

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmEdf, &buffer) == SchedulerStatusOk);
    RunTicks(&scheduler, 5);
    CHECK(EventBufferCount(&buffer) == 0);
    CHECK(scheduler.tick == 5);

    CHECK(SchedulerInit(NULL, SchedulerAlgorithmEdf, &buffer) == SchedulerStatusInvalidArgument);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmEdf, NULL) == SchedulerStatusInvalidArgument);
    CHECK(SchedulerInit(&scheduler, (SchedulerAlgorithm)7, &buffer) == SchedulerStatusInvalidArgument);
    CHECK(SchedulerAddTask(NULL, &good, 1) == SchedulerStatusInvalidArgument);
    CHECK(SchedulerAddTask(&scheduler, NULL, 1) == SchedulerStatusInvalidArgument);
    CHECK(SchedulerReleaseSporadic(NULL, 1) == SchedulerStatusInvalidArgument);
    CHECK(SchedulerTick(NULL) == SchedulerStatusInvalidArgument);
}

int main(void)
{
    TestRmsPreemption();
    TestEdfTieKeepsRunningTask();
    TestTieBreakByCriticalityThenId();
    TestEdfFullLoadMeetsDeadlines();
    TestEightTasksRunInIdOrder();
    TestSporadicRelease();
    TestOverrunAbortsAndReleasesAgain();
    TestJobFinishingBeforeDeadlineMeetsIt();
    TestJobLateByOneTickIsAborted();
    TestAbortOfWaitingJobKeepsRunningTask();
    TestSeveralAbortsInOneTick();
    TestOverloadedSetKeepsRunning();
    TestInvalidTasks();
    TestIdleTicksAndNullArguments();

    if (failureCount != 0) {
        printf("test_scheduler: %d failure(s)\n", failureCount);
        return 1;
    }
    printf("test_scheduler: all checks passed\n");
    return 0;
}
