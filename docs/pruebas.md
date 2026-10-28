# Pruebas

Cada archivo `test_*.c` dentro de una carpeta `tests/` es un programa independiente: devuelve 0 si pasa y distinto de 0 si falla. Se corren sin placas, con nodos simulados (`tools/fake_node`) donde hace falta.

```
make test       # corre todas las pruebas
make asan       # igual, con AddressSanitizer (accesos inválidos a memoria)
make valgrind   # igual, con Valgrind (fugas y lecturas sin inicializar; falla si hay un solo error)
```

Las tres están limpias al cierre de M5.

## Pruebas de robustez y entradas inválidas (M5)

Qué requisito cubre cada prueba y dónde está.

### Tramas corruptas y protocolo

| Qué se prueba | Archivo | Función |
|---|---|---|
| La trama con CRC malo se descarta y se cuenta | `common/tests/test_protocol.c` | `TestCorruptedFrameIsDiscardedAndCounted` |
| Ningún cambio de un solo bit es aceptado | `common/tests/test_protocol.c` | `TestNoSingleBitFlipIsAccepted` |
| Basura antes de la marca de inicio se salta | `common/tests/test_protocol.c` | `TestGarbageBeforeStartIsSkipped` |
| Encabezado inválido se rechaza pronto | `common/tests/test_protocol.c` | `TestBadHeaderIsRejectedEarly` |
| Una trama a medias se descarta al reiniciar | `common/tests/test_protocol.c` | `TestResetDropsPartialFrame` |
| Tipo, largo y rangos del payload se verifican | `common/tests/test_protocol.c` | `TestUnpackChecksTypeLengthAndRanges` |
| Argumentos nulos y búfer pequeño no revientan | `common/tests/test_protocol.c` | `TestNullArguments`, `TestEncodeRejectsSmallBuffer` |
| El CRC detecta cualquier bit cambiado | `common/tests/test_crc16.c` | `TestSingleBitFlipChangesCrc` |
| El nodo no responde a una trama corrupta | `nodes/core/tests/test_node_link.c` | `TestCorruptFrameGetsNoReply` |
| El nodo no aplica dos veces una retransmisión | `nodes/core/tests/test_node_link.c` | `TestRetransmissionIsNotAppliedTwice` |
| Mensajes fuera de orden dan error de estado | `nodes/core/tests/test_node_link.c` | `TestMessagesOutOfOrderAreBadState` |
| Tick, tareas y límites inválidos se rechazan | `nodes/core/tests/test_node_link.c` | `TestBadTickIsRejected`, `TestBadTasksAndLimits` |
| El driver reintenta con un nodo mudo, descarta la trama corrupta y reporta puerto inexistente | `host/tests/test_driver.c` | `TestSilentNodeAndCorruptFrame`, `TestMissingPortIsReported` |
| El reporte cuenta las tramas corruptas | `host/tests/test_report.c` | `TestFullRun` |

### Entradas inválidas del usuario

| Qué se prueba | Archivo | Función |
|---|---|---|
| Archivo de configuración: líneas mal formadas, opciones, números y puertos inválidos, línea larguísima, byte NUL, bytes al azar | `host/tests/test_config.c` | `TestMalformedLines`, `TestInvalidChoices`, `TestInvalidNumbers`, `TestInvalidPorts`, `TestOverlongLine`, `TestNulByte`, `TestRandomBytesNeverCrash` |
| Claves obligatorias que faltan y valores por defecto | `host/tests/test_config.c` | `TestMissingRequiredKeys`, `TestDefaultsForOptionalKeys` |
| Clave opcional `admission = on\|off` | `host/tests/test_config.c` | `TestAdmissionKey` |
| Archivo de tareas: campos de más o de menos, números y tipos malos, reglas de la tarea | `host/tests/test_taskfile.c` | `TestFieldCount`, `TestBadNumbers`, `TestBadType`, `TestTaskRules` |
| Una línea mala no detiene las demás; sin tareas válidas; demasiadas tareas; lista de errores acotada | `host/tests/test_taskfile.c` | `TestBadLinesDoNotStopTheRest`, `TestNoValidTask`, `TestTooManyTasks`, `TestErrorListIsBounded` |
| Líneas largas o binarias y bytes al azar no causan fallas | `host/tests/test_taskfile.c` | `TestOverlongAndBinaryLines`, `TestRandomBytesNeverCrash` |
| Tarea inválida y costo con datos inválidos | `host/tests/test_task.c` | `TestValidate`, `TestCostInvalidInput` |
| Tarea inválida o nodo inutilizable en la asignación; argumentos nulos | `host/tests/test_assign.c` | `TestInvalidTaskAndUnusableNode`, `TestInvalidArguments` |
| Comando `a` durante la ejecución: tarea inválida, repetida o que no cabe se rechaza; `s` libera la esporádica | `host/tests/test_planner.c` | `TestManualModeAndSporadicCommand` |
| La tecla `q` o el fin de la entrada terminan limpio | `host/tests/test_host_demo.c` | `TestStopsOnQ`, `TestStopsWhenInputEnds` |

### Falla de nodo y replanificación

| Qué se prueba | Archivo | Función |
|---|---|---|
| Nodo caído por reportes perdidos: se reubican sus tareas con la misma heurística | `host/tests/test_planner.c` | `TestNodeLostIsReplanned` |
| Falla por comando: si no caben todas, se rechaza primero la de menor criticidad | `host/tests/test_planner.c` | `TestCommandDownRejectsLowCriticality` |
| Nodo ausente al arrancar o ningún nodo | `host/tests/test_planner.c` | `TestMissingNodeIsLeftOut`, `TestNoNodeAtAll` |

La falla por **cable desconectado** no se puede simular en una prueba automática; se probó con las placas reales (ver abajo).

### Memoria y recursos

| Qué se prueba | Archivo | Función |
|---|---|---|
| La cola de eventos bloquea al productor cuando se llena, despierta a todos al cerrar y suelta la memoria compartida | `host/tests/test_event_queue.c` | `TestFullQueueBlocksProducer`, `TestShutdownWakesBlockedConsumer`, `TestShutdownWakesBlockedProducers`, `TestDestroyRemovesSharedMemory` |
| Varios productores y un consumidor sin perder eventos | `host/tests/test_event_queue.c` | `TestManyProducersOneConsumer` |
| El búfer de eventos del nodo descarta y cuenta lo que no cabe | `nodes/core/tests/test_event_buffer.c` | `TestFullBufferDropsNewEvents`, `TestCounterOverflow` |

`make asan` y `make valgrind` corren estas mismas pruebas buscando accesos inválidos, fugas y lecturas de memoria sin inicializar.

## Resto de las pruebas por módulo

| Módulo | Archivo | Qué cubre |
|---|---|---|
| CRC-16 | `common/tests/test_crc16.c` | Valor estándar, entrada vacía, cálculo incremental, bytes conocidos |
| Protocolo | `common/tests/test_protocol.c` | Formato en el cable (little-endian, CRC), ida y vuelta de cada mensaje, tramas seguidas |
| Admisión | `host/tests/test_admission.c` | Utilización, cota de RMS, límite de EDF, EDF acepta lo que RMS rechaza |
| Asignación | `host/tests/test_assign.c` | First-Fit y Worst-Fit con asignaciones distintas, nodo más rápido primero, esporádica única, admisión desactivada |
| Costo de tarea | `host/tests/test_task.c` | C = ⌈W / sₙ⌉, ejemplo de la especificación, extremos |
| Driver | `host/tests/test_driver.c` | Sesión completa con un nodo simulado |
| Planificador | `host/tests/test_planner.c` | Dos nodos de punta a punta |
| Reporte | `host/tests/test_report.c` | Contadores por tarea, cambios de tarea, tablas llenas, resumen impreso, desfase entre nodos |
| Calibración | `nodes/core/tests/test_calibration.c` | Escala al tick, mínimo de una unidad, valores enormes |
| Botón | `nodes/core/tests/test_debounce.c` | Primer pulso, rebotes descartados, vuelta del contador |
| Eventos del nodo | `nodes/core/tests/test_event_buffer.c`, `test_event_tx.c` | Orden FIFO, envío parcial, puerto bloqueado |
| LEDs | `nodes/core/tests/test_indicators.c` | Código binario de la tarea, LED de plazo perdido |
| Calendarizador | `nodes/core/tests/test_scheduler.c` | RMS y EDF, desempate, 8 tareas, esporádica, plazos perdidos y aborto, sobrecarga |

## Pruebas con las placas reales

Se hicieron con el ESP32 y la Raspberry Pi Pico conectadas. Los reportes completos están en `docs/resultados/` y el análisis en [`experimentos.md`](experimentos.md).

| Prueba | Reporte | Resultado |
|---|---|---|
| Corrida normal (`s`, `a`, `q`) | `m5_demo_normal.txt` | 0 plazos perdidos, 0 tramas corruptas, cierre limpio |
| Falla de nodo por comando (`k 2`) | `m5_demo_k.txt` | Nodo caído en 458-485 ms, reubicación y rechazo por criticidad |
| Falla de nodo por cable desconectado | `m5_demo_cable.txt` | Nodo caído en 458 ms, la tarea 3 reinició en el nodo 2, las tareas 4 y 6 se rechazaron |
| Estrés con 8 tareas y botón físico | `estres_8*.txt` | Sin plazos perdidos |
| RMS vs EDF, First-Fit vs Worst-Fit | `exp_*.txt` | Ver `experimentos.md` |

## Lo que estas pruebas no cubren

- No hay una prueba automática que escriba comandos al azar en el teclado del host mientras corre (`a`, `k`, `s` con argumentos inválidos); lo que sí se prueba son los comandos válidos y los inválidos más comunes (`TestManualModeAndSporadicCommand`).
- El cable desconectado y el botón físico solo se probaron a mano, con las placas.
