#include "event_buffer.h"

_Static_assert((EventBufferCapacity & (EventBufferCapacity - 1)) == 0,
               "EventBufferCapacity must be a power of two");

/* Buffer: empties it and clears the drop counter */
void EventBufferInit(EventBuffer *buffer)
{
    if (buffer == NULL) {
        return;
    }
    for (size_t i = 0; i < EventBufferCapacity; i++) {
        buffer->events[i].type = EventTypeRelease;
        buffer->events[i].tick = 0;
        buffer->events[i].taskId = 0;
        buffer->events[i].value = 0;
    }
    atomic_init(&buffer->head, 0u);
    atomic_init(&buffer->tail, 0u);
    atomic_init(&buffer->droppedCount, 0u);
}

/* Buffer: writes an event, drops it if full */
bool EventBufferPush(EventBuffer *buffer, Event event)
{
    if (buffer == NULL) {
        return false;
    }

    unsigned int head = atomic_load_explicit(&buffer->head, memory_order_relaxed);
    unsigned int tail = atomic_load_explicit(&buffer->tail, memory_order_acquire);

    if (head - tail >= (unsigned int)EventBufferCapacity) {
        unsigned int dropped = atomic_load_explicit(&buffer->droppedCount,
                                                    memory_order_relaxed);
        atomic_store_explicit(&buffer->droppedCount, dropped + 1u, memory_order_relaxed);
        return false;
    }

    buffer->events[head % EventBufferCapacity] = event;
    atomic_store_explicit(&buffer->head, head + 1u, memory_order_release);
    return true;
}

/* Buffer: reads the oldest event, false if empty */
bool EventBufferPop(EventBuffer *buffer, Event *outEvent)
{
    if (buffer == NULL || outEvent == NULL) {
        return false;
    }

    unsigned int tail = atomic_load_explicit(&buffer->tail, memory_order_relaxed);
    unsigned int head = atomic_load_explicit(&buffer->head, memory_order_acquire);

    if (head == tail) {
        return false;
    }

    *outEvent = buffer->events[tail % EventBufferCapacity];
    atomic_store_explicit(&buffer->tail, tail + 1u, memory_order_release);
    return true;
}

/* Buffer: number of stored events */
size_t EventBufferCount(const EventBuffer *buffer)
{
    if (buffer == NULL) {
        return 0;
    }
    unsigned int tail = atomic_load_explicit(&buffer->tail, memory_order_acquire);
    unsigned int head = atomic_load_explicit(&buffer->head, memory_order_acquire);
    return (size_t)(head - tail);
}

/* Buffer: events lost because it was full */
uint32_t EventBufferDroppedCount(const EventBuffer *buffer)
{
    if (buffer == NULL) {
        return 0;
    }
    return atomic_load_explicit(&buffer->droppedCount, memory_order_relaxed);
}
