#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc16.h"
#include "protocol.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

typedef struct {
    uint8_t bytes[ProtocolMaxFrameSize];
    size_t length;
} WireFrame;

static WireFrame Encode(const Frame *frame)
{
    WireFrame wire = { .length = 0 };

    CHECK(ProtocolEncodeFrame(frame, wire.bytes, sizeof(wire.bytes), &wire.length) ==
          ProtocolStatusOk);
    return wire;
}

typedef struct {
    size_t readyCount;
    size_t errorCount;
    Frame lastFrame;
} FeedResult;

static FeedResult FeedBytes(FrameParser *parser, const uint8_t *bytes, size_t length)
{
    FeedResult result = { 0 };

    for (size_t i = 0; i < length; i++) {
        FrameParserResult step = FrameParserFeed(parser, bytes[i], &result.lastFrame);
        if (step == FrameParserReady) {
            result.readyCount++;
        } else if (step == FrameParserError) {
            result.errorCount++;
        }
    }
    return result;
}

static Frame ThroughWire(const Frame *frame)
{
    FrameParser parser;
    WireFrame wire = Encode(frame);

    FrameParserInit(&parser);
    FeedResult result = FeedBytes(&parser, wire.bytes, wire.length);
    CHECK(result.readyCount == 1 && result.errorCount == 0);
    return result.lastFrame;
}

static Frame SampleFrame(uint8_t seq)
{
    Frame frame = { 0 };
    TickReportMessage report = { .tick = 1000, .runningTaskId = 3, .droppedEvents = 0,
                                 .missCount = 1 };

    CHECK(ProtocolPackTickReport(&report, &frame) == ProtocolStatusOk);
    frame.seq = seq;
    return frame;
}

static void TestWireLayoutIsLittleEndianWithCrcHighFirst(void)
{
    Frame frame = { 0 };
    CalibRequestMessage request = { .tickUs = 10000 };

    CHECK(ProtocolPackCalibRequest(&request, &frame) == ProtocolStatusOk);
    frame.seq = 5;
    WireFrame wire = Encode(&frame);

    const uint8_t expectedStart[] = { 0x7E, 0x01, 0x05, 0x04, 0x10, 0x27, 0x00, 0x00 };
    uint16_t crc = Crc16Compute(expectedStart + 1, sizeof(expectedStart) - 1);

    CHECK(wire.length == sizeof(expectedStart) + ProtocolCrcSize);
    CHECK(memcmp(wire.bytes, expectedStart, sizeof(expectedStart)) == 0);
    CHECK(wire.bytes[8] == (uint8_t)(crc >> 8));
    CHECK(wire.bytes[9] == (uint8_t)(crc & 0xFF));
}

static void TestCalibrationRoundTrip(void)
{
    Frame frame = { 0 };
    CalibRequestMessage request = { .tickUs = 10000 }, requestOut = { 0 };
    CalibResponseMessage response = { .unitsPerTick = 0xDEADBEEF }, responseOut = { 0 };

    CHECK(ProtocolPackCalibRequest(&request, &frame) == ProtocolStatusOk);
    frame.seq = 1;
    Frame received = ThroughWire(&frame);
    CHECK(received.seq == 1);
    CHECK(ProtocolUnpackCalibRequest(&received, &requestOut) == ProtocolStatusOk);
    CHECK(requestOut.tickUs == 10000);

    CHECK(ProtocolPackCalibResponse(&response, &frame) == ProtocolStatusOk);
    received = ThroughWire(&frame);
    CHECK(ProtocolUnpackCalibResponse(&received, &responseOut) == ProtocolStatusOk);
    CHECK(responseOut.unitsPerTick == 0xDEADBEEF);
}

static void TestDeployRoundTrip(void)
{
    Frame frame = { 0 };
    DeployConfigMessage config = { .algorithm = ProtocolAlgorithmEdf, .tickReportEvery = 500 };
    DeployConfigMessage configOut = { 0 };
    DeployTaskMessage task = { .id = 7, .type = ProtocolTaskSporadic, .cost = 3, .period = 50,
                               .deadline = 40, .criticality = UINT32_MAX };
    DeployTaskMessage taskOut = { 0 };

    CHECK(ProtocolPackDeployConfig(&config, &frame) == ProtocolStatusOk);
    Frame received = ThroughWire(&frame);
    CHECK(ProtocolUnpackDeployConfig(&received, &configOut) == ProtocolStatusOk);
    CHECK(configOut.algorithm == ProtocolAlgorithmEdf && configOut.tickReportEvery == 500);

    CHECK(ProtocolPackDeployTask(&task, &frame) == ProtocolStatusOk);
    received = ThroughWire(&frame);
    CHECK(ProtocolUnpackDeployTask(&received, &taskOut) == ProtocolStatusOk);
    CHECK(taskOut.id == 7 && taskOut.type == ProtocolTaskSporadic && taskOut.cost == 3);
    CHECK(taskOut.period == 50 && taskOut.deadline == 40 && taskOut.criticality == UINT32_MAX);
}

static void TestRuntimeMessagesRoundTrip(void)
{
    Frame frame = { 0 };
    EventMessage event = { .eventType = 4, .tick = 123456, .taskId = 2, .value = 9 };
    EventMessage eventOut = { 0 };
    TickReportMessage reportOut = { 0 };
    SporadicTriggerMessage trigger = { .taskId = 8 }, triggerOut = { 0 };
    AckMessage ack = { .ackedSeq = 200, .status = AckStatusFull }, ackOut = { 0 };

    CHECK(ProtocolPackEvent(&event, &frame) == ProtocolStatusOk);
    Frame received = ThroughWire(&frame);
    CHECK(ProtocolUnpackEvent(&received, &eventOut) == ProtocolStatusOk);
    CHECK(eventOut.eventType == 4 && eventOut.tick == 123456);
    CHECK(eventOut.taskId == 2 && eventOut.value == 9);

    frame = SampleFrame(0);
    received = ThroughWire(&frame);
    CHECK(ProtocolUnpackTickReport(&received, &reportOut) == ProtocolStatusOk);
    CHECK(reportOut.tick == 1000 && reportOut.runningTaskId == 3);
    CHECK(reportOut.droppedEvents == 0 && reportOut.missCount == 1);

    CHECK(ProtocolPackSporadicTrigger(&trigger, &frame) == ProtocolStatusOk);
    received = ThroughWire(&frame);
    CHECK(ProtocolUnpackSporadicTrigger(&received, &triggerOut) == ProtocolStatusOk);
    CHECK(triggerOut.taskId == 8);

    CHECK(ProtocolPackAck(&ack, &frame) == ProtocolStatusOk);
    received = ThroughWire(&frame);
    CHECK(ProtocolUnpackAck(&received, &ackOut) == ProtocolStatusOk);
    CHECK(ackOut.ackedSeq == 200 && ackOut.status == AckStatusFull);
}

static void TestStartAndStopCarryNoPayload(void)
{
    Frame frame = { 0 };

    CHECK(ProtocolPackEmpty(MessageTypeStart, &frame) == ProtocolStatusOk);
    CHECK(frame.length == 0);
    Frame received = ThroughWire(&frame);
    CHECK(ProtocolUnpackEmpty(&received, MessageTypeStart) == ProtocolStatusOk);
    CHECK(ProtocolUnpackEmpty(&received, MessageTypeStop) == ProtocolStatusInvalidArgument);

    CHECK(ProtocolPackEmpty(MessageTypeStop, &frame) == ProtocolStatusOk);
    received = ThroughWire(&frame);
    CHECK(ProtocolUnpackEmpty(&received, MessageTypeStop) == ProtocolStatusOk);

    CHECK(ProtocolPackEmpty(MessageTypeAck, &frame) == ProtocolStatusInvalidArgument);
}

static void TestCorruptedFrameIsDiscardedAndCounted(void)
{
    FrameParser parser;
    Frame frame = SampleFrame(9);
    WireFrame wire = Encode(&frame);

    FrameParserInit(&parser);
    wire.bytes[6] ^= 0x01;
    FeedResult result = FeedBytes(&parser, wire.bytes, wire.length);
    CHECK(result.readyCount == 0 && result.errorCount == 1);
    CHECK(parser.badFrames == 1);

    wire = Encode(&frame);
    result = FeedBytes(&parser, wire.bytes, wire.length);
    CHECK(result.readyCount == 1 && result.lastFrame.seq == 9);
    CHECK(parser.badFrames == 1);
}

static void TestNoSingleBitFlipIsAccepted(void)
{
    Frame frame = SampleFrame(3);
    WireFrame original = Encode(&frame);

    for (size_t i = 1; i < original.length; i++) {
        for (int bit = 0; bit < 8; bit++) {
            FrameParser parser;
            WireFrame wire = original;

            FrameParserInit(&parser);
            wire.bytes[i] ^= (uint8_t)(1u << bit);
            FeedResult result = FeedBytes(&parser, wire.bytes, wire.length);
            CHECK(result.readyCount == 0);
        }
    }
}

static void TestGarbageBeforeStartIsSkipped(void)
{
    FrameParser parser;
    Frame frame = SampleFrame(4);
    WireFrame wire = Encode(&frame);
    uint8_t stream[8 + ProtocolMaxFrameSize] = { 0x00, 0xFF, 0x13, 0x55, 0xAA, 0x01, 0x02, 0x03 };

    memcpy(stream + 8, wire.bytes, wire.length);
    FrameParserInit(&parser);
    FeedResult result = FeedBytes(&parser, stream, 8 + wire.length);
    CHECK(result.readyCount == 1 && result.errorCount == 0);
    CHECK(parser.badFrames == 0);
}

static void TestBadHeaderIsRejectedEarly(void)
{
    FrameParser parser;
    const uint8_t unknownType[] = { 0x7E, 0x42 };
    const uint8_t tooLong[] = { 0x7E, MessageTypeEvent, 0x00, ProtocolMaxPayload + 1 };

    FrameParserInit(&parser);
    FeedResult result = FeedBytes(&parser, unknownType, sizeof(unknownType));
    CHECK(result.errorCount == 1 && parser.badFrames == 1);

    result = FeedBytes(&parser, tooLong, sizeof(tooLong));
    CHECK(result.errorCount == 1 && parser.badFrames == 2);
    CHECK(parser.state == FrameParserWaitStart);
}

static void TestBackToBackFrames(void)
{
    FrameParser parser;
    Frame first = SampleFrame(1);
    Frame second;
    uint8_t stream[2 * ProtocolMaxFrameSize];

    CHECK(ProtocolPackEmpty(MessageTypeStart, &second) == ProtocolStatusOk);
    second.seq = 2;
    WireFrame a = Encode(&first);
    WireFrame b = Encode(&second);
    memcpy(stream, a.bytes, a.length);
    memcpy(stream + a.length, b.bytes, b.length);

    FrameParserInit(&parser);
    FeedResult result = FeedBytes(&parser, stream, a.length);
    CHECK(result.readyCount == 1 && result.lastFrame.seq == 1);
    result = FeedBytes(&parser, stream + a.length, b.length);
    CHECK(result.readyCount == 1 && result.lastFrame.seq == 2);
    CHECK(result.lastFrame.type == MessageTypeStart);
}

static void TestResetDropsPartialFrame(void)
{
    FrameParser parser;
    Frame frame = SampleFrame(6);
    WireFrame wire = Encode(&frame);

    FrameParserInit(&parser);
    FeedBytes(&parser, wire.bytes, wire.length / 2);
    FrameParserReset(&parser);
    CHECK(parser.badFrames == 0);

    FeedResult result = FeedBytes(&parser, wire.bytes, wire.length);
    CHECK(result.readyCount == 1 && result.lastFrame.seq == 6);
}

static void TestEncodeRejectsSmallBuffer(void)
{
    Frame frame = SampleFrame(0);
    uint8_t out[ProtocolMaxFrameSize];
    size_t length = 77;

    memset(out, 0xCC, sizeof(out));
    CHECK(ProtocolEncodeFrame(&frame, out, ProtocolHeaderSize + frame.length + 1, &length) ==
          ProtocolStatusBufferTooSmall);
    CHECK(length == 77);
    CHECK(out[0] == 0xCC);

    frame.length = ProtocolMaxPayload + 1;
    CHECK(ProtocolEncodeFrame(&frame, out, sizeof(out), &length) == ProtocolStatusBadPayload);
}

static void TestUnpackChecksTypeLengthAndRanges(void)
{
    Frame frame = SampleFrame(0);
    EventMessage event;
    DeployConfigMessage config = { .algorithm = (ProtocolAlgorithm)2, .tickReportEvery = 1 };
    DeployTaskMessage task = { .id = 1, .type = (ProtocolTaskType)5, .cost = 1, .period = 1,
                               .deadline = 1, .criticality = 0 };
    AckMessage ack = { .ackedSeq = 0, .status = (AckStatus)9 };

    CHECK(ProtocolUnpackEvent(&frame, &event) == ProtocolStatusInvalidArgument);

    frame.type = MessageTypeEvent;
    CHECK(ProtocolUnpackEvent(&frame, &event) == ProtocolStatusBadPayload);

    CHECK(ProtocolPackDeployConfig(&config, &frame) == ProtocolStatusBadPayload);
    CHECK(ProtocolPackDeployTask(&task, &frame) == ProtocolStatusBadPayload);
    CHECK(ProtocolPackAck(&ack, &frame) == ProtocolStatusBadPayload);

    config.algorithm = ProtocolAlgorithmRms;
    CHECK(ProtocolPackDeployConfig(&config, &frame) == ProtocolStatusOk);
    frame.payload[0] = 2;
    CHECK(ProtocolUnpackDeployConfig(&frame, &config) == ProtocolStatusBadPayload);
}

static void TestNullArguments(void)
{
    Frame frame = SampleFrame(0);
    FrameParser parser;
    uint8_t out[ProtocolMaxFrameSize];
    size_t length;

    FrameParserInit(&parser);
    CHECK(ProtocolEncodeFrame(NULL, out, sizeof(out), &length) == ProtocolStatusInvalidArgument);
    CHECK(ProtocolEncodeFrame(&frame, NULL, sizeof(out), &length) == ProtocolStatusInvalidArgument);
    CHECK(ProtocolEncodeFrame(&frame, out, sizeof(out), NULL) == ProtocolStatusInvalidArgument);
    CHECK(ProtocolPackEvent(NULL, &frame) == ProtocolStatusInvalidArgument);
    CHECK(ProtocolUnpackTickReport(&frame, NULL) == ProtocolStatusInvalidArgument);
    CHECK(FrameParserFeed(NULL, 0x7E, &frame) == FrameParserError);
    CHECK(FrameParserFeed(&parser, 0x7E, NULL) == FrameParserError);
    FrameParserInit(NULL);
    FrameParserReset(NULL);
}

int main(void)
{
    TestWireLayoutIsLittleEndianWithCrcHighFirst();
    TestCalibrationRoundTrip();
    TestDeployRoundTrip();
    TestRuntimeMessagesRoundTrip();
    TestStartAndStopCarryNoPayload();
    TestCorruptedFrameIsDiscardedAndCounted();
    TestNoSingleBitFlipIsAccepted();
    TestGarbageBeforeStartIsSkipped();
    TestBadHeaderIsRejectedEarly();
    TestBackToBackFrames();
    TestResetDropsPartialFrame();
    TestEncodeRejectsSmallBuffer();
    TestUnpackChecksTypeLengthAndRanges();
    TestNullArguments();

    if (failureCount == 0) {
        printf("test_protocol: all passed\n");
        return 0;
    }
    printf("test_protocol: %d failure(s)\n", failureCount);
    return 1;
}
