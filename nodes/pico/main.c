/* Entry point of the Raspberry Pi Pico node. All the node logic is shared
   with the ESP32 in nodes/app/node_app.c; this board only provides
   nodes/core/board.h. */

#include "node_app.h"

int main(void)
{
    NodeRun();
    return 0;
}
