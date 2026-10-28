#include "admission.h"

#include <math.h>

/* Absorbs floating point rounding when comparing against a limit */
static const double AdmissionTolerance = 1e-9;

/* Admission: sums cost / period of a task list */
double AdmissionUtilization(const LoadEntry *entries, size_t count)
{
    if (count == 0) {
        return 0.0;
    }
    if (entries == NULL) {
        return -1.0;
    }

    double total = 0.0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].period == 0) {
            return -1.0;
        }
        total += (double)entries[i].cost / (double)entries[i].period;
    }
    return total;
}

/* Admission: RMS utilization limit n(2^(1/n) - 1) */
double AdmissionRmsBound(size_t count)
{
    if (count == 0) {
        return 1.0;
    }
    double taskCount = (double)count;
    return taskCount * (pow(2.0, 1.0 / taskCount) - 1.0);
}

/* Admission: EDF test, accepts if utilization <= 1 */
bool AdmissionAcceptsEdf(const LoadEntry *entries, size_t count)
{
    double utilization = AdmissionUtilization(entries, count);
    if (utilization < 0.0) {
        return false;
    }
    return utilization <= 1.0 + AdmissionTolerance;
}

/* Admission: RMS test, accepts if utilization <= bound */
bool AdmissionAcceptsRms(const LoadEntry *entries, size_t count)
{
    double utilization = AdmissionUtilization(entries, count);
    if (utilization < 0.0) {
        return false;
    }
    return utilization <= AdmissionRmsBound(count) + AdmissionTolerance;
}