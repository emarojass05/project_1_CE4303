#include "driver.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "serial_port.h"

/* State shared by the two threads of one driver process */
typedef struct {
    DriverChannel *channel;
    EventQueue *queue;
    int fd;
    int quitPipe[2];   /* The main thread writes it to stop the reader */
    FrameParser parser; /* Used by the reader thread only */
    uint8_t nextSeq;
    /* True from sending Start until the Ack of Stop: events and tick reports
       outside a run are leftovers of an earlier run and are dropped */
    atomic_bool collecting;

    /* Reply mailbox between the reader and the main thread */
    pthread_mutex_t mutex;
    pthread_cond_t replied;
    bool waiting;
    uint8_t waitingSeq;
    bool haveReply;
    Frame reply;
    uint64_t replyUs; /* Arrival of the reply */
} Driver;

static bool WaitSemaphore(sem_t *semaphore)
{
    while (sem_wait(semaphore) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

ControlBlock *ControlCreate(const char *name, const char ports[][ConfigPortMaxLength],
                            uint32_t portCount)
{
    ControlBlock *control;
    void *memory;
    uint32_t i;
    int fd;

    if (name == NULL || ports == NULL || portCount == 0 || portCount > AssignMaxNodes) {
        return NULL;
    }
    (void)shm_unlink(name); /* Leftover from a crashed run */
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, (off_t)sizeof(ControlBlock)) != 0) {
        close(fd);
        shm_unlink(name);
        return NULL;
    }
    memory = mmap(NULL, sizeof(ControlBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (memory == MAP_FAILED) {
        shm_unlink(name);
        return NULL;
    }
    control = memory;
    memset(control, 0, sizeof(*control));

    for (i = 0; i < portCount; i++) {
        DriverChannel *channel = &control->channels[i];

        if (sem_init(&channel->commandReady, 1, 0) != 0 ||
            sem_init(&channel->commandDone, 1, 0) != 0 ||
            sem_init(&channel->statsMutex, 1, 1) != 0) {
            control->channelCount = i; /* Only the complete channels are destroyed */
            ControlDestroy(control, name);
            return NULL;
        }
        channel->nodeId = i + 1;
        snprintf(channel->port, sizeof(channel->port), "%s", ports[i]);
    }
    control->channelCount = portCount;
    return control;
}

void ControlDestroy(ControlBlock *control, const char *name)
{
    uint32_t i;

    if (control != NULL) {
        for (i = 0; i < control->channelCount; i++) {
            sem_destroy(&control->channels[i].commandReady);
            sem_destroy(&control->channels[i].commandDone);
            sem_destroy(&control->channels[i].statsMutex);
        }
        munmap(control, sizeof(ControlBlock));
    }
    if (name != NULL) {
        shm_unlink(name);
    }
}

void DriverPost(DriverChannel *channel, const Frame *request, uint32_t timeoutMs,
                uint32_t attempts)
{
    if (channel == NULL || request == NULL) {
        return;
    }
    channel->command = DriverCommandSend;
    channel->request = *request;
    channel->timeoutMs = timeoutMs;
    channel->attempts = attempts == 0 ? 1 : attempts;
    sem_post(&channel->commandReady);
}

DriverResult DriverWait(DriverChannel *channel, Frame *outReply)
{
    if (channel == NULL || !WaitSemaphore(&channel->commandDone)) {
        return DriverResultPortError;
    }
    if (outReply != NULL) {
        *outReply = channel->reply;
    }
    return channel->result;
}

DriverResult DriverRequest(DriverChannel *channel, const Frame *request, uint32_t timeoutMs,
                           uint32_t attempts, Frame *outReply)
{
    DriverPost(channel, request, timeoutMs, attempts);
    return DriverWait(channel, outReply);
}

void DriverPostQuit(DriverChannel *channel)
{
    if (channel == NULL) {
        return;
    }
    channel->command = DriverCommandQuit;
    sem_post(&channel->commandReady);
}

DriverStats DriverReadStats(DriverChannel *channel)
{
    DriverStats stats;

    memset(&stats, 0, sizeof(stats));
    if (channel == NULL || !WaitSemaphore(&channel->statsMutex)) {
        return stats;
    }
    stats = channel->stats;
    sem_post(&channel->statsMutex);
    return stats;
}

/* ---- Reader thread ---- */

static void LockStats(Driver *driver)
{
    (void)WaitSemaphore(&driver->channel->statsMutex);
}

static void UnlockStats(Driver *driver)
{
    sem_post(&driver->channel->statsMutex);
}

static void HandleEvent(Driver *driver, const Frame *frame)
{
    EventMessage message;
    HostEvent hostEvent;

    if (ProtocolUnpackEvent(frame, &message) != ProtocolStatusOk) {
        LockStats(driver);
        driver->channel->stats.badEvents++;
        UnlockStats(driver);
        return;
    }
    LockStats(driver);
    driver->channel->stats.eventsReceived++;
    UnlockStats(driver);

    /* An unknown eventType reaches the report, which counts it as ignored */
    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventNode;
    hostEvent.nodeId = driver->channel->nodeId;
    hostEvent.event.type = (EventType)message.eventType;
    hostEvent.event.tick = message.tick;
    hostEvent.event.taskId = message.taskId;
    hostEvent.event.value = message.value;
    (void)EventQueuePush(driver->queue, &hostEvent); /* Waits while the queue is full */
}

static void HandleTickReport(Driver *driver, const Frame *frame)
{
    TickReportMessage report;
    HostEvent hostEvent;
    uint64_t arrival = HostClockUs();

    if (ProtocolUnpackTickReport(frame, &report) != ProtocolStatusOk) {
        LockStats(driver);
        driver->channel->stats.badEvents++;
        UnlockStats(driver);
        return;
    }
    LockStats(driver);
    driver->channel->stats.tickReports++;
    driver->channel->stats.lastReportTick = report.tick;
    driver->channel->stats.lastRunningTask = report.runningTaskId;
    driver->channel->stats.missCount = report.missCount;
    driver->channel->stats.droppedEvents = report.droppedEvents;
    driver->channel->stats.lastReportUs = arrival;
    UnlockStats(driver);

    /* The report measures the skew between nodes from the arrival times (F12) */
    memset(&hostEvent, 0, sizeof(hostEvent));
    hostEvent.kind = HostEventTickReport;
    hostEvent.nodeId = driver->channel->nodeId;
    hostEvent.event.tick = report.tick;
    hostEvent.event.taskId = report.runningTaskId;
    hostEvent.event.value = report.missCount;
    hostEvent.hostUs = arrival;
    (void)EventQueuePush(driver->queue, &hostEvent);
}

/* Hands an Ack or CalibResponse to the main thread if it is the awaited one */
static void HandleReply(Driver *driver, const Frame *frame)
{
    pthread_mutex_lock(&driver->mutex);
    if (driver->waiting && !driver->haveReply && frame->seq == driver->waitingSeq) {
        driver->reply = *frame;
        driver->replyUs = HostClockUs();
        driver->haveReply = true;
        pthread_cond_signal(&driver->replied);
    }
    pthread_mutex_unlock(&driver->mutex);
}

static void HandleFrame(Driver *driver, const Frame *frame)
{
    LockStats(driver);
    driver->channel->stats.framesReceived++;
    UnlockStats(driver);

    switch (frame->type) {
    case MessageTypeEvent:
    case MessageTypeTickReport:
        if (!atomic_load(&driver->collecting)) {
            LockStats(driver);
            driver->channel->stats.staleFrames++;
            UnlockStats(driver);
        } else if (frame->type == MessageTypeEvent) {
            HandleEvent(driver, frame);
        } else {
            HandleTickReport(driver, frame);
        }
        break;
    case MessageTypeAck:
    case MessageTypeCalibResponse:
        HandleReply(driver, frame);
        break;
    default:
        break; /* Host to node types are not expected here */
    }
}

static void MarkPortLost(Driver *driver)
{
    LockStats(driver);
    driver->channel->stats.portOpen = 0;
    UnlockStats(driver);
}

/* Sleeps in poll() until bytes arrive or the main thread asks to stop */
static void *ReaderMain(void *argument)
{
    Driver *driver = argument;
    struct pollfd fds[2];
    uint8_t bytes[256];
    ssize_t count;
    ssize_t i;
    Frame frame;
    int timeout;
    int ready;

    fds[0].fd = driver->fd;
    fds[0].events = POLLIN;
    fds[1].fd = driver->quitPipe[0];
    fds[1].events = POLLIN;

    for (;;) {
        /* A frame cut in half (cable pulled) is dropped after the reply timeout */
        timeout = driver->parser.state == FrameParserWaitStart ? -1 : DriverReplyTimeoutMs;
        fds[0].revents = 0;
        fds[1].revents = 0;
        ready = poll(fds, 2, timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (fds[1].revents != 0) {
            break;
        }
        if (ready == 0) {
            FrameParserReset(&driver->parser);
            continue;
        }
        if (fds[0].revents & (POLLERR | POLLNVAL)) {
            break;
        }
        count = read(driver->fd, bytes, sizeof(bytes));
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        if (count <= 0) {
            break; /* Device unplugged */
        }
        for (i = 0; i < count; i++) {
            if (FrameParserFeed(&driver->parser, bytes[i], &frame) == FrameParserReady) {
                HandleFrame(driver, &frame);
            }
        }
        LockStats(driver);
        driver->channel->stats.badFrames = driver->parser.badFrames;
        UnlockStats(driver);
    }

    if (fds[1].revents == 0) {
        MarkPortLost(driver);
    }
    return NULL;
}

/* ---- Main thread ---- */

static bool PortWorks(Driver *driver)
{
    bool open;

    LockStats(driver);
    open = driver->channel->stats.portOpen != 0;
    UnlockStats(driver);
    return open;
}

static struct timespec DeadlineAfter(uint32_t milliseconds)
{
    struct timespec deadline;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += milliseconds / 1000u;
    deadline.tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

/* Sends the request with a new seq and waits for its reply, resending the
   same frame (same seq) after each timeout */
static DriverResult SendRequest(Driver *driver, Frame request, uint32_t timeoutMs,
                                uint32_t attempts, Frame *outReply)
{
    uint8_t bytes[ProtocolMaxFrameSize];
    size_t length = 0;
    struct timespec deadline;
    uint32_t attempt;
    bool got = false;

    request.seq = driver->nextSeq++;
    if (ProtocolEncodeFrame(&request, bytes, sizeof(bytes), &length) != ProtocolStatusOk) {
        return DriverResultNoReply;
    }

    for (attempt = 0; attempt < attempts && !got; attempt++) {
        if (!PortWorks(driver)) {
            return DriverResultPortError;
        }
        if (attempt > 0) {
            LockStats(driver);
            driver->channel->stats.retransmissions++;
            UnlockStats(driver);
        }

        pthread_mutex_lock(&driver->mutex);
        driver->waiting = true;
        driver->waitingSeq = request.seq;
        driver->haveReply = false;
        pthread_mutex_unlock(&driver->mutex);

        if (!SerialWriteAll(driver->fd, bytes, length, (int)timeoutMs)) {
            pthread_mutex_lock(&driver->mutex);
            driver->waiting = false;
            pthread_mutex_unlock(&driver->mutex);
            continue;
        }

        deadline = DeadlineAfter(timeoutMs);
        pthread_mutex_lock(&driver->mutex);
        while (!driver->haveReply) {
            if (pthread_cond_timedwait(&driver->replied, &driver->mutex, &deadline) == ETIMEDOUT) {
                break;
            }
        }
        got = driver->haveReply;
        if (got) {
            *outReply = driver->reply;
        }
        driver->waiting = false;
        pthread_mutex_unlock(&driver->mutex);
    }
    return got ? DriverResultOk : DriverResultNoReply;
}

static bool InitSync(Driver *driver)
{
    pthread_condattr_t attributes;
    bool ok;

    if (pthread_condattr_init(&attributes) != 0) {
        return false;
    }
    ok = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0 &&
         pthread_cond_init(&driver->replied, &attributes) == 0;
    pthread_condattr_destroy(&attributes);
    if (!ok) {
        return false;
    }
    if (pthread_mutex_init(&driver->mutex, NULL) != 0) {
        pthread_cond_destroy(&driver->replied);
        return false;
    }
    return true;
}

/* Answers every command with a port error until Quit */
static void ServeWithoutPort(DriverChannel *channel)
{
    for (;;) {
        if (!WaitSemaphore(&channel->commandReady) || channel->command == DriverCommandQuit) {
            return;
        }
        channel->result = DriverResultPortError;
        sem_post(&channel->commandDone);
    }
}

int DriverRun(DriverChannel *channel, EventQueue *queue)
{
    Driver driver;
    pthread_t reader;
    char byte = 'q';

    if (channel == NULL || queue == NULL) {
        return 1;
    }
    memset(&driver, 0, sizeof(driver));
    driver.channel = channel;
    driver.queue = queue;
    /* A seq that changes between runs, so a node never takes the first frame
       of a new run for a retransmission of the last one */
    driver.nextSeq = (uint8_t)(time(NULL) ^ getpid());
    atomic_init(&driver.collecting, false);
    FrameParserInit(&driver.parser);

    driver.fd = SerialOpen(channel->port);
    LockStats(&driver);
    channel->stats.portOpen = driver.fd >= 0;
    UnlockStats(&driver);
    if (driver.fd < 0) {
        ServeWithoutPort(channel);
        return 0;
    }
    if (pipe(driver.quitPipe) != 0) {
        SerialClose(driver.fd);
        ServeWithoutPort(channel);
        return 1;
    }
    if (!InitSync(&driver)) {
        close(driver.quitPipe[0]);
        close(driver.quitPipe[1]);
        SerialClose(driver.fd);
        ServeWithoutPort(channel);
        return 1;
    }
    if (pthread_create(&reader, NULL, ReaderMain, &driver) != 0) {
        MarkPortLost(&driver);
        ServeWithoutPort(channel);
        pthread_mutex_destroy(&driver.mutex);
        pthread_cond_destroy(&driver.replied);
        close(driver.quitPipe[0]);
        close(driver.quitPipe[1]);
        SerialClose(driver.fd);
        return 1;
    }

    for (;;) {
        if (!WaitSemaphore(&channel->commandReady) || channel->command == DriverCommandQuit) {
            break;
        }
        if (channel->request.type == MessageTypeStart) {
            atomic_store(&driver.collecting, true);
        }
        channel->result = SendRequest(&driver, channel->request, channel->timeoutMs,
                                      channel->attempts, &channel->reply);
        channel->replyUs = channel->result == DriverResultOk ? driver.replyUs : HostClockUs();
        if (channel->request.type == MessageTypeStop && channel->result == DriverResultOk) {
            /* The node sent its last events before this Ack */
            atomic_store(&driver.collecting, false);
        }
        sem_post(&channel->commandDone);
    }

    (void)write(driver.quitPipe[1], &byte, 1);
    pthread_join(reader, NULL);
    pthread_mutex_destroy(&driver.mutex);
    pthread_cond_destroy(&driver.replied);
    close(driver.quitPipe[0]);
    close(driver.quitPipe[1]);
    SerialClose(driver.fd);
    return 0;
}
