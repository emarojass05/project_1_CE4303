#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "event_queue.h"
#include "host_demo.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static char queueName[64];

static void SleepMilliseconds(long milliseconds)
{
    struct timespec pause = { .tv_sec = 0, .tv_nsec = milliseconds * 1000000L };
    nanosleep(&pause, NULL);
}

/* Everything the demo wrote to the file, as text */
static void ReadAll(FILE *file, char *text, size_t size)
{
    size_t length;

    fflush(file);
    rewind(file);
    length = fread(text, 1, size - 1, file);
    text[length] = '\0';
}

/* The producer ends by itself after a few passes; nobody types q */
static void TestEndsWhenProducerFinishes(void)
{
    FILE *out = tmpfile();
    int quietPipe[2];
    char text[4096];
    int result;

    CHECK(out != NULL);
    CHECK(pipe(quietPipe) == 0);
    if (out == NULL) {
        return;
    }
    result = HostRunDemo(queueName, quietPipe[0], out, 5, 0);
    CHECK(result == 0);

    ReadAll(out, text, sizeof(text));
    CHECK(strstr(text, "Events received: 70") != NULL);
    CHECK(strstr(text, "Events ignored: 0") != NULL);
    CHECK(strstr(text, "Node 1: 5 task switches") != NULL);
    CHECK(EventQueueOpen(queueName) == NULL);

    fclose(out);
    close(quietPipe[0]);
    close(quietPipe[1]);
}

/* A very long producer is stopped by typing q */
static void TestStopsOnQ(void)
{
    FILE *out = tmpfile();
    int inputPipe[2];
    char text[4096];
    int result;
    pid_t typist;

    CHECK(out != NULL);
    CHECK(pipe(inputPipe) == 0);
    if (out == NULL) {
        return;
    }
    typist = fork();
    if (typist == 0) {
        SleepMilliseconds(300);
        (void)write(inputPipe[1], "hello\nq\n", 8);
        _exit(0);
    }

    result = HostRunDemo(queueName, inputPipe[0], out, 1000000, 1);
    CHECK(result == 0);

    ReadAll(out, text, sizeof(text));
    CHECK(strstr(text, "===== Report =====") != NULL);
    CHECK(strstr(text, "Events received:") != NULL);
    CHECK(strstr(text, "Events ignored: 0") != NULL);
    CHECK(EventQueueOpen(queueName) == NULL);

    waitpid(typist, NULL, 0);
    fclose(out);
    close(inputPipe[0]);
    close(inputPipe[1]);
}

/* Closing the input counts as a request to stop */
static void TestStopsWhenInputEnds(void)
{
    FILE *out = tmpfile();
    int inputPipe[2];
    char text[4096];
    int result;
    pid_t closer;

    CHECK(out != NULL);
    CHECK(pipe(inputPipe) == 0);
    if (out == NULL) {
        return;
    }
    closer = fork();
    if (closer == 0) {
        SleepMilliseconds(300);
        close(inputPipe[1]);
        _exit(0);
    }
    close(inputPipe[1]);

    result = HostRunDemo(queueName, inputPipe[0], out, 1000000, 1);
    CHECK(result == 0);

    ReadAll(out, text, sizeof(text));
    CHECK(strstr(text, "===== Report =====") != NULL);
    CHECK(EventQueueOpen(queueName) == NULL);

    waitpid(closer, NULL, 0);
    fclose(out);
    close(inputPipe[0]);
}

static void TestInvalidArguments(void)
{
    CHECK(HostRunDemo(NULL, 0, stdout, 1, 0) != 0);
    CHECK(HostRunDemo(queueName, 0, NULL, 1, 0) != 0);
}

int main(void)
{
    snprintf(queueName, sizeof(queueName), "/rt_events_demo_%d", (int)getpid());

    TestEndsWhenProducerFinishes();
    TestStopsOnQ();
    TestStopsWhenInputEnds();
    TestInvalidArguments();

    if (failureCount != 0) {
        printf("test_host_demo: %d failure(s)\n", failureCount);
        return 1;
    }
    printf("test_host_demo: all checks passed\n");
    return 0;
}