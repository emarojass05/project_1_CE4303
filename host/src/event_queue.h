#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event_buffer.h"

/* Number of events the queue holds */
enum {
    EventQueueCapacity = 256
};

/* Event together with the node that produced it */
typedef struct {
    uint32_t nodeId;
    Event event;
} HostEvent;

/* Bounded queue kept in POSIX shared memory; many producers, one consumer */
typedef struct {
    sem_t mutex;     /* Lets one process at a time change head, tail and events */
    sem_t freeSlots; /* Counts empty slots; producers wait on it */
    sem_t usedSlots; /* Counts filled slots; the consumer waits on it */
    uint32_t head;   /* Events written so far */
    uint32_t tail;   /* Events read so far */
    uint32_t closed; /* Nonzero after EventQueueShutdown */
    HostEvent events[EventQueueCapacity];
} EventQueue;

/* Creates the shared memory called name and returns the queue, or NULL on failure */
EventQueue *EventQueueCreate(const char *name);

/* Maps an existing queue called name, or returns NULL on failure */
EventQueue *EventQueueOpen(const char *name);

/* Stops using the queue in this process */
void EventQueueClose(EventQueue *queue);

/* Creator only: frees the semaphores and removes the shared memory */
void EventQueueDestroy(EventQueue *queue, const char *name);

/* Waits for a free slot and appends; returns false if the queue is closed */
bool EventQueuePush(EventQueue *queue, const HostEvent *event);

/* Waits for an event and removes it; returns false when closed and empty */
bool EventQueuePop(EventQueue *queue, HostEvent *outEvent);

/* Marks the queue closed and wakes every waiting process */
void EventQueueShutdown(EventQueue *queue);

/* Number of events currently stored */
size_t EventQueueCount(EventQueue *queue);

#endif /* EVENT_QUEUE_H */