/* Node program shared by the ESP32 and the Pico.

   The node waits for the host on the serial port (docs/protocolo.md): it
   measures sn when a CalibRequest arrives, with the tick that the host sends,
   loads the algorithm and the tasks from DeployConfig and DeployTask, and runs
   the scheduler of nodes/core on the hardware tick from Start until Stop.
   Every tick executes real work with unidad_trabajo(), shows the running task
   in binary on the LEDs, releases the sporadic task with the button and sends
   the events in the background as Event frames. Nothing is fixed in the code:
   tasks, tick and sn come from the host and the calibration of this run. */

#include "node_app.h"

#include "board.h"
#include "calibration.h"
#include "indicators.h"
#include "node_link.h"
#include "scheduler.h"
#include "ut.h"

/* Loop period before the first CalibRequest, only to read the serial port */
#define NODE_IDLE_TICK_US 10000u

/* Ticks the deadline miss LED stays on (0 = until reset) */
#define NODE_MISS_HOLD_TICKS 4u

/* Calibration times at least this many units and at least one tick, but no
   more than NODE_CALIBRATION_MAX_US, so a long tick does not make the host
   time out while waiting for the response */
#define NODE_CALIBRATION_MIN_UNITS 20u
#define NODE_CALIBRATION_MAX_US 100000u

/* Most writes and bytes read in one tick, so serial work never delays the next tick */
#define NODE_MAX_WRITES_PER_TICK 8u
#define NODE_MAX_READ_PER_TICK 256u

static EventBuffer events;
static Scheduler scheduler;
static Indicators indicators;
static NodeLink link;

/* Counts how many units run in about one tick and keeps CalibrationDefaultPercent */
static uint32_t Calibrate(uint32_t tickUs, void *context)
{
    uint64_t window = tickUs < NODE_CALIBRATION_MAX_US ? tickUs : NODE_CALIBRATION_MAX_US;
    uint64_t start;
    uint64_t elapsed;
    uint32_t units = 0;

    (void)context;
    unidad_trabajo(); /* Warm up caches so the first call does not skew it */
    start = BoardMicros();
    do {
        unidad_trabajo();
        units++;
        elapsed = BoardMicros() - start;
    } while (elapsed < window || units < NODE_CALIBRATION_MIN_UNITS);
    return CalibrationUnitsPerTick(units, elapsed, tickUs, CalibrationDefaultPercent);
}

static bool StartTicks(uint32_t tickUs, void *context)
{
    (void)context;
    return BoardTickStart(tickUs);
}

/* One tick of real work for the running task: sn calls to unidad_trabajo() */
static void RunWork(uint32_t taskId, void *context)
{
    uint32_t i;

    (void)taskId;
    (void)context;
    for (i = 0; i < link.unitsPerTick; i++) {
        unidad_trabajo();
    }
}

/* Applies the frames that arrived since the last tick */
static void ReceiveFrames(void)
{
    uint8_t bytes[64];
    size_t total = 0;
    size_t count;

    while (total < NODE_MAX_READ_PER_TICK &&
           (count = BoardSerialRead(bytes, sizeof(bytes))) > 0) {
        NodeLinkReceive(&link, bytes, count);
        total += count;
    }
}

/* Sends what the port accepts now; the rest stays queued for the next tick */
static void SendFrames(void)
{
    const uint8_t *bytes;
    size_t pending;
    size_t written;
    uint32_t writes = 0;

    while (writes < NODE_MAX_WRITES_PER_TICK &&
           (pending = NodeLinkPending(&link, &bytes)) > 0) {
        written = BoardSerialWrite(bytes, pending);
        NodeLinkConsumed(&link, written);
        if (written < pending) {
            return; /* Port full or host not connected: try after the next tick */
        }
        writes++;
    }
}

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
    const NodeLinkHooks hooks = { .calibrate = Calibrate, .startTicks = StartTicks,
                                  .work = RunWork, .context = NULL };
    bool wasRunning = false;

    EventBufferInit(&events);
    if (!BoardLedsInit() || !BoardSerialInit() || !BoardButtonInit() ||
        SchedulerInit(&scheduler, SchedulerAlgorithmRms, &events) != SchedulerStatusOk ||
        !IndicatorsInit(&indicators, BoardTaskLedCount(), NODE_MISS_HOLD_TICKS)) {
        Halt();
    }
    NodeLinkInit(&link, &scheduler, &events, &hooks);
    if (!BoardTickStart(NODE_IDLE_TICK_US)) {
        Halt();
    }

    for (;;) {
        BoardTickWait();
        if (NodeLinkIsRunning(&link)) {
            if (!wasRunning) {
                /* A new run after Start: the miss LED starts off */
                (void)IndicatorsInit(&indicators, BoardTaskLedCount(), NODE_MISS_HOLD_TICKS);
                wasRunning = true;
            }
            if (BoardButtonTakePress() && link.sporadicTaskId != 0) {
                /* Ignored presses are counted by the scheduler as events */
                (void)SchedulerReleaseSporadic(&scheduler, link.sporadicTaskId);
            }
            SchedulerTick(&scheduler);
            BoardLedsShow(IndicatorsUpdate(&indicators, &scheduler));
            NodeLinkAfterTick(&link);
        } else {
            if (wasRunning) {
                BoardLedsShow((IndicatorState){ .taskPattern = 0, .missLed = false });
                wasRunning = false;
            }
            (void)BoardButtonTakePress(); /* Presses outside a run are dropped */
        }
        /* Serial work happens after the tick, so it never delays the scheduler */
        ReceiveFrames();
        SendFrames();
    }
}
