#ifndef DEBOUNCE_H
#define DEBOUNCE_H

#include <stdbool.h>
#include <stdint.h>

/* Button debounce by time, safe to call from an interrupt: a press is accepted
   only when at least minGapUs passed since the last accepted one. Bounces are
   discarded here; the sporadic minimum interval is checked by the scheduler. */
typedef struct {
    uint32_t minGapUs;
    uint32_t lastUs; /* Time of the last accepted press */
    bool any;        /* False until the first press is accepted */
} Debounce;

void DebounceInit(Debounce *debounce, uint32_t minGapUs);

/* nowUs is a free-running microsecond counter; wrap-around is handled */
bool DebounceAccept(Debounce *debounce, uint32_t nowUs);

#endif /* DEBOUNCE_H */
