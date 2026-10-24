#include "node_link.h"

#include <string.h>

void NodeLinkInit(NodeLink *link, Scheduler *scheduler, EventBuffer *events,
                  const NodeLinkHooks *hooks)
{
    if (link == NULL) {
        return;
    }
    memset(link, 0, sizeof(*link));
    link->scheduler = scheduler;
    link->events = events;
    if (hooks != NULL) {
        link->hooks = *hooks;
    }
    link->state = NodeLinkIdle;
    link->sending = NodeLinkSendingNone;
    FrameParserInit(&link->parser);
    EventTxInit(&link->eventTx);
}

/* Encodes frame at the end of the control queue; a full queue counts the drop */
static void QueueFrame(NodeLink *link, const Frame *frame, bool afterEvents)
{
    NodeLinkOutFrame *slot;

    if (link->controlCount >= (size_t)NodeLinkControlSlots) {
        link->droppedControl++;
        return;
    }
    slot = &link->control[(link->controlHead + link->controlCount) % NodeLinkControlSlots];
    if (ProtocolEncodeFrame(frame, slot->bytes, sizeof(slot->bytes), &slot->length) !=
        ProtocolStatusOk) {
        return;
    }
    slot->afterEvents = afterEvents;
    link->controlCount++;
}

static Frame MakeAck(uint8_t seq, AckStatus status)
{
    AckMessage message = { .ackedSeq = seq, .status = status };
    Frame reply;

    (void)ProtocolPackAck(&message, &reply);
    reply.seq = seq;
    return reply;
}

static AckStatus StatusFromUnpack(ProtocolStatus status)
{
    return status == ProtocolStatusOk ? AckStatusOk : AckStatusBadPayload;
}

static Frame HandleCalibRequest(NodeLink *link, const Frame *frame)
{
    CalibRequestMessage request;
    CalibResponseMessage response;
    Frame reply;
    uint32_t units;

    if (link->state != NodeLinkIdle && link->state != NodeLinkCalibrated) {
        return MakeAck(frame->seq, AckStatusBadState);
    }
    if (ProtocolUnpackCalibRequest(frame, &request) != ProtocolStatusOk ||
        request.tickUs < (uint32_t)NodeLinkMinTickUs ||
        request.tickUs > (uint32_t)NodeLinkMaxTickUs) {
        return MakeAck(frame->seq, AckStatusBadPayload);
    }
    units = link->hooks.calibrate != NULL ?
            link->hooks.calibrate(request.tickUs, link->hooks.context) : 0;
    /* The loop keeps waking once per tick, now with the tick of the host */
    if (units == 0 || link->hooks.startTicks == NULL ||
        !link->hooks.startTicks(request.tickUs, link->hooks.context)) {
        return MakeAck(frame->seq, AckStatusBadState);
    }
    link->tickUs = request.tickUs;
    link->unitsPerTick = units;
    link->state = NodeLinkCalibrated;

    response.unitsPerTick = units;
    (void)ProtocolPackCalibResponse(&response, &reply);
    reply.seq = frame->seq;
    return reply;
}

static AckStatus HandleDeployConfig(NodeLink *link, const Frame *frame)
{
    DeployConfigMessage message;
    SchedulerAlgorithm algorithm;
    AckStatus status;

    if (link->state != NodeLinkCalibrated && link->state != NodeLinkDeploying) {
        return AckStatusBadState;
    }
    status = StatusFromUnpack(ProtocolUnpackDeployConfig(frame, &message));
    if (status != AckStatusOk) {
        return status;
    }
    algorithm = message.algorithm == ProtocolAlgorithmEdf ? SchedulerAlgorithmEdf :
                                                            SchedulerAlgorithmRms;
    /* A new configuration replaces every task deployed before; tick 0 again */
    EventBufferInit(link->events);
    if (SchedulerInit(link->scheduler, algorithm, link->events) != SchedulerStatusOk) {
        return AckStatusBadState;
    }
    SchedulerSetWork(link->scheduler, link->hooks.work, link->hooks.context);
    link->tickReportEvery = message.tickReportEvery;
    link->sporadicTaskId = 0;
    link->state = NodeLinkDeploying;
    return AckStatusOk;
}

static AckStatus HandleDeployTask(NodeLink *link, const Frame *frame)
{
    DeployTaskMessage message;
    Task task;
    AckStatus status;
    SchedulerStatus added;

    /* While running it is the manual mode: the task joins at the current tick */
    if (link->state != NodeLinkDeploying && link->state != NodeLinkRunning) {
        return AckStatusBadState;
    }
    status = StatusFromUnpack(ProtocolUnpackDeployTask(frame, &message));
    if (status != AckStatusOk) {
        return status;
    }
    if (message.id == 0 || message.cost == 0) {
        return AckStatusBadPayload;
    }
    if (message.type == ProtocolTaskSporadic && link->sporadicTaskId != 0) {
        return AckStatusFull; /* At most one sporadic task per node (spec 3.7) */
    }
    task.id = message.id;
    task.type = message.type == ProtocolTaskSporadic ? TaskTypeSporadic : TaskTypePeriodic;
    task.work = 0; /* The node only needs the cost in ticks */
    task.period = message.period;
    task.deadline = message.deadline;
    task.criticality = message.criticality;

    added = SchedulerAddTask(link->scheduler, &task, message.cost);
    if (added == SchedulerStatusFull) {
        return AckStatusFull;
    }
    if (added != SchedulerStatusOk) {
        return AckStatusBadPayload;
    }
    if (task.type == TaskTypeSporadic) {
        link->sporadicTaskId = task.id;
    }
    return AckStatusOk;
}

static AckStatus HandleStart(NodeLink *link, const Frame *frame)
{
    if (link->state != NodeLinkDeploying) {
        return AckStatusBadState;
    }
    if (ProtocolUnpackEmpty(frame, MessageTypeStart) != ProtocolStatusOk) {
        return AckStatusBadPayload;
    }
    /* Restarting the timer here makes tick 0 begin with this message */
    if (link->hooks.startTicks == NULL ||
        !link->hooks.startTicks(link->tickUs, link->hooks.context)) {
        return AckStatusBadState;
    }
    link->state = NodeLinkRunning;
    return AckStatusOk;
}

static AckStatus HandleStop(NodeLink *link, const Frame *frame)
{
    if (ProtocolUnpackEmpty(frame, MessageTypeStop) != ProtocolStatusOk) {
        return AckStatusBadPayload;
    }
    link->state = NodeLinkIdle;
    return AckStatusOk;
}

static AckStatus HandleSporadicTrigger(NodeLink *link, const Frame *frame)
{
    SporadicTriggerMessage message;
    AckStatus status;

    if (link->state != NodeLinkRunning) {
        return AckStatusBadState;
    }
    status = StatusFromUnpack(ProtocolUnpackSporadicTrigger(frame, &message));
    if (status != AckStatusOk) {
        return status;
    }
    /* A release before the minimum interval is ignored and reported as an event */
    if (SchedulerReleaseSporadic(link->scheduler, message.taskId) ==
        SchedulerStatusInvalidArgument) {
        return AckStatusBadPayload;
    }
    return AckStatusOk;
}

static bool SameFrame(const Frame *a, const Frame *b)
{
    return a->type == b->type && a->seq == b->seq && a->length == b->length &&
           memcmp(a->payload, b->payload, a->length) == 0;
}

void NodeLinkHandleFrame(NodeLink *link, const Frame *frame)
{
    Frame reply;

    if (link == NULL || frame == NULL || link->scheduler == NULL || link->events == NULL) {
        return;
    }
    if (link->haveLast && SameFrame(frame, &link->lastRequest)) {
        /* Retransmission: the reply was lost, send it again without reapplying */
        QueueFrame(link, &link->lastReply, frame->type == MessageTypeStop);
        return;
    }

    switch (frame->type) {
    case MessageTypeCalibRequest:
        reply = HandleCalibRequest(link, frame);
        break;
    case MessageTypeDeployConfig:
        reply = MakeAck(frame->seq, HandleDeployConfig(link, frame));
        break;
    case MessageTypeDeployTask:
        reply = MakeAck(frame->seq, HandleDeployTask(link, frame));
        break;
    case MessageTypeStart:
        reply = MakeAck(frame->seq, HandleStart(link, frame));
        break;
    case MessageTypeStop:
        reply = MakeAck(frame->seq, HandleStop(link, frame));
        break;
    case MessageTypeSporadicTrigger:
        reply = MakeAck(frame->seq, HandleSporadicTrigger(link, frame));
        break;
    default:
        return; /* Messages that only the node sends are ignored */
    }

    link->lastRequest = *frame;
    link->lastReply = reply;
    link->haveLast = true;
    /* Stop is confirmed after the pending events (docs/protocolo.md, section 5) */
    QueueFrame(link, &reply, frame->type == MessageTypeStop);
}

void NodeLinkReceive(NodeLink *link, const uint8_t *bytes, size_t count)
{
    Frame frame;
    size_t i;

    if (link == NULL || bytes == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        if (FrameParserFeed(&link->parser, bytes[i], &frame) == FrameParserReady) {
            NodeLinkHandleFrame(link, &frame);
        }
    }
}

bool NodeLinkIsRunning(const NodeLink *link)
{
    return link != NULL && link->state == NodeLinkRunning;
}

void NodeLinkAfterTick(NodeLink *link)
{
    const Scheduler *scheduler;
    TickReportMessage report;
    Frame frame;

    if (link == NULL || link->scheduler == NULL || link->tickReportEvery == 0) {
        return;
    }
    scheduler = link->scheduler;
    if (scheduler->tick % link->tickReportEvery != 0) {
        return;
    }
    report.tick = scheduler->tick;
    report.runningTaskId = scheduler->lastRunIndex == SchedulerNoTask ? 0 :
                           scheduler->tasks[scheduler->lastRunIndex].task.id;
    report.droppedEvents = EventBufferDroppedCount(link->events);
    report.missCount = scheduler->missCount;
    (void)ProtocolPackTickReport(&report, &frame);
    frame.seq = link->reportSeq++;
    QueueFrame(link, &frame, false);
}

/* True when every event left the node, so a Stop can be confirmed */
static bool EventsDrained(NodeLink *link)
{
    return link->sending != NodeLinkSendingEvent && EventBufferCount(link->events) == 0;
}

size_t NodeLinkPending(NodeLink *link, const uint8_t **outBytes)
{
    const NodeLinkOutFrame *head;
    size_t pending;

    if (link == NULL || outBytes == NULL || link->events == NULL) {
        return 0;
    }
    /* Frames are never interleaved: a half-written Event frame goes first */
    if (link->sending == NodeLinkSendingEvent) {
        pending = EventTxPending(&link->eventTx, link->events, outBytes);
        if (pending > 0) {
            return pending;
        }
    }
    if (link->controlCount > 0) {
        head = &link->control[link->controlHead];
        if (link->controlSent > 0 || !head->afterEvents || EventsDrained(link)) {
            link->sending = NodeLinkSendingControl;
            *outBytes = head->bytes + link->controlSent;
            return head->length - link->controlSent;
        }
    }
    pending = EventTxPending(&link->eventTx, link->events, outBytes);
    link->sending = pending > 0 ? NodeLinkSendingEvent : NodeLinkSendingNone;
    return pending;
}

void NodeLinkConsumed(NodeLink *link, size_t count)
{
    const NodeLinkOutFrame *head;

    if (link == NULL) {
        return;
    }
    if (link->sending == NodeLinkSendingEvent) {
        EventTxConsumed(&link->eventTx, count);
        if (link->eventTx.sent >= link->eventTx.length) {
            link->sending = NodeLinkSendingNone;
        }
        return;
    }
    if (link->sending != NodeLinkSendingControl || link->controlCount == 0) {
        return;
    }
    head = &link->control[link->controlHead];
    link->controlSent += count;
    if (link->controlSent >= head->length) {
        link->controlSent = 0;
        link->controlHead = (link->controlHead + 1) % NodeLinkControlSlots;
        link->controlCount--;
        link->sending = NodeLinkSendingNone;
    }
}
