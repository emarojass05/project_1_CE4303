#include "assign.h"

#include <stdlib.h>

typedef struct {
    size_t index;
    double utilization;
    uint32_t criticality;
    uint32_t id;
} SortEntry;

static bool AcceptsLoad(AssignAlgorithm algorithm, const LoadEntry *entries, size_t count)
{
    if (algorithm == AssignAlgorithmEdf) {
        return AdmissionAcceptsEdf(entries, count);
    }
    return AdmissionAcceptsRms(entries, count);
}

static double FreeCapacity(AssignAlgorithm algorithm, const LoadEntry *entries, size_t count)
{
    double limit = (algorithm == AssignAlgorithmEdf) ? 1.0 : AdmissionRmsBound(count);
    return limit - AdmissionUtilization(entries, count);
}

/* Node indices from fastest to slowest; equal speeds keep input order */
static void OrderBySpeed(const NodeState *nodes, size_t nodeCount, size_t *order)
{
    for (size_t i = 0; i < nodeCount; i++) {
        size_t position = i;
        while (position > 0 &&
               nodes[order[position - 1]].info.unitsPerTick < nodes[i].info.unitsPerTick) {
            order[position] = order[position - 1];
            position--;
        }
        order[position] = i;
    }
}

/* Utilization on the fastest node, or -1 when it cannot be computed */
static double SortUtilization(const Task *task, uint32_t fastestSpeed)
{
    uint32_t cost = 0;

    if (fastestSpeed == 0 || TaskValidate(task) != TaskStatusOk) {
        return -1.0;
    }
    if (TaskCost(task, fastestSpeed, &cost) != TaskStatusOk) {
        return -1.0;
    }
    return (double)cost / (double)task->period;
}

/* Higher utilization first, then higher criticality, lower id, lower index */
static int CompareSortEntries(const void *left, const void *right)
{
    const SortEntry *a = left;
    const SortEntry *b = right;

    if (a->utilization > b->utilization) {
        return -1;
    }
    if (a->utilization < b->utilization) {
        return 1;
    }
    if (a->criticality != b->criticality) {
        return a->criticality > b->criticality ? -1 : 1;
    }
    if (a->id != b->id) {
        return a->id < b->id ? -1 : 1;
    }
    if (a->index != b->index) {
        return a->index < b->index ? -1 : 1;
    }
    return 0;
}

void AssignInitNode(NodeState *state, NodeInfo info)
{
    if (state == NULL) {
        return;
    }
    state->info = info;
    state->taskCount = 0;
    state->hasSporadic = false;
    for (size_t i = 0; i < AssignMaxTasksPerNode; i++) {
        state->entries[i].cost = 0;
        state->entries[i].period = 0;
    }
}

AssignStatus AssignPlaceTask(NodeState *nodes, size_t nodeCount, const Task *task,
                             AssignAlgorithm algorithm, AssignHeuristic heuristic,
                             Assignment *outAssignment)
{
    if ((nodes == NULL && nodeCount > 0) || task == NULL || outAssignment == NULL ||
        nodeCount > AssignMaxNodes) {
        return AssignStatusInvalidArgument;
    }

    outAssignment->accepted = false;
    outAssignment->nodeIndex = 0;
    outAssignment->reason = AssignRejectNone;

    if (TaskValidate(task) != TaskStatusOk) {
        outAssignment->reason = AssignRejectInvalidTask;
        return AssignStatusOk;
    }

    size_t order[AssignMaxNodes];
    OrderBySpeed(nodes, nodeCount, order);

    bool foundNode = false;
    size_t bestNode = 0;
    uint32_t bestCost = 0;
    double bestFree = 0.0;
    size_t usableCount = 0;
    size_t sporadicBlockedCount = 0;
    size_t fullCount = 0;
    size_t notSchedulableCount = 0;

    for (size_t i = 0; i < nodeCount; i++) {
        size_t nodeIndex = order[i];
        const NodeState *node = &nodes[nodeIndex];

        if (node->info.unitsPerTick == 0) {
            continue;
        }
        usableCount++;

        if (task->type == TaskTypeSporadic && node->hasSporadic) {
            sporadicBlockedCount++;
            continue;
        }
        if (node->taskCount >= AssignMaxTasksPerNode) {
            fullCount++;
            continue;
        }

        uint32_t cost = 0;
        if (TaskCost(task, node->info.unitsPerTick, &cost) != TaskStatusOk) {
            notSchedulableCount++;
            continue;
        }

        LoadEntry candidate[AssignMaxTasksPerNode + 1];
        for (size_t j = 0; j < node->taskCount; j++) {
            candidate[j] = node->entries[j];
        }
        candidate[node->taskCount].cost = cost;
        candidate[node->taskCount].period = task->period;
        size_t candidateCount = node->taskCount + 1;

        if (!AcceptsLoad(algorithm, candidate, candidateCount)) {
            notSchedulableCount++;
            continue;
        }

        double freeCapacity = FreeCapacity(algorithm, candidate, candidateCount);
        if (!foundNode || freeCapacity > bestFree) {
            foundNode = true;
            bestNode = nodeIndex;
            bestCost = cost;
            bestFree = freeCapacity;
        }
        if (heuristic == AssignHeuristicFirstFit) {
            break;
        }
    }

    if (foundNode) {
        NodeState *chosen = &nodes[bestNode];
        chosen->entries[chosen->taskCount].cost = bestCost;
        chosen->entries[chosen->taskCount].period = task->period;
        chosen->taskCount++;
        if (task->type == TaskTypeSporadic) {
            chosen->hasSporadic = true;
        }
        outAssignment->accepted = true;
        outAssignment->nodeIndex = bestNode;
        return AssignStatusOk;
    }

    if (usableCount == 0) {
        outAssignment->reason = AssignRejectNoUsableNode;
    } else if (notSchedulableCount > 0) {
        outAssignment->reason = AssignRejectNotSchedulable;
    } else if (fullCount > 0) {
        outAssignment->reason = AssignRejectNodeFull;
    } else {
        outAssignment->reason = AssignRejectSporadicLimit;
    }
    return AssignStatusOk;
}

AssignStatus AssignTasks(const Task *tasks, size_t taskCount,
                         const NodeInfo *nodes, size_t nodeCount,
                         AssignAlgorithm algorithm, AssignHeuristic heuristic,
                         Assignment *outAssignments, NodeState *outNodes)
{
    if ((tasks == NULL && taskCount > 0) || (nodes == NULL && nodeCount > 0) ||
        (outAssignments == NULL && taskCount > 0) || nodeCount > AssignMaxNodes) {
        return AssignStatusInvalidArgument;
    }

    NodeState states[AssignMaxNodes];
    uint32_t fastestSpeed = 0;
    for (size_t i = 0; i < nodeCount; i++) {
        AssignInitNode(&states[i], nodes[i]);
        if (nodes[i].unitsPerTick > fastestSpeed) {
            fastestSpeed = nodes[i].unitsPerTick;
        }
    }

    if (taskCount > 0) {
        if (taskCount > SIZE_MAX / sizeof(SortEntry)) {
            return AssignStatusInvalidArgument;
        }
        SortEntry *sorted = malloc(taskCount * sizeof(SortEntry));
        if (sorted == NULL) {
            return AssignStatusOutOfMemory;
        }

        for (size_t i = 0; i < taskCount; i++) {
            sorted[i].index = i;
            sorted[i].id = tasks[i].id;
            sorted[i].criticality = tasks[i].criticality;
            sorted[i].utilization = SortUtilization(&tasks[i], fastestSpeed);
        }
        qsort(sorted, taskCount, sizeof(SortEntry), CompareSortEntries);

        for (size_t i = 0; i < taskCount; i++) {
            size_t taskIndex = sorted[i].index;
            AssignStatus status = AssignPlaceTask(states, nodeCount, &tasks[taskIndex],
                                                  algorithm, heuristic,
                                                  &outAssignments[taskIndex]);
            if (status != AssignStatusOk) {
                free(sorted);
                return status;
            }
        }
        free(sorted);
    }

    if (outNodes != NULL) {
        for (size_t i = 0; i < nodeCount; i++) {
            outNodes[i] = states[i];
        }
    }
    return AssignStatusOk;
}