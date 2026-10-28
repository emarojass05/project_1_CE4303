#ifndef DRIVER_H
#define DRIVER_H

#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>

#include "assign.h"
#include "config.h"
#include "event_queue.h"
#include "protocol.h"

/* Driver process of one node (F8, F9). It owns the serial port of the node:
   a reader thread parses every frame that arrives and pushes the events into
   the shared EventQueue, and the main thread runs the commands that the
   planner leaves in its DriverChannel, sending a frame and waiting for the
   reply with timeout and retries (docs/protocolo.md, section 7).

   The planner and the drivers share one ControlBlock in POSIX shared memory.
   Each channel is a mailbox guarded by two POSIX semaphores: the planner
   fills the command and posts commandReady; the driver runs it, fills the
   result and posts commandDone. Nobody busy waits. */

enum {
    DriverReplyTimeoutMs = 200, /* docs/protocolo.md, section 7 */
    DriverAttempts = 4          /* First try plus 3 retries */
};

typedef enum {
    DriverCommandSend = 0, /* Send request and wait for its reply */
    DriverCommandQuit      /* Close the port and end the process */
} DriverCommand;

typedef enum {
    DriverResultOk = 0,     /* reply holds the Ack or CalibResponse */
    DriverResultNoReply,    /* No reply after every attempt */
    DriverResultPortError   /* The port could not be opened or was lost */
} DriverResult;

/* Link counters, written by the driver and read by the planner */
typedef struct {
    uint32_t framesReceived;
    uint32_t badFrames;       /* Corrupt frames discarded (spec 3.6) */
    uint32_t eventsReceived;
    uint32_t badEvents;       /* Event frames with a wrong payload */
    uint32_t tickReports;
    uint32_t lastReportTick;
    uint32_t lastRunningTask;
    uint32_t missCount;       /* From the last TickReport */
    uint32_t droppedEvents;   /* From the last TickReport */
    uint32_t retransmissions; /* Requests sent again after a timeout */
    uint32_t staleFrames;     /* Events and tick reports outside a run, dropped */
    uint32_t portOpen;        /* Nonzero while the port works */
    uint64_t lastReportUs;    /* HostClockUs() when the last TickReport arrived */
} DriverStats;

typedef struct {
    sem_t commandReady;
    sem_t commandDone;
    sem_t statsMutex;
    uint32_t nodeId;
    char port[ConfigPortMaxLength];

    /* Command, written by the planner before posting commandReady */
    DriverCommand command;
    Frame request;       /* type, length and payload; the driver sets seq */
    uint32_t timeoutMs;  /* For each attempt */
    uint32_t attempts;

    /* Result, written by the driver before posting commandDone */
    DriverResult result;
    Frame reply;
    uint64_t replyUs; /* HostClockUs() when the command ended */

    DriverStats stats; /* Guarded by statsMutex */
} DriverChannel;

typedef struct {
    uint32_t channelCount;
    DriverChannel channels[AssignMaxNodes];
} ControlBlock;

/* Creates the shared memory called name with one channel per port; NULL on failure */
ControlBlock *ControlCreate(const char *name, const char ports[][ConfigPortMaxLength],
                            uint32_t portCount);

/* Frees the semaphores, unmaps and removes the shared memory */
void ControlDestroy(ControlBlock *control, const char *name);

/* Planner side: leaves a request for the driver, without waiting */
void DriverPost(DriverChannel *channel, const Frame *request, uint32_t timeoutMs,
                uint32_t attempts);

/* Planner side: waits for the result of the posted request */
DriverResult DriverWait(DriverChannel *channel, Frame *outReply);

/* Planner side: DriverPost followed by DriverWait */
DriverResult DriverRequest(DriverChannel *channel, const Frame *request, uint32_t timeoutMs,
                           uint32_t attempts, Frame *outReply);

/* Planner side: asks the driver to close its port and end */
void DriverPostQuit(DriverChannel *channel);

/* Planner side: copy of the counters */
DriverStats DriverReadStats(DriverChannel *channel);

/* Body of the driver process: opens the port of channel and serves commands
   until DriverCommandQuit. Events go to queue tagged with channel->nodeId.
   Returns 0 when it ended cleanly. */
int DriverRun(DriverChannel *channel, EventQueue *queue);

#endif /* DRIVER_H */
