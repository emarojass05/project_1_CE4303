#include <stdint.h>
#include <stdio.h>

#include "task.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

/* Valid periodic task with the given work */
static Task MakeTask(uint32_t work)
{
    Task task = { .id = 1, .type = TaskTypePeriodic, .work = work,
                  .period = 20, .deadline = 20, .criticality = 1 };
    return task;
}

static void TestCostSpecExample(void)
{
    Task task = MakeTask(40);
    uint32_t cost = 0;

    CHECK(TaskCost(&task, 50, &cost) == TaskStatusOk && cost == 1);
    CHECK(TaskCost(&task, 20, &cost) == TaskStatusOk && cost == 2);
}

static void TestCostCeiling(void)
{
    Task task = MakeTask(40);
    uint32_t cost = 0;

    CHECK(TaskCost(&task, 40, &cost) == TaskStatusOk && cost == 1);
    task.work = 41;
    CHECK(TaskCost(&task, 40, &cost) == TaskStatusOk && cost == 2);
    task.work = 1;
    CHECK(TaskCost(&task, 1000, &cost) == TaskStatusOk && cost == 1);
}

static void TestCostExtremes(void)
{
    Task task = MakeTask(UINT32_MAX);
    uint32_t cost = 0;

    CHECK(TaskCost(&task, 1, &cost) == TaskStatusOk && cost == UINT32_MAX);
    CHECK(TaskCost(&task, UINT32_MAX, &cost) == TaskStatusOk && cost == 1);
}

static void TestCostInvalidInput(void)
{
    Task task = MakeTask(40);
    uint32_t cost = 77;

    CHECK(TaskCost(&task, 0, &cost) == TaskStatusInvalidArgument);
    CHECK(cost == 77);
    CHECK(TaskCost(NULL, 10, &cost) == TaskStatusInvalidArgument);
    CHECK(TaskCost(&task, 10, NULL) == TaskStatusInvalidArgument);
    task.work = 0;
    CHECK(TaskCost(&task, 10, &cost) == TaskStatusInvalidTask);
}

static void TestValidate(void)
{
    Task task = MakeTask(40);

    CHECK(TaskValidate(&task) == TaskStatusOk);
    CHECK(TaskValidate(NULL) == TaskStatusInvalidArgument);

    task = MakeTask(0);
    CHECK(TaskValidate(&task) == TaskStatusInvalidTask);
    task = MakeTask(40);
    task.period = 0;
    CHECK(TaskValidate(&task) == TaskStatusInvalidTask);
    task = MakeTask(40);
    task.deadline = 0;
    CHECK(TaskValidate(&task) == TaskStatusInvalidTask);
    task = MakeTask(40);
    task.type = (TaskType)99;
    CHECK(TaskValidate(&task) == TaskStatusInvalidTask);
}

int main(void)
{
    TestCostSpecExample();
    TestCostCeiling();
    TestCostExtremes();
    TestCostInvalidInput();
    TestValidate();

    if (failureCount == 0) {
        printf("test_task: all passed\n");
        return 0;
    }
    printf("test_task: %d failure(s)\n", failureCount);
    return 1;
}