#include <stdio.h>
#include <string.h>

#include "node_link.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

enum {
    TestUnitsPerTick = 42,
    TestTickUs = 10000,
    MaxOutFrames = 128
};

/* Fake board: records what the link asked for */
typedef struct {
    uint32_t calibratedTickUs;
    uint32_t startedTickUs;
    int startCount;
    int workCalls;
} FakeBoard;

static uint32_t FakeCalibrate(uint32_t tickUs, void *context)
{
    FakeBoard *board = context;

    board->calibratedTickUs = tickUs;
    return TestUnitsPerTick;
}

static bool FakeStartTicks(uint32_t tickUs, void *context)
{
    FakeBoard *board = context;

    board->startedTickUs = tickUs;
    board->startCount++;
    return true;
}

static void FakeWork(uint32_t taskId, void *context)
{
    FakeBoard *board = context;

    (void)taskId;
    board->workCalls++;
}

typedef struct {
    FakeBoard board;
    EventBuffer events;
    Scheduler scheduler;
    NodeLink link;
    FrameParser hostParser; /* Reads what the node sends */
    Frame out[MaxOutFrames];
    size_t outCount;
} Fixture;

static void Setup(Fixture *fixture)
{
    NodeLinkHooks hooks = { .calibrate = FakeCalibrate, .startTicks = FakeStartTicks,
                            .work = FakeWork, .context = &fixture->board };

    memset(fixture, 0, sizeof(*fixture));
    hooks.context = &fixture->board;
    EventBufferInit(&fixture->events);
    (void)SchedulerInit(&fixture->scheduler, SchedulerAlgorithmRms, &fixture->events);
    NodeLinkInit(&fixture->link, &fixture->scheduler, &fixture->events, &hooks);
    FrameParserInit(&fixture->hostParser);
}

/* Writes at most chunk bytes per call, like a slow port, and parses the frames */
static void DrainChunked(Fixture *fixture, size_t chunk)
{
    const uint8_t *bytes;
    size_t pending;
    size_t i;
    Frame frame;

    while ((pending = NodeLinkPending(&fixture->link, &bytes)) > 0) {
        if (pending > chunk) {
            pending = chunk;
        }
        for (i = 0; i < pending; i++) {
            if (FrameParserFeed(&fixture->hostParser, bytes[i], &frame) == FrameParserReady &&
                fixture->outCount < MaxOutFrames) {
                fixture->out[fixture->outCount++] = frame;
            }
        }
        NodeLinkConsumed(&fixture->link, pending);
    }
}

static void Drain(Fixture *fixture)
{
    DrainChunked(fixture, ProtocolMaxFrameSize);
}

/* Sends a frame as wire bytes, like the host driver */
static void Send(Fixture *fixture, Frame frame, uint8_t seq)
{
    uint8_t bytes[ProtocolMaxFrameSize];
    size_t length = 0;

    frame.seq = seq;
    CHECK(ProtocolEncodeFrame(&frame, bytes, sizeof(bytes), &length) == ProtocolStatusOk);
    NodeLinkReceive(&fixture->link, bytes, length);
}

/* Sends a frame and returns the status of the Ack that comes back */
static int SendForAck(Fixture *fixture, Frame frame, uint8_t seq)
{
    AckMessage ack;
    int result = -1;
    int acks = 0;
    size_t i;

    fixture->outCount = 0;
    Send(fixture, frame, seq);
    Drain(fixture);
    /* Events may come out too (a sporadic release); exactly one Ack must */
    for (i = 0; i < fixture->outCount; i++) {
        if (fixture->out[i].type != MessageTypeAck) {
            continue;
        }
        acks++;
        if (fixture->out[i].seq == seq &&
            ProtocolUnpackAck(&fixture->out[i], &ack) == ProtocolStatusOk && ack.ackedSeq == seq) {
            result = (int)ack.status;
        }
    }
    return acks == 1 ? result : -1;
}

static Frame CalibFrame(uint32_t tickUs)
{
    CalibRequestMessage message = { .tickUs = tickUs };
    Frame frame;

    (void)ProtocolPackCalibRequest(&message, &frame);
    return frame;
}

static Frame ConfigFrame(ProtocolAlgorithm algorithm, uint16_t every)
{
    DeployConfigMessage message = { .algorithm = algorithm, .tickReportEvery = every };
    Frame frame;

    (void)ProtocolPackDeployConfig(&message, &frame);
    return frame;
}

static Frame TaskFrame(uint32_t id, ProtocolTaskType type, uint32_t cost, uint32_t period)
{
    DeployTaskMessage message = { .id = id, .type = type, .cost = cost, .period = period,
                                  .deadline = period, .criticality = 1 };
    Frame frame;

    (void)ProtocolPackDeployTask(&message, &frame);
    return frame;
}

static Frame EmptyFrame(MessageType type)
{
    Frame frame;

    (void)ProtocolPackEmpty(type, &frame);
    return frame;
}

static Frame SporadicFrame(uint32_t taskId)
{
    SporadicTriggerMessage message = { .taskId = taskId };
    Frame frame;

    (void)ProtocolPackSporadicTrigger(&message, &frame);
    return frame;
}

/* Calibrates and loads the algorithm, leaving the node in Deploying */
static void ReachDeploying(Fixture *fixture, uint16_t every)
{
    fixture->outCount = 0;
    Send(fixture, CalibFrame(TestTickUs), 1);
    Drain(fixture);
    CHECK(SendForAck(fixture, ConfigFrame(ProtocolAlgorithmEdf, every), 2) == AckStatusOk);
}

static void Tick(Fixture *fixture)
{
    CHECK(SchedulerTick(&fixture->scheduler) == SchedulerStatusOk);
    NodeLinkAfterTick(&fixture->link);
}

static void TestCalibrationAnswersWithMeasuredUnits(void)
{
    Fixture fixture;
    CalibResponseMessage response;

    Setup(&fixture);
    Send(&fixture, CalibFrame(TestTickUs), 9);
    Drain(&fixture);
    CHECK(fixture.outCount == 1);
    CHECK(fixture.out[0].type == MessageTypeCalibResponse);
    CHECK(fixture.out[0].seq == 9);
    CHECK(ProtocolUnpackCalibResponse(&fixture.out[0], &response) == ProtocolStatusOk);
    CHECK(response.unitsPerTick == TestUnitsPerTick);
    CHECK(fixture.board.calibratedTickUs == TestTickUs);
    CHECK(fixture.board.startedTickUs == TestTickUs);
    CHECK(fixture.link.state == NodeLinkCalibrated);
    CHECK(!NodeLinkIsRunning(&fixture.link));
}

static void TestBadTickIsRejected(void)
{
    Fixture fixture;

    Setup(&fixture);
    CHECK(SendForAck(&fixture, CalibFrame(0), 1) == AckStatusBadPayload);
    CHECK(SendForAck(&fixture, CalibFrame(NodeLinkMaxTickUs + 1), 2) == AckStatusBadPayload);
    CHECK(fixture.board.calibratedTickUs == 0);
    CHECK(fixture.link.state == NodeLinkIdle);
}

static void TestFullSessionProducesEventsAndReports(void)
{
    Fixture fixture;
    int events = 0;
    int reports = 0;
    size_t i;
    TickReportMessage report;

    Setup(&fixture);
    ReachDeploying(&fixture, 2);
    CHECK(SendForAck(&fixture, TaskFrame(1, ProtocolTaskPeriodic, 1, 4), 3) == AckStatusOk);
    CHECK(SendForAck(&fixture, TaskFrame(2, ProtocolTaskPeriodic, 2, 8), 4) == AckStatusOk);
    CHECK(fixture.scheduler.taskCount == 2);
    CHECK(fixture.scheduler.algorithm == SchedulerAlgorithmEdf);

    fixture.board.startCount = 0;
    CHECK(SendForAck(&fixture, EmptyFrame(MessageTypeStart), 5) == AckStatusOk);
    CHECK(NodeLinkIsRunning(&fixture.link));
    CHECK(fixture.board.startCount == 1);
    CHECK(fixture.scheduler.tick == 0);

    fixture.outCount = 0;
    for (i = 0; i < 4; i++) {
        Tick(&fixture);
    }
    Drain(&fixture);
    CHECK(fixture.board.workCalls == 3); /* 1 + 2 ticks of work, then idle */
    for (i = 0; i < fixture.outCount; i++) {
        if (fixture.out[i].type == MessageTypeEvent) {
            events++;
        } else if (fixture.out[i].type == MessageTypeTickReport) {
            CHECK(ProtocolUnpackTickReport(&fixture.out[i], &report) == ProtocolStatusOk);
            reports++;
        }
    }
    CHECK(reports == 2); /* After ticks 2 and 4 */
    CHECK(report.tick == 4);
    CHECK(report.missCount == 0);
    CHECK(events >= 6); /* 2 releases, 2 starts, 2 completions */
}

static void TestMessagesOutOfOrderAreBadState(void)
{
    Fixture fixture;

    Setup(&fixture);
    CHECK(SendForAck(&fixture, ConfigFrame(ProtocolAlgorithmRms, 0), 1) == AckStatusBadState);
    CHECK(SendForAck(&fixture, TaskFrame(1, ProtocolTaskPeriodic, 1, 4), 2) == AckStatusBadState);
    CHECK(SendForAck(&fixture, EmptyFrame(MessageTypeStart), 3) == AckStatusBadState);
    CHECK(SendForAck(&fixture, SporadicFrame(1), 4) == AckStatusBadState);

    ReachDeploying(&fixture, 0);
    CHECK(SendForAck(&fixture, SporadicFrame(1), 5) == AckStatusBadState);
    CHECK(SendForAck(&fixture, EmptyFrame(MessageTypeStart), 6) == AckStatusOk);
    CHECK(SendForAck(&fixture, CalibFrame(TestTickUs), 7) == AckStatusBadState);
    CHECK(SendForAck(&fixture, ConfigFrame(ProtocolAlgorithmRms, 0), 8) == AckStatusBadState);
    CHECK(SendForAck(&fixture, EmptyFrame(MessageTypeStart), 9) == AckStatusBadState);
    CHECK(NodeLinkIsRunning(&fixture.link));
}

static void TestBadTasksAndLimits(void)
{
    Fixture fixture;
    uint32_t id;

    Setup(&fixture);
    ReachDeploying(&fixture, 0);
    CHECK(SendForAck(&fixture, TaskFrame(0, ProtocolTaskPeriodic, 1, 4), 3) == AckStatusBadPayload);
    CHECK(SendForAck(&fixture, TaskFrame(1, ProtocolTaskPeriodic, 0, 4), 4) == AckStatusBadPayload);
    CHECK(SendForAck(&fixture, TaskFrame(1, ProtocolTaskPeriodic, 1, 0), 5) == AckStatusBadPayload);
    CHECK(SendForAck(&fixture, TaskFrame(1, ProtocolTaskSporadic, 1, 20), 6) == AckStatusOk);
    CHECK(SendForAck(&fixture, TaskFrame(2, ProtocolTaskSporadic, 1, 20), 7) == AckStatusFull);
    CHECK(SendForAck(&fixture, TaskFrame(1, ProtocolTaskPeriodic, 1, 4), 8) == AckStatusBadPayload);
    for (id = 2; id <= SchedulerMaxTasks; id++) {
        CHECK(SendForAck(&fixture, TaskFrame(id, ProtocolTaskPeriodic, 1, 100),
                         (uint8_t)(10 + id)) == AckStatusOk);
    }
    CHECK(SendForAck(&fixture, TaskFrame(99, ProtocolTaskPeriodic, 1, 100), 30) == AckStatusFull);
    CHECK(fixture.scheduler.taskCount == SchedulerMaxTasks);
    CHECK(fixture.link.sporadicTaskId == 1);
}

/* A lost Ack makes the host send the same frame again: it must not deploy twice */
static void TestRetransmissionIsNotAppliedTwice(void)
{
    Fixture fixture;
    Frame task = TaskFrame(5, ProtocolTaskPeriodic, 1, 10);

    Setup(&fixture);
    ReachDeploying(&fixture, 0);
    CHECK(SendForAck(&fixture, task, 40) == AckStatusOk);
    CHECK(SendForAck(&fixture, task, 40) == AckStatusOk);
    CHECK(fixture.scheduler.taskCount == 1);
    /* Same content with a new seq is a new message, and now a repeated id */
    CHECK(SendForAck(&fixture, task, 41) == AckStatusBadPayload);
}

static void TestSporadicTrigger(void)
{
    Fixture fixture;

    Setup(&fixture);
    ReachDeploying(&fixture, 0);
    CHECK(SendForAck(&fixture, TaskFrame(3, ProtocolTaskSporadic, 1, 20), 3) == AckStatusOk);
    CHECK(SendForAck(&fixture, TaskFrame(4, ProtocolTaskPeriodic, 1, 20), 4) == AckStatusOk);
    CHECK(SendForAck(&fixture, EmptyFrame(MessageTypeStart), 5) == AckStatusOk);
    CHECK(SendForAck(&fixture, SporadicFrame(3), 6) == AckStatusOk);
    CHECK(fixture.scheduler.tasks[0].active);
    CHECK(SendForAck(&fixture, SporadicFrame(4), 7) == AckStatusBadPayload);
    CHECK(SendForAck(&fixture, SporadicFrame(77), 8) == AckStatusBadPayload);
}

/* The Ack of Stop comes after every pending event, and nothing is interleaved */
static void TestStopIsConfirmedAfterEvents(void)
{
    Fixture fixture;
    AckMessage ack;
    int i;

    Setup(&fixture);
    ReachDeploying(&fixture, 0);
    CHECK(SendForAck(&fixture, TaskFrame(1, ProtocolTaskPeriodic, 1, 2), 3) == AckStatusOk);
    CHECK(SendForAck(&fixture, EmptyFrame(MessageTypeStart), 4) == AckStatusOk);
    for (i = 0; i < 6; i++) {
        Tick(&fixture);
    }
    fixture.outCount = 0;
    Send(&fixture, EmptyFrame(MessageTypeStop), 5);
    CHECK(!NodeLinkIsRunning(&fixture.link));
    DrainChunked(&fixture, 7);
    CHECK(fixture.outCount == 10); /* 3 x (release, start, complete) + Ack */
    CHECK(fixture.out[0].type == MessageTypeEvent);
    CHECK(fixture.out[fixture.outCount - 1].type == MessageTypeAck);
    CHECK(ProtocolUnpackAck(&fixture.out[fixture.outCount - 1], &ack) == ProtocolStatusOk);
    CHECK(ack.ackedSeq == 5 && ack.status == AckStatusOk);
    CHECK(fixture.hostParser.badFrames == 0);
    CHECK(fixture.link.state == NodeLinkIdle);
}

/* A reply queued while an Event frame is half written waits for that frame */
static void TestReplyDoesNotCutAnEventFrame(void)
{
    Fixture fixture;
    const uint8_t *bytes;
    size_t pending;

    Setup(&fixture);
    ReachDeploying(&fixture, 0);
    CHECK(SendForAck(&fixture, TaskFrame(1, ProtocolTaskPeriodic, 1, 2), 3) == AckStatusOk);
    CHECK(SendForAck(&fixture, EmptyFrame(MessageTypeStart), 4) == AckStatusOk);
    Tick(&fixture);

    fixture.outCount = 0;
    pending = NodeLinkPending(&fixture.link, &bytes);
    CHECK(pending == 19);
    CHECK(FrameParserFeed(&fixture.hostParser, bytes[0], &fixture.out[0]) == FrameParserNone);
    CHECK(FrameParserFeed(&fixture.hostParser, bytes[1], &fixture.out[0]) == FrameParserNone);
    NodeLinkConsumed(&fixture.link, 2);
    Send(&fixture, SporadicFrame(1), 5); /* Not sporadic: Ack BadPayload is queued */
    Drain(&fixture);
    CHECK(fixture.hostParser.badFrames == 0);
    CHECK(fixture.outCount >= 2);
    CHECK(fixture.out[0].type == MessageTypeEvent);
    CHECK(fixture.out[1].type == MessageTypeAck);
}

static void TestCorruptFrameGetsNoReply(void)
{
    Fixture fixture;
    uint8_t bytes[ProtocolMaxFrameSize];
    size_t length = 0;
    Frame frame = CalibFrame(TestTickUs);

    Setup(&fixture);
    frame.seq = 1;
    CHECK(ProtocolEncodeFrame(&frame, bytes, sizeof(bytes), &length) == ProtocolStatusOk);
    bytes[5] ^= 0x01;
    NodeLinkReceive(&fixture.link, bytes, length);
    Drain(&fixture);
    CHECK(fixture.outCount == 0);
    CHECK(fixture.link.parser.badFrames == 1);
    CHECK(fixture.link.state == NodeLinkIdle);
}

int main(void)
{
    TestCalibrationAnswersWithMeasuredUnits();
    TestBadTickIsRejected();
    TestFullSessionProducesEventsAndReports();
    TestMessagesOutOfOrderAreBadState();
    TestBadTasksAndLimits();
    TestRetransmissionIsNotAppliedTwice();
    TestSporadicTrigger();
    TestStopIsConfirmedAfterEvents();
    TestReplyDoesNotCutAnEventFrame();
    TestCorruptFrameGetsNoReply();

    if (failureCount != 0) {
        printf("test_node_link: %d check(s) failed\n", failureCount);
        return 1;
    }
    printf("test_node_link: all checks passed\n");
    return 0;
}
