#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "event_queue.h"

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

/* Event whose fields all derive from node and sequence */
static HostEvent MakeHostEvent(uint32_t nodeId, uint32_t sequence)
{
    HostEvent hostEvent;

    hostEvent.nodeId = nodeId;
    hostEvent.event.type = EventTypeStart;
    hostEvent.event.tick = sequence;
    hostEvent.event.taskId = sequence + 100;
    hostEvent.event.value = sequence + 200;
    return hostEvent;
}

/* Exit code of a finished child, or -1 if it did not exit normally */
static int WaitForChild(pid_t child)
{
    int status;

    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/* True while the child has not exited yet */
static int ChildIsRunning(pid_t child)
{
    return waitpid(child, NULL, WNOHANG) == 0;
}

static void TestSingleProcessFifo(void)
{
    EventQueue *queue = EventQueueCreate(queueName);
    HostEvent hostEvent;

    CHECK(queue != NULL);
    if (queue == NULL) {
        return;
    }
    CHECK(EventQueueCount(queue) == 0);

    hostEvent = MakeHostEvent(7, 1);
    CHECK(EventQueuePush(queue, &hostEvent));
    hostEvent = MakeHostEvent(8, 2);
    CHECK(EventQueuePush(queue, &hostEvent));
    hostEvent = MakeHostEvent(9, 3);
    CHECK(EventQueuePush(queue, &hostEvent));
    CHECK(EventQueueCount(queue) == 3);

    CHECK(EventQueuePop(queue, &hostEvent));
    CHECK(hostEvent.nodeId == 7 && hostEvent.event.tick == 1);
    CHECK(hostEvent.event.type == EventTypeStart);
    CHECK(hostEvent.event.taskId == 101 && hostEvent.event.value == 201);
    CHECK(EventQueuePop(queue, &hostEvent) && hostEvent.nodeId == 8);
    CHECK(EventQueuePop(queue, &hostEvent) && hostEvent.nodeId == 9);
    CHECK(EventQueueCount(queue) == 0);

    EventQueueDestroy(queue, queueName);
}

static void TestInvalidArguments(void)
{
    EventQueue *queue = EventQueueCreate(queueName);
    HostEvent hostEvent = MakeHostEvent(1, 1);

    CHECK(queue != NULL);
    CHECK(EventQueueCreate(NULL) == NULL);
    CHECK(EventQueueOpen(NULL) == NULL);
    CHECK(EventQueueOpen("/rt_events_missing_queue") == NULL);
    CHECK(!EventQueuePush(NULL, &hostEvent));
    CHECK(!EventQueuePop(NULL, &hostEvent));
    if (queue != NULL) {
        CHECK(!EventQueuePush(queue, NULL));
        CHECK(!EventQueuePop(queue, NULL));
    }
    CHECK(EventQueueCount(NULL) == 0);
    EventQueueShutdown(NULL);
    EventQueueClose(NULL);
    EventQueueDestroy(queue, queueName);
}

static void TestShutdownDrainsThenStops(void)
{
    EventQueue *queue = EventQueueCreate(queueName);
    HostEvent hostEvent;

    CHECK(queue != NULL);
    if (queue == NULL) {
        return;
    }
    hostEvent = MakeHostEvent(1, 1);
    CHECK(EventQueuePush(queue, &hostEvent));
    hostEvent = MakeHostEvent(1, 2);
    CHECK(EventQueuePush(queue, &hostEvent));

    EventQueueShutdown(queue);
    CHECK(!EventQueuePush(queue, &hostEvent));
    CHECK(EventQueuePop(queue, &hostEvent) && hostEvent.event.tick == 1);
    CHECK(EventQueuePop(queue, &hostEvent) && hostEvent.event.tick == 2);
    CHECK(!EventQueuePop(queue, &hostEvent));
    CHECK(!EventQueuePop(queue, &hostEvent));

    EventQueueDestroy(queue, queueName);
}

static void TestFullQueueBlocksProducer(void)
{
    EventQueue *queue = EventQueueCreate(queueName);
    HostEvent hostEvent;
    pid_t child;
    uint32_t i;

    CHECK(queue != NULL);
    if (queue == NULL) {
        return;
    }
    for (i = 0; i < EventQueueCapacity; i++) {
        hostEvent = MakeHostEvent(1, i);
        CHECK(EventQueuePush(queue, &hostEvent));
    }
    CHECK(EventQueueCount(queue) == EventQueueCapacity);

    child = fork();
    if (child == 0) {
        HostEvent extra = MakeHostEvent(2, 1000);
        _exit(EventQueuePush(queue, &extra) ? 0 : 1);
    }

    SleepMilliseconds(200);
    CHECK(ChildIsRunning(child));
    CHECK(EventQueueCount(queue) == EventQueueCapacity);

    CHECK(EventQueuePop(queue, &hostEvent) && hostEvent.event.tick == 0);
    CHECK(WaitForChild(child) == 0);
    CHECK(EventQueueCount(queue) == EventQueueCapacity);

    EventQueueDestroy(queue, queueName);
}

static void TestShutdownWakesBlockedConsumer(void)
{
    EventQueue *queue = EventQueueCreate(queueName);
    pid_t child;

    CHECK(queue != NULL);
    if (queue == NULL) {
        return;
    }
    child = fork();
    if (child == 0) {
        HostEvent hostEvent;
        _exit(EventQueuePop(queue, &hostEvent) ? 1 : 0);
    }

    SleepMilliseconds(200);
    CHECK(ChildIsRunning(child));
    EventQueueShutdown(queue);
    CHECK(WaitForChild(child) == 0);

    EventQueueDestroy(queue, queueName);
}

static void TestShutdownWakesBlockedProducers(void)
{
    EventQueue *queue = EventQueueCreate(queueName);
    HostEvent hostEvent;
    pid_t children[2];
    uint32_t i;

    CHECK(queue != NULL);
    if (queue == NULL) {
        return;
    }
    for (i = 0; i < EventQueueCapacity; i++) {
        hostEvent = MakeHostEvent(1, i);
        CHECK(EventQueuePush(queue, &hostEvent));
    }
    for (i = 0; i < 2; i++) {
        children[i] = fork();
        if (children[i] == 0) {
            HostEvent extra = MakeHostEvent(2, 1000);
            _exit(EventQueuePush(queue, &extra) ? 1 : 0);
        }
    }

    SleepMilliseconds(200);
    CHECK(ChildIsRunning(children[0]));
    CHECK(ChildIsRunning(children[1]));
    EventQueueShutdown(queue);
    CHECK(WaitForChild(children[0]) == 0);
    CHECK(WaitForChild(children[1]) == 0);

    EventQueueDestroy(queue, queueName);
}

enum {
    ProducerCount = 3,
    EventsPerProducer = 1000
};

static void TestManyProducersOneConsumer(void)
{
    EventQueue *queue = EventQueueCreate(queueName);
    uint32_t nextSequence[ProducerCount] = { 0 };
    pid_t children[ProducerCount];
    HostEvent hostEvent;
    uint32_t producer;
    uint32_t received;

    CHECK(queue != NULL);
    if (queue == NULL) {
        return;
    }
    for (producer = 0; producer < ProducerCount; producer++) {
        children[producer] = fork();
        if (children[producer] == 0) {
            EventQueue *mine = EventQueueOpen(queueName);
            uint32_t sequence;

            if (mine == NULL) {
                _exit(2);
            }
            for (sequence = 0; sequence < EventsPerProducer; sequence++) {
                HostEvent produced = MakeHostEvent(producer, sequence);
                if (!EventQueuePush(mine, &produced)) {
                    _exit(1);
                }
            }
            EventQueueClose(mine);
            _exit(0);
        }
    }

    for (received = 0; received < ProducerCount * EventsPerProducer; received++) {
        CHECK(EventQueuePop(queue, &hostEvent));
        CHECK(hostEvent.nodeId < ProducerCount);
        if (hostEvent.nodeId < ProducerCount) {
            CHECK(hostEvent.event.tick == nextSequence[hostEvent.nodeId]);
            nextSequence[hostEvent.nodeId]++;
        }
    }
    for (producer = 0; producer < ProducerCount; producer++) {
        CHECK(nextSequence[producer] == EventsPerProducer);
        CHECK(WaitForChild(children[producer]) == 0);
    }
    CHECK(EventQueueCount(queue) == 0);

    EventQueueDestroy(queue, queueName);
}

static void TestDestroyRemovesSharedMemory(void)
{
    EventQueue *queue = EventQueueCreate(queueName);
    EventQueue *second;

    CHECK(queue != NULL);
    if (queue == NULL) {
        return;
    }
    second = EventQueueOpen(queueName);
    CHECK(second != NULL);
    EventQueueClose(second);

    EventQueueDestroy(queue, queueName);
    CHECK(EventQueueOpen(queueName) == NULL);
}

int main(void)
{
    snprintf(queueName, sizeof(queueName), "/rt_events_test_%d", (int)getpid());

    TestSingleProcessFifo();
    TestInvalidArguments();
    TestShutdownDrainsThenStops();
    TestFullQueueBlocksProducer();
    TestShutdownWakesBlockedConsumer();
    TestShutdownWakesBlockedProducers();
    TestManyProducersOneConsumer();
    TestDestroyRemovesSharedMemory();

    if (failureCount != 0) {
        printf("test_event_queue: %d failure(s)\n", failureCount);
        return 1;
    }
    printf("test_event_queue: all checks passed\n");
    return 0;
}