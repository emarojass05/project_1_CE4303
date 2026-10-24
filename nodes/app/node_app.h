#ifndef NODE_APP_H
#define NODE_APP_H

/* Node program shared by every board: calibrates, loads the demo tasks and
   runs the scheduler on the hardware tick forever. Each board calls it from
   its entry point after its own startup (nodes/esp32, nodes/pico). */
void NodeRun(void);

#endif /* NODE_APP_H */
