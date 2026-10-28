#ifndef FAKE_NODE_H
#define FAKE_NODE_H

#include <stdint.h>

/* Simulated node for testing the host without boards. It runs the same
   protocol code (nodes/core/node_link.c) and scheduler as the real nodes,
   on a pseudo terminal instead of a USB port, with the tick taken from the
   PC clock. Calibration answers unitsPerTick instead of timing ut.c. */

/* Opens a pseudo terminal and writes the path the host must open in
   path (size bytes). Returns the master descriptor or -1. */
int FakeNodeOpen(char *path, unsigned int size);

/* Serves the node side on the master descriptor forever, like a board that
   keeps running when the host goes away. Returns only on an error. */
int FakeNodeServe(int masterFd, uint32_t unitsPerTick);

#endif /* FAKE_NODE_H */
