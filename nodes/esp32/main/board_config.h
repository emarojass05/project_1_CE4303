#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* Pines del ESP32. Cambielos para que coincidan con su circuito.
   Evite los pines de arranque 0, 2, 5, 12 y 15, y los 34-39 (solo entrada). */

/* LEDs del numero de tarea, del bit menos significativo al mas significativo.
   Con 3 LEDs se ven las tareas 1-7; para 8 tareas hacen falta 4 LEDs. */
#define BOARD_TASK_LED_PINS { 16, 17, 18 }

/* LED de plazo perdido */
#define BOARD_MISS_LED_PIN 19

/* 1 si el LED enciende con el pin en alto (anodo al pin), 0 si es al reves */
#define BOARD_LED_ON_LEVEL 1

#endif /* BOARD_CONFIG_H */