#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "taskfile.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static TaskFile result;

static TaskFileStatus LoadBytes(const char *bytes, size_t length)
{
    FILE *input = fmemopen((void *)bytes, length, "r");
    TaskFileStatus status;

    CHECK(input != NULL);
    if (input == NULL) {
        return TaskFileStatusCannotOpen;
    }
    status = TaskFileLoadStream(input, &result);
    fclose(input);
    return status;
}

static TaskFileStatus LoadText(const char *text)
{
    return LoadBytes(text, strlen(text));
}

/* A valid task first, so the rejected line never leaves the file empty */
static void CheckLineRejected(const char *badLine, const char *expectedWord, int callerLine)
{
    char text[ParseLineMax * 2];

    snprintf(text, sizeof(text), "1,P,10,20,20,1\n%s\n", badLine);
    if (LoadText(text) != TaskFileStatusRejectedLines || result.taskCount != 1 ||
        result.rejectedCount != 1 || result.errors[0].line != 2 ||
        strstr(result.errors[0].message, expectedWord) == NULL) {
        printf("  FAILED line %d: '%s' should be rejected at line 2 mentioning '%s', "
               "got %zu task(s), %zu rejection(s), first: line %zu '%s'\n",
               callerLine, badLine, expectedWord, result.taskCount, result.rejectedCount,
               result.errors[0].line, result.errors[0].message);
        failureCount++;
    }
}

#define CHECK_LINE_REJECTED(line, word) CheckLineRejected((line), (word), __LINE__)

static void TestValidFile(void)
{
    const char *text =
        "# comment\r\n"
        "ID , Tipo , W , T , D , Criticidad\r\n"
        "\r\n"
        "1,P,200,20,20,5\r\n"
        "  2 , p , 300 , 40 , 30 , 0   # trailing comment\r\n"
        "6,S,120,100,60,4294967295";

    CHECK(LoadText(text) == TaskFileStatusOk);
    CHECK(result.taskCount == 3 && result.rejectedCount == 0);

    const Task *first = &result.tasks[0];
    CHECK(first->id == 1 && first->type == TaskTypePeriodic && first->work == 200);
    CHECK(first->period == 20 && first->deadline == 20 && first->criticality == 5);

    const Task *second = &result.tasks[1];
    CHECK(second->id == 2 && second->type == TaskTypePeriodic && second->work == 300);
    CHECK(second->period == 40 && second->deadline == 30 && second->criticality == 0);

    const Task *third = &result.tasks[2];
    CHECK(third->id == 6 && third->type == TaskTypeSporadic);
    CHECK(third->criticality == UINT32_MAX);
}

static void TestHeaderIsOptional(void)
{
    CHECK(LoadText("1,P,10,20,20,1\n") == TaskFileStatusOk);
    CHECK(result.taskCount == 1);
}

static void TestHeaderOnlyAsFirstDataLine(void)
{
    CHECK(LoadText("1,P,10,20,20,1\nid,tipo,W,T,D,criticidad\n") == TaskFileStatusRejectedLines);
    CHECK(result.taskCount == 1 && result.rejectedCount == 1);
    CHECK(result.errors[0].line == 2);
}

static void TestFieldCount(void)
{
    CHECK_LINE_REJECTED("2,P,10,20,20", "only 5 field");
    CHECK_LINE_REJECTED("2", "only 1 field");
    CHECK_LINE_REJECTED("2,P,10,20,20,1,9", "more than 6");
    CHECK_LINE_REJECTED("2,P,10,20,20,1,", "more than 6");
    CHECK_LINE_REJECTED("2;P;10;20;20;1", "only 1 field");
}

static void TestBadNumbers(void)
{
    CHECK_LINE_REJECTED("x,P,10,20,20,1", "id must be");
    CHECK_LINE_REJECTED("-2,P,10,20,20,1", "id must be");
    CHECK_LINE_REJECTED("2,P,,20,20,1", "W must be");
    CHECK_LINE_REJECTED("2,P,1.5,20,20,1", "W must be");
    CHECK_LINE_REJECTED("2,P,10,4294967296,20,1", "T must be");
    CHECK_LINE_REJECTED("2,P,10,20,2 0,1", "D must be");
    CHECK_LINE_REJECTED("2,P,10,20,20,-1", "criticality must be");
}

static void TestBadType(void)
{
    CHECK_LINE_REJECTED("2,X,10,20,20,1", "type must be P or S");
    CHECK_LINE_REJECTED("2,,10,20,20,1", "type must be P or S");
    CHECK_LINE_REJECTED("2,PS,10,20,20,1", "type must be P or S");
}

static void TestTaskRules(void)
{
    CHECK_LINE_REJECTED("0,P,10,20,20,1", "at least 1");
    CHECK_LINE_REJECTED("2,P,0,20,20,1", "greater than 0");
    CHECK_LINE_REJECTED("2,P,10,0,0,1", "greater than 0");
    CHECK_LINE_REJECTED("2,P,10,20,0,1", "greater than 0");
    CHECK_LINE_REJECTED("2,P,10,20,21,1", "must not exceed T");
    CHECK_LINE_REJECTED("1,S,10,20,20,1", "id 1 is used");
}

static void TestBadLinesDoNotStopTheRest(void)
{
    const char *text =
        "1,P,10,20,20,1\n"
        "garbage\n"
        "2,P,10,20,20,1\n"
        "2,P,10,20,20,1\n"
        "3,S,10,20,20,1\n";

    CHECK(LoadText(text) == TaskFileStatusRejectedLines);
    CHECK(result.taskCount == 3 && result.rejectedCount == 2);
    CHECK(result.tasks[2].id == 3);
    CHECK(result.errors[0].line == 2 && result.errors[1].line == 4);
}

static void TestNoValidTask(void)
{
    CHECK(LoadText("# nothing here\n\n") == TaskFileStatusRejectedLines);
    CHECK(result.taskCount == 0 && result.rejectedCount == 1);
    CHECK(result.errors[0].line == 0 && strstr(result.errors[0].message, "no valid task") != NULL);

    CHECK(LoadText("id,tipo,W,T,D,criticidad\nbad\n") == TaskFileStatusRejectedLines);
    CHECK(result.taskCount == 0 && result.rejectedCount == 2);
}

static void TestTooManyTasks(void)
{
    static char text[(TaskFileMaxTasks + 2) * 32];
    size_t used = 0;

    for (int id = 1; id <= TaskFileMaxTasks + 2; id++) {
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%d,P,1,10,10,1\n", id);
    }
    CHECK(LoadText(text) == TaskFileStatusRejectedLines);
    CHECK(result.taskCount == TaskFileMaxTasks && result.rejectedCount == 2);
    CHECK(strstr(result.errors[0].message, "more than") != NULL);
}

static void TestErrorListIsBounded(void)
{
    static char text[(TaskFileMaxErrors + 10) * 8];
    size_t used = 0;

    for (int i = 0; i < TaskFileMaxErrors + 10; i++) {
        used += (size_t)snprintf(text + used, sizeof(text) - used, "bad\n");
    }
    CHECK(LoadText(text) == TaskFileStatusRejectedLines);
    CHECK(result.errorCount == TaskFileMaxErrors);
    CHECK(result.rejectedCount == TaskFileMaxErrors + 10 + 1);
}

static void TestOverlongAndBinaryLines(void)
{
    char text[ParseLineMax + 64];
    const char binary[] = "1,P,10,20,20,1\n2,P,1\0,20,20,1\n3,P,10,20,20,1\n";

    memset(text, '7', sizeof(text));
    text[sizeof(text) - 1] = '\n';
    CHECK(LoadBytes(text, sizeof(text)) == TaskFileStatusRejectedLines);
    CHECK(result.errors[0].line == 1 && strstr(result.errors[0].message, "longer than") != NULL);

    CHECK(LoadBytes(binary, sizeof(binary) - 1) == TaskFileStatusRejectedLines);
    CHECK(result.taskCount == 2 && result.rejectedCount == 1);
    CHECK(result.errors[0].line == 2 && strstr(result.errors[0].message, "NUL") != NULL);
}

static void TestRandomBytesNeverCrash(void)
{
    char bytes[4096];
    uint32_t state = 777;

    for (int round = 0; round < 200; round++) {
        for (size_t i = 0; i < sizeof(bytes); i++) {
            state = state * 1664525u + 1013904223u;
            bytes[i] = (char)(state >> 24);
        }
        TaskFileStatus status = LoadBytes(bytes, sizeof(bytes));
        CHECK(status == TaskFileStatusOk || status == TaskFileStatusRejectedLines);
        CHECK(result.taskCount <= TaskFileMaxTasks && result.errorCount <= TaskFileMaxErrors);
    }
}

static void TestFiles(void)
{
    CHECK(TaskFileLoadFile("tasks/ejemplo.csv", &result) == TaskFileStatusOk);
    CHECK(result.taskCount == 6 && result.rejectedCount == 0);
    CHECK(result.tasks[5].type == TaskTypeSporadic);

    CHECK(TaskFileLoadFile("tasks/does-not-exist.csv", &result) == TaskFileStatusCannotOpen);
    CHECK(result.taskCount == 0 && result.errorCount == 1);
    CHECK(strstr(result.errors[0].message, "does-not-exist") != NULL);

    CHECK(TaskFileLoadFile("tasks", &result) == TaskFileStatusCannotOpen);
}

static void TestNullArguments(void)
{
    CHECK(TaskFileLoadStream(NULL, &result) == TaskFileStatusInvalidArgument);
    CHECK(result.taskCount == 0);
    CHECK(TaskFileLoadStream(stdin, NULL) == TaskFileStatusInvalidArgument);
    CHECK(TaskFileLoadFile(NULL, &result) == TaskFileStatusInvalidArgument);
    CHECK(TaskFileLoadFile("tasks/ejemplo.csv", NULL) == TaskFileStatusInvalidArgument);
}

int main(void)
{
    TestValidFile();
    TestHeaderIsOptional();
    TestHeaderOnlyAsFirstDataLine();
    TestFieldCount();
    TestBadNumbers();
    TestBadType();
    TestTaskRules();
    TestBadLinesDoNotStopTheRest();
    TestNoValidTask();
    TestTooManyTasks();
    TestErrorListIsBounded();
    TestOverlongAndBinaryLines();
    TestRandomBytesNeverCrash();
    TestFiles();
    TestNullArguments();

    if (failureCount == 0) {
        printf("test_taskfile: all passed\n");
        return 0;
    }
    printf("test_taskfile: %d failure(s)\n", failureCount);
    return 1;
}
