#include <stdio.h>

#include "debounce.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static void TestFirstPressIsAccepted(void)
{
    Debounce debounce;

    DebounceInit(&debounce, 50000);
    CHECK(DebounceAccept(&debounce, 0));
}

static void TestBouncesAreDiscarded(void)
{
    Debounce debounce;

    DebounceInit(&debounce, 50000);
    CHECK(DebounceAccept(&debounce, 1000));
    CHECK(!DebounceAccept(&debounce, 1200));
    CHECK(!DebounceAccept(&debounce, 50999));
    CHECK(DebounceAccept(&debounce, 51000));
    CHECK(!DebounceAccept(&debounce, 51001));
}

static void TestCounterWrapAround(void)
{
    Debounce debounce;

    DebounceInit(&debounce, 50000);
    CHECK(DebounceAccept(&debounce, UINT32_MAX - 10000));
    CHECK(!DebounceAccept(&debounce, 20000));
    CHECK(DebounceAccept(&debounce, 40000));
}

static void TestNullDebounce(void)
{
    DebounceInit(NULL, 1);
    CHECK(!DebounceAccept(NULL, 0));
}

int main(void)
{
    TestFirstPressIsAccepted();
    TestBouncesAreDiscarded();
    TestCounterWrapAround();
    TestNullDebounce();

    if (failureCount != 0) {
        printf("test_debounce: %d check(s) failed\n", failureCount);
        return 1;
    }
    printf("test_debounce: all checks passed\n");
    return 0;
}
