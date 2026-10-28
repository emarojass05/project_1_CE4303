#include "host_demo.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "event_queue.h"
#include "report.h"

/* Node that the test producer pretends to be */
enum {
    DemoNodeId = 1
};

typedef struct {
    EventType type;
    uint32_t taskId;
    uint32_t value;
} DemoStep;

/* One pass of fake events: two tasks and one sporadic task */
static const DemoStep demoSteps[] = {
    { EventTypeRelease, 1, 0 },
    { EventTypeRelease, 2, 0 },
    { EventTypeStart, 1, 0 },
    { EventTypeComplete, 1, 2 },
    { EventTypeStart, 2, 0 },
    { EventTypeRelease, 1, 0 },
    { EventTypePreempt, 2, 1 },
    { EventTypeStart, 1, 0 },
    { EventTypeComplete, 1, 3 },
    { EventTypeStart, 2, 0 },
    { EventTypeDeadlineMiss, 2, 1 },
    { EventTypeAbort, 2, 0 },
    { EventTypeSporadicRelease, 9, 0 },
    { EventTypeSporadicIgnored, 9, 0 }
};

/* Write end of the pipe that SIGCHLD uses to wake the main process */
static int wakeWriteFd = -1;

/* Demo: SIGCHLD handler, wakes the main loop */
static void OnChildExit(int signalNumber)
{
    int savedErrno = errno;
    char byte = 'c';

    (void)signalNumber;
    (void)write(wakeWriteFd, &byte, 1);
    errno = savedErrno;
}

/* Demo: sleeps without busy waiting */
static void SleepMilliseconds(long milliseconds)
{
    struct timespec pause;

    pause.tv_sec = milliseconds / 1000;
    pause.tv_nsec = (milliseconds % 1000) * 1000000L;
    nanosleep(&pause, NULL);
}

/* Demo: report process, counts until queue closes */
static int RunReporter(EventQueue *queue, FILE *out)
{
    static Report report;
    HostEvent hostEvent;

    ReportInit(&report);
    while (EventQueuePop(queue, &hostEvent)) {
        (void)ReportRecord(&report, &hostEvent);
    }
    ReportPrint(&report, out);
    fflush(out);
    ReportFree(&report);
    return 0;
}

/* Demo: producer process, plays fake events */
static int RunProducer(EventQueue *queue, uint32_t passes, long delayMilliseconds)
{
    uint32_t sequence = 0;
    uint32_t pass;
    size_t step;

    for (pass = 0; pass < passes; pass++) {
        for (step = 0; step < sizeof(demoSteps) / sizeof(demoSteps[0]); step++) {
            HostEvent hostEvent;

            memset(&hostEvent, 0, sizeof(hostEvent));
            hostEvent.kind = HostEventNode;
            hostEvent.nodeId = DemoNodeId;
            hostEvent.event.type = demoSteps[step].type;
            hostEvent.event.tick = sequence++;
            hostEvent.event.taskId = demoSteps[step].taskId;
            hostEvent.event.value = demoSteps[step].value;
            if (!EventQueuePush(queue, &hostEvent)) {
                return 0;
            }
            if (delayMilliseconds > 0) {
                SleepMilliseconds(delayMilliseconds);
            }
        }
    }
    return 0;
}

/* Demo: sleeps until q, EOF or child exit (poll) */
static void WaitForStop(int inputFd, int wakeReadFd)
{
    struct pollfd fds[2];
    char buffer[64];
    ssize_t count;

    fds[0].fd = inputFd;
    fds[0].events = POLLIN;
    fds[1].fd = wakeReadFd;
    fds[1].events = POLLIN;

    for (;;) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (fds[1].revents & POLLIN) {
            return;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            count = read(inputFd, buffer, sizeof(buffer));
            if (count <= 0 || memchr(buffer, 'q', (size_t)count) != NULL) {
                return;
            }
        }
    }
}

/* Demo: waits for a child, 0 if clean */
static int WaitForChild(pid_t child)
{
    int status;

    if (child <= 0) {
        return 1;
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status);
}

/* Demo: creates queue, forks, waits, shuts down clean */
int HostRunDemo(const char *queueName, int inputFd, FILE *out,
                uint32_t passes, long eventDelayMilliseconds)
{
    struct sigaction action;
    struct sigaction previous;
    EventQueue *queue;
    pid_t reporter;
    pid_t producer = -1;
    int wakePipe[2];
    int result = 0;

    if (queueName == NULL || out == NULL) {
        return 1;
    }
    queue = EventQueueCreate(queueName);
    if (queue == NULL) {
        return 1;
    }
    if (pipe(wakePipe) != 0) {
        EventQueueDestroy(queue, queueName);
        return 1;
    }
    fcntl(wakePipe[1], F_SETFL, O_NONBLOCK);
    wakeWriteFd = wakePipe[1];

    memset(&action, 0, sizeof(action));
    action.sa_handler = OnChildExit;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &action, &previous);

    fflush(out);
    fflush(stdout);
    reporter = fork();
    if (reporter == 0) {
        _exit(RunReporter(queue, out));
    }
    producer = (reporter > 0) ? fork() : -1;
    if (producer == 0) {
        _exit(RunProducer(queue, passes, eventDelayMilliseconds));
    }

    if (reporter > 0 && producer > 0) {
        WaitForStop(inputFd, wakePipe[0]);
    } else {
        result = 1;
    }

    /* Closing the queue stops the producer and lets the reporter finish */
    EventQueueShutdown(queue);
    result |= WaitForChild(producer);
    result |= WaitForChild(reporter);

    sigaction(SIGCHLD, &previous, NULL);
    wakeWriteFd = -1;
    close(wakePipe[0]);
    close(wakePipe[1]);
    EventQueueDestroy(queue, queueName);
    return result;
}