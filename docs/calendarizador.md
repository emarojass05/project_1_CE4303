# Calendarizador local de los nodos (F1)

Implementación: `nodes/core/scheduler.c`, igual en el ESP32 y en la Pico. Pruebas en `nodes/core/tests/test_scheduler.c`.

## Funcionamiento por tick

En cada interrupción del timer de hardware el nodo llama a `SchedulerTick`, que hace en este orden:

1. **Aborta los trabajos vencidos**: todo trabajo pendiente cuyo plazo absoluto ya llegó genera `DeadlineMiss` y `Abort`, enciende el LED de plazo perdido y se descarta. Las demás tareas siguen.
2. **Libera los trabajos periódicos** que tocan en este tick (`Release`).
3. **Elige la tarea** de mayor prioridad entre las que tienen trabajo pendiente (criterio abajo).
4. **Ejecuta un tick de trabajo real**: sₙ llamadas a `unidad_trabajo()` (función registrada con `SchedulerSetWork`). El trabajo de la tarea baja en 1 tick; si llega a 0, genera `Complete` con el tiempo de respuesta.

La apropiación solo ocurre entre ticks, como pide la especificación (§3.2). Las tareas esporádicas se liberan con `SchedulerReleaseSporadic`, llamado desde el ciclo principal cuando la interrupción del botón registró una pulsación.

## Prioridad

| Algoritmo | Clave (menor = más prioridad) |
|---|---|
| RMS | Período `T` (intervalo mínimo en las esporádicas). No cambia durante la ejecución |
| EDF | Plazo absoluto del trabajo actual |

## Criterio de desempate

Cuando dos tareas tienen la misma clave, se decide en este orden:

| Paso | RMS | EDF |
|---|---|---|
| 1 | — | Gana la tarea que **ya está corriendo** (evita cambios de contexto innecesarios) |
| 2 | Mayor **criticidad** | Mayor **criticidad** |
| 3 | Menor **id** | Menor **id** |

En RMS no se usa la regla de "la que ya está corriendo": así la prioridad de cada tarea depende solo de su período, criticidad e id, y queda **fija durante toda la ejecución**, como exige la especificación. En EDF esa regla es válida porque la prioridad ya cambia con cada trabajo.

El criterio es el mismo en todos los nodos y en toda la ejecución. Lo verifican `TestEdfTieKeepsRunningTask`, `TestTieBreakByCriticalityThenId` y `TestRmsTieIsFixedPriority`.

## Calibración (sₙ)

El host envía `CalibRequest` con la duración del tick. El nodo llama a `unidad_trabajo()` una y otra vez durante un tick (mínimo 20 llamadas, máximo 100 ms de medición) midiendo con el reloj de la placa, y calcula cuántas caben en un tick usando solo el **70 %** del tick (`CalibrationDefaultPercent`). El 30 % restante queda para el calendarizador, los LEDs y el envío de eventos (§3.3). El resultado se devuelve en `CalibResponse`. Se mide en cada ejecución y nunca está fijo en el código (`nodes/core/calibration.c`, `nodes/app/node_app.c`).

## Eventos

El calendarizador escribe los eventos en un búfer circular en RAM (`nodes/core/event_buffer.c`, 64 eventos). Después de cada tick, el ciclo principal los envía como tramas `Event` (`nodes/core/event_tx.c`, a través de `nodes/core/node_link.c`) escribiendo solo lo que el puerto acepta en ese momento, con un tope de 8 escrituras por tick. Las respuestas al host (`Ack`, `CalibResponse`) y los `TickReport` pasan antes que los eventos, pero nunca cortan una trama a medias. Lo que no cabe espera al siguiente tick, así el envío nunca retrasa el calendarizador. Si el búfer se llena, el evento se descarta y se cuenta (`EventBufferDroppedCount`).
