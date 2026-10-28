#ifndef PLANNER_H
#define PLANNER_H

#include <stdbool.h>
#include <stdio.h>

#include "config.h"
#include "taskfile.h"

/* Automatic mode of the host (F10, F9 part 3). The planner process starts
   one report process and one driver process per node, all sharing POSIX
   shared memory and semaphores, and then:
     1. greets every node with Stop, so a node left running by an earlier
        run goes back to Idle,
     2. calibrates every node with the tick of the configuration,
     3. computes C(i,n), assigns the tasks with the configured heuristic and
        admission test, and prints the accepted and rejected tasks,
     4. deploys the algorithm and the tasks to each node and sends Start,
     5. waits until q is typed (or input ends, Ctrl+C, or a child dies),
     6. stops the nodes, ends the drivers, closes the ports, lets the report
        process print the report, and frees every resource.
   A node that does not answer is left out of the assignment.
   Messages and the link summary go to out. Returns 0 when everything ended
   cleanly and at least one node took part. */
int PlannerRun(const HostConfig *config, const TaskFile *tasks, int inputFd, FILE *out,
               bool verbose);

#endif /* PLANNER_H */
