#include <limits.h>
#include <stdio.h>

#include "event_buffer.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

/* Event whose tick doubles as a sequence number */
static Event MakeEvent(uint32_t sequence)
{
    Event event = { .type = EventTypeStart, .tick = sequence,
                    .taskId = sequence + 100, .value = sequence + 200 };
    return event;
}

static void TestEmptyBuffer(void)
{
    EventBuffer buffer;
    Event event;

    EventBufferInit(&buffer);
    CHECK(EventBufferCount(&buffer) == 0);
    CHECK(EventBufferDroppedCount(&buffer) == 0);
    CHECK(!EventBufferPop(&buffer, &event));
}

static void TestFifoOrderAndFields(void)
{
    EventBuffer buffer;
    Event event;

    EventBufferInit(&buffer);
    CHECK(EventBufferPush(&buffer, MakeEvent(1)));
    CHECK(EventBufferPush(&buffer, MakeEvent(2)));
    CHECK(EventBufferPush(&buffer, MakeEvent(3)));
    CHECK(EventBufferCount(&buffer) == 3);

    CHECK(EventBufferPop(&buffer, &event));
    CHECK(event.type == EventTypeStart && event.tick == 1);
    CHECK(event.taskId == 101 && event.value == 201);
    CHECK(EventBufferPop(&buffer, &event) && event.tick == 2);
    CHECK(EventBufferPop(&buffer, &event) && event.tick == 3);
    CHECK(!EventBufferPop(&buffer, &event));
    CHECK(EventBufferCount(&buffer) == 0);
}

static void TestWrapAround(void)
{
    EventBuffer buffer;
    Event event;
    uint32_t nextPush = 0;
    uint32_t nextPop = 0;
    bool orderKept = true;

    EventBufferInit(&buffer);
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 37; i++) {
            if (!EventBufferPush(&buffer, MakeEvent(nextPush))) {
                orderKept = false;
            }
            nextPush++;
        }
        for (int i = 0; i < 37; i++) {
            if (!EventBufferPop(&buffer, &event) || event.tick != nextPop) {
                orderKept = false;
            }
            nextPop++;
        }
    }
    CHECK(orderKept);
    CHECK(EventBufferCount(&buffer) == 0);
    CHECK(EventBufferDroppedCount(&buffer) == 0);
}

static void TestFullBufferDropsNewEvents(void)
{
    EventBuffer buffer;
    Event event;
    bool oldestKept = true;

    EventBufferInit(&buffer);
    for (uint32_t i = 0; i < EventBufferCapacity; i++) {
        CHECK(EventBufferPush(&buffer, MakeEvent(i)));
    }
    CHECK(EventBufferCount(&buffer) == EventBufferCapacity);

    for (uint32_t i = 0; i < 6; i++) {
        CHECK(!EventBufferPush(&buffer, MakeEvent(1000 + i)));
    }
    CHECK(EventBufferDroppedCount(&buffer) == 6);
    CHECK(EventBufferCount(&buffer) == EventBufferCapacity);

    for (uint32_t i = 0; i < EventBufferCapacity; i++) {
        if (!EventBufferPop(&buffer, &event) || event.tick != i) {
            oldestKept = false;
        }
    }
    CHECK(oldestKept);
    CHECK(!EventBufferPop(&buffer, &event));

    CHECK(EventBufferPush(&buffer, MakeEvent(7)));
    CHECK(EventBufferDroppedCount(&buffer) == 6);
}

static void TestCounterOverflow(void)
{
    EventBuffer buffer;
    Event event;
    unsigned int start = UINT_MAX - 5u;
    bool orderKept = true;

    EventBufferInit(&buffer);
    atomic_store(&buffer.head, start);
    atomic_store(&buffer.tail, start);

    for (uint32_t i = 0; i < 20; i++) {
        if (!EventBufferPush(&buffer, MakeEvent(i))) {
            orderKept = false;
        }
    }
    CHECK(EventBufferCount(&buffer) == 20);
    for (uint32_t i = 0; i < 20; i++) {
        if (!EventBufferPop(&buffer, &event) || event.tick != i) {
            orderKept = false;
        }
    }
    CHECK(orderKept);
    CHECK(EventBufferCount(&buffer) == 0);
}

static void TestInvalidArguments(void)
{
    EventBuffer buffer;
    Event event;

    EventBufferInit(&buffer);
    EventBufferInit(NULL);
    CHECK(!EventBufferPush(NULL, MakeEvent(1)));
    CHECK(!EventBufferPop(NULL, &event));
    CHECK(!EventBufferPop(&buffer, NULL));
    CHECK(EventBufferCount(NULL) == 0);
    CHECK(EventBufferDroppedCount(NULL) == 0);
}

int main(void)
{
    TestEmptyBuffer();
    TestFifoOrderAndFields();
    TestWrapAround();
    TestFullBufferDropsNewEvents();
    TestCounterOverflow();
    TestInvalidArguments();

    if (failureCount == 0) {
        printf("test_event_buffer: all passed\n");
        return 0;
    }
    printf("test_event_buffer: %d failure(s)\n", failureCount);
    return 1;
}