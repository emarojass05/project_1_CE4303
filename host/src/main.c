#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "host_demo.h"

int main(void)
{
    char queueName[64];
    int result;

    snprintf(queueName, sizeof(queueName), "/rt_events_%d", (int)getpid());
    printf("Fake events are being produced. Type q and press Enter to stop.\n");
    result = HostRunDemo(queueName, STDIN_FILENO, stdout, UINT32_MAX, 200);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}