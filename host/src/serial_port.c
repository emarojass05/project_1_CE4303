#include "serial_port.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Raw mode by hand: cfmakeraw is not part of POSIX */
static void MakeRaw(struct termios *options)
{
    options->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                                    ICRNL | IXON | IXOFF | IXANY);
    options->c_oflag &= ~(tcflag_t)OPOST;
    options->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    options->c_cflag &= ~(tcflag_t)(CSIZE | PARENB | CSTOPB);
    options->c_cflag |= CS8 | CREAD | CLOCAL; /* 8N1, ignore modem lines */
    options->c_cc[VMIN] = 0;
    options->c_cc[VTIME] = 0;
}

int SerialOpen(const char *path)
{
    struct termios options;
    int fd;

    if (path == NULL) {
        return -1;
    }
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    if (tcgetattr(fd, &options) != 0) {
        close(fd);
        return -1;
    }
    MakeRaw(&options);
    if (cfsetispeed(&options, B115200) != 0 || cfsetospeed(&options, B115200) != 0 ||
        tcsetattr(fd, TCSANOW, &options) != 0) {
        close(fd);
        return -1;
    }
    /* Bytes the board sent before we opened (boot messages) are not frames */
    (void)tcflush(fd, TCIFLUSH);
    return fd;
}

static long MillisecondsSince(const struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - start->tv_sec) * 1000L +
           (now.tv_nsec - start->tv_nsec) / 1000000L;
}

bool SerialWriteAll(int fd, const uint8_t *bytes, size_t count, int timeoutMs)
{
    struct timespec start;
    struct pollfd pfd;
    size_t done = 0;
    ssize_t written;
    long left;

    if (fd < 0 || (bytes == NULL && count > 0)) {
        return false;
    }
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (done < count) {
        written = write(fd, bytes + done, count - done);
        if (written > 0) {
            done += (size_t)written;
            continue;
        }
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return false;
        }
        left = timeoutMs - MillisecondsSince(&start);
        if (left <= 0) {
            return false;
        }
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, (int)left) < 0 && errno != EINTR) {
            return false;
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            return false;
        }
    }
    return true;
}

void SerialClose(int fd)
{
    if (fd < 0) {
        return;
    }
    (void)tcflush(fd, TCIOFLUSH);
    close(fd);
}
