#ifndef TASKFILE_H
#define TASKFILE_H

#include <stddef.h>
#include <stdio.h>

#include "assign.h"
#include "parse_util.h"
#include "task.h"

/* Task file: one task per line as "id,type,W,T,D,criticality".
   type is P (periodic) or S (sporadic); W in work units, T and D in ticks.
   Blank lines and '#' comments are ignored; an optional first line starting
   with "id" is taken as a header. See tasks/ejemplo.csv. */

enum {
    TaskFileMaxTasks = AssignMaxNodes * AssignMaxTasksPerNode,
    TaskFileMaxErrors = 32,
    TaskFileFieldCount = 6
};

typedef struct {
    Task tasks[TaskFileMaxTasks];
    size_t taskCount;
    ParseError errors[TaskFileMaxErrors]; /* First TaskFileMaxErrors problems */
    size_t errorCount;                    /* Problems stored in errors */
    size_t rejectedCount;                 /* All problems, including those not stored */
} TaskFile;

typedef enum {
    TaskFileStatusOk = 0,
    TaskFileStatusRejectedLines = 1,  /* Some lines were skipped; the valid tasks are kept */
    TaskFileStatusInvalidArgument = -1,
    TaskFileStatusCannotOpen = -2
} TaskFileStatus;

/* Parses every line. An invalid line is recorded in errors and skipped, so
   one bad task does not stop the others; the caller decides whether to go on.
   A file without any valid task is reported as a rejection with line 0.
   Rules beyond TaskValidate: id >= 1 (0 means "no task" in the protocol),
   ids are unique, and D <= T. */
TaskFileStatus TaskFileLoadStream(FILE *input, TaskFile *result);

/* Same as TaskFileLoadStream, reading the file at path */
TaskFileStatus TaskFileLoadFile(const char *path, TaskFile *result);

#endif /* TASKFILE_H */
