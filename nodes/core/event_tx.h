#ifndef EVENT_TX_H
#define EVENT_TX_H

#include <stddef.h>
#include <stdint.h>

#include "event_buffer.h"
#include "protocol.h"

/* Sends the events of an EventBuffer as Event frames (docs/protocolo.md)
   without ever blocking the tick. The board asks for the pending bytes,
   writes as many as its serial port accepts right now, and reports how many
   it wrote; the rest waits for the next call. One frame is kept encoded at a
   time, so an event is never lost after it leaves the buffer. */
typedef struct {
    uint8_t frame[ProtocolMaxFrameSize];
    size_t length; /* Bytes of the encoded frame, 0 when there is none */
    size_t sent;   /* Bytes of the frame already written */
    uint8_t seq;   /* Sequence number of the next frame */
} EventTx;

void EventTxInit(EventTx *tx);

/* Bytes still to write, encoding the next event from events when needed.
   Returns 0 when there is nothing to send. */
size_t EventTxPending(EventTx *tx, EventBuffer *events, const uint8_t **outBytes);

/* The board wrote count of the bytes returned by EventTxPending */
void EventTxConsumed(EventTx *tx, size_t count);

#endif /* EVENT_TX_H */
