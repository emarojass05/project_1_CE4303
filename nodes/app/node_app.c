/* Node demo for milestones 1 and 2, shared by the ESP32 and the Pico.

   It measures sn at startup, runs fixed test tasks with the scheduler of
   nodes/core on the hardware tick, executes real work with unidad_trabajo(),
   shows the running task in binary on the LEDs, releases the sporadic task
   with the button and sends every event in the background as an Event frame.

   The fixed tasks are only for this demo; in M3 the host sends the tick in
   CalibRequest, receives sn and deploys the tasks over the protocol. */

#include "node_app.h"

#include <inttypes.h>
#include <stdio.h>

#include "board.h"
#include "board_config.h"
#include "calibration.h"
#include "event_tx.h"
#include "indicators.h"
#include "scheduler.h"
#include "ut.h"

/* Algorithm of the demo: SchedulerAlgorithmRms or SchedulerAlgorithmEdf */
#ifndef NODE_DEMO_ALGORITHM
#define NODE_DEMO_ALGORITHM SchedulerAlgorithmRms
#endif

/* Slow tick so the LEDs can be followed by eye. The project value is 10 ms
   and in M3 the host sends it in the calibration request. */
#ifndef NODE_DEMO_TICK_US
#define NODE_DEMO_TICK_US 250000u
#endif

/* Ticks the deadline miss LED stays on (0 = until reset) */
#define NODE_MISS_HOLD_TICKS 4u

/* Units timed at startup to measure sn */
#define NODE_CALIBRATION_UNITS 200u

/* Most frames sent in one tick, so a burst of events never delays the next tick */
#define NODE_MAX_FRAMES_PER_TICK 8u

typedef struct {
    Task task;
    uint32_t cost; /* Ticks per job; in M3 it comes from C(i,n) = ceil(W / sn) */
} DemoTask;

/* Periodic part: U = 2/5 + 3/7 + 1/10 = 0.93. EDF meets every deadline and RMS
   misses one about every 70 ticks. The sporadic task 4 is released with the
   button and needs at least 20 ticks between presses; closer presses are
   ignored and counted. The LEDs show 1 to 4 (position in the table). */
static const DemoTask demoTasks[] = {
    { { .id = 1, .type = TaskTypePeriodic, .work = 0, .period = 5,  .deadline = 5,  .criticality = 1 }, 2 },
    { { .id = 2, .type = TaskTypePeriodic, .work = 0, .period = 7,  .deadline = 7,  .criticality = 1 }, 3 },
    { { .id = 3, .type = TaskTypePeriodic, .work = 0, .period = 10, .deadline = 10, .criticality = 1 }, 1 },
    { { .id = 4, .type = TaskTypeSporadic, .work = 0, .period = 20, .deadline = 20, .criticality = 2 }, 1 },
};

/* Identifier of the sporadic task released by the button */
#define NODE_SPORADIC_TASK_ID 4u

static EventBuffer events;
static Scheduler scheduler;
static Indicators indicators;
static EventTx eventTx;
static uint32_t unitsPerTick; /* sn measured at startup */

/* Times NODE_CALIBRATION_UNITS calls and scales them to the tick */
static uint32_t Calibrate(uint32_t tickUs)
{
    uint64_t start;
    uint64_t elapsed;
    uint32_t i;

    unidad_trabajo(); /* Warm up caches so the first call does not skew it */
    start = BoardMicros();
    for (i = 0; i < NODE_CALIBRATION_UNITS; i++) {
        unidad_trabajo();
    }
    elapsed = BoardMicros() - start;
    return CalibrationUnitsPerTick(NODE_CALIBRATION_UNITS, elapsed, tickUs,
                                   CalibrationDefaultPercent);
}

/* One tick of real work for the running task */
static void RunWork(uint32_t taskId, void *context)
{
    uint32_t i;

    (void)taskId;
    (void)context;
    for (i = 0; i < unitsPerTick; i++) {
        unidad_trabajo();
    }
}

#if BOARD_TEXT_EVENTS
static const char *EventName(EventType type)
{
    switch (type) {
    case EventTypeRelease:         return "release";
    case EventTypeStart:           return "start";
    case EventTypePreempt:         return "preempt";
    case EventTypeComplete:        return "complete";
    case EventTypeDeadlineMiss:    return "deadline-miss";
    case EventTypeAbort:           return "abort";
    case EventTypeSporadicRelease: return "sporadic-release";
    case EventTypeSporadicIgnored: return "sporadic-ignored";
    default:                       return "?";
    }
}

/* Debug mode: readable lines instead of frames (they block, demo only) */
static void SendEvents(void)
{
    Event event;

    while (EventBufferPop(&events, &event)) {
        printf("tick %" PRIu32 " task %" PRIu32 " %s %" PRIu32 "\n",
               event.tick, event.taskId, EventName(event.type), event.value);
    }
}
#else
/* Sends what the port accepts now; the rest stays queued for the next tick */
static void SendEvents(void)
{
    const uint8_t *bytes;
    size_t pending;
    size_t written;
    uint32_t frames = 0;

    while (frames < NODE_MAX_FRAMES_PER_TICK &&
           (pending = EventTxPending(&eventTx, &events, &bytes)) > 0) {
        written = BoardSerialWrite(bytes, pending);
        EventTxConsumed(&eventTx, written);
        if (written < pending) {
            return; /* Port full: try again after the next tick */
        }
        frames++;
    }
}
#endif

/* Shows a fixed pattern forever when startup fails, so it is visible */
static void Halt(void)
{
    BoardLedsShow((IndicatorState){ .taskPattern = 0, .missLed = true });
    for (;;) {
        BoardTickWait();
    }
}

void NodeRun(void)
{
    size_t i;

    EventBufferInit(&events);
    EventTxInit(&eventTx);
    if (!BoardLedsInit() || !BoardSerialInit() || !BoardButtonInit() ||
        SchedulerInit(&scheduler, NODE_DEMO_ALGORITHM, &events) != SchedulerStatusOk ||
        !IndicatorsInit(&indicators, BoardTaskLedCount(), NODE_MISS_HOLD_TICKS)) {
        Halt();
    }

    unitsPerTick = Calibrate(NODE_DEMO_TICK_US);
    if (unitsPerTick == 0) {
        Halt();
    }
    SchedulerSetWork(&scheduler, RunWork, NULL);
    for (i = 0; i < sizeof(demoTasks) / sizeof(demoTasks[0]); i++) {
        (void)SchedulerAddTask(&scheduler, &demoTasks[i].task, demoTasks[i].cost);
    }
#if BOARD_TEXT_EVENTS
    printf("%s, tick %u us, sn = %" PRIu32 " units per tick\n",
           NODE_DEMO_ALGORITHM == SchedulerAlgorithmRms ? "RMS" : "EDF",
           (unsigned int)NODE_DEMO_TICK_US, unitsPerTick);
#endif
    if (!BoardTickStart(NODE_DEMO_TICK_US)) {
        Halt();
    }

    for (;;) {
        BoardTickWait();
        if (BoardButtonTakePress()) {
            /* Ignored presses are counted by the scheduler as events */
            (void)SchedulerReleaseSporadic(&scheduler, NODE_SPORADIC_TASK_ID);
        }
        SchedulerTick(&scheduler);
        BoardLedsShow(IndicatorsUpdate(&indicators, &scheduler));
        SendEvents();
    }
}
