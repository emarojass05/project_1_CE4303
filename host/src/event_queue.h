#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event_buffer.h"
#include "task.h"

/* Number of events the queue holds */
enum {
    EventQueueCapacity = 256
};

/* What a HostEvent carries. Drivers produce node events and tick reports;
   the planner produces the records the report needs about the run (spec
   3.10: the report gets all its data through this queue). */
typedef enum {
    HostEventNode = 0,     /* event: one Event frame of node nodeId */
    HostEventTickReport,   /* event.tick, event.taskId = running task,
                              event.value = misses; hostUs = arrival */
    HostEventRunInfo,      /* run: configuration of this run */
    HostEventCalibration,  /* value = sn of node nodeId, text = port */
    HostEventStarted,      /* hostUs = time the Ack of Start arrived */
    HostEventTaskAccepted, /* task placed on nodeId, value = cost, origin */
    HostEventTaskRejected, /* task not placed, reason (HostRejectReason), origin */
    HostEventNodeDown,     /* nodeId declared down, event.tick = last reported
                              tick, value = HostDownCause */
    HostEventLinkStats     /* link: counters of the driver of nodeId */
} HostEventKind;

/* How a task got to the planner */
typedef enum {
    HostOriginFile = 0, /* Automatic mode: the task file */
    HostOriginManual,   /* Manual mode: typed during the run */
    HostOriginRelocated /* Moved from a node that went down */
} HostOrigin;

/* Why a task was rejected; the first values match AssignRejectReason */
typedef enum {
    HostRejectInvalidTask = 1,
    HostRejectNoUsableNode,
    HostRejectSporadicLimit,
    HostRejectNodeFull,
    HostRejectNotSchedulable,
    HostRejectNodeRefused,    /* The node answered the DeployTask with an error */
    HostRejectDuplicateId,    /* Manual task with an id already in use */
    HostRejectSporadicOfDown, /* Sporadic task of a node that went down (spec 3.8) */
    HostRejectNoRoomAfterDown /* Task of a node that went down that fits nowhere */
} HostRejectReason;

typedef enum {
    HostDownMissedReports = 0, /* No tick report for missed_reports_for_failure periods */
    HostDownByCommand          /* Stopped with the k command, then detected the same way */
} HostDownCause;

enum {
    HostEventTextMax = 64
};

typedef struct {
    uint32_t algorithm; /* AssignAlgorithm */
    uint32_t heuristic; /* AssignHeuristic */
    uint32_t tickMs;
    uint32_t nodeCount;
    uint32_t tickReportEvery;
    uint32_t missedReports;
} HostRunInfo;

/* Driver counters at the end of the run (DriverStats in driver.h) */
typedef struct {
    uint32_t framesReceived;
    uint32_t badFrames; /* Corrupt frames discarded, including bad Event payloads */
    uint32_t eventsReceived;
    uint32_t tickReports;
    uint32_t retransmissions;
    uint32_t staleFrames;
    uint32_t droppedEvents; /* Lost in the node, from its last tick report */
    uint32_t missCount;     /* From the last tick report */
} HostLinkStats;

/* One element of the queue; kind says which fields are used */
typedef struct {
    uint32_t kind;   /* HostEventKind */
    uint32_t nodeId; /* 1-based, 0 when not tied to a node */
    Event event;
    Task task;
    uint32_t value;
    uint32_t origin; /* HostOrigin */
    uint32_t reason; /* HostRejectReason */
    uint64_t hostUs; /* CLOCK_MONOTONIC in microseconds */
    HostRunInfo run;
    HostLinkStats link;
    char text[HostEventTextMax];
} HostEvent;

/* Microseconds of CLOCK_MONOTONIC, the same clock in every host process */
uint64_t HostClockUs(void);

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