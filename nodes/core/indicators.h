#ifndef INDICATORS_H
#define INDICATORS_H

#include <stdbool.h>
#include <stdint.h>

#include "scheduler.h"

/* Most LEDs a board may use for the running task number */
enum {
    IndicatorsMaxTaskBits = 8
};

/* Decides what the LEDs show from the scheduler state, independent of the board.

   Task LEDs: the slot of the task that ran in the last tick, plus one, in binary
   (0 = no task ran, 1 = first task added, 2 = second, ...). A slot that does not
   fit in the available bits shows every LED on.

   Miss LED: turns on in the tick where a deadline is missed and stays on for
   missHoldTicks ticks so it can be seen; with missHoldTicks = 0 it stays on for
   the rest of the run. */
typedef struct {
    uint32_t taskBits;      /* LEDs available for the task number */
    uint32_t missHoldTicks; /* Ticks the miss LED stays on, 0 = forever */
    uint32_t seenMisses;    /* Scheduler missCount at the last update */
    uint32_t missTicksLeft; /* Ticks left with the miss LED on */
    bool missLatched;       /* Miss LED on for good (missHoldTicks = 0) */
} Indicators;

/* What the board has to write to its pins */
typedef struct {
    uint32_t taskPattern; /* Bit i drives task LED i (bit 0 = least significant) */
    bool missLed;
} IndicatorState;

/* Prepares the indicators; returns false when taskBits is 0 or too large */
bool IndicatorsInit(Indicators *indicators, uint32_t taskBits, uint32_t missHoldTicks);

/* Call once after every SchedulerTick; returns what the LEDs must show now */
IndicatorState IndicatorsUpdate(Indicators *indicators, const Scheduler *scheduler);

#endif /* INDICATORS_H */