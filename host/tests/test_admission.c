#include <math.h>
#include <stdio.h>

#include "admission.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static bool IsClose(double actual, double expected)
{
    return fabs(actual - expected) < 1e-4;
}

static void TestUtilization(void)
{
    LoadEntry entries[] = { { 2, 5 }, { 4, 7 } };
    LoadEntry zeroPeriod[] = { { 2, 5 }, { 1, 0 } };

    CHECK(IsClose(AdmissionUtilization(entries, 2), 0.9714));
    CHECK(AdmissionUtilization(NULL, 0) == 0.0);
    CHECK(AdmissionUtilization(NULL, 1) < 0.0);
    CHECK(AdmissionUtilization(zeroPeriod, 2) < 0.0);
}

static void TestRmsBound(void)
{
    CHECK(AdmissionRmsBound(0) == 1.0);
    CHECK(IsClose(AdmissionRmsBound(1), 1.0));
    CHECK(IsClose(AdmissionRmsBound(2), 0.8284));
    CHECK(IsClose(AdmissionRmsBound(3), 0.7798));
    CHECK(AdmissionRmsBound(100) > 0.6931);
    CHECK(AdmissionRmsBound(100) < AdmissionRmsBound(10));
}

static void TestEmptyAndInvalid(void)
{
    LoadEntry zeroPeriod[] = { { 1, 0 } };

    CHECK(AdmissionAcceptsEdf(NULL, 0));
    CHECK(AdmissionAcceptsRms(NULL, 0));
    CHECK(!AdmissionAcceptsEdf(NULL, 3));
    CHECK(!AdmissionAcceptsRms(NULL, 3));
    CHECK(!AdmissionAcceptsEdf(zeroPeriod, 1));
    CHECK(!AdmissionAcceptsRms(zeroPeriod, 1));
}

static void TestEdfLimit(void)
{
    LoadEntry exactlyFull[] = { { 1, 2 }, { 1, 2 } };
    LoadEntry overFull[] = { { 1, 2 }, { 1, 2 }, { 1, 100 } };
    LoadEntry ninths[9];

    CHECK(AdmissionAcceptsEdf(exactlyFull, 2));
    CHECK(!AdmissionAcceptsEdf(overFull, 3));

    /* Nine tasks of 1/9 sum to slightly above 1.0 in floating point */
    for (size_t i = 0; i < 9; i++) {
        ninths[i].cost = 1;
        ninths[i].period = 9;
    }
    CHECK(AdmissionUtilization(ninths, 9) > 1.0);
    CHECK(AdmissionAcceptsEdf(ninths, 9));
}

static void TestRmsLimit(void)
{
    LoadEntry singleFull[] = { { 5, 5 } };
    LoadEntry singleOver[] = { { 6, 5 } };
    LoadEntry belowBound[] = { { 41, 100 }, { 41, 100 } };
    LoadEntry aboveBound[] = { { 42, 100 }, { 42, 100 } };

    CHECK(AdmissionAcceptsRms(singleFull, 1));
    CHECK(!AdmissionAcceptsRms(singleOver, 1));
    CHECK(AdmissionAcceptsRms(belowBound, 2));
    CHECK(!AdmissionAcceptsRms(aboveBound, 2));
}

static void TestEdfAcceptsWhatRmsRejects(void)
{
    LoadEntry edfOnly[] = { { 2, 5 }, { 4, 7 } };
    LoadEntry exerciseSet[] = { { 3, 6 }, { 2, 5 } };

    CHECK(AdmissionAcceptsEdf(edfOnly, 2));
    CHECK(!AdmissionAcceptsRms(edfOnly, 2));
    CHECK(AdmissionAcceptsEdf(exerciseSet, 2));
    CHECK(!AdmissionAcceptsRms(exerciseSet, 2));
}

int main(void)
{
    TestUtilization();
    TestRmsBound();
    TestEmptyAndInvalid();
    TestEdfLimit();
    TestRmsLimit();
    TestEdfAcceptsWhatRmsRejects();

    if (failureCount == 0) {
        printf("test_admission: all passed\n");
        return 0;
    }
    printf("test_admission: %d failure(s)\n", failureCount);
    return 1;
}