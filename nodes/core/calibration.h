#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <stdint.h>

/* Share of the tick given to task work; the rest is left for the scheduler,
   the LEDs and sending events (spec 3.3) */
enum {
    CalibrationDefaultPercent = 70
};

/* Work units per tick (sn) from a measurement: units calls to unidad_trabajo()
   took elapsedUs microseconds. Only percent of the tick is used. Returns 0
   when the arguments make no sense, and at least 1 otherwise. */
uint32_t CalibrationUnitsPerTick(uint32_t units, uint64_t elapsedUs, uint32_t tickUs,
                                 uint32_t percent);

#endif /* CALIBRATION_H */
