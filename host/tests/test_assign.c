#include <stdio.h>

#include "assign.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static Task MakeTask(uint32_t id, TaskType type, uint32_t work, uint32_t period,
                     uint32_t criticality)
{
    Task task = { .id = id, .type = type, .work = work, .period = period,
                  .deadline = period, .criticality = criticality };
    return task;
}

static NodeInfo MakeNode(uint32_t id, uint32_t unitsPerTick)
{
    NodeInfo node = { .id = id, .unitsPerTick = unitsPerTick };
    return node;
}

/* Four tasks with utilization 0.5, 0.4, 0.3 and 0.2 on nodes of speed 10 */
static void FillContrastSet(Task *tasks)
{
    tasks[0] = MakeTask(1, TaskTypePeriodic, 50, 10, 1);
    tasks[1] = MakeTask(2, TaskTypePeriodic, 40, 10, 1);
    tasks[2] = MakeTask(3, TaskTypePeriodic, 30, 10, 1);
    tasks[3] = MakeTask(4, TaskTypePeriodic, 20, 10, 1);
}

static void TestFirstFitContrast(void)
{
    Task tasks[4];
    NodeInfo nodes[2] = { MakeNode(1, 10), MakeNode(2, 10) };
    Assignment result[4];
    NodeState states[2];

    FillContrastSet(tasks);
    CHECK(AssignTasks(tasks, 4, nodes, 2, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      result, states) == AssignStatusOk);
    CHECK(result[0].accepted && result[0].nodeIndex == 0);
    CHECK(result[1].accepted && result[1].nodeIndex == 0);
    CHECK(result[2].accepted && result[2].nodeIndex == 1);
    CHECK(result[3].accepted && result[3].nodeIndex == 1);
    CHECK(states[0].taskCount == 2 && states[1].taskCount == 2);
}

static void TestWorstFitContrast(void)
{
    Task tasks[4];
    NodeInfo nodes[2] = { MakeNode(1, 10), MakeNode(2, 10) };
    Assignment result[4];

    FillContrastSet(tasks);
    CHECK(AssignTasks(tasks, 4, nodes, 2, AssignAlgorithmEdf, AssignHeuristicWorstFit,
                      result, NULL) == AssignStatusOk);
    CHECK(result[0].accepted && result[0].nodeIndex == 0);
    CHECK(result[1].accepted && result[1].nodeIndex == 1);
    CHECK(result[2].accepted && result[2].nodeIndex == 1);
    CHECK(result[3].accepted && result[3].nodeIndex == 0);
}

static void TestFirstFitPrefersFastestNode(void)
{
    Task tasks[1] = { MakeTask(3, TaskTypePeriodic, 40, 20, 2) };
    NodeInfo nodes[2] = { MakeNode(1, 10), MakeNode(2, 50) };
    Assignment result[1];
    NodeState states[2];

    CHECK(AssignTasks(tasks, 1, nodes, 2, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      result, states) == AssignStatusOk);
    CHECK(result[0].accepted && result[0].nodeIndex == 1);
    CHECK(states[1].entries[0].cost == 1);
    CHECK(states[1].entries[0].period == 20);
}

static void TestNotSchedulable(void)
{
    Task tasks[2] = {
        MakeTask(1, TaskTypePeriodic, 450, 10, 1),
        MakeTask(2, TaskTypePeriodic, 100, 10, 1)
    };
    NodeInfo nodes[1] = { MakeNode(1, 50) };
    Assignment result[2];

    CHECK(AssignTasks(tasks, 2, nodes, 1, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      result, NULL) == AssignStatusOk);
    CHECK(result[0].accepted);
    CHECK(!result[1].accepted && result[1].reason == AssignRejectNotSchedulable);
}

static void TestRmsRejectsWhatEdfAccepts(void)
{
    Task tasks[2] = {
        MakeTask(1, TaskTypePeriodic, 2, 5, 1),
        MakeTask(2, TaskTypePeriodic, 4, 7, 1)
    };
    NodeInfo nodes[1] = { MakeNode(1, 1) };
    Assignment result[2];

    CHECK(AssignTasks(tasks, 2, nodes, 1, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      result, NULL) == AssignStatusOk);
    CHECK(result[0].accepted && result[1].accepted);

    CHECK(AssignTasks(tasks, 2, nodes, 1, AssignAlgorithmRms, AssignHeuristicFirstFit,
                      result, NULL) == AssignStatusOk);
    CHECK(!result[0].accepted && result[0].reason == AssignRejectNotSchedulable);
    CHECK(result[1].accepted);
}

static void TestSporadicLimit(void)
{
    Task tasks[2] = {
        MakeTask(1, TaskTypeSporadic, 10, 100, 1),
        MakeTask(2, TaskTypeSporadic, 10, 100, 1)
    };
    NodeInfo oneNode[1] = { MakeNode(1, 10) };
    NodeInfo twoNodes[2] = { MakeNode(1, 10), MakeNode(2, 10) };
    Assignment result[2];
    NodeState states[2];

    CHECK(AssignTasks(tasks, 2, oneNode, 1, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      result, NULL) == AssignStatusOk);
    CHECK(result[0].accepted);
    CHECK(!result[1].accepted && result[1].reason == AssignRejectSporadicLimit);

    CHECK(AssignTasks(tasks, 2, twoNodes, 2, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      result, states) == AssignStatusOk);
    CHECK(result[0].accepted && result[1].accepted);
    CHECK(result[0].nodeIndex != result[1].nodeIndex);
    CHECK(states[0].hasSporadic && states[1].hasSporadic);
}

static void TestNodeFull(void)
{
    Task tasks[9];
    NodeInfo nodes[1] = { MakeNode(1, 10) };
    Assignment result[9];

    for (uint32_t i = 0; i < 9; i++) {
        tasks[i] = MakeTask(i + 1, TaskTypePeriodic, 10, 100, 1);
    }
    CHECK(AssignTasks(tasks, 9, nodes, 1, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      result, NULL) == AssignStatusOk);
    CHECK(result[7].accepted);
    CHECK(!result[8].accepted && result[8].reason == AssignRejectNodeFull);
}

static void TestInvalidTaskAndUnusableNode(void)
{
    Task invalidTask[1] = { MakeTask(1, TaskTypePeriodic, 0, 10, 1) };
    Task validTask[1] = { MakeTask(1, TaskTypePeriodic, 10, 10, 1) };
    NodeInfo goodNode[1] = { MakeNode(1, 10) };
    NodeInfo zeroNode[1] = { MakeNode(1, 0) };
    Assignment result[1];

    CHECK(AssignTasks(invalidTask, 1, goodNode, 1, AssignAlgorithmEdf,
                      AssignHeuristicFirstFit, result, NULL) == AssignStatusOk);
    CHECK(!result[0].accepted && result[0].reason == AssignRejectInvalidTask);

    CHECK(AssignTasks(validTask, 1, zeroNode, 1, AssignAlgorithmEdf,
                      AssignHeuristicFirstFit, result, NULL) == AssignStatusOk);
    CHECK(!result[0].accepted && result[0].reason == AssignRejectNoUsableNode);

    CHECK(AssignTasks(validTask, 1, NULL, 0, AssignAlgorithmEdf,
                      AssignHeuristicFirstFit, result, NULL) == AssignStatusOk);
    CHECK(!result[0].accepted && result[0].reason == AssignRejectNoUsableNode);
}

static void TestSortTieBreak(void)
{
    Task tasks[3] = {
        MakeTask(1, TaskTypePeriodic, 60, 100, 1),
        MakeTask(2, TaskTypePeriodic, 60, 100, 5),
        MakeTask(3, TaskTypePeriodic, 60, 100, 5)
    };
    NodeInfo nodes[1] = { MakeNode(1, 1) };
    Assignment result[3];

    CHECK(AssignTasks(tasks, 3, nodes, 1, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      result, NULL) == AssignStatusOk);
    CHECK(!result[0].accepted);
    CHECK(result[1].accepted);
    CHECK(!result[2].accepted);
}

static void TestIncrementalPlacement(void)
{
    NodeState states[2];
    Task task = MakeTask(1, TaskTypePeriodic, 50, 10, 1);
    Assignment placement;

    AssignInitNode(&states[0], MakeNode(1, 10));
    AssignInitNode(&states[1], MakeNode(2, 10));

    CHECK(AssignPlaceTask(states, 2, &task, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                          &placement) == AssignStatusOk);
    CHECK(placement.accepted && placement.nodeIndex == 0);
    CHECK(states[0].taskCount == 1 && states[1].taskCount == 0);

    CHECK(AssignPlaceTask(states, 2, &task, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                          &placement) == AssignStatusOk);
    CHECK(placement.accepted && placement.nodeIndex == 0);
    CHECK(AssignPlaceTask(states, 2, &task, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                          &placement) == AssignStatusOk);
    CHECK(placement.accepted && placement.nodeIndex == 1);
}

static void TestInvalidArguments(void)
{
    Task task = MakeTask(1, TaskTypePeriodic, 10, 10, 1);
    NodeInfo nodes[1] = { MakeNode(1, 10) };
    NodeInfo tooManyNodes[AssignMaxNodes + 1];
    Assignment result[1];
    NodeState state;

    for (size_t i = 0; i < AssignMaxNodes + 1; i++) {
        tooManyNodes[i] = MakeNode((uint32_t)i, 10);
    }

    CHECK(AssignTasks(&task, 1, nodes, 1, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      NULL, NULL) == AssignStatusInvalidArgument);
    CHECK(AssignTasks(NULL, 1, nodes, 1, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      result, NULL) == AssignStatusInvalidArgument);
    CHECK(AssignTasks(&task, 1, tooManyNodes, AssignMaxNodes + 1, AssignAlgorithmEdf,
                      AssignHeuristicFirstFit, result, NULL) == AssignStatusInvalidArgument);
    CHECK(AssignTasks(NULL, 0, NULL, 0, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                      NULL, NULL) == AssignStatusOk);

    AssignInitNode(&state, nodes[0]);
    CHECK(AssignPlaceTask(&state, 1, NULL, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                          result) == AssignStatusInvalidArgument);
    CHECK(AssignPlaceTask(&state, 1, &task, AssignAlgorithmEdf, AssignHeuristicFirstFit,
                          NULL) == AssignStatusInvalidArgument);
    AssignInitNode(NULL, nodes[0]);
}

int main(void)
{
    TestFirstFitContrast();
    TestWorstFitContrast();
    TestFirstFitPrefersFastestNode();
    TestNotSchedulable();
    TestRmsRejectsWhatEdfAccepts();
    TestSporadicLimit();
    TestNodeFull();
    TestInvalidTaskAndUnusableNode();
    TestSortTieBreak();
    TestIncrementalPlacement();
    TestInvalidArguments();

    if (failureCount == 0) {
        printf("test_assign: all passed\n");
        return 0;
    }
    printf("test_assign: %d failure(s)\n", failureCount);
    return 1;
}