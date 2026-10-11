#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>

#include "indicators.h"

/* Pin layer that each board implements (nodes/esp32, nodes/pico).
   The scheduler core never touches pins directly. */

/* Configures the LED pins as outputs and turns every LED off */
bool BoardLedsInit(void);

/* Number of task LEDs wired on this board */
unsigned int BoardTaskLedCount(void);

/* Writes the pattern to the LEDs; must be quick enough to call every tick */
void BoardLedsShow(IndicatorState state);

#endif /* BOARD_H */