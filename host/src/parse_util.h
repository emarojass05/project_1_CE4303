#ifndef PARSE_UTIL_H
#define PARSE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Helpers shared by the configuration and task file parsers */

enum {
    ParseLineMax = 256,   /* Longest accepted line, without the newline */
    ParseMessageMax = 128
};

/* Where and why an input was rejected; line is 1-based, 0 when not tied to a line */
typedef struct {
    size_t line;
    char message[ParseMessageMax];
} ParseError;

typedef enum {
    LineReadOk = 0,
    LineReadEnd,     /* No more input */
    LineReadTooLong, /* Line exceeded ParseLineMax; the rest of it was skipped */
    LineReadBinary   /* Line contained a NUL byte */
} LineRead;

/* Reads one line into buffer (capacity ParseLineMax + 1) without the newline.
   Never writes past the buffer; an overlong or binary line is consumed whole. */
LineRead ParseReadLine(FILE *input, char buffer[ParseLineMax + 1]);

/* True when the line can be parsed; otherwise fills error with the reason */
bool ParseLineUsable(LineRead result, size_t line, ParseError *error);

/* Cuts text at the first '#' */
void ParseStripComment(char *text);

/* Removes leading and trailing spaces, tabs and '\r' in place; returns the start */
char *ParseTrim(char *text);

/* Accepts only decimal digits that fit in 32 bits; no sign, no spaces */
bool ParseUint32(const char *text, uint32_t *outValue);

/* Fills error, if not NULL, with printf-style text */
void ParseErrorSet(ParseError *error, size_t line, const char *format, ...);

#endif /* PARSE_UTIL_H */
