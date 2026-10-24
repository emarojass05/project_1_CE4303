/* Host of the real-time scheduler (automatic mode).

     ./build/bin/host [-v] [config]

   Reads the configuration (config/ejemplo.conf by default) and its task
   file, calibrates the nodes, assigns and deploys the tasks, starts the
   nodes and prints the report when q is pressed. -v also prints every event
   as it arrives. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "planner.h"
#include "taskfile.h"

#define DEFAULT_CONFIG "config/ejemplo.conf"

static int Usage(const char *program)
{
    fprintf(stderr, "Usage: %s [-v] [config file]\n", program);
    return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    static HostConfig config;
    static TaskFile tasks;
    const char *configPath = DEFAULT_CONFIG;
    ParseError error;
    TaskFileStatus taskStatus;
    bool verbose = false;
    bool configGiven = false;
    size_t i;
    int arg;

    for (arg = 1; arg < argc; arg++) {
        if (strcmp(argv[arg], "-v") == 0) {
            verbose = true;
        } else if (argv[arg][0] == '-' || configGiven) {
            return Usage(argv[0]);
        } else {
            configPath = argv[arg];
            configGiven = true;
        }
    }

    if (ConfigLoadFile(configPath, &config, &error) != ConfigStatusOk) {
        fprintf(stderr, "%s:%u: %s\n", configPath, (unsigned)error.line, error.message);
        return EXIT_FAILURE;
    }
    taskStatus = TaskFileLoadFile(config.tasksFile, &tasks);
    if (taskStatus == TaskFileStatusCannotOpen || taskStatus == TaskFileStatusInvalidArgument) {
        fprintf(stderr, "%s: cannot read the task file\n", config.tasksFile);
        return EXIT_FAILURE;
    }
    for (i = 0; i < tasks.errorCount; i++) {
        fprintf(stderr, "%s:%u: %s (line skipped)\n", config.tasksFile,
                (unsigned)tasks.errors[i].line, tasks.errors[i].message);
    }
    if (tasks.taskCount == 0) {
        fprintf(stderr, "%s: no valid task\n", config.tasksFile);
        return EXIT_FAILURE;
    }
    printf("%u task(s) from %s, %u node(s)\n", (unsigned)tasks.taskCount, config.tasksFile,
           (unsigned)config.portCount);

    return PlannerRun(&config, &tasks, STDIN_FILENO, stdout, verbose) == 0 ? EXIT_SUCCESS :
                                                                          EXIT_FAILURE;
}
