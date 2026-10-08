#ifndef ADMISSION_H
#define ADMISSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Load of one task on one node */
typedef struct {
    uint32_t cost;   /* Ticks per job (C) */
    uint32_t period; /* Ticks between releases (T) */
} LoadEntry;

/* Sum of cost / period, or a negative value when the input is invalid */
double AdmissionUtilization(const LoadEntry *entries, size_t count);

/* Liu and Layland bound for count tasks; 1.0 when count is zero */
double AdmissionRmsBound(size_t count);

/* True when the total utilization is at most 1 */
bool AdmissionAcceptsEdf(const LoadEntry *entries, size_t count);

/* True when the total utilization is within the RMS bound */
bool AdmissionAcceptsRms(const LoadEntry *entries, size_t count);

#endif /* ADMISSION_H */