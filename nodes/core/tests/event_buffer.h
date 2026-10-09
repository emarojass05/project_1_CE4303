#ifndef EVENT_BUFFER_H
#define EVENT_BUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Number of events the buffer holds; must be a power of two */
enum {
    EventBufferCapacity = 64
};

/* Provisional event kinds until the serial protocol is fixed */
typedef enum {
    EventTypeRelease = 0,
    EventTypeStart,
    EventTypePreempt,
    EventTypeComplete,
    EventTypeDeadlineMiss,
    EventTypeAbort,
    EventTypeSporadicRelease,
    EventTypeSporadicIgnored
} EventType;

typedef struct {
    EventType type;
    uint32_t tick;   /* Ticks since the start message */
    uint32_t taskId;
    uint32_t value;  /* Extra data, for example a latency */
} Event;

/* Single producer, single consumer ring buffer */
typedef struct {
    Event events[EventBufferCapacity];
    atomic_uint head;         /* Events written so far; producer only */
    atomic_uint tail;         /* Events read so far; consumer only */
    atomic_uint droppedCount; /* Events lost because the buffer was full */
} EventBuffer;

/* Empties the buffer and clears the dropped counter */
void EventBufferInit(EventBuffer *buffer);

/* Appends an event; returns false and counts a drop when full */
bool EventBufferPush(EventBuffer *buffer, Event event);

/* Removes the oldest event; returns false when empty */
bool EventBufferPop(EventBuffer *buffer, Event *outEvent);

/* Number of events currently stored */
size_t EventBufferCount(const EventBuffer *buffer);

/* Number of events dropped since initialization */
uint32_t EventBufferDroppedCount(const EventBuffer *buffer);

#endif /* EVENT_BUFFER_H */