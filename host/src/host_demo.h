#ifndef HOST_DEMO_H
#define HOST_DEMO_H

#include <stdint.h>
#include <stdio.h>

/* Runs the report process and a test producer around a shared event queue.
   Stops when q is read from inputFd, when inputFd ends, or when a child exits.
   The summary is written to out. Returns 0 when everything ended cleanly. */
int HostRunDemo(const char *queueName, int inputFd, FILE *out,
                uint32_t passes, long eventDelayMilliseconds);

#endif /* HOST_DEMO_H */