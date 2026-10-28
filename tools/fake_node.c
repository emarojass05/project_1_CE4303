#define _XOPEN_SOURCE 700 /* posix_openpt, grantpt, unlockpt, ptsname */

#include "fake_node.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "node_link.h"

typedef struct {
    uint32_t unitsPerTick;
    bool ticking;
    uint32_t tickUs;
    struct timespec nextTick;
} FakeBoard;

static void AddMicros(struct timespec *time, uint32_t micros)
{
    time->tv_nsec += (long)micros * 1000L;
    while (time->tv_nsec >= 1000000000L) {
        time->tv_sec++;
        time->tv_nsec -= 1000000000L;
    }
}

/* Milliseconds from now until time, rounded up; 0 when already due */
static int MillisUntil(const struct timespec *time)
{
    struct timespec now;
    long long nanos;

    clock_gettime(CLOCK_MONOTONIC, &now);
    nanos = (long long)(time->tv_sec - now.tv_sec) * 1000000000LL + (time->tv_nsec - now.tv_nsec);
    if (nanos <= 0) {
        return 0;
    }
    return (int)((nanos + 999999LL) / 1000000LL);
}

static uint32_t FakeCalibrate(uint32_t tickUs, void *context)
{
    FakeBoard *board = context;

    (void)tickUs;
    return board->unitsPerTick;
}

static bool FakeStartTicks(uint32_t tickUs, void *context)
{
    FakeBoard *board = context;

    board->tickUs = tickUs;
    board->ticking = true;
    clock_gettime(CLOCK_MONOTONIC, &board->nextTick);
    AddMicros(&board->nextTick, tickUs);
    return true;
}

static void MakeRaw(int fd)
{
    struct termios options;

    if (tcgetattr(fd, &options) != 0) {
        return;
    }
    options.c_iflag = 0;
    options.c_oflag = 0;
    options.c_lflag = 0;
    options.c_cflag = (options.c_cflag & ~(tcflag_t)(CSIZE | PARENB | CSTOPB)) | CS8 | CREAD | CLOCAL;
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;
    (void)tcsetattr(fd, TCSANOW, &options);
}

int FakeNodeOpen(char *path, unsigned int size)
{
    int fd;
    const char *name;

    fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0) {
        return -1;
    }
    if (grantpt(fd) != 0 || unlockpt(fd) != 0 || (name = ptsname(fd)) == NULL) {
        close(fd);
        return -1;
    }
    snprintf(path, size, "%s", name);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

int FakeNodeServe(int masterFd, uint32_t unitsPerTick)
{
    FakeBoard board = { .unitsPerTick = unitsPerTick };
    NodeLinkHooks hooks = { .calibrate = FakeCalibrate, .startTicks = FakeStartTicks,
                            .work = NULL, .context = &board };
    static EventBuffer events;
    static Scheduler scheduler;
    static NodeLink link;
    struct pollfd pfd;
    uint8_t bytes[256];
    const uint8_t *out;
    size_t pending;
    ssize_t count;
    int slaveFd;
    int timeout;

    /* Holding the slave open keeps the master readable while no host is
       connected, and the raw mode protects the binary frames */
    slaveFd = open(ptsname(masterFd), O_RDWR | O_NOCTTY);
    if (slaveFd < 0) {
        return 1;
    }
    MakeRaw(slaveFd);

    EventBufferInit(&events);
    (void)SchedulerInit(&scheduler, SchedulerAlgorithmRms, &events);
    NodeLinkInit(&link, &scheduler, &events, &hooks);

    for (;;) {
        pending = NodeLinkPending(&link, &out);
        timeout = board.ticking ? MillisUntil(&board.nextTick) : -1;
        pfd.fd = masterFd;
        pfd.events = POLLIN | (pending > 0 ? POLLOUT : 0);
        pfd.revents = 0;
        if (poll(&pfd, 1, timeout) < 0 && errno != EINTR) {
            close(slaveFd);
            return 1;
        }

        if (board.ticking && MillisUntil(&board.nextTick) == 0) {
            AddMicros(&board.nextTick, board.tickUs);
            if (NodeLinkIsRunning(&link)) {
                SchedulerTick(&scheduler);
                NodeLinkAfterTick(&link);
            }
        }
        if (pfd.revents & POLLIN) {
            count = read(masterFd, bytes, sizeof(bytes));
            if (count > 0) {
                NodeLinkReceive(&link, bytes, (size_t)count);
            }
        }
        while ((pending = NodeLinkPending(&link, &out)) > 0) {
            count = write(masterFd, out, pending);
            if (count <= 0) {
                break;
            }
            NodeLinkConsumed(&link, (size_t)count);
        }
    }
}
