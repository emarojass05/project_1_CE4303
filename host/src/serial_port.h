#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Serial port of one node, configured with termios as the protocol asks
   (docs/protocolo.md, section 1): 115200 baud, 8N1, no flow control, raw.
   The descriptor is non-blocking; waits always go through poll(). */

/* Opens and configures path; returns the descriptor or -1 */
int SerialOpen(const char *path);

/* Writes every byte, waiting with poll() up to timeoutMs in total for the
   port to accept them. Returns false on error or timeout. */
bool SerialWriteAll(int fd, const uint8_t *bytes, size_t count, int timeoutMs);

/* Discards unread input and closes the port; ignores fd < 0 */
void SerialClose(int fd);

#endif /* SERIAL_PORT_H */
