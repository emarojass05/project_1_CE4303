#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

#define REQUIRED_KEYS                                                  \
    "algorithm = RMS\n"                                                \
    "heuristic = first-fit\n"                                          \
    "ports = /dev/ttyUSB0\n"                                           \
    "tasks_file = tasks/a.csv\n"

static ConfigStatus LoadBytes(const char *bytes, size_t length, HostConfig *config,
                              ParseError *error)
{
    FILE *input = fmemopen((void *)bytes, length, "r");
    ConfigStatus status;

    CHECK(input != NULL);
    if (input == NULL) {
        return ConfigStatusCannotOpen;
    }
    status = ConfigLoadStream(input, config, error);
    fclose(input);
    return status;
}

static ConfigStatus LoadText(const char *text, HostConfig *config, ParseError *error)
{
    return LoadBytes(text, strlen(text), config, error);
}

/* The text must be rejected, the error must point at the given line and
   mention the given word, and the previous configuration must survive */
static void CheckRejected(const char *text, size_t expectedLine, const char *expectedWord,
                          int callerLine)
{
    HostConfig config;
    HostConfig before;
    ParseError error = { 0 };

    ConfigSetDefaults(&config);
    config.tickMs = 77;
    before = config;

    if (LoadText(text, &config, &error) != ConfigStatusInvalid ||
        error.line != expectedLine || strstr(error.message, expectedWord) == NULL ||
        memcmp(&config, &before, sizeof(config)) != 0) {
        printf("  FAILED line %d: expected rejection at line %zu mentioning '%s', "
               "got line %zu '%s'\n",
               callerLine, expectedLine, expectedWord, error.line, error.message);
        failureCount++;
    }
}

#define CHECK_REJECTED(text, line, word) CheckRejected((text), (line), (word), __LINE__)

static void TestFullConfig(void)
{
    HostConfig config;
    ParseError error = { 0 };
    const char *text =
        "# comment line\r\n"
        "\r\n"
        "  algorithm =   edf   # inline comment\r\n"
        "heuristic=Worst-Fit\r\n"
        "tick_ms = 20\r\n"
        "ports = /dev/ttyUSB0 ,/dev/ttyACM0,  /dev/ttyUSB1\r\n"
        "tick_report_every = 65535\r\n"
        "missed_reports_for_failure = 5\r\n"
        "tasks_file = tasks/set 1.csv\r\n";

    CHECK(LoadText(text, &config, &error) == ConfigStatusOk);
    CHECK(config.algorithm == AssignAlgorithmEdf);
    CHECK(config.heuristic == AssignHeuristicWorstFit);
    CHECK(config.tickMs == 20);
    CHECK(config.portCount == 3);
    CHECK(strcmp(config.ports[0], "/dev/ttyUSB0") == 0);
    CHECK(strcmp(config.ports[1], "/dev/ttyACM0") == 0);
    CHECK(strcmp(config.ports[2], "/dev/ttyUSB1") == 0);
    CHECK(config.tickReportEvery == 65535);
    CHECK(config.missedReportsForFailure == 5);
    CHECK(strcmp(config.tasksFile, "tasks/set 1.csv") == 0);
}

static void TestDefaultsForOptionalKeys(void)
{
    HostConfig config;
    ParseError error = { 0 };

    CHECK(LoadText(REQUIRED_KEYS, &config, &error) == ConfigStatusOk);
    CHECK(config.algorithm == AssignAlgorithmRms);
    CHECK(config.heuristic == AssignHeuristicFirstFit);
    CHECK(config.tickMs == ConfigDefaultTickMs);
    CHECK(config.tickReportEvery == ConfigDefaultTickReportEvery);
    CHECK(config.missedReportsForFailure == ConfigDefaultMissedReports);
    CHECK(config.portCount == 1);
}

static void TestLastLineWithoutNewline(void)
{
    HostConfig config;
    ParseError error = { 0 };

    CHECK(LoadText(REQUIRED_KEYS "tick_ms = 5", &config, &error) == ConfigStatusOk);
    CHECK(config.tickMs == 5);
}

static void TestMissingRequiredKeys(void)
{
    CHECK_REJECTED("heuristic = first-fit\nports = a\ntasks_file = t\n", 0, "algorithm");
    CHECK_REJECTED("algorithm = RMS\nports = a\ntasks_file = t\n", 0, "heuristic");
    CHECK_REJECTED("algorithm = RMS\nheuristic = first-fit\ntasks_file = t\n", 0, "ports");
    CHECK_REJECTED("algorithm = RMS\nheuristic = first-fit\nports = a\n", 0, "tasks_file");
    CHECK_REJECTED("# only a comment\n\n", 0, "algorithm");
}

static void TestMalformedLines(void)
{
    CHECK_REJECTED(REQUIRED_KEYS "speed = 3\n", 5, "unknown key");
    CHECK_REJECTED(REQUIRED_KEYS "tick_ms 10\n", 5, "key = value");
    CHECK_REJECTED(REQUIRED_KEYS "= 10\n", 5, "unknown key");
    CHECK_REJECTED(REQUIRED_KEYS "tick_ms =\n", 5, "no value");
    CHECK_REJECTED(REQUIRED_KEYS "tick_ms = # nothing\n", 5, "no value");
    CHECK_REJECTED(REQUIRED_KEYS "algorithm = EDF\n", 5, "twice");
    CHECK_REJECTED("TICK_MS = 10\n" REQUIRED_KEYS, 1, "unknown key");
}

static void TestInvalidChoices(void)
{
    CHECK_REJECTED("algorithm = FIFO\n", 1, "RMS or EDF");
    CHECK_REJECTED("algorithm = RMS EDF\n", 1, "RMS or EDF");
    CHECK_REJECTED("heuristic = best-fit\n", 1, "first-fit or worst-fit");
}

static void TestInvalidNumbers(void)
{
    CHECK_REJECTED("tick_ms = 0\n", 1, "tick_ms");
    CHECK_REJECTED("tick_ms = -5\n", 1, "tick_ms");
    CHECK_REJECTED("tick_ms = +5\n", 1, "tick_ms");
    CHECK_REJECTED("tick_ms = 1001\n", 1, "tick_ms");
    CHECK_REJECTED("tick_ms = 10ms\n", 1, "tick_ms");
    CHECK_REJECTED("tick_ms = 1 0\n", 1, "tick_ms");
    CHECK_REJECTED("tick_ms = 0x10\n", 1, "tick_ms");
    CHECK_REJECTED("tick_ms = 99999999999999999999999\n", 1, "tick_ms");
    CHECK_REJECTED("tick_report_every = 65536\n", 1, "tick_report_every");
    CHECK_REJECTED("tick_report_every = 0\n", 1, "tick_report_every");
    CHECK_REJECTED("missed_reports_for_failure = 0\n", 1, "missed_reports_for_failure");
    CHECK_REJECTED("missed_reports_for_failure = 4294967296\n", 1, "missed_reports_for_failure");
}

static void TestInvalidPorts(void)
{
    char longPort[ConfigPortMaxLength + 32];

    CHECK_REJECTED("ports = a,,b\n", 1, "empty entry");
    CHECK_REJECTED("ports = a,\n", 1, "empty entry");
    CHECK_REJECTED("ports = ,a\n", 1, "empty entry");
    CHECK_REJECTED("ports = a, b, a\n", 1, "twice");
    CHECK_REJECTED("ports = 1,2,3,4,5,6,7,8,9\n", 1, "more than");

    snprintf(longPort, sizeof(longPort), "ports = /dev/%0*d\n", ConfigPortMaxLength, 0);
    CHECK_REJECTED(longPort, 1, "longer than");
}

static void TestMaximumPortCount(void)
{
    HostConfig config;
    ParseError error = { 0 };

    CHECK(LoadText("algorithm = RMS\nheuristic = first-fit\ntasks_file = t\n"
                   "ports = 1,2,3,4,5,6,7,8\n", &config, &error) == ConfigStatusOk);
    CHECK(config.portCount == ConfigMaxPorts);
    CHECK(strcmp(config.ports[7], "8") == 0);
}

static void TestOverlongLine(void)
{
    char text[ParseLineMax + 64];
    HostConfig config;
    ParseError error = { 0 };

    memset(text, 'x', sizeof(text));
    memcpy(text, "tasks_file = ", 13);
    text[sizeof(text) - 1] = '\n';
    CHECK(LoadBytes(text, sizeof(text), &config, &error) == ConfigStatusInvalid);
    CHECK(error.line == 1 && strstr(error.message, "longer than") != NULL);
}

static void TestNulByte(void)
{
    const char text[] = "algorithm = R\0MS\n";
    HostConfig config;
    ParseError error = { 0 };

    CHECK(LoadBytes(text, sizeof(text) - 1, &config, &error) == ConfigStatusInvalid);
    CHECK(error.line == 1 && strstr(error.message, "NUL") != NULL);
}

static void TestRandomBytesNeverCrash(void)
{
    char bytes[2048];
    uint32_t state = 12345;
    HostConfig config;
    ParseError error;

    for (int round = 0; round < 200; round++) {
        for (size_t i = 0; i < sizeof(bytes); i++) {
            state = state * 1664525u + 1013904223u;
            bytes[i] = (char)(state >> 24);
        }
        CHECK(LoadBytes(bytes, sizeof(bytes), &config, &error) != ConfigStatusOk);
    }
}

static void TestFiles(void)
{
    HostConfig config;
    ParseError error = { 0 };

    CHECK(ConfigLoadFile("config/ejemplo.conf", &config, &error) == ConfigStatusOk);
    CHECK(config.algorithm == AssignAlgorithmEdf);
    CHECK(config.heuristic == AssignHeuristicWorstFit);
    CHECK(config.portCount == 2);
    CHECK(strcmp(config.tasksFile, "tasks/ejemplo.csv") == 0);

    CHECK(ConfigLoadFile("config/does-not-exist.conf", &config, &error) == ConfigStatusCannotOpen);
    CHECK(strstr(error.message, "does-not-exist") != NULL);
    CHECK(ConfigLoadFile("config", &config, &error) == ConfigStatusCannotOpen);
}

static void TestNullArguments(void)
{
    HostConfig config;
    ParseError error;

    CHECK(ConfigLoadStream(NULL, &config, &error) == ConfigStatusInvalidArgument);
    CHECK(ConfigLoadFile(NULL, &config, &error) == ConfigStatusInvalidArgument);
    CHECK(ConfigLoadFile("config/ejemplo.conf", NULL, &error) == ConfigStatusInvalidArgument);
    CHECK(ConfigLoadFile("config/ejemplo.conf", &config, NULL) == ConfigStatusOk);
    CHECK(LoadText("algorithm = nope\n", &config, NULL) == ConfigStatusInvalid);
    ConfigSetDefaults(NULL);
}

int main(void)
{
    TestFullConfig();
    TestDefaultsForOptionalKeys();
    TestLastLineWithoutNewline();
    TestMissingRequiredKeys();
    TestMalformedLines();
    TestInvalidChoices();
    TestInvalidNumbers();
    TestInvalidPorts();
    TestMaximumPortCount();
    TestOverlongLine();
    TestNulByte();
    TestRandomBytesNeverCrash();
    TestFiles();
    TestNullArguments();

    if (failureCount == 0) {
        printf("test_config: all passed\n");
        return 0;
    }
    printf("test_config: %d failure(s)\n", failureCount);
    return 1;
}
