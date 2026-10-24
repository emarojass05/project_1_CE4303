#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "assign.h"
#include "parse_util.h"

/* Host configuration file: one "key = value" per line, '#' starts a comment.
   See config/ejemplo.conf for every key. Relative paths are taken from the
   directory where the host is started. */

enum {
    ConfigMaxPorts = AssignMaxNodes,
    ConfigPortMaxLength = 64,
    ConfigPathMaxLength = 256,
    ConfigDefaultTickMs = 10,
    ConfigDefaultTickReportEvery = 10,
    ConfigDefaultMissedReports = 3,
    ConfigMaxTickMs = 1000,
    ConfigMaxTickReportEvery = UINT16_MAX /* Sent as u16 in DeployConfig */
};

typedef struct {
    AssignAlgorithm algorithm;
    AssignHeuristic heuristic;
    uint32_t tickMs;
    char ports[ConfigMaxPorts][ConfigPortMaxLength]; /* One serial port per node */
    size_t portCount;
    uint32_t tickReportEvery;         /* Node sends a tick report every N ticks */
    uint32_t missedReportsForFailure; /* Consecutive missing reports that mark a node as down */
    char tasksFile[ConfigPathMaxLength];
} HostConfig;

typedef enum {
    ConfigStatusOk = 0,
    ConfigStatusInvalidArgument = -1, /* Null pointer */
    ConfigStatusCannotOpen = -2,      /* File missing or unreadable */
    ConfigStatusInvalid = -3          /* Content rejected; see the ParseError */
} ConfigStatus;

/* Fills every optional key with its default and clears the required ones */
void ConfigSetDefaults(HostConfig *config);

/* Parses input and stops at the first problem, described in error (may be NULL).
   algorithm, heuristic, ports and tasks_file are required. On failure config
   is left unchanged. */
ConfigStatus ConfigLoadStream(FILE *input, HostConfig *config, ParseError *error);

/* Same as ConfigLoadStream, reading the file at path */
ConfigStatus ConfigLoadFile(const char *path, HostConfig *config, ParseError *error);

#endif /* CONFIG_H */
