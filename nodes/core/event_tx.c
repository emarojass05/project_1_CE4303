#include "event_tx.h"

void EventTxInit(EventTx *tx)
{
    if (tx == NULL) {
        return;
    }
    tx->length = 0;
    tx->sent = 0;
    tx->seq = 0;
}

/* Encodes the oldest event into tx->frame; returns 0 when the buffer is empty */
static int EncodeNext(EventTx *tx, EventBuffer *events)
{
    Event event;
    EventMessage message;
    Frame frame;

    while (EventBufferPop(events, &event)) {
        message.eventType = (uint8_t)event.type;
        message.tick = event.tick;
        message.taskId = event.taskId;
        message.value = event.value;
        if (ProtocolPackEvent(&message, &frame) != ProtocolStatusOk) {
            continue;
        }
        frame.seq = tx->seq;
        if (ProtocolEncodeFrame(&frame, tx->frame, sizeof(tx->frame), &tx->length) !=
            ProtocolStatusOk) {
            continue;
        }
        tx->seq++;
        tx->sent = 0;
        return 1;
    }
    return 0;
}

size_t EventTxPending(EventTx *tx, EventBuffer *events, const uint8_t **outBytes)
{
    if (tx == NULL || events == NULL || outBytes == NULL) {
        return 0;
    }
    if (tx->sent >= tx->length && !EncodeNext(tx, events)) {
        tx->length = 0;
        tx->sent = 0;
        return 0;
    }
    *outBytes = tx->frame + tx->sent;
    return tx->length - tx->sent;
}

void EventTxConsumed(EventTx *tx, size_t count)
{
    if (tx == NULL) {
        return;
    }
    tx->sent += count;
    if (tx->sent > tx->length) {
        tx->sent = tx->length;
    }
}
