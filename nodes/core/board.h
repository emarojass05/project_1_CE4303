#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "indicators.h"

/* Hardware layer that each board implements (nodes/esp32, nodes/pico).
   The scheduler core never touches pins, timers or ports directly. */

/* Configures the LED pins as outputs and turns every LED off */
bool BoardLedsInit(void);

/* Number of task LEDs wired on this board */
unsigned int BoardTaskLedCount(void);

/* Writes the pattern to the LEDs; must be quick enough to call every tick */
void BoardLedsShow(IndicatorState state);

/* Starts the hardware timer that marks a tick every tickUs microseconds.
   Calling it again restarts the timer from now with the new period and drops
   the ticks not yet taken, so a Start message can set tick zero. */
bool BoardTickStart(uint32_t tickUs);

/* Sleeps until a tick is due and returns once per tick, so none is lost.
   It waits on an interrupt: never delay(), sleep or busy waiting. */
void BoardTickWait(void);

/* Free-running microsecond clock, used for calibration */
uint64_t BoardMicros(void);

/* Sets up the sporadic task button with its interrupt and debounce */
bool BoardButtonInit(void);

/* True once per debounced press since the last call; clears it.
   The interrupt only records the press, the main loop does the rest. */
bool BoardButtonTakePress(void);

/* Opens the serial port of the protocol (docs/protocolo.md) */
bool BoardSerialInit(void);

/* Writes up to count bytes without waiting and returns how many it wrote;
   0 when the port is busy or the host is not connected */
size_t BoardSerialWrite(const uint8_t *bytes, size_t count);

/* Reads up to capacity bytes that already arrived, without waiting;
   returns 0 when there is nothing to read */
size_t BoardSerialRead(uint8_t *bytes, size_t capacity);

#endif /* BOARD_H */
