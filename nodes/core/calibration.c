#include "calibration.h"

uint32_t CalibrationUnitsPerTick(uint32_t units, uint64_t elapsedUs, uint32_t tickUs,
                                 uint32_t percent)
{
    uint64_t perTick;

    if (units == 0 || elapsedUs == 0 || tickUs == 0 || percent == 0 || percent > 100) {
        return 0;
    }
    /* units * tickUs * percent fits in 64 bits for any 32-bit inputs */
    perTick = (uint64_t)units * tickUs * percent / (elapsedUs * 100u);
    if (perTick == 0) {
        return 1;
    }
    if (perTick > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)perTick;
}
