#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "fake_node.h"
#include "planner.h"
#include "taskfile.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static void SleepMilliseconds(long milliseconds)
{
    struct timespec pause = { .tv_sec = milliseconds / 1000,
                              .tv_nsec = (milliseconds % 1000) * 1000000L };
    nanosleep(&pause, NULL);
}

static pid_t StartFakeNode(char *path, unsigned int size, uint32_t unitsPerTick)
{
    pid_t child;
    int fd = FakeNodeOpen(path, size);

    if (fd < 0) {
        return -1;
    }
    child = fork();
    if (child == 0) {
        _exit(FakeNodeServe(fd, unitsPerTick));
    }
    close(fd);
    return child;
}

static void StopChild(pid_t child)
{
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
}

/* Child that types q after a while, like the user */
static pid_t TypeQLater(int writeFd, long milliseconds)
{
    pid_t child = fork();

    if (child == 0) {
        SleepMilliseconds(milliseconds);
        (void)write(writeFd, "q", 1);
        _exit(0);
    }
    return child;
}

static void LoadTasks(TaskFile *tasks, const char *text)
{
    FILE *input = tmpfile();

    if (input == NULL) {
        return;
    }
    fputs(text, input);
    rewind(input);
    (void)TaskFileLoadStream(input, tasks);
    fclose(input);
}

static void ReadAll(FILE *file, char *text, size_t size)
{
    size_t length;

    fflush(file);
    rewind(file);
    length = fread(text, 1, size - 1, file);
    text[length] = '\0';
}

/* Runs the planner with the given ports and returns its exit value; output in text */
static int RunPlanner(HostConfig *config, const TaskFile *tasks, char *text, size_t size)
{
    FILE *out = tmpfile();
    int input[2];
    pid_t typist;
    int result;

    if (out == NULL || pipe(input) != 0) {
        return -1;
    }
    typist = TypeQLater(input[1], 400);
    result = PlannerRun(config, tasks, input[0], out, false);
    waitpid(typist, NULL, 0);
    close(input[0]);
    close(input[1]);
    ReadAll(out, text, size);
    fclose(out);
    return result;
}

static const char *taskText =
    "id,tipo,W,T,D,criticidad\n"
    "1,P,120,10,10,3\n"   /* C = 2 on the fast node, 5 on the slow one */
    "2,P,100,20,20,2\n"
    "3,P,5000,10,10,1\n"; /* Too long for both nodes */

/* Two simulated nodes of different speed: calibrate, assign, run, report */
static void TestTwoNodesEndToEnd(void)
{
    static HostConfig config;
    static TaskFile tasks;
    static char text[16384];
    pid_t fast;
    pid_t slow;

    ConfigSetDefaults(&config);
    config.algorithm = AssignAlgorithmEdf;
    config.heuristic = AssignHeuristicFirstFit;
    config.tickReportEvery = 5;
    config.portCount = 2;
    fast = StartFakeNode(config.ports[0], sizeof(config.ports[0]), 60);
    slow = StartFakeNode(config.ports[1], sizeof(config.ports[1]), 25);
    CHECK(fast > 0 && slow > 0);
    LoadTasks(&tasks, taskText);
    CHECK(tasks.taskCount == 3);

    CHECK(RunPlanner(&config, &tasks, text, sizeof(text)) == 0);
    CHECK(strstr(text, "Node 1 (") != NULL && strstr(text, "sn = 60 work units") != NULL);
    CHECK(strstr(text, "sn = 25 work units") != NULL);
    CHECK(strstr(text, "task 1: node 1, C = 2 ticks") != NULL);
    CHECK(strstr(text, "task 2: node 1, C = 2 ticks") != NULL); /* First-fit: fastest node */
    CHECK(strstr(text, "task 3: rejected, fails the admission test") != NULL);
    CHECK(strstr(text, "Running.") != NULL);
    CHECK(strstr(text, "===== Report =====") != NULL);
    CHECK(strstr(text, "Events received: 0") == NULL);
    CHECK(strstr(text, "Events ignored: 0") != NULL);
    CHECK(strstr(text, "corrupt frames 0") != NULL);
    if (failureCount != 0) {
        printf("%s", text);
    }

    /* A second run against the same nodes, which kept running, works too */
    CHECK(RunPlanner(&config, &tasks, text, sizeof(text)) == 0);
    CHECK(strstr(text, "task 1: node 1, C = 2 ticks") != NULL);
    CHECK(strstr(text, "Events received: 0") == NULL);

    StopChild(fast);
    StopChild(slow);
}

/* A port that does not exist leaves that node out; the other one runs everything */
static void TestMissingNodeIsLeftOut(void)
{
    static HostConfig config;
    static TaskFile tasks;
    static char text[16384];
    pid_t node;

    ConfigSetDefaults(&config);
    config.algorithm = AssignAlgorithmRms;
    config.heuristic = AssignHeuristicWorstFit;
    config.portCount = 2;
    snprintf(config.ports[0], sizeof(config.ports[0]), "/dev/no-such-port");
    node = StartFakeNode(config.ports[1], sizeof(config.ports[1]), 60);
    LoadTasks(&tasks, taskText);

    CHECK(RunPlanner(&config, &tasks, text, sizeof(text)) == 0);
    CHECK(strstr(text, "Node 1 (/dev/no-such-port): not available, left out") != NULL);
    CHECK(strstr(text, "task 1: node 2") != NULL);
    CHECK(strstr(text, "task 2: node 2") != NULL);
    CHECK(strstr(text, "Events received: 0") == NULL);
    StopChild(node);
}

static void TestNoNodeAtAll(void)
{
    static HostConfig config;
    static TaskFile tasks;
    static char text[16384];

    ConfigSetDefaults(&config);
    config.portCount = 1;
    snprintf(config.ports[0], sizeof(config.ports[0]), "/dev/no-such-port");
    LoadTasks(&tasks, taskText);
    CHECK(RunPlanner(&config, &tasks, text, sizeof(text)) != 0);
    CHECK(strstr(text, "No node answered") != NULL);
}

int main(void)
{
    TestTwoNodesEndToEnd();
    TestMissingNodeIsLeftOut();
    TestNoNodeAtAll();

    if (failureCount != 0) {
        printf("test_planner: %d check(s) failed\n", failureCount);
        return 1;
    }
    printf("test_planner: all checks passed\n");
    return 0;
}
