#include <stdio.h>

#include "calibration.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static void TestScalesToTheTick(void)
{
    /* 1000 units in 5 ms with a 10 ms tick: 2000 fit, 70 % of them are used */
    CHECK(CalibrationUnitsPerTick(1000, 5000, 10000, 70) == 1400);
    CHECK(CalibrationUnitsPerTick(1000, 5000, 10000, 100) == 2000);
    /* A tick twice as long doubles sn */
    CHECK(CalibrationUnitsPerTick(1000, 5000, 20000, 70) == 2800);
}

static void TestSlowNodeGetsAtLeastOne(void)
{
    /* One unit takes longer than the whole tick */
    CHECK(CalibrationUnitsPerTick(1, 50000, 10000, 70) == 1);
}

static void TestInvalidArguments(void)
{
    CHECK(CalibrationUnitsPerTick(0, 5000, 10000, 70) == 0);
    CHECK(CalibrationUnitsPerTick(1000, 0, 10000, 70) == 0);
    CHECK(CalibrationUnitsPerTick(1000, 5000, 0, 70) == 0);
    CHECK(CalibrationUnitsPerTick(1000, 5000, 10000, 0) == 0);
    CHECK(CalibrationUnitsPerTick(1000, 5000, 10000, 101) == 0);
}

static void TestHugeValuesSaturate(void)
{
    CHECK(CalibrationUnitsPerTick(UINT32_MAX, 1, UINT32_MAX, 100) == UINT32_MAX);
}

int main(void)
{
    TestScalesToTheTick();
    TestSlowNodeGetsAtLeastOne();
    TestInvalidArguments();
    TestHugeValuesSaturate();

    if (failureCount != 0) {
        printf("test_calibration: %d check(s) failed\n", failureCount);
        return 1;
    }
    printf("test_calibration: all checks passed\n");
    return 0;
}
