# Nodos: conexiones, compilación y carga

Los dos nodos corren el mismo programa (`nodes/app/node_app.c`) y el mismo calendarizador (`nodes/core`). Cada placa solo implementa la capa de hardware de `nodes/core/board.h`: LEDs, timer del tick, botón y puerto serial.

## Funcionamiento

El nodo ya no trae tareas fijas: todo lo recibe del host por el puerto serial (`docs/protocolo.md`, `docs/host.md`).

1. Al encender, el nodo queda en `Idle` revisando el puerto cada 10 ms (el timer de hardware lo despierta; no hay busy waiting). Los LEDs quedan apagados.
2. `CalibRequest` trae la duración del tick. El nodo mide sₙ en ese momento con `unidad_trabajo()` y responde `CalibResponse`. Desde ahí el timer late con el tick del host.
3. `DeployConfig` fija el algoritmo (RMS o EDF) y cada cuántos ticks se envía `TickReport`. Cada `DeployTask` agrega una tarea con su costo en ticks (máximo 8, y como máximo una esporádica).
4. `Start` reinicia el timer: ese es el **tick cero**. En cada tick el nodo ejecuta el calendarizador (sₙ llamadas a `unidad_trabajo()` para la tarea elegida), muestra la tarea en los LEDs, libera la esporádica si se presionó el botón, y después lee el puerto y envía eventos.
5. `Stop` detiene el calendarizador, envía los eventos pendientes, responde `Ack` y vuelve a `Idle`.

Si el host se cae o se desconecta el cable, el nodo **sigue ejecutando** sus tareas; solo `Stop` lo detiene. Si un paso del arranque falla, el LED de plazo perdido queda encendido fijo y los de tarea apagados.

La lógica del protocolo del nodo está en `nodes/core/node_link.c` (igual en las dos placas y probada en la PC con `nodes/core/tests/test_node_link.c`).

Ya no hay modo texto (`BOARD_TEXT_EVENTS`): el puerto es solo del protocolo. Para ver los eventos legibles se usa `./build/bin/host -v`.

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

El protocolo usa la UART0, la misma del USB de la placa. Los logs de ESP-IDF se apagan al arrancar para no mezclarse con las tramas. Los mensajes del bootloader que salen al reiniciar no molestan: el host los descarta al abrir el puerto, y el parser ignora todo lo que no es una trama válida.

`make esp32-monitor` ya no sirve para ver eventos (son binarios). Cierren el monitor antes de correr el host: dos programas no pueden usar el mismo puerto.

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

Después de cargar, la Pico aparece en Linux como `/dev/ttyACM0` (es el puerto que trae `config/ejemplo.conf`).

Detalles de la capa de la Pico (`nodes/pico/board_pico.c`):

- **Tick**: `add_repeating_timer_us` sobre la alarma de hardware del RP2040. La interrupción solo cuenta el tick.
- **Espera**: el ciclo principal duerme con `__wfi()` (espera de interrupción) con las interrupciones apagadas durante la comprobación, así no se pierde un tick y no hay busy waiting. No se usa RTOS.
- **Botón**: interrupción por flanco de bajada con debounce de 50 ms (`nodes/core/debounce.c`). La interrupción solo marca la pulsación; el ciclo principal llama al calendarizador.
- **Serial**: USB CDC del SDK. Se escribe solo lo que cabe en el búfer USB en ese momento, y si el host no está conectado no se escribe nada y el nodo sigue ejecutando sus tareas. La lectura usa `getchar_timeout_us(0)`, que nunca espera.
