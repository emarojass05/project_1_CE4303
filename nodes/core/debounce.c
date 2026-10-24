#include "debounce.h"

#include <stddef.h>

void DebounceInit(Debounce *debounce, uint32_t minGapUs)
{
    if (debounce == NULL) {
        return;
    }
    debounce->minGapUs = minGapUs;
    debounce->lastUs = 0;
    debounce->any = false;
}

bool DebounceAccept(Debounce *debounce, uint32_t nowUs)
{
    if (debounce == NULL) {
        return false;
    }
    /* Unsigned subtraction gives the right gap even after the counter wraps */
    if (debounce->any && nowUs - debounce->lastUs < debounce->minGapUs) {
        return false;
    }
    debounce->any = true;
    debounce->lastUs = nowUs;
    return true;
}
