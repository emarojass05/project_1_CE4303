#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* Pines del ESP32. Cambielos para que coincidan con su circuito.
   Evite los pines de arranque 0, 2, 5, 12 y 15, y los 34-39 (solo entrada). */

/* LEDs del numero de tarea, del bit menos significativo al mas significativo.
   Con 4 LEDs se ven las tareas 1-15, suficiente para las 8 que pide el proyecto. */
#define BOARD_TASK_LED_PINS { 16, 17, 26, 25}

/* LED de plazo perdido */
#define BOARD_MISS_LED_PIN 19

/* 1 si el LED enciende con el pin en alto (anodo al pin), 0 si es al reves */
#define BOARD_LED_ON_LEVEL 1

/* Boton de la tarea esporadica, entre el pin y GND (usa el pull-up interno) */
#define BOARD_BUTTON_PIN 27

/* Rebotes del boton: se ignoran las pulsaciones a menos de esto de la anterior */
#define BOARD_DEBOUNCE_US 50000u

/* 1: eventos como texto legible (solo para depurar con idf.py monitor).
   0: eventos como tramas Event del protocolo, que es lo que lee el host. */
#define BOARD_TEXT_EVENTS 0

#endif /* BOARD_CONFIG_H */
