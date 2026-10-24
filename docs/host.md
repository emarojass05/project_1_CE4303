# Host: procesos, driver serial, modos automático y manual, caída de nodos y reporte

Implementación en `host/src/`. Pruebas en `host/tests/` (las de `test_driver.c` y `test_planner.c` usan nodos simulados, no hace falta hardware).

## Uso

```bash
make                          # compila host, nodo simulado y corre las pruebas
./build/bin/host              # usa config/ejemplo.conf
./build/bin/host -v config/demo_m4.conf   # -v imprime cada evento al llegar
```

El host lee la configuración y el archivo de tareas, calibra los nodos, asigna, despliega e inicia. Mientras corre acepta estos comandos (en una terminal basta la tecla, sin Enter; los que llevan dato se terminan con Enter):

| Tecla | Dato | Qué hace |
|---|---|---|
| `q` | — | Detiene los nodos, termina los procesos y genera el reporte |
| `a` | `id,tipo,W,T,D,criticidad` | **Modo manual** (F10): agrega una tarea sin detener nada. Mismo formato y validación que una línea del archivo de tareas |
| `k` | número de nodo | **Tumba un nodo** (F11): le envía `Stop` y el host lo detecta como caído igual que si se desconectara el cable |
| `s` | número de nodo | Libera la esporádica de ese nodo con `SporadicTrigger`, igual que su botón |
| `h` | — | Ayuda |

Ejemplos: `a` y luego `9,P,200,40,40,2` Enter; `k` y luego `2` Enter. Si la entrada no es una terminal (por ejemplo un pipe), cada comando va en una línea: `a 9,P,200,40,40,2`, `k 2`, `s 1`, `q`.

También termina con Ctrl+C o si muere uno de sus procesos hijos; en todos los casos detiene los nodos, cierra los puertos y genera el reporte.

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
- **Drivers y planificador → reporte**: la cola de eventos de `event_queue.c` (acotada, 256 elementos, semáforos `freeSlots`/`usedSlots`/`mutex`). Cada `HostEvent` lleva un `kind`: los drivers producen eventos de los nodos y reportes de tick; el planificador, los registros de la corrida (ver Reporte).
- Nadie hace busy waiting: el planificador duerme en `sem_wait` o en `poll` (teclado y señales, con un tiempo máximo de un período de reporte para revisar si un nodo cayó), cada driver duerme en `sem_wait` y en `poll` sobre el puerto, y el reporte duerme en `sem_wait`.

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
6. **Ejecución**: los drivers pasan los eventos y los reportes de tick al reporte; el planificador atiende la consola y vigila los reportes de tick hasta que se presiona q.
7. **Cierre**: `Stop` a los nodos que siguen vivos (cada nodo envía sus eventos pendientes antes del `Ack`), los drivers cierran sus puertos y terminan, el planificador manda los contadores del enlace por la cola, la cola se cierra, el reporte imprime y se liberan la memoria compartida y los semáforos.

## Modo manual (F10)

Con `a` el usuario escribe una tarea con el mismo formato del archivo (`id,tipo,W,T,D,criticidad`). El host:

1. La valida igual que una línea del archivo (`TaskFileParseTask`) y rechaza un `id` que ya existe.
2. Reconstruye la carga actual de cada nodo vivo (las tareas que están corriendo) y corre `AssignPlaceTask` con la misma heurística y la misma prueba de admisión de la configuración.
3. Si pasa, envía `DeployTask` al nodo elegido, que está en `Running` (`docs/protocolo.md` §6): el nodo la agrega en su tick actual y la libera en el siguiente, sin detener a las demás.
4. Imprime el resultado y lo manda al reporte (aceptada con su nodo y costo, o rechazada con su motivo).

## Caída de nodos (F11)

Cada nodo envía un `TickReport` cada `tick_report_every` ticks. El planificador despierta del `poll` al menos una vez por período de reporte (sin busy waiting) y, por cada nodo, compara cuántos reportes lleva su driver. Si pasan `missed_reports_for_failure` períodos sin reportes nuevos, más un margen para el jitter del USB (la mitad de un período, mínimo 100 ms), lo declara **caído**. Con el tick de 10 ms, un reporte cada 10 ticks y 3 perdidos son unos 400 ms.

La caída se detecta igual en los dos casos de la especificación:

- **Cable desconectado**: el driver ve el error de lectura, marca el puerto perdido y ya no llegan reportes.
- **Comando `k`**: el host envía `Stop` a ese nodo (sus LEDs se apagan), así que deja de enviar reportes.

Al declararlo caído, el host:

1. Si el puerto sigue abierto, le envía `Stop` por si estaba vivo pero mudo, para que no siga ejecutando tareas que se van a mover.
2. Deja **fijas** las tareas de los demás nodos.
3. Rechaza las **esporádicas** del nodo caído (no se reubican).
4. Ordena sus periódicas por **criticidad descendente** (después utilización y `id`) y las coloca una por una con la misma heurística y la misma prueba de admisión. Así, si no caben todas, las primeras rechazadas son las de menor criticidad.
5. Envía `DeployTask` de cada una al nodo nuevo: **reinicia** su ejecución ahí, en el tick actual de ese nodo.

Un nodo caído no recibe más comandos; si todos caen, el host espera `q` para el reporte.

## Reporte (F13)

El proceso de reporte recibe **todo** por la cola de eventos en memoria compartida: los eventos de los nodos, los reportes de tick (con la hora de llegada) y los registros del planificador (configuración, calibración, tareas aceptadas y rechazadas, nodos caídos y contadores del enlace). Al presionar q imprime:

| Sección | Contenido |
|---|---|
| Encabezado | Algoritmo, heurística, tick, duración de la corrida, eventos recibidos e ignorados |
| Tareas aceptadas y rechazadas | Cada decisión en orden, con su hora, su origen (archivo, manual, reubicada) y el nodo y costo o el motivo del rechazo; nodos caídos |
| Nodos | sₙ, si cayó (cuándo y por qué), utilización **teórica** (ΣC/T de las tareas que quedan en el nodo y su cota) y **medida** (ticks con tarea en ejecución / ticks de la corrida), cambios de tarea y desalojos |
| Tareas | Por nodo: liberaciones (periódicas y esporádicas aceptadas en una sola columna `Rel`), inicios, terminadas, plazos perdidos, abortadas, desalojos, esporádicas ignoradas, ticks ejecutados, tiempo de respuesta mínimo, promedio y máximo, U teórica y medida |
| Esporádicas | Liberaciones aceptadas e ignoradas y **latencia** de respuesta (de la liberación al fin del trabajo) mínima, promedio y máxima, en ticks y en ms |
| Gantt por nodo | Una fila por tarea, una columna por tick, para los primeros 100 ticks y los últimos 100: `#` ejecutando, `-` liberada esperando, `!` plazo perdido (trabajo abortado), `.` sin trabajo |
| Cambios de tarea y plazos perdidos | Los primeros 20 de cada nodo, con su tick (`12:3>1` es "en el tick 12 la tarea 3 deja el procesador a la 1") |
| Enlace | Tramas, eventos, **tramas corruptas**, reportes de tick, plazos perdidos y eventos perdidos según el nodo, retransmisiones y restos de una corrida anterior |
| Desfase | Desfase entre nodos al inicio y al final y su deriva (siguiente sección) |

## Desfase entre nodos (F12)

`Start` llega a cada nodo por su propio puerto, así que el tick cero de cada nodo empieza en un momento un poco distinto. El host lo mide de dos formas, con el reloj `CLOCK_MONOTONIC`, que es el mismo en todos sus procesos:

1. **Ack de `Start`**: el nodo reinicia su timer y responde el `Ack` en la misma vuelta del ciclo, sin trabajo de por medio. La diferencia entre la llegada de los `Ack` de dos nodos es una estimación directa del desfase inicial.
2. **Reportes de tick**: cada `TickReport` dice en qué tick va el nodo. El driver anota la hora de llegada, y el reporte estima la hora del tick cero de ese nodo como `llegada − tick × tick_ms`. Los retrasos (USB, el trabajo del tick que se ejecuta antes de enviar) solo suman, así que de cada 10 reportes se queda con el **mínimo**. El desfase entre dos nodos es la diferencia de esas estimaciones: al inicio (primeros 10 reportes) y en el último momento en que los dos nodos seguían reportando (si uno cae antes, se compara hasta ahí). La diferencia entre las dos, dividida entre el tiempo transcurrido, es la **deriva** entre los relojes de las placas, en ppm.

Limitaciones: el nodo lee el puerto una vez por tick, así que el desfase puede llegar a un tick (10 ms) por diseño; el `TickReport` sale después del trabajo del tick, así que si la tarea en ejecución coincide con los ticks de reporte la estimación se corre hasta unos 7 ms en ese nodo; y la deriva en corridas de pocos segundos es ruido del USB. Para documentar el valor conviene una corrida de al menos un minuto.

Resultados medidos con las placas (ESP32 = nodo 1, Pico = nodo 2, `config/demo_m4.conf`, tick de 10 ms, un `TickReport` cada 100 ms). El signo negativo indica que el tick cero estimado de la Pico queda antes que el del ESP32:

| Corrida | Ack de Start (Pico − ESP32) | Desfase al inicio | Desfase al final | Tiempo comparado | Deriva |
|---|---|---|---|---|---|
| 1, `k 2` a los 59 s | +0.45 ms | −2.76 ms | −6.33 ms | 57 s | −63 ppm |
| 2, cable del ESP32 a los 201 s¹ | +0.07 ms | −1.31 ms | — | — | — |

¹ Corrida hecha antes de corregir la comparación del desfase (el valor final que dio ese reporte no sirve).

Conclusiones:

- **Desfase inicial**: menos de 0.5 ms según el `Ack` de `Start` y entre 1 y 3 ms según los reportes de tick, siempre por debajo de un tick. La diferencia entre los dos métodos viene del sesgo de los `TickReport` explicado arriba.
- **Deriva**: unos 60 ppm entre los cristales de las dos placas, es decir unos 3.6 ms por minuto. Es un valor razonable para dos cristales comunes (±20–50 ppm cada uno); para confirmarlo conviene repetir una corrida larga. Como cada nodo cuenta sus ticks con su propio timer y no se vuelve a sincronizar después de `Start`, en una corrida de 10 minutos el desfase llega a unos 36 ms (3–4 ticks). Esto no afecta la calendarización, que es local en cada nodo, pero sí la comparación de marcas de tiempo entre nodos en el reporte.



## Probar sin placas

`build/bin/fake_node` simula un nodo sobre una pseudo terminal, con el mismo código de protocolo y calendarizador de `nodes/core` que las placas:

```bash
./build/bin/fake_node 60 &     # imprime: Nodo simulado con sn = 60 en /dev/pts/3
./build/bin/fake_node 25 &     # otro nodo, más lento
# poner esos dos /dev/pts/N en "ports" de una copia de la configuración
./build/bin/host -v mi.conf
```

Para probar una caída sin placas: `k` con el número del nodo, o matar uno de los nodos simulados con `kill -9` (equivale a desconectar el cable).

El nodo simulado sigue corriendo cuando el host termina, igual que una placa, así que se puede volver a correr el host sin reiniciarlo.
