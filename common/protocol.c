#include "protocol.h"

#include <string.h>

#include "crc16.h"

enum {
    CalibRequestLength = 4,
    CalibResponseLength = 4,
    DeployConfigLength = 3,
    DeployTaskLength = 21,
    EventLength = 13,
    TickReportLength = 16,
    SporadicTriggerLength = 4,
    AckLength = 2
};

typedef struct {
    Frame *frame;
} PayloadWriter;

typedef struct {
    const Frame *frame;
    uint8_t offset;
} PayloadReader;

static PayloadWriter StartPayload(Frame *frame, MessageType type)
{
    frame->type = (uint8_t)type;
    frame->length = 0;
    return (PayloadWriter){ .frame = frame };
}

static void PutU8(PayloadWriter *writer, uint8_t value)
{
    writer->frame->payload[writer->frame->length++] = value;
}

static void PutU16(PayloadWriter *writer, uint16_t value)
{
    PutU8(writer, (uint8_t)(value & 0xFFu));
    PutU8(writer, (uint8_t)(value >> 8));
}

static void PutU32(PayloadWriter *writer, uint32_t value)
{
    PutU16(writer, (uint16_t)(value & 0xFFFFu));
    PutU16(writer, (uint16_t)(value >> 16));
}

static ProtocolStatus StartReading(const Frame *frame, MessageType type, uint8_t length,
                                   PayloadReader *outReader)
{
    if (frame->type != (uint8_t)type) {
        return ProtocolStatusInvalidArgument;
    }
    if (frame->length != length) {
        return ProtocolStatusBadPayload;
    }
    *outReader = (PayloadReader){ .frame = frame, .offset = 0 };
    return ProtocolStatusOk;
}

static uint8_t GetU8(PayloadReader *reader)
{
    return reader->frame->payload[reader->offset++];
}

static uint16_t GetU16(PayloadReader *reader)
{
    uint16_t low = GetU8(reader);
    uint16_t high = GetU8(reader);
    return (uint16_t)(low | (high << 8));
}

static uint32_t GetU32(PayloadReader *reader)
{
    uint32_t low = GetU16(reader);
    uint32_t high = GetU16(reader);
    return low | (high << 16);
}

static uint16_t FrameCrc(const Frame *frame)
{
    const uint8_t header[] = { frame->type, frame->seq, frame->length };
    uint16_t crc = Crc16Update(Crc16Initial, header, sizeof(header));
    return Crc16Update(crc, frame->payload, frame->length);
}

static int IsKnownType(uint8_t type)
{
    return type >= MessageTypeCalibRequest && type <= MessageTypeAck;
}

ProtocolStatus ProtocolEncodeFrame(const Frame *frame, uint8_t *out, size_t outCapacity,
                                   size_t *outLength)
{
    if (frame == NULL || out == NULL || outLength == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    if (frame->length > ProtocolMaxPayload || !IsKnownType(frame->type)) {
        return ProtocolStatusBadPayload;
    }

    size_t total = ProtocolHeaderSize + frame->length + ProtocolCrcSize;
    if (outCapacity < total) {
        return ProtocolStatusBufferTooSmall;
    }

    uint16_t crc = FrameCrc(frame);
    out[0] = ProtocolStartOfFrame;
    out[1] = frame->type;
    out[2] = frame->seq;
    out[3] = frame->length;
    memcpy(out + ProtocolHeaderSize, frame->payload, frame->length);
    out[ProtocolHeaderSize + frame->length] = (uint8_t)(crc >> 8);
    out[ProtocolHeaderSize + frame->length + 1] = (uint8_t)(crc & 0xFFu);
    *outLength = total;
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolPackCalibRequest(const CalibRequestMessage *message, Frame *outFrame)
{
    if (message == NULL || outFrame == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    PayloadWriter writer = StartPayload(outFrame, MessageTypeCalibRequest);
    PutU32(&writer, message->tickUs);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolUnpackCalibRequest(const Frame *frame, CalibRequestMessage *outMessage)
{
    PayloadReader reader;

    if (frame == NULL || outMessage == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    ProtocolStatus status = StartReading(frame, MessageTypeCalibRequest, CalibRequestLength, &reader);
    if (status != ProtocolStatusOk) {
        return status;
    }
    outMessage->tickUs = GetU32(&reader);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolPackCalibResponse(const CalibResponseMessage *message, Frame *outFrame)
{
    if (message == NULL || outFrame == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    PayloadWriter writer = StartPayload(outFrame, MessageTypeCalibResponse);
    PutU32(&writer, message->unitsPerTick);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolUnpackCalibResponse(const Frame *frame, CalibResponseMessage *outMessage)
{
    PayloadReader reader;

    if (frame == NULL || outMessage == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    ProtocolStatus status = StartReading(frame, MessageTypeCalibResponse, CalibResponseLength, &reader);
    if (status != ProtocolStatusOk) {
        return status;
    }
    outMessage->unitsPerTick = GetU32(&reader);
    return ProtocolStatusOk;
}

static int IsValidAlgorithm(uint8_t value)
{
    return value == ProtocolAlgorithmRms || value == ProtocolAlgorithmEdf;
}

ProtocolStatus ProtocolPackDeployConfig(const DeployConfigMessage *message, Frame *outFrame)
{
    if (message == NULL || outFrame == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    if (!IsValidAlgorithm((uint8_t)message->algorithm)) {
        return ProtocolStatusBadPayload;
    }
    PayloadWriter writer = StartPayload(outFrame, MessageTypeDeployConfig);
    PutU8(&writer, (uint8_t)message->algorithm);
    PutU16(&writer, message->tickReportEvery);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolUnpackDeployConfig(const Frame *frame, DeployConfigMessage *outMessage)
{
    PayloadReader reader;

    if (frame == NULL || outMessage == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    ProtocolStatus status = StartReading(frame, MessageTypeDeployConfig, DeployConfigLength, &reader);
    if (status != ProtocolStatusOk) {
        return status;
    }
    uint8_t algorithm = GetU8(&reader);
    if (!IsValidAlgorithm(algorithm)) {
        return ProtocolStatusBadPayload;
    }
    outMessage->algorithm = (ProtocolAlgorithm)algorithm;
    outMessage->tickReportEvery = GetU16(&reader);
    return ProtocolStatusOk;
}

static int IsValidTaskType(uint8_t value)
{
    return value == ProtocolTaskPeriodic || value == ProtocolTaskSporadic;
}

ProtocolStatus ProtocolPackDeployTask(const DeployTaskMessage *message, Frame *outFrame)
{
    if (message == NULL || outFrame == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    if (!IsValidTaskType((uint8_t)message->type)) {
        return ProtocolStatusBadPayload;
    }
    PayloadWriter writer = StartPayload(outFrame, MessageTypeDeployTask);
    PutU32(&writer, message->id);
    PutU8(&writer, (uint8_t)message->type);
    PutU32(&writer, message->cost);
    PutU32(&writer, message->period);
    PutU32(&writer, message->deadline);
    PutU32(&writer, message->criticality);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolUnpackDeployTask(const Frame *frame, DeployTaskMessage *outMessage)
{
    PayloadReader reader;

    if (frame == NULL || outMessage == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    ProtocolStatus status = StartReading(frame, MessageTypeDeployTask, DeployTaskLength, &reader);
    if (status != ProtocolStatusOk) {
        return status;
    }
    uint32_t id = GetU32(&reader);
    uint8_t type = GetU8(&reader);
    if (!IsValidTaskType(type)) {
        return ProtocolStatusBadPayload;
    }
    outMessage->id = id;
    outMessage->type = (ProtocolTaskType)type;
    outMessage->cost = GetU32(&reader);
    outMessage->period = GetU32(&reader);
    outMessage->deadline = GetU32(&reader);
    outMessage->criticality = GetU32(&reader);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolPackEvent(const EventMessage *message, Frame *outFrame)
{
    if (message == NULL || outFrame == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    PayloadWriter writer = StartPayload(outFrame, MessageTypeEvent);
    PutU8(&writer, message->eventType);
    PutU32(&writer, message->tick);
    PutU32(&writer, message->taskId);
    PutU32(&writer, message->value);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolUnpackEvent(const Frame *frame, EventMessage *outMessage)
{
    PayloadReader reader;

    if (frame == NULL || outMessage == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    ProtocolStatus status = StartReading(frame, MessageTypeEvent, EventLength, &reader);
    if (status != ProtocolStatusOk) {
        return status;
    }
    outMessage->eventType = GetU8(&reader);
    outMessage->tick = GetU32(&reader);
    outMessage->taskId = GetU32(&reader);
    outMessage->value = GetU32(&reader);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolPackTickReport(const TickReportMessage *message, Frame *outFrame)
{
    if (message == NULL || outFrame == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    PayloadWriter writer = StartPayload(outFrame, MessageTypeTickReport);
    PutU32(&writer, message->tick);
    PutU32(&writer, message->runningTaskId);
    PutU32(&writer, message->droppedEvents);
    PutU32(&writer, message->missCount);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolUnpackTickReport(const Frame *frame, TickReportMessage *outMessage)
{
    PayloadReader reader;

    if (frame == NULL || outMessage == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    ProtocolStatus status = StartReading(frame, MessageTypeTickReport, TickReportLength, &reader);
    if (status != ProtocolStatusOk) {
        return status;
    }
    outMessage->tick = GetU32(&reader);
    outMessage->runningTaskId = GetU32(&reader);
    outMessage->droppedEvents = GetU32(&reader);
    outMessage->missCount = GetU32(&reader);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolPackSporadicTrigger(const SporadicTriggerMessage *message, Frame *outFrame)
{
    if (message == NULL || outFrame == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    PayloadWriter writer = StartPayload(outFrame, MessageTypeSporadicTrigger);
    PutU32(&writer, message->taskId);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolUnpackSporadicTrigger(const Frame *frame, SporadicTriggerMessage *outMessage)
{
    PayloadReader reader;

    if (frame == NULL || outMessage == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    ProtocolStatus status = StartReading(frame, MessageTypeSporadicTrigger, SporadicTriggerLength,
                                         &reader);
    if (status != ProtocolStatusOk) {
        return status;
    }
    outMessage->taskId = GetU32(&reader);
    return ProtocolStatusOk;
}

static int IsValidAckStatus(uint8_t value)
{
    return value <= AckStatusBadState;
}

ProtocolStatus ProtocolPackAck(const AckMessage *message, Frame *outFrame)
{
    if (message == NULL || outFrame == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    if (!IsValidAckStatus((uint8_t)message->status)) {
        return ProtocolStatusBadPayload;
    }
    PayloadWriter writer = StartPayload(outFrame, MessageTypeAck);
    PutU8(&writer, message->ackedSeq);
    PutU8(&writer, (uint8_t)message->status);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolUnpackAck(const Frame *frame, AckMessage *outMessage)
{
    PayloadReader reader;

    if (frame == NULL || outMessage == NULL) {
        return ProtocolStatusInvalidArgument;
    }
    ProtocolStatus status = StartReading(frame, MessageTypeAck, AckLength, &reader);
    if (status != ProtocolStatusOk) {
        return status;
    }
    uint8_t ackedSeq = GetU8(&reader);
    uint8_t ackStatus = GetU8(&reader);
    if (!IsValidAckStatus(ackStatus)) {
        return ProtocolStatusBadPayload;
    }
    outMessage->ackedSeq = ackedSeq;
    outMessage->status = (AckStatus)ackStatus;
    return ProtocolStatusOk;
}

static int IsEmptyType(MessageType type)
{
    return type == MessageTypeStart || type == MessageTypeStop;
}

ProtocolStatus ProtocolPackEmpty(MessageType type, Frame *outFrame)
{
    if (outFrame == NULL || !IsEmptyType(type)) {
        return ProtocolStatusInvalidArgument;
    }
    StartPayload(outFrame, type);
    return ProtocolStatusOk;
}

ProtocolStatus ProtocolUnpackEmpty(const Frame *frame, MessageType type)
{
    PayloadReader reader;

    if (frame == NULL || !IsEmptyType(type)) {
        return ProtocolStatusInvalidArgument;
    }
    return StartReading(frame, type, 0, &reader);
}

void FrameParserInit(FrameParser *parser)
{
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
    parser->state = FrameParserWaitStart;
}

void FrameParserReset(FrameParser *parser)
{
    if (parser == NULL) {
        return;
    }
    parser->state = FrameParserWaitStart;
    parser->received = 0;
}

static FrameParserResult DiscardFrame(FrameParser *parser)
{
    parser->badFrames++;
    FrameParserReset(parser);
    return FrameParserError;
}

static FrameParserState StateAfterLength(uint8_t length)
{
    return length == 0 ? FrameParserWaitCrcHigh : FrameParserWaitPayload;
}

static FrameParserResult FinishFrame(FrameParser *parser, uint8_t crcLow, Frame *outFrame)
{
    uint16_t received = (uint16_t)(((uint16_t)parser->crcHigh << 8) | crcLow);

    if (received != FrameCrc(&parser->frame)) {
        return DiscardFrame(parser);
    }
    *outFrame = parser->frame;
    FrameParserReset(parser);
    return FrameParserReady;
}

FrameParserResult FrameParserFeed(FrameParser *parser, uint8_t byte, Frame *outFrame)
{
    if (parser == NULL || outFrame == NULL) {
        return FrameParserError;
    }

    switch (parser->state) {
    case FrameParserWaitStart:
        if (byte == ProtocolStartOfFrame) {
            parser->state = FrameParserWaitType;
        }
        return FrameParserNone;

    case FrameParserWaitType:
        if (!IsKnownType(byte)) {
            return DiscardFrame(parser);
        }
        parser->frame.type = byte;
        parser->state = FrameParserWaitSeq;
        return FrameParserNone;

    case FrameParserWaitSeq:
        parser->frame.seq = byte;
        parser->state = FrameParserWaitLength;
        return FrameParserNone;

    case FrameParserWaitLength:
        if (byte > ProtocolMaxPayload) {
            return DiscardFrame(parser);
        }
        parser->frame.length = byte;
        parser->received = 0;
        parser->state = StateAfterLength(byte);
        return FrameParserNone;

    case FrameParserWaitPayload:
        parser->frame.payload[parser->received++] = byte;
        if (parser->received == parser->frame.length) {
            parser->state = FrameParserWaitCrcHigh;
        }
        return FrameParserNone;

    case FrameParserWaitCrcHigh:
        parser->crcHigh = byte;
        parser->state = FrameParserWaitCrcLow;
        return FrameParserNone;

    case FrameParserWaitCrcLow:
        return FinishFrame(parser, byte, outFrame);
    }

    return DiscardFrame(parser);
}
