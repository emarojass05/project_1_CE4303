#ifndef NODE_LINK_H
#define NODE_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event_buffer.h"
#include "event_tx.h"
#include "protocol.h"
#include "scheduler.h"

/* Node side of the serial protocol (docs/protocolo.md), independent of the
   board. It parses the bytes that arrive from the host, applies each message
   to the scheduler following the node states of section 6, and queues the
   replies, the tick reports and the Event frames to send back. Nothing here
   blocks: the board feeds the bytes it read and writes what the port accepts. */

typedef enum {
    NodeLinkIdle = 0,
    NodeLinkCalibrated,
    NodeLinkDeploying,
    NodeLinkRunning
} NodeLinkState;

/* Which queue the bytes last returned by NodeLinkPending came from */
typedef enum {
    NodeLinkSendingNone = 0,
    NodeLinkSendingControl,
    NodeLinkSendingEvent
} NodeLinkSending;

enum {
    NodeLinkMinTickUs = 1000,    /* 1 ms */
    NodeLinkMaxTickUs = 1000000, /* 1 s, same limit as tick_ms in the host config */
    NodeLinkControlSlots = 4     /* Replies and tick reports waiting to be sent */
};

/* What the board does for the link */
typedef struct {
    /* Measures sn for a tick of tickUs with unidad_trabajo(); 0 on failure */
    uint32_t (*calibrate)(uint32_t tickUs, void *context);
    /* Restarts the hardware tick every tickUs from now, dropping pending ticks */
    bool (*startTicks)(uint32_t tickUs, void *context);
    /* Work of one tick, given to SchedulerSetWork after each DeployConfig */
    SchedulerWorkFn work;
    void *context; /* Passed to every hook */
} NodeLinkHooks;

typedef struct {
    uint8_t bytes[ProtocolMaxFrameSize];
    size_t length;
    bool afterEvents; /* Wait until every event is sent (Ack of Stop) */
} NodeLinkOutFrame;

typedef struct {
    Scheduler *scheduler;
    EventBuffer *events;
    NodeLinkHooks hooks;
    NodeLinkState state;
    uint32_t tickUs;          /* From the last CalibRequest */
    uint32_t unitsPerTick;    /* sn measured with that tick */
    uint16_t tickReportEvery; /* From DeployConfig; 0 = no tick reports */
    uint32_t sporadicTaskId;  /* Deployed sporadic task, 0 when none */
    FrameParser parser;
    EventTx eventTx;

    /* Last message applied and its reply, to answer a retransmission
       without applying it twice (docs/protocolo.md, section 7) */
    bool haveLast;
    Frame lastRequest;
    Frame lastReply;

    /* Replies and tick reports, sent before the events */
    NodeLinkOutFrame control[NodeLinkControlSlots];
    size_t controlHead;  /* Index of the oldest queued frame */
    size_t controlCount;
    size_t controlSent;  /* Bytes of the oldest frame already written */
    NodeLinkSending sending; /* Source of the frame being written; an Event
                                frame is always finished before anything else */
    uint8_t reportSeq;   /* Sequence number of the next TickReport */
    uint32_t droppedControl; /* Frames lost because the control queue was full */
} NodeLink;

/* Starts in Idle; scheduler and events belong to the caller */
void NodeLinkInit(NodeLink *link, Scheduler *scheduler, EventBuffer *events,
                  const NodeLinkHooks *hooks);

/* Feeds bytes read from the serial port and applies every complete frame */
void NodeLinkReceive(NodeLink *link, const uint8_t *bytes, size_t count);

/* Applies one frame that already passed the CRC check */
void NodeLinkHandleFrame(NodeLink *link, const Frame *frame);

/* True after Start until Stop: the main loop must call SchedulerTick */
bool NodeLinkIsRunning(const NodeLink *link);

/* Call after every SchedulerTick; queues a TickReport every N ticks */
void NodeLinkAfterTick(NodeLink *link);

/* Bytes to write next; 0 when there is nothing to send now */
size_t NodeLinkPending(NodeLink *link, const uint8_t **outBytes);

/* The board wrote count of the bytes returned by NodeLinkPending */
void NodeLinkConsumed(NodeLink *link, size_t count);

#endif /* NODE_LINK_H */
