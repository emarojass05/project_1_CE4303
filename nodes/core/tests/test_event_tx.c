#include <stdio.h>

#include "event_tx.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

/* Feeds bytes to a frame parser and returns how many Event frames came out */
static int ParseEvents(FrameParser *parser, const uint8_t *bytes, size_t count,
                       EventMessage *outLast, uint8_t *outSeq)
{
    Frame frame;
    int frames = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        if (FrameParserFeed(parser, bytes[i], &frame) == FrameParserReady) {
            CHECK(ProtocolUnpackEvent(&frame, outLast) == ProtocolStatusOk);
            *outSeq = frame.seq;
            frames++;
        }
    }
    return frames;
}

static void TestEmptyBufferSendsNothing(void)
{
    EventBuffer events;
    EventTx tx;
    const uint8_t *bytes = NULL;

    EventBufferInit(&events);
    EventTxInit(&tx);
    CHECK(EventTxPending(&tx, &events, &bytes) == 0);
}

static void TestEventBecomesFrame(void)
{
    EventBuffer events;
    EventTx tx;
    FrameParser parser;
    EventMessage message;
    uint8_t seq = 0xFF;
    const uint8_t *bytes = NULL;
    size_t count;
    Event event = { .type = EventTypeDeadlineMiss, .tick = 1234, .taskId = 7, .value = 3 };

    EventBufferInit(&events);
    EventTxInit(&tx);
    FrameParserInit(&parser);
    CHECK(EventBufferPush(&events, event));

    count = EventTxPending(&tx, &events, &bytes);
    CHECK(count == 19); /* 4 header + 13 payload + 2 CRC */
    CHECK(ParseEvents(&parser, bytes, count, &message, &seq) == 1);
    EventTxConsumed(&tx, count);
    CHECK(message.eventType == EventTypeDeadlineMiss);
    CHECK(message.tick == 1234);
    CHECK(message.taskId == 7);
    CHECK(message.value == 3);
    CHECK(seq == 0);
    CHECK(EventTxPending(&tx, &events, &bytes) == 0);
}

/* A port that accepts 5 bytes at a time still delivers every frame, in order */
static void TestPartialWritesKeepEveryEvent(void)
{
    EventBuffer events;
    EventTx tx;
    FrameParser parser;
    EventMessage message;
    uint8_t seq = 0;
    const uint8_t *bytes = NULL;
    size_t count;
    uint32_t i;
    int frames = 0;
    uint32_t lastTick = 0;

    EventBufferInit(&events);
    EventTxInit(&tx);
    FrameParserInit(&parser);
    for (i = 0; i < 10; i++) {
        Event event = { .type = EventTypeStart, .tick = i, .taskId = 1, .value = 0 };
        CHECK(EventBufferPush(&events, event));
    }

    while ((count = EventTxPending(&tx, &events, &bytes)) > 0) {
        if (count > 5) {
            count = 5;
        }
        if (ParseEvents(&parser, bytes, count, &message, &seq) == 1) {
            CHECK(message.tick == (uint32_t)frames);
            CHECK(seq == (uint8_t)frames);
            lastTick = message.tick;
            frames++;
        }
        EventTxConsumed(&tx, count);
    }
    CHECK(frames == 10);
    CHECK(lastTick == 9);
    CHECK(parser.badFrames == 0);
}

/* Nothing written: the same bytes are offered again and no event is lost */
static void TestBlockedPortKeepsFrame(void)
{
    EventBuffer events;
    EventTx tx;
    const uint8_t *first = NULL;
    const uint8_t *second = NULL;
    Event event = { .type = EventTypeRelease, .tick = 1, .taskId = 2, .value = 0 };

    EventBufferInit(&events);
    EventTxInit(&tx);
    CHECK(EventBufferPush(&events, event));
    CHECK(EventTxPending(&tx, &events, &first) == 19);
    EventTxConsumed(&tx, 0);
    CHECK(EventTxPending(&tx, &events, &second) == 19);
    CHECK(first == second);
    EventTxConsumed(&tx, 100); /* More than offered is clamped */
    CHECK(EventTxPending(&tx, &events, &second) == 0);
}

static void TestNullArguments(void)
{
    EventBuffer events;
    EventTx tx;
    const uint8_t *bytes = NULL;

    EventBufferInit(&events);
    EventTxInit(&tx);
    EventTxInit(NULL);
    EventTxConsumed(NULL, 1);
    CHECK(EventTxPending(NULL, &events, &bytes) == 0);
    CHECK(EventTxPending(&tx, NULL, &bytes) == 0);
    CHECK(EventTxPending(&tx, &events, NULL) == 0);
}

int main(void)
{
    TestEmptyBufferSendsNothing();
    TestEventBecomesFrame();
    TestPartialWritesKeepEveryEvent();
    TestBlockedPortKeepsFrame();
    TestNullArguments();

    if (failureCount != 0) {
        printf("test_event_tx: %d check(s) failed\n", failureCount);
        return 1;
    }
    printf("test_event_tx: all checks passed\n");
    return 0;
}
