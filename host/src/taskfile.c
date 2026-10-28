#include "taskfile.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

static void AddRejection(TaskFile *result, const ParseError *error)
{
    if (result->errorCount < TaskFileMaxErrors) {
        result->errors[result->errorCount++] = *error;
    }
    result->rejectedCount++;
}

static bool IsHeader(const char *text)
{
    return strncasecmp(text, "id", 2) == 0 &&
           (text[2] == ',' || text[2] == ' ' || text[2] == '\t' || text[2] == '\0');
}

/* Returns TaskFileFieldCount + 1 as soon as there are too many fields */
static size_t SplitFields(char *text, char *fields[TaskFileFieldCount])
{
    size_t count = 0;

    for (;;) {
        char *comma = strchr(text, ',');

        if (comma != NULL) {
            *comma = '\0';
        }
        if (count == TaskFileFieldCount) {
            return count + 1;
        }
        fields[count++] = ParseTrim(text);
        if (comma == NULL) {
            return count;
        }
        text = comma + 1;
    }
}

static bool ParseNumberField(const char *text, const char *name, uint32_t *outValue,
                             size_t line, ParseError *error)
{
    if (!ParseUint32(text, outValue)) {
        ParseErrorSet(error, line, "%s must be a whole number from 0 to 4294967295, got '%s'",
                      name, text);
        return false;
    }
    return true;
}

static bool ParseTaskType(const char *text, TaskType *outType, size_t line, ParseError *error)
{
    if (strcasecmp(text, "P") == 0) {
        *outType = TaskTypePeriodic;
    } else if (strcasecmp(text, "S") == 0) {
        *outType = TaskTypeSporadic;
    } else {
        ParseErrorSet(error, line, "type must be P or S, got '%s'", text);
        return false;
    }
    return true;
}

static bool HasTaskId(const TaskFile *result, uint32_t id)
{
    if (result == NULL) {
        return false;
    }
    for (size_t i = 0; i < result->taskCount; i++) {
        if (result->tasks[i].id == id) {
            return true;
        }
    }
    return false;
}

static bool CheckTaskRules(const TaskFile *result, const Task *task, size_t line,
                           ParseError *error)
{
    if (task->id == 0) {
        ParseErrorSet(error, line, "id must be at least 1");
        return false;
    }
    if (TaskValidate(task) != TaskStatusOk) {
        ParseErrorSet(error, line, "W, T and D must be greater than 0");
        return false;
    }
    /* The node scheduler keeps one pending job per task and silently skips a
       release while the previous job is still active, so D > T would lose jobs */
    if (task->deadline > task->period) {
        ParseErrorSet(error, line, "D (%u) must not exceed T (%u)",
                      (unsigned)task->deadline, (unsigned)task->period);
        return false;
    }
    if (HasTaskId(result, task->id)) {
        ParseErrorSet(error, line, "id %u is used by an earlier task", (unsigned)task->id);
        return false;
    }
    return true;
}

static bool ParseTaskLine(char *text, size_t line, const TaskFile *result, Task *outTask,
                          ParseError *error)
{
    char *fields[TaskFileFieldCount];
    size_t fieldCount = SplitFields(text, fields);
    Task task;

    if (fieldCount > TaskFileFieldCount) {
        ParseErrorSet(error, line, "more than %d fields; expected id,type,W,T,D,criticality",
                      TaskFileFieldCount);
        return false;
    }
    if (fieldCount < TaskFileFieldCount) {
        ParseErrorSet(error, line, "only %zu field(s); expected id,type,W,T,D,criticality",
                      fieldCount);
        return false;
    }
    if (!ParseNumberField(fields[0], "id", &task.id, line, error) ||
        !ParseTaskType(fields[1], &task.type, line, error) ||
        !ParseNumberField(fields[2], "W", &task.work, line, error) ||
        !ParseNumberField(fields[3], "T", &task.period, line, error) ||
        !ParseNumberField(fields[4], "D", &task.deadline, line, error) ||
        !ParseNumberField(fields[5], "criticality", &task.criticality, line, error) ||
        !CheckTaskRules(result, &task, line, error)) {
        return false;
    }
    *outTask = task;
    return true;
}

static bool AcceptLine(TaskFile *result, char *text, size_t line, bool *seenData,
                       ParseError *error)
{
    ParseStripComment(text);
    text = ParseTrim(text);
    if (*text == '\0') {
        return true;
    }
    if (!*seenData && IsHeader(text)) {
        *seenData = true;
        return true;
    }
    *seenData = true;

    if (result->taskCount >= TaskFileMaxTasks) {
        ParseErrorSet(error, line, "more than %d tasks; line ignored", TaskFileMaxTasks);
        return false;
    }
    if (!ParseTaskLine(text, line, result, &result->tasks[result->taskCount], error)) {
        return false;
    }
    result->taskCount++;
    return true;
}

TaskFileStatus TaskFileLoadStream(FILE *input, TaskFile *result)
{
    char buffer[ParseLineMax + 1];
    size_t line = 0;
    bool seenData = false;
    ParseError error;

    if (result == NULL) {
        return TaskFileStatusInvalidArgument;
    }
    memset(result, 0, sizeof(*result));
    if (input == NULL) {
        return TaskFileStatusInvalidArgument;
    }

    for (;;) {
        LineRead status = ParseReadLine(input, buffer);

        if (status == LineReadEnd) {
            break;
        }
        line++;
        if (!ParseLineUsable(status, line, &error) ||
            !AcceptLine(result, buffer, line, &seenData, &error)) {
            AddRejection(result, &error);
        }
    }
    if (ferror(input)) {
        ParseErrorSet(&error, line, "read error");
        AddRejection(result, &error);
        return TaskFileStatusCannotOpen;
    }
    if (result->taskCount == 0) {
        ParseErrorSet(&error, 0, "no valid task in the file");
        AddRejection(result, &error);
    }
    return result->rejectedCount == 0 ? TaskFileStatusOk : TaskFileStatusRejectedLines;
}

bool TaskFileParseTask(const char *text, Task *outTask, ParseError *error)
{
    char buffer[ParseLineMax + 1];

    if (text == NULL || outTask == NULL) {
        ParseErrorSet(error, 0, "no task given");
        return false;
    }
    if (strlen(text) > ParseLineMax) {
        ParseErrorSet(error, 0, "line longer than %d characters", ParseLineMax);
        return false;
    }
    snprintf(buffer, sizeof(buffer), "%s", text);
    ParseStripComment(buffer);
    return ParseTaskLine(ParseTrim(buffer), 0, NULL, outTask, error);
}

TaskFileStatus TaskFileLoadFile(const char *path, TaskFile *result)
{
    if (path == NULL || result == NULL) {
        return TaskFileStatusInvalidArgument;
    }

    FILE *input = fopen(path, "r");
    if (input == NULL) {
        ParseError error;

        memset(result, 0, sizeof(*result));
        ParseErrorSet(&error, 0, "cannot open '%s': %s", path, strerror(errno));
        AddRejection(result, &error);
        return TaskFileStatusCannotOpen;
    }
    TaskFileStatus status = TaskFileLoadStream(input, result);
    fclose(input);
    return status;
}
