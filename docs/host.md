# Host: procesos, driver serial y modo automático

Implementación en `host/src/`. Pruebas en `host/tests/` (las de `test_driver.c` y `test_planner.c` usan nodos simulados, no hace falta hardware).

## Uso

```bash
make                          # compila host, nodo simulado y corre las pruebas
./build/bin/host              # usa config/ejemplo.conf
./build/bin/host -v config/demo_m3.conf   # -v imprime cada evento al llegar
```

El host lee la configuración y el archivo de tareas, calibra los nodos, asigna, despliega e inicia. Para terminar se presiona **q** (no hace falta Enter cuando la entrada es una terminal). También termina con Ctrl+C o si un proceso hijo muere; en todos los casos detiene los nodos, cierra los puertos y genera el reporte.

## Procesos (F9)

```
                 memoria compartida /rt_control_<pid>
              ┌──────── (un buzón por nodo, 2 semáforos) ────────┐
              ▼                                                  ▼
 ┌────────────────────┐   comandos   ┌───────────────────┐   ┌───────────────────┐
 │ Planificador (main)│─────────────▶│ Driver nodo 1     │   │ Driver nodo 2     │ ...
 │ planner.c          │◀─────────────│ driver.c          │   │ driver.c          │
 └────────────────────┘  resultados  │ /dev/ttyUSB0      │   │ /dev/ttyACM0      │
                                     └─────────┬─────────┘   └─────────┬─────────┘
                                               │ eventos (productores) │
                                               ▼                       ▼
                                 cola acotada /rt_events_<pid> (event_queue.c)
                                               │ (consumidor)
                                               ▼
                                     ┌───────────────────┐
                                     │ Reporte           │
                                     └───────────────────┘
```

Con dos nodos hay **4 procesos**: planificador, dos drivers y reporte (§3.6).

- **Planificador ↔ drivers** (`driver.h`, `ControlBlock`): un buzón por nodo en memoria compartida POSIX. El planificador escribe el mensaje y hace `sem_post(commandReady)`; el driver lo envía, espera la respuesta y hace `sem_post(commandDone)`. Así el planificador puede enviar `Start` a todos los nodos a la vez y luego esperar cada respuesta.
- **Drivers → reporte**: la cola de eventos de `event_queue.c` (acotada, 256 eventos, semáforos `freeSlots`/`usedSlots`/`mutex`).
- Nadie hace busy waiting: el planificador duerme en `sem_wait` o en `poll` (teclado y señales), cada driver duerme en `sem_wait` y en `poll` sobre el puerto, y el reporte duerme en `sem_wait`.

## Driver serial (F8)

Cada driver es un proceso con dos hilos:

| Hilo | Qué hace |
|---|---|
| Principal | Espera comandos del planificador. Asigna `seq`, envía la trama y espera la respuesta con el mismo `seq` (`pthread_cond_timedwait`). Si no llega, reenvía **la misma trama** hasta 3 veces (`docs/protocolo.md` §7) |
| Lector | Duerme en `poll` sobre el puerto. Cada byte pasa por `FrameParserFeed`; los `Ack`/`CalibResponse` se entregan al hilo principal, los `Event` van a la cola compartida y los `TickReport` actualizan las estadísticas del nodo. Si una trama queda a medias 200 ms, llama a `FrameParserReset` |

Puerto (`serial_port.c`): `termios` en modo crudo, 115200 baudios, 8N1, sin control de flujo, `CLOCAL`, descriptor no bloqueante. Al abrir se descarta lo que la placa haya enviado antes (mensajes de arranque).

Detalles:

- El primer `seq` de cada corrida sale del reloj y el pid, para que un nodo nunca confunda el primer mensaje de una corrida nueva con una retransmisión de la anterior.
- El tiempo de espera es 200 ms + 2 ticks, porque el nodo lee el puerto una vez por tick. Con el tick de 10 ms son 220 ms.
- Los `Event` y `TickReport` que llegan fuera de una corrida (antes de enviar `Start` o después del `Ack` de `Stop`) son restos de una corrida anterior: se descartan y se cuentan.
- Las tramas corruptas se descartan y se cuentan (`badFrames`); aparecen en el resumen del enlace al final.
- Si el puerto no abre o se desconecta, el driver responde "port error" a cada comando y el planificador deja ese nodo fuera.

## Modo automático (F10)

`planner.c`, en este orden:

1. **Saludo**: `Stop` a cada nodo (hasta 10 intentos, por si la placa se está reiniciando). Un nodo que quedó corriendo de una ejecución anterior vuelve a `Idle`.
2. **Calibración**: `CalibRequest` con el tick de la configuración. El nodo mide sₙ en ese momento y responde `CalibResponse`. Un nodo que no responde queda fuera (sₙ = 0) y la asignación no lo usa.
3. **Asignación**: `AssignTasks` (costo C(i,n) = ⌈W/sₙ⌉, orden por utilización en el nodo más rápido, First-Fit o Worst-Fit, admisión EDF o RMS). Se imprime cada tarea con su nodo y costo, o el motivo del rechazo.
4. **Despliegue**: a cada nodo calibrado, `DeployConfig` (algoritmo y cada cuántos ticks envía `TickReport`) y un `DeployTask` por tarea con el costo para ese nodo.
5. **Inicio**: `Start` a todos los nodos a la vez. Fija el tick cero.
6. **Ejecución**: los drivers pasan los eventos al reporte hasta que se presiona q.
7. **Cierre**: `Stop` a todos (cada nodo envía sus eventos pendientes antes del `Ack`), los drivers cierran sus puertos y terminan, la cola se cierra, el reporte imprime y se liberan la memoria compartida y los semáforos.

Al final se imprime un resumen del enlace por nodo: sₙ, tramas, eventos, tramas corruptas, reportes de tick, plazos perdidos, eventos perdidos en el nodo y retransmisiones.

## Probar sin placas

`build/bin/fake_node` simula un nodo sobre una pseudo terminal, con el mismo código de protocolo y calendarizador de `nodes/core` que las placas:

```bash
./build/bin/fake_node 60 &     # imprime: Nodo simulado con sn = 60 en /dev/pts/3
./build/bin/fake_node 25 &     # otro nodo, más lento
# poner esos dos /dev/pts/N en "ports" de una copia de la configuración
./build/bin/host -v mi.conf
```

El nodo simulado sigue corriendo cuando el host termina, igual que una placa, así que se puede volver a correr el host sin reiniciarlo.
