#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* Pines de la Raspberry Pi Pico (numeros GPxx, no el numero de pata).
   Cambielos para que coincidan con su circuito. GP25 es el LED de la placa. */

/* LEDs del numero de tarea, del bit menos significativo al mas significativo.
   Con 4 LEDs se ven las tareas 1-15, suficiente para las 8 que pide el proyecto. */
#define BOARD_TASK_LED_PINS { 2, 3, 4, 5 }

/* LED de plazo perdido */
#define BOARD_MISS_LED_PIN 6

/* 1 si el LED enciende con el pin en alto (anodo al pin), 0 si es al reves */
#define BOARD_LED_ON_LEVEL 1

/* Boton de la tarea esporadica, entre el pin y GND (usa el pull-up interno) */
#define BOARD_BUTTON_PIN 14

/* Rebotes del boton: se ignoran las pulsaciones a menos de esto de la anterior */
#define BOARD_DEBOUNCE_US 50000u

/* 1: eventos como texto legible (solo para depurar con un monitor serial).
   0: eventos como tramas Event del protocolo, que es lo que lee el host. */
#define BOARD_TEXT_EVENTS 0

#endif /* BOARD_CONFIG_H */