#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "driver.h"
#include "event_queue.h"
#include "fake_node.h"
#include "serial_port.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static char queueName[64];
static char controlName[64];

static void SleepMilliseconds(long milliseconds)
{
    struct timespec pause = { .tv_sec = milliseconds / 1000,
                              .tv_nsec = (milliseconds % 1000) * 1000000L };
    nanosleep(&pause, NULL);
}

/* Starts a simulated node in a child process; writes its port to path */
static pid_t StartFakeNode(char *path, unsigned int size, uint32_t unitsPerTick)
{
    pid_t child;
    int fd = FakeNodeOpen(path, size);

    if (fd < 0) {
        return -1;
    }
    child = fork();
    if (child == 0) {
        _exit(FakeNodeServe(fd, unitsPerTick));
    }
    close(fd);
    return child;
}

static void StopChild(pid_t child)
{
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
}

static pid_t StartDriver(ControlBlock *control, EventQueue *queue)
{
    pid_t child = fork();

    if (child == 0) {
        _exit(DriverRun(&control->channels[0], queue));
    }
    return child;
}

static int EndDriver(ControlBlock *control, pid_t driver)
{
    int status = 1;

    DriverPostQuit(&control->channels[0]);
    if (waitpid(driver, &status, 0) != driver || !WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status);
}

/* Sends an empty or packed message and returns the Ack status, -1 on no Ack */
static int AckOf(DriverChannel *channel, const Frame *request)
{
    Frame reply;
    AckMessage ack;

    if (DriverRequest(channel, request, DriverReplyTimeoutMs, DriverAttempts, &reply) !=
            DriverResultOk ||
        ProtocolUnpackAck(&reply, &ack) != ProtocolStatusOk) {
        return -1;
    }
    return (int)ack.status;
}

static Frame Empty(MessageType type)
{
    Frame frame;

    (void)ProtocolPackEmpty(type, &frame);
    return frame;
}

/* A whole run against the simulated node: replies come back and events reach the queue */
static void TestSessionWithFakeNode(void)
{
    char ports[1][ConfigPortMaxLength];
    ControlBlock *control;
    EventQueue *queue;
    CalibRequestMessage calib = { .tickUs = 10000 };
    CalibResponseMessage response;
    DeployConfigMessage config = { .algorithm = ProtocolAlgorithmEdf, .tickReportEvery = 2 };
    DeployTaskMessage task = { .id = 3, .type = ProtocolTaskPeriodic, .cost = 1, .period = 4,
                               .deadline = 4, .criticality = 1 };
    HostEvent hostEvent;
    DriverStats stats;
    Frame request;
    Frame reply;
    pid_t node;
    pid_t driver;
    size_t events;
    size_t reports;

    node = StartFakeNode(ports[0], sizeof(ports[0]), 33);
    CHECK(node > 0);
    queue = EventQueueCreate(queueName);
    control = ControlCreate(controlName, (const char (*)[ConfigPortMaxLength])ports, 1);
    CHECK(queue != NULL && control != NULL);
    if (node <= 0 || queue == NULL || control == NULL) {
        return;
    }
    driver = StartDriver(control, queue);

    request = Empty(MessageTypeStop);
    CHECK(AckOf(&control->channels[0], &request) == AckStatusOk);

    (void)ProtocolPackCalibRequest(&calib, &request);
    CHECK(DriverRequest(&control->channels[0], &request, 1000, DriverAttempts, &reply) ==
          DriverResultOk);
    CHECK(ProtocolUnpackCalibResponse(&reply, &response) == ProtocolStatusOk);
    CHECK(response.unitsPerTick == 33);

    (void)ProtocolPackDeployConfig(&config, &request);
    CHECK(AckOf(&control->channels[0], &request) == AckStatusOk);
    (void)ProtocolPackDeployTask(&task, &request);
    CHECK(AckOf(&control->channels[0], &request) == AckStatusOk);
    request = Empty(MessageTypeStart);
    CHECK(AckOf(&control->channels[0], &request) == AckStatusOk);
    SleepMilliseconds(120);
    request = Empty(MessageTypeStop);
    CHECK(AckOf(&control->channels[0], &request) == AckStatusOk);

    /* Node events and tick reports share the queue; count each kind */
    events = 0;
    reports = 0;
    while (EventQueueCount(queue) > 0 && EventQueuePop(queue, &hostEvent)) {
        CHECK(hostEvent.nodeId == 1);
        if (hostEvent.kind == HostEventTickReport) {
            CHECK(hostEvent.hostUs > 0);
            CHECK(hostEvent.event.tick % 2 == 0);
            reports++;
            continue;
        }
        CHECK(hostEvent.kind == HostEventNode);
        if (events == 0) {
            CHECK(hostEvent.event.type == EventTypeRelease);
            CHECK(hostEvent.event.tick == 0);
            CHECK(hostEvent.event.taskId == 3);
        }
        events++;
    }
    CHECK(events >= 6); /* At least 2 jobs: release, start, complete each */

    stats = DriverReadStats(&control->channels[0]);
    CHECK(stats.portOpen);
    CHECK(stats.tickReports >= 3);
    CHECK(stats.tickReports == reports);
    CHECK(stats.lastReportUs > 0);
    CHECK(stats.eventsReceived == events);
    CHECK(stats.badFrames == 0);
    CHECK(stats.retransmissions == 0);

    CHECK(EndDriver(control, driver) == 0);
    StopChild(node);
    ControlDestroy(control, controlName);
    EventQueueDestroy(queue, queueName);
}

static void TestMissingPortIsReported(void)
{
    char ports[1][ConfigPortMaxLength] = { "/dev/no-such-port" };
    ControlBlock *control = ControlCreate(controlName,
                                          (const char (*)[ConfigPortMaxLength])ports, 1);
    EventQueue *queue = EventQueueCreate(queueName);
    Frame request = Empty(MessageTypeStop);
    Frame reply;
    pid_t driver;

    CHECK(control != NULL && queue != NULL);
    if (control == NULL || queue == NULL) {
        return;
    }
    driver = StartDriver(control, queue);
    CHECK(DriverRequest(&control->channels[0], &request, 50, 2, &reply) == DriverResultPortError);
    CHECK(!DriverReadStats(&control->channels[0]).portOpen);
    CHECK(EndDriver(control, driver) == 0);
    ControlDestroy(control, controlName);
    EventQueueDestroy(queue, queueName);
}

/* Nobody answers: the driver resends the same frame and gives up; a corrupt
   frame that arrives meanwhile is counted */
static void TestSilentNodeAndCorruptFrame(void)
{
    char ports[1][ConfigPortMaxLength];
    ControlBlock *control;
    EventQueue *queue;
    Frame request = Empty(MessageTypeStart);
    Frame reply;
    DriverStats stats;
    uint8_t corrupt[] = { 0x7E, MessageTypeAck, 1, 2, 1, 0, 0x00, 0x00 };
    uint8_t sent[64];
    ssize_t count;
    pid_t driver;
    int master;
    int slave;

    master = FakeNodeOpen(ports[0], sizeof(ports[0]));
    CHECK(master >= 0);
    control = ControlCreate(controlName, (const char (*)[ConfigPortMaxLength])ports, 1);
    queue = EventQueueCreate(queueName);
    CHECK(control != NULL && queue != NULL);
    if (master < 0 || control == NULL || queue == NULL) {
        return;
    }
    slave = SerialOpen(ports[0]); /* Keeps the pair alive and raw while the driver starts */
    driver = StartDriver(control, queue);

    CHECK(DriverRequest(&control->channels[0], &request, 40, 3, &reply) == DriverResultNoReply);
    stats = DriverReadStats(&control->channels[0]);
    CHECK(stats.retransmissions == 2);
    count = read(master, sent, sizeof(sent));
    CHECK(count == 18); /* The same 6-byte Start frame, three times */
    CHECK(count == 18 && memcmp(sent, sent + 6, 6) == 0 && memcmp(sent, sent + 12, 6) == 0);

    CHECK(write(master, corrupt, sizeof(corrupt)) == (ssize_t)sizeof(corrupt));
    SleepMilliseconds(50);
    stats = DriverReadStats(&control->channels[0]);
    CHECK(stats.badFrames == 1);
    CHECK(stats.framesReceived == 0);

    CHECK(EndDriver(control, driver) == 0);
    SerialClose(slave);
    close(master);
    ControlDestroy(control, controlName);
    EventQueueDestroy(queue, queueName);
}

int main(void)
{
    snprintf(queueName, sizeof(queueName), "/test_driver_events_%d", (int)getpid());
    snprintf(controlName, sizeof(controlName), "/test_driver_control_%d", (int)getpid());

    TestSessionWithFakeNode();
    TestMissingPortIsReported();
    TestSilentNodeAndCorruptFrame();

    if (failureCount != 0) {
        printf("test_driver: %d check(s) failed\n", failureCount);
        return 1;
    }
    printf("test_driver: all checks passed\n");
    return 0;
}
