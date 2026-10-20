#include "parse_util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* getc instead of fgets: fgets cannot tell a NUL byte inside the line from
   the end of the string, and a binary file must be rejected, not truncated. */
LineRead ParseReadLine(FILE *input, char buffer[ParseLineMax + 1])
{
    size_t length = 0;
    bool tooLong = false;
    bool binary = false;
    int c = getc(input);

    if (c == EOF) {
        return LineReadEnd;
    }
    while (c != EOF && c != '\n') {
        if (c == '\0') {
            binary = true;
        } else if (length < ParseLineMax) {
            buffer[length++] = (char)c;
        } else {
            tooLong = true;
        }
        c = getc(input);
    }
    buffer[length] = '\0';

    if (binary) {
        return LineReadBinary;
    }
    return tooLong ? LineReadTooLong : LineReadOk;
}

void ParseStripComment(char *text)
{
    char *mark = strchr(text, '#');

    if (mark != NULL) {
        *mark = '\0';
    }
}

static bool IsBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

char *ParseTrim(char *text)
{
    size_t length;

    while (IsBlank(*text)) {
        text++;
    }
    length = strlen(text);
    while (length > 0 && IsBlank(text[length - 1])) {
        text[--length] = '\0';
    }
    return text;
}

/* strtoul alone would accept "-1" (wrapping it), leading spaces and "+5" */
static bool IsAllDigits(const char *text)
{
    if (*text == '\0') {
        return false;
    }
    for (; *text != '\0'; text++) {
        if (!isdigit((unsigned char)*text)) {
            return false;
        }
    }
    return true;
}

bool ParseUint32(const char *text, uint32_t *outValue)
{
    char *end;
    unsigned long long value;

    if (text == NULL || outValue == NULL || !IsAllDigits(text)) {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *outValue = (uint32_t)value;
    return true;
}

void ParseErrorSet(ParseError *error, size_t line, const char *format, ...)
{
    va_list arguments;

    if (error == NULL) {
        return;
    }
    error->line = line;
    va_start(arguments, format);
    vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}
