/* Entry point of the ESP32 node. All the node logic is shared with the Pico
   in nodes/app/node_app.c; this board only provides nodes/core/board.h. */

#include "node_app.h"

void app_main(void)
{
    NodeRun();
}
