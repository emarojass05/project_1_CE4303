#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Serial protocol shared by the host and the nodes. The full specification,
   with the byte layout of every message, is in docs/protocolo.md.

   Frame on the wire:
     | 0x7E | type | seq | length | payload[length] | crc high | crc low |
   The CRC-16 (common/crc16.h) covers type, seq, length and payload.
   Multi-byte payload fields are little-endian. */

enum {
    ProtocolStartOfFrame = 0x7E,
    ProtocolMaxPayload = 32,
    ProtocolHeaderSize = 4, /* start, type, seq, length */
    ProtocolCrcSize = 2,
    ProtocolMaxFrameSize = ProtocolHeaderSize + ProtocolMaxPayload + ProtocolCrcSize
};

typedef enum {
    MessageTypeCalibRequest = 0x01,  /* Host to node */
    MessageTypeCalibResponse = 0x02, /* Node to host */
    MessageTypeDeployConfig = 0x03,  /* Host to node */
    MessageTypeDeployTask = 0x04,    /* Host to node */
    MessageTypeStart = 0x05,         /* Host to node, sets tick zero */
    MessageTypeStop = 0x06,          /* Host to node */
    MessageTypeEvent = 0x07,         /* Node to host */
    MessageTypeTickReport = 0x08,    /* Node to host */
    MessageTypeSporadicTrigger = 0x09, /* Host to node */
    MessageTypeAck = 0x0A            /* Both directions */
} MessageType;

/* Same numbering as SchedulerAlgorithm in nodes/core/scheduler.h */
typedef enum {
    ProtocolAlgorithmRms = 0,
    ProtocolAlgorithmEdf = 1
} ProtocolAlgorithm;

/* Same numbering as TaskType in task.h */
typedef enum {
    ProtocolTaskPeriodic = 0,
    ProtocolTaskSporadic = 1
} ProtocolTaskType;

typedef enum {
    AckStatusOk = 0,
    AckStatusBadPayload = 1, /* Frame was valid but its content was not */
    AckStatusFull = 2,       /* Node cannot hold another task */
    AckStatusBadState = 3    /* Message not allowed right now, e.g. deploy after start */
} AckStatus;

typedef enum {
    ProtocolStatusOk = 0,
    ProtocolStatusInvalidArgument = -1, /* Null pointer or unexpected message type */
    ProtocolStatusBufferTooSmall = -2,  /* Output buffer cannot hold the frame */
    ProtocolStatusBadPayload = -3       /* Wrong length or field out of range */
} ProtocolStatus;

/* A decoded frame. seq is chosen by the sender; an Ack repeats it. */
typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t length;
    uint8_t payload[ProtocolMaxPayload];
} Frame;

typedef struct {
    uint32_t tickUs;
} CalibRequestMessage;

typedef struct {
    uint32_t unitsPerTick; /* Measured sn */
} CalibResponseMessage;

typedef struct {
    ProtocolAlgorithm algorithm;
    uint16_t tickReportEvery; /* Send a TickReport every this many ticks; 0 = never */
} DeployConfigMessage;

typedef struct {
    uint32_t id;
    ProtocolTaskType type;
    uint32_t cost; /* C(i,n) in ticks, computed by the host */
    uint32_t period;
    uint32_t deadline;
    uint32_t criticality;
} DeployTaskMessage;

typedef struct {
    uint8_t eventType; /* EventType value from nodes/core/event_buffer.h */
    uint32_t tick;
    uint32_t taskId;
    uint32_t value;
} EventMessage;

typedef struct {
    uint32_t tick;
    uint32_t runningTaskId; /* 0 when idle */
    uint32_t droppedEvents;
    uint32_t missCount;
} TickReportMessage;

typedef struct {
    uint32_t taskId;
} SporadicTriggerMessage;

typedef struct {
    uint8_t ackedSeq;
    AckStatus status;
} AckMessage;

/* Writes frame to out as wire bytes and stores the byte count in outLength */
ProtocolStatus ProtocolEncodeFrame(const Frame *frame, uint8_t *out, size_t outCapacity,
                                   size_t *outLength);

/* Pack functions fill type, length and payload of outFrame; the caller sets seq.
   Unpack functions check the type and the exact payload length. */
ProtocolStatus ProtocolPackCalibRequest(const CalibRequestMessage *message, Frame *outFrame);
ProtocolStatus ProtocolUnpackCalibRequest(const Frame *frame, CalibRequestMessage *outMessage);

ProtocolStatus ProtocolPackCalibResponse(const CalibResponseMessage *message, Frame *outFrame);
ProtocolStatus ProtocolUnpackCalibResponse(const Frame *frame, CalibResponseMessage *outMessage);

ProtocolStatus ProtocolPackDeployConfig(const DeployConfigMessage *message, Frame *outFrame);
ProtocolStatus ProtocolUnpackDeployConfig(const Frame *frame, DeployConfigMessage *outMessage);

ProtocolStatus ProtocolPackDeployTask(const DeployTaskMessage *message, Frame *outFrame);
ProtocolStatus ProtocolUnpackDeployTask(const Frame *frame, DeployTaskMessage *outMessage);

ProtocolStatus ProtocolPackEvent(const EventMessage *message, Frame *outFrame);
ProtocolStatus ProtocolUnpackEvent(const Frame *frame, EventMessage *outMessage);

ProtocolStatus ProtocolPackTickReport(const TickReportMessage *message, Frame *outFrame);
ProtocolStatus ProtocolUnpackTickReport(const Frame *frame, TickReportMessage *outMessage);

ProtocolStatus ProtocolPackSporadicTrigger(const SporadicTriggerMessage *message, Frame *outFrame);
ProtocolStatus ProtocolUnpackSporadicTrigger(const Frame *frame, SporadicTriggerMessage *outMessage);

ProtocolStatus ProtocolPackAck(const AckMessage *message, Frame *outFrame);
ProtocolStatus ProtocolUnpackAck(const Frame *frame, AckMessage *outMessage);

/* Start and Stop carry no payload; type must be MessageTypeStart or MessageTypeStop */
ProtocolStatus ProtocolPackEmpty(MessageType type, Frame *outFrame);
ProtocolStatus ProtocolUnpackEmpty(const Frame *frame, MessageType type);

typedef enum {
    FrameParserNone = 0, /* Byte consumed, no complete frame yet */
    FrameParserReady,    /* A valid frame was written to the output */
    FrameParserError     /* A started frame was discarded and counted */
} FrameParserResult;

typedef enum {
    FrameParserWaitStart = 0,
    FrameParserWaitType,
    FrameParserWaitSeq,
    FrameParserWaitLength,
    FrameParserWaitPayload,
    FrameParserWaitCrcHigh,
    FrameParserWaitCrcLow
} FrameParserState;

/* Byte-at-a-time receiver. Bytes before a start marker are skipped silently;
   a frame with an unknown type, a length above ProtocolMaxPayload or a wrong
   CRC is discarded, counted in badFrames, and the search for 0x7E restarts. */
typedef struct {
    FrameParserState state;
    Frame frame;
    uint8_t received; /* Payload bytes stored so far */
    uint8_t crcHigh;
    uint32_t badFrames;
} FrameParser;

void FrameParserInit(FrameParser *parser);

/* Drops a half-received frame without counting it, e.g. after a read timeout */
void FrameParserReset(FrameParser *parser);

FrameParserResult FrameParserFeed(FrameParser *parser, uint8_t byte, Frame *outFrame);

#endif /* PROTOCOL_H */
