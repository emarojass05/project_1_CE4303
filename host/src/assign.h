#ifndef ASSIGN_H
#define ASSIGN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "admission.h"
#include "task.h"

/* Fixed limits for the host planner */
enum {
    AssignMaxNodes = 8,
    AssignMaxTasksPerNode = 8
};

typedef enum {
    AssignAlgorithmEdf = 0,
    AssignAlgorithmRms = 1
} AssignAlgorithm;

typedef enum {
    AssignHeuristicFirstFit = 0,
    AssignHeuristicWorstFit = 1
} AssignHeuristic;

/* Why a task was not placed */
typedef enum {
    AssignRejectNone = 0,
    AssignRejectInvalidTask,
    AssignRejectNoUsableNode,
    AssignRejectSporadicLimit,
    AssignRejectNodeFull,
    AssignRejectNotSchedulable
} AssignRejectReason;

typedef enum {
    AssignStatusOk = 0,
    AssignStatusInvalidArgument = -1,
    AssignStatusOutOfMemory = -2
} AssignStatus;

/* Tasks already placed on one node */
typedef struct {
    NodeInfo info;
    LoadEntry entries[AssignMaxTasksPerNode];
    size_t taskCount;
    bool hasSporadic;
} NodeState;

/* Placement result for one task */
typedef struct {
    bool accepted;
    size_t nodeIndex;
    AssignRejectReason reason;
} Assignment;

/* Turns the schedulability test on (default) or off. Off is for experiments
   only: every task that keeps the node at U <= 1 is accepted, whatever the
   algorithm, so RMS can be run on a load that only EDF can meet. */
void AssignSetAdmissionCheck(bool enabled);

/* Resets a node state to empty with the given speed */
void AssignInitNode(NodeState *state, NodeInfo info);

/* Places one task on the current node states */
AssignStatus AssignPlaceTask(NodeState *nodes, size_t nodeCount, const Task *task,
                             AssignAlgorithm algorithm, AssignHeuristic heuristic,
                             Assignment *outAssignment);

/* Adds a task that was already admitted to the node, without the admission
   test, e.g. to rebuild the state of a node; false if it has no room */
bool AssignCommitTask(NodeState *node, const Task *task);

/* Places every task; outAssignments follows the input task order */
AssignStatus AssignTasks(const Task *tasks, size_t taskCount,
                         const NodeInfo *nodes, size_t nodeCount,
                         AssignAlgorithm algorithm, AssignHeuristic heuristic,
                         Assignment *outAssignments, NodeState *outNodes);

#endif /* ASSIGN_H */