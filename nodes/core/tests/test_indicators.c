#include <stdio.h>

#include "indicators.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static Task MakeTask(uint32_t id, uint32_t period, uint32_t deadline)
{
    Task task = { .id = id, .type = TaskTypePeriodic, .work = 1, .period = period,
                  .deadline = deadline, .criticality = 1 };
    return task;
}

/* Runs one tick and returns what the LEDs show after it */
static IndicatorState Step(Scheduler *scheduler, Indicators *indicators)
{
    CHECK(SchedulerTick(scheduler) == SchedulerStatusOk);
    return IndicatorsUpdate(indicators, scheduler);
}

/* A: cost 2, period 4 (slot 0). B: cost 3, period 8 (slot 1). Under RMS:
   A A B B A A B idle. The last tick of every job must still be shown. */
static void TestPatternFollowsRunningTask(void)
{
    static const uint32_t expected[] = { 1, 1, 2, 2, 1, 1, 2, 0 };
    EventBuffer buffer;
    Scheduler scheduler;
    Indicators indicators;
    Task taskA = MakeTask(1, 4, 4);
    Task taskB = MakeTask(2, 8, 8);
    size_t i;

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &taskA, 2) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &taskB, 3) == SchedulerStatusOk);
    CHECK(IndicatorsInit(&indicators, 3, 2));

    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        IndicatorState state = Step(&scheduler, &indicators);

        CHECK(state.taskPattern == expected[i]);
        CHECK(!state.missLed);
    }
}

/* Cost 3 with deadline 2: the job is aborted at tick 2, period 4 */
static void TestMissLedHoldsForConfiguredTicks(void)
{
    static const int expected[] = { 0, 0, 1, 1, 0, 0, 1, 1, 0 };
    EventBuffer buffer;
    Scheduler scheduler;
    Indicators indicators;
    Task late = MakeTask(1, 4, 2);
    size_t i;

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmEdf, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &late, 3) == SchedulerStatusOk);
    CHECK(IndicatorsInit(&indicators, 3, 2));

    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        IndicatorState state = Step(&scheduler, &indicators);

        CHECK(state.missLed == (expected[i] != 0));
    }
}

static void TestMissLedLatchesWhenHoldIsZero(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Indicators indicators;
    Task late = MakeTask(1, 4, 2);
    unsigned int i;

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmEdf, &buffer) == SchedulerStatusOk);
    CHECK(SchedulerAddTask(&scheduler, &late, 3) == SchedulerStatusOk);
    CHECK(IndicatorsInit(&indicators, 3, 0));

    CHECK(!Step(&scheduler, &indicators).missLed);
    CHECK(!Step(&scheduler, &indicators).missLed);
    for (i = 0; i < 20; i++) {
        CHECK(Step(&scheduler, &indicators).missLed);
    }
}

/* Slot 4 needs three bits; with two LEDs every LED turns on */
static void TestSlotTooLargeShowsAllOn(void)
{
    EventBuffer buffer;
    Scheduler scheduler;
    Indicators indicators;
    IndicatorState state;
    uint32_t id;

    EventBufferInit(&buffer);
    CHECK(SchedulerInit(&scheduler, SchedulerAlgorithmRms, &buffer) == SchedulerStatusOk);
    /* Shorter period = higher RMS priority, so the last task added runs first */
    for (id = 1; id <= 4; id++) {
        Task task = MakeTask(id, 20 - id, 20 - id);

        CHECK(SchedulerAddTask(&scheduler, &task, 1) == SchedulerStatusOk);
    }
    CHECK(IndicatorsInit(&indicators, 2, 1));

    state = Step(&scheduler, &indicators);
    CHECK(state.taskPattern == 3);
    state = Step(&scheduler, &indicators);
    CHECK(state.taskPattern == 3);
    state = Step(&scheduler, &indicators);
    CHECK(state.taskPattern == 2);
    state = Step(&scheduler, &indicators);
    CHECK(state.taskPattern == 1);
    state = Step(&scheduler, &indicators);
    CHECK(state.taskPattern == 0);
}

static void TestInvalidArguments(void)
{
    Indicators indicators;
    IndicatorState state;

    CHECK(!IndicatorsInit(NULL, 3, 1));
    CHECK(!IndicatorsInit(&indicators, 0, 1));
    CHECK(!IndicatorsInit(&indicators, IndicatorsMaxTaskBits + 1, 1));
    CHECK(IndicatorsInit(&indicators, IndicatorsMaxTaskBits, 1));

    state = IndicatorsUpdate(NULL, NULL);
    CHECK(state.taskPattern == 0 && !state.missLed);
    state = IndicatorsUpdate(&indicators, NULL);
    CHECK(state.taskPattern == 0 && !state.missLed);
}

int main(void)
{
    TestPatternFollowsRunningTask();
    TestMissLedHoldsForConfiguredTicks();
    TestMissLedLatchesWhenHoldIsZero();
    TestSlotTooLargeShowsAllOn();
    TestInvalidArguments();

    if (failureCount != 0) {
        printf("test_indicators: %d failure(s)\n", failureCount);
        return 1;
    }
    printf("test_indicators: all checks passed\n");
    return 0;
}