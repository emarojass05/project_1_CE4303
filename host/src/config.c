#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

typedef bool (*ValueSetter)(HostConfig *config, char *value, size_t line, ParseError *error);

typedef struct {
    const char *name;
    ValueSetter set;
    bool required;
} ConfigKey;

static bool SetNumber(uint32_t *target, const char *value, uint32_t min, uint32_t max,
                      const char *key, size_t line, ParseError *error)
{
    uint32_t number;

    if (!ParseUint32(value, &number) || number < min || number > max) {
        ParseErrorSet(error, line, "'%s' must be a whole number from %u to %u, got '%s'",
                      key, (unsigned)min, (unsigned)max, value);
        return false;
    }
    *target = number;
    return true;
}

static bool SetAlgorithm(HostConfig *config, char *value, size_t line, ParseError *error)
{
    if (strcasecmp(value, "RMS") == 0) {
        config->algorithm = AssignAlgorithmRms;
    } else if (strcasecmp(value, "EDF") == 0) {
        config->algorithm = AssignAlgorithmEdf;
    } else {
        ParseErrorSet(error, line, "'algorithm' must be RMS or EDF, got '%s'", value);
        return false;
    }
    return true;
}

static bool SetHeuristic(HostConfig *config, char *value, size_t line, ParseError *error)
{
    if (strcasecmp(value, "first-fit") == 0) {
        config->heuristic = AssignHeuristicFirstFit;
    } else if (strcasecmp(value, "worst-fit") == 0) {
        config->heuristic = AssignHeuristicWorstFit;
    } else {
        ParseErrorSet(error, line, "'heuristic' must be first-fit or worst-fit, got '%s'", value);
        return false;
    }
    return true;
}

static bool SetAdmission(HostConfig *config, char *value, size_t line, ParseError *error)
{
    if (strcasecmp(value, "on") == 0) {
        config->admissionCheck = true;
    } else if (strcasecmp(value, "off") == 0) {
        config->admissionCheck = false;
    } else {
        ParseErrorSet(error, line, "'admission' must be on or off, got '%s'", value);
        return false;
    }
    return true;
}

static bool SetTickMs(HostConfig *config, char *value, size_t line, ParseError *error)
{
    return SetNumber(&config->tickMs, value, 1, ConfigMaxTickMs, "tick_ms", line, error);
}

static bool SetTickReportEvery(HostConfig *config, char *value, size_t line, ParseError *error)
{
    return SetNumber(&config->tickReportEvery, value, 1, ConfigMaxTickReportEvery,
                     "tick_report_every", line, error);
}

static bool SetMissedReports(HostConfig *config, char *value, size_t line, ParseError *error)
{
    return SetNumber(&config->missedReportsForFailure, value, 1, UINT32_MAX,
                     "missed_reports_for_failure", line, error);
}

static bool IsKnownPort(const HostConfig *config, const char *port)
{
    for (size_t i = 0; i < config->portCount; i++) {
        if (strcmp(config->ports[i], port) == 0) {
            return true;
        }
    }
    return false;
}

static bool AddPort(HostConfig *config, char *entry, size_t line, ParseError *error)
{
    char *port = ParseTrim(entry);

    if (*port == '\0') {
        ParseErrorSet(error, line, "'ports' has an empty entry");
        return false;
    }
    if (strlen(port) >= ConfigPortMaxLength) {
        ParseErrorSet(error, line, "port name longer than %d characters", ConfigPortMaxLength - 1);
        return false;
    }
    if (IsKnownPort(config, port)) {
        ParseErrorSet(error, line, "port '%s' is listed twice", port);
        return false;
    }
    if (config->portCount >= ConfigMaxPorts) {
        ParseErrorSet(error, line, "'ports' lists more than %d nodes", ConfigMaxPorts);
        return false;
    }
    strcpy(config->ports[config->portCount++], port);
    return true;
}

static bool SetPorts(HostConfig *config, char *value, size_t line, ParseError *error)
{
    char *entry = value;

    for (;;) {
        char *comma = strchr(entry, ',');

        if (comma != NULL) {
            *comma = '\0';
        }
        if (!AddPort(config, entry, line, error)) {
            return false;
        }
        if (comma == NULL) {
            return true;
        }
        entry = comma + 1;
    }
}

static bool SetTasksFile(HostConfig *config, char *value, size_t line, ParseError *error)
{
    if (strlen(value) >= ConfigPathMaxLength) {
        ParseErrorSet(error, line, "'tasks_file' longer than %d characters", ConfigPathMaxLength - 1);
        return false;
    }
    strcpy(config->tasksFile, value);
    return true;
}

static const ConfigKey configKeys[] = {
    { "algorithm", SetAlgorithm, true },
    { "heuristic", SetHeuristic, true },
    { "tick_ms", SetTickMs, false },
    { "ports", SetPorts, true },
    { "tick_report_every", SetTickReportEvery, false },
    { "missed_reports_for_failure", SetMissedReports, false },
    { "tasks_file", SetTasksFile, true },
    { "admission", SetAdmission, false },
};

enum {
    ConfigKeyCount = sizeof(configKeys) / sizeof(configKeys[0])
};

static int FindKey(const char *name)
{
    for (int i = 0; i < ConfigKeyCount; i++) {
        if (strcmp(configKeys[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool ApplyLine(HostConfig *config, bool seen[ConfigKeyCount], char *text, size_t line,
                      ParseError *error)
{
    ParseStripComment(text);
    text = ParseTrim(text);
    if (*text == '\0') {
        return true;
    }

    char *equals = strchr(text, '=');
    if (equals == NULL) {
        ParseErrorSet(error, line, "expected 'key = value'");
        return false;
    }
    *equals = '\0';
    char *name = ParseTrim(text);
    char *value = ParseTrim(equals + 1);

    int index = FindKey(name);
    if (index < 0) {
        ParseErrorSet(error, line, "unknown key '%s'", name);
        return false;
    }
    if (seen[index]) {
        ParseErrorSet(error, line, "key '%s' appears twice", name);
        return false;
    }
    if (*value == '\0') {
        ParseErrorSet(error, line, "key '%s' has no value", name);
        return false;
    }
    seen[index] = true;
    return configKeys[index].set(config, value, line, error);
}

static bool HasRequiredKeys(const bool seen[ConfigKeyCount], ParseError *error)
{
    for (int i = 0; i < ConfigKeyCount; i++) {
        if (configKeys[i].required && !seen[i]) {
            ParseErrorSet(error, 0, "missing required key '%s'", configKeys[i].name);
            return false;
        }
    }
    return true;
}

void ConfigSetDefaults(HostConfig *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->algorithm = AssignAlgorithmEdf;
    config->heuristic = AssignHeuristicFirstFit;
    config->admissionCheck = true;
    config->tickMs = ConfigDefaultTickMs;
    config->tickReportEvery = ConfigDefaultTickReportEvery;
    config->missedReportsForFailure = ConfigDefaultMissedReports;
}

ConfigStatus ConfigLoadStream(FILE *input, HostConfig *config, ParseError *error)
{
    HostConfig loaded;
    bool seen[ConfigKeyCount] = { false };
    char buffer[ParseLineMax + 1];
    size_t line = 0;

    if (input == NULL || config == NULL) {
        ParseErrorSet(error, 0, "null argument");
        return ConfigStatusInvalidArgument;
    }
    ConfigSetDefaults(&loaded);

    for (;;) {
        LineRead result = ParseReadLine(input, buffer);

        if (result == LineReadEnd) {
            break;
        }
        line++;
        if (!ParseLineUsable(result, line, error) ||
            !ApplyLine(&loaded, seen, buffer, line, error)) {
            return ConfigStatusInvalid;
        }
    }
    if (ferror(input)) {
        ParseErrorSet(error, line, "read error");
        return ConfigStatusCannotOpen;
    }
    if (!HasRequiredKeys(seen, error)) {
        return ConfigStatusInvalid;
    }

    *config = loaded;
    return ConfigStatusOk;
}

ConfigStatus ConfigLoadFile(const char *path, HostConfig *config, ParseError *error)
{
    if (path == NULL || config == NULL) {
        ParseErrorSet(error, 0, "null argument");
        return ConfigStatusInvalidArgument;
    }

    FILE *input = fopen(path, "r");
    if (input == NULL) {
        ParseErrorSet(error, 0, "cannot open '%s': %s", path, strerror(errno));
        return ConfigStatusCannotOpen;
    }
    ConfigStatus status = ConfigLoadStream(input, config, error);
    fclose(input);
    return status;
}
