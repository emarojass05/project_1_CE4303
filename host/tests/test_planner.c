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

/* One step of a scripted session: after delayMs, type text and/or kill a node */
typedef struct {
    long delayMs;
    const char *text;
    pid_t kill;
} Step;

/* Child that plays the steps in order, like the user (and a pulled cable) */
static pid_t PlayLater(int writeFd, const Step *steps, size_t count)
{
    pid_t child = fork();
    size_t i;

    if (child == 0) {
        for (i = 0; i < count; i++) {
            SleepMilliseconds(steps[i].delayMs);
            if (steps[i].kill > 0) {
                kill(steps[i].kill, SIGKILL);
            }
            if (steps[i].text != NULL) {
                (void)write(writeFd, steps[i].text, strlen(steps[i].text));
            }
        }
        _exit(0);
    }
    return child;
}

/* Runs the planner while the steps are played; output in text */
static int RunPlannerSteps(HostConfig *config, const TaskFile *tasks, const Step *steps,
                           size_t count, char *text, size_t size)
{
    FILE *out = tmpfile();
    int input[2];
    pid_t player;
    int result;

    if (out == NULL || pipe(input) != 0) {
        return -1;
    }
    player = PlayLater(input[1], steps, count);
    result = PlannerRun(config, tasks, input[0], out, false);
    waitpid(player, NULL, 0);
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

static void ConfigTwoNodes(HostConfig *config, AssignHeuristic heuristic)
{
    ConfigSetDefaults(config);
    config->algorithm = AssignAlgorithmEdf;
    config->heuristic = heuristic;
    config->tickReportEvery = 5;    /* A report every 50 ms */
    config->missedReportsForFailure = 3;
    config->portCount = 2;
}

static const char *failureText =
    "id,tipo,W,T,D,criticidad\n"
    "1,P,120,10,10,3\n"   /* Worst-fit: node 1 (C = 2), would cost 5 on node 2 */
    "2,P,100,20,20,2\n"   /* Node 2 */
    "4,S,60,50,50,5\n";   /* Sporadic on node 1 */

/* Killing a node (like pulling its cable) stops its tick reports: it is
   declared down, its periodic task restarts on the other node and its
   sporadic task is rejected */
static void TestNodeLostIsReplanned(void)
{
    static HostConfig config;
    static TaskFile tasks;
    static char text[65536];
    pid_t fast;
    pid_t slow;
    Step steps[2];

    ConfigTwoNodes(&config, AssignHeuristicWorstFit);
    fast = StartFakeNode(config.ports[0], sizeof(config.ports[0]), 60);
    slow = StartFakeNode(config.ports[1], sizeof(config.ports[1]), 25);
    CHECK(fast > 0 && slow > 0);
    LoadTasks(&tasks, failureText);
    steps[0] = (Step){ .delayMs = 500, .kill = fast };
    steps[1] = (Step){ .delayMs = 900, .text = "q" };

    CHECK(RunPlannerSteps(&config, &tasks, steps, 2, text, sizeof(text)) == 0);
    CHECK(strstr(text, "task 1: node 1, C = 2 ticks") != NULL);
    CHECK(strstr(text, "task 2: node 2, C = 4 ticks") != NULL);
    CHECK(strstr(text, "task 4: node 1") != NULL);
    CHECK(strstr(text, "Node 1 DOWN") != NULL);
    CHECK(strstr(text, "task 1: restarted on node 2, C = 5 ticks") != NULL);
    CHECK(strstr(text, "task 4: rejected, sporadic task of a node that went down") != NULL);
    CHECK(strstr(text, "tick reports stopped") != NULL);
    CHECK(strstr(text, "relocated): accepted on node 2, C = 5") != NULL);
    CHECK(strstr(text, "relocated): rejected, sporadic task of a node that went down") != NULL);
    CHECK(strstr(text, "Node 2:\n  Task") != NULL);
    if (failureCount != 0) {
        printf("%s", text);
    }
    StopChild(fast);
    StopChild(slow);
}

static const char *criticalityText =
    "id,tipo,W,T,D,criticidad\n"
    "1,P,240,10,10,1\n"   /* U 0.4 on node 1, 1.0 on node 2 */
    "2,P,240,10,10,3\n"
    "3,P,25,10,10,2\n";   /* U 0.1 on both */

/* k takes node 1 down; only one task fits on node 2: the most critical one
   is kept and the other two are rejected */
static void TestCommandDownRejectsLowCriticality(void)
{
    static HostConfig config;
    static TaskFile tasks;
    static char text[65536];
    pid_t fast;
    pid_t slow;
    Step steps[2];

    ConfigTwoNodes(&config, AssignHeuristicFirstFit);
    fast = StartFakeNode(config.ports[0], sizeof(config.ports[0]), 60);
    slow = StartFakeNode(config.ports[1], sizeof(config.ports[1]), 25);
    CHECK(fast > 0 && slow > 0);
    LoadTasks(&tasks, criticalityText);
    steps[0] = (Step){ .delayMs = 400, .text = "k1\n" };
    steps[1] = (Step){ .delayMs = 900, .text = "q" };

    CHECK(RunPlannerSteps(&config, &tasks, steps, 2, text, sizeof(text)) == 0);
    CHECK(strstr(text, "task 1: node 1") != NULL);
    CHECK(strstr(text, "task 2: node 1") != NULL);
    CHECK(strstr(text, "task 3: node 1") != NULL);
    CHECK(strstr(text, "Node 1 stopped; it is declared down") != NULL);
    CHECK(strstr(text, "Node 1 DOWN") != NULL);
    CHECK(strstr(text, "task 2: restarted on node 2, C = 10 ticks") != NULL);
    CHECK(strstr(text, "task 3 (criticality 2): rejected") != NULL);
    CHECK(strstr(text, "task 1 (criticality 1): rejected") != NULL);
    CHECK(strstr(text, "k command") != NULL);
    /* The order of the replanning: criticality 3, then 2, then 1 */
    CHECK(strstr(text, "task 2: restarted") < strstr(text, "task 3 (criticality 2)"));
    CHECK(strstr(text, "task 3 (criticality 2)") < strstr(text, "task 1 (criticality 1)"));
    if (failureCount != 0) {
        printf("%s", text);
    }
    StopChild(fast);
    StopChild(slow);
}

/* Manual mode: a task typed during the run is admitted and deployed without
   stopping; invalid, repeated and unschedulable tasks are rejected; s
   releases the sporadic task */
static void TestManualModeAndSporadicCommand(void)
{
    static HostConfig config;
    static TaskFile tasks;
    static char text[65536];
    pid_t fast;
    pid_t slow;
    Step steps[6];

    ConfigTwoNodes(&config, AssignHeuristicWorstFit);
    fast = StartFakeNode(config.ports[0], sizeof(config.ports[0]), 60);
    slow = StartFakeNode(config.ports[1], sizeof(config.ports[1]), 25);
    CHECK(fast > 0 && slow > 0);
    LoadTasks(&tasks, failureText);
    steps[0] = (Step){ .delayMs = 300, .text = "a 9,P,100,20,20,1\n" };
    steps[1] = (Step){ .delayMs = 50, .text = "a 2,P,100,20,20,1\n" };
    steps[2] = (Step){ .delayMs = 50, .text = "a 10,X,1,1,1,1\n" };
    steps[3] = (Step){ .delayMs = 50, .text = "a 11,P,9000,10,10,1\n" };
    steps[4] = (Step){ .delayMs = 50, .text = "s 1\ns 2\nh" };
    steps[5] = (Step){ .delayMs = 400, .text = "q" };

    CHECK(RunPlannerSteps(&config, &tasks, steps, 6, text, sizeof(text)) == 0);
    CHECK(strstr(text, "task 9: added to node") != NULL);
    CHECK(strstr(text, "task 2: rejected, id already in use") != NULL);
    CHECK(strstr(text, "Task not added: type must be P or S") != NULL);
    CHECK(strstr(text, "task 11: rejected, fails the admission test") != NULL);
    CHECK(strstr(text, "Node 1: sporadic task 4 released") != NULL);
    CHECK(strstr(text, "Node 2 has no sporadic task") != NULL);
    CHECK(strstr(text, "Commands:") != NULL);
    CHECK(strstr(text, "task 9 (P, W=100, T=20, D=20, crit=1, manual): accepted") != NULL);
    CHECK(strstr(text, "Node 1 task 4: 1 release(s) accepted") != NULL);
    CHECK(strstr(text, "latency from release to completion") != NULL);
    CHECK(strstr(text, "DOWN") == NULL);
    CHECK(strstr(text, "Node 2 vs node 1:") != NULL);
    if (failureCount != 0) {
        printf("%s", text);
    }
    StopChild(fast);
    StopChild(slow);
}

int main(void)
{
    TestTwoNodesEndToEnd();
    TestMissingNodeIsLeftOut();
    TestNoNodeAtAll();
    TestNodeLostIsReplanned();
    TestCommandDownRejectsLowCriticality();
    TestManualModeAndSporadicCommand();

    if (failureCount != 0) {
        printf("test_planner: %d check(s) failed\n", failureCount);
        return 1;
    }
    printf("test_planner: all checks passed\n");
    return 0;
}
