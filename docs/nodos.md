# Nodos: conexiones, compilación y carga

Los dos nodos corren el mismo programa (`nodes/app/node_app.c`) y el mismo calendarizador (`nodes/core`). Cada placa solo implementa la capa de hardware de `nodes/core/board.h`: LEDs, timer del tick, botón y puerto serial.

## Demo actual (M1 y M2)

Mientras no exista el lado del host (M3), el nodo arranca solo con 4 tareas de prueba:

| Tarea | Tipo | C (ticks) | T | D | LEDs |
|---|---|---|---|---|---|
| 1 | Periódica | 2 | 5 | 5 | `0001` |
| 2 | Periódica | 3 | 7 | 7 | `0010` |
| 3 | Periódica | 1 | 10 | 10 | `0011` |
| 4 | Esporádica (botón) | 1 | 20 (intervalo mínimo) | 20 | `0100` |

- Las periódicas suman U = 0.93: con **RMS** se pierde un plazo cada ~70 ticks (se enciende el LED rojo) y con **EDF** ninguno. El algoritmo se cambia con `NODE_DEMO_ALGORITHM` en `nodes/app/node_app.c`.
- El tick de la demo es de **250 ms** para poder seguir los LEDs a simple vista (`NODE_DEMO_TICK_US`). El valor del proyecto es 10 ms y en M3 lo envía el host.
- Cada tick ejecuta trabajo real: sₙ llamadas a `unidad_trabajo()`, con sₙ medido al arrancar (ver `docs/calendarizador.md`).
- Si un paso del arranque falla, el LED de plazo perdido queda encendido fijo y los de tarea apagados.

### Ver los eventos

Por defecto los eventos salen como tramas binarias `Event` (`docs/protocolo.md`), que es lo que leerá el host. Para verlos como texto con un monitor serial, poner `BOARD_TEXT_EVENTS` en `1` en el `board_config.h` de la placa y volver a compilar. En modo texto la escritura puede esperar al puerto, así que es solo para depurar.

## Conexiones

En las dos placas: cada LED va del pin a una resistencia de 220–330 Ω y de ahí a GND (ánodo hacia el pin). El botón va entre el pin y GND; se usa el pull-up interno, no hace falta resistencia. Los pines se cambian en `board_config.h`.

| Señal | ESP32 (`nodes/esp32/main/board_config.h`) | Pico (`nodes/pico/board_config.h`) |
|---|---|---|
| LED tarea bit 0 | GPIO 16 | GP2 |
| LED tarea bit 1 | GPIO 17 | GP3 |
| LED tarea bit 2 | GPIO 26 | GP4 |
| LED tarea bit 3 | GPIO 25 | GP5 |
| LED plazo perdido | GPIO 19 | GP6 |
| Botón esporádica | GPIO 27 | GP14 |

Con 4 LEDs de tarea se pueden mostrar las 8 tareas que pide el proyecto.

## ESP32

Requiere ESP-IDF 5.3 o posterior.

```bash
make esp32           # compila
make esp32-run       # carga y abre el monitor (salir con Ctrl+])
make esp32-run PORT=/dev/ttyUSB1
```

El protocolo usa la UART0, la misma del USB de la placa. Con `BOARD_TEXT_EVENTS = 0` los logs de ESP-IDF se apagan al arrancar para no mezclarse con las tramas.

## Raspberry Pi Pico

Requiere el Pico SDK (2.x) y el compilador de ARM:

```bash
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib build-essential
mkdir -p ~/pico && cd ~/pico
git clone -b 2.1.1 https://github.com/raspberrypi/pico-sdk
cd pico-sdk && git submodule update --init lib/tinyusb
```

Compilar y cargar desde la raíz del repo:

```bash
make pico                                   # genera nodes/pico/build/rt_node_pico.uf2
make pico PICO_SDK_PATH=/otra/ruta/pico-sdk # si el SDK no está en ~/pico/pico-sdk
```

Para cargarlo: mantener presionado **BOOTSEL**, conectar el USB y soltar. Aparece una unidad `RPI-RP2`; copiar ahí `rt_node_pico.uf2`. La Pico se reinicia sola. Con `picotool` instalado basta `make pico-flash`.

Después de cargar, la Pico aparece en Linux como `/dev/ttyACM0` (es el puerto que trae `config/ejemplo.conf`). Para ver el modo texto: `screen /dev/ttyACM0 115200` o `minicom -D /dev/ttyACM0`.

Detalles de la capa de la Pico (`nodes/pico/board_pico.c`):

- **Tick**: `add_repeating_timer_us` sobre la alarma de hardware del RP2040. La interrupción solo cuenta el tick.
- **Espera**: el ciclo principal duerme con `__wfi()` (espera de interrupción) con las interrupciones apagadas durante la comprobación, así no se pierde un tick y no hay busy waiting. No se usa RTOS.
- **Botón**: interrupción por flanco de bajada con debounce de 50 ms (`nodes/core/debounce.c`). La interrupción solo marca la pulsación; el ciclo principal llama al calendarizador.
- **Serial**: USB CDC del SDK. Se escribe solo lo que cabe en el búfer USB en ese momento, y si el host no está conectado no se escribe nada y el nodo sigue ejecutando sus tareas.
