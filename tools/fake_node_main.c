/* Simulated node for trying the host without boards:
     ./build/bin/fake_node [sn]
   prints the pseudo terminal to put in "ports" of the host configuration
   and serves it until Ctrl+C. sn is the speed it reports (default 60). */

#include <stdio.h>
#include <stdlib.h>

#include "fake_node.h"

int main(int argc, char **argv)
{
    char path[128];
    unsigned long units = 60;
    char *end = NULL;
    int fd;

    if (argc > 1) {
        units = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || units == 0 || units > 1000000UL) {
            fprintf(stderr, "Uso: %s [sn]   (sn entre 1 y 1000000)\n", argv[0]);
            return EXIT_FAILURE;
        }
    }
    fd = FakeNodeOpen(path, sizeof(path));
    if (fd < 0) {
        perror("posix_openpt");
        return EXIT_FAILURE;
    }
    printf("Nodo simulado con sn = %lu en %s\n", units, path);
    fflush(stdout);
    return FakeNodeServe(fd, (uint32_t)units) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
