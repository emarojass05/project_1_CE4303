# Protocolo serial host ↔ nodo (F0)

Versión **1.0**. Implementación de referencia: `common/protocol.h`, `common/protocol.c` y `common/crc16.c`, compartida por el host y los nodos. Pruebas en `common/tests/test_protocol.c` y `common/tests/test_crc16.c`.

> **Congelado al cerrar M1.** Cualquier cambio (tipos, campos, tamaños, semántica) requiere acuerdo de los 4 integrantes, una nueva fila en [Historial](#historial) y actualizar código y pruebas en el mismo commit.

## 1. Enlace físico

| Parámetro | Valor |
|---|---|
| Velocidad | 115200 baudios |
| Formato | 8N1, sin control de flujo |
| Puerto | uno por nodo (`ports` en el archivo de configuración del host) |

El puerto del protocolo **no debe compartirse con mensajes de consola** (`printf`, `ESP_LOG*`). El parser ignora la basura que aparece antes de una marca de inicio, pero un texto que contenga `~` (0x7E) abriría una trama falsa, que después se descartaría por CRC y contaría como trama mala.

Capacidad aproximada: 115200 baudios equivalen a unos 11 520 B/s. Una trama `Event` mide 19 bytes, así que caben unos 600 eventos por segundo, o sea unos 6 por tick de 10 ms. El búfer circular del nodo (64 eventos) absorbe ráfagas mayores.

## 2. Formato de trama

```
  0      1      2      3      4 … 3+len        4+len     5+len
+------+------+------+------+---------------+---------+---------+
| 0x7E | type | seq  | len  | payload[len]  | CRC alto| CRC bajo|
+------+------+------+------+---------------+---------+---------+
```

| Campo | Tamaño | Descripción |
|---|---|---|
| Inicio | 1 | Siempre `0x7E` |
| `type` | 1 | Tipo de mensaje (sección 4) |
| `seq` | 1 | Número de secuencia que elige el emisor (0–255, da la vuelta). La respuesta lo repite |
| `len` | 1 | Bytes de payload, de 0 a **32** |
| payload | `len` | Campos del mensaje (sección 5) |
| CRC | 2 | CRC-16 de `type`, `seq`, `len` y payload, **byte alto primero** |

- Tamaño máximo de una trama: 4 + 32 + 2 = **38 bytes**.
- No hay escape de bytes: `0x7E` puede aparecer dentro de la trama, y el CRC se encarga de detectar las tramas falsas.
- Los enteros de varios bytes del payload van en **little-endian** y se codifican byte a byte. Nunca se envía un `struct` con `memcpy`, porque el ESP32, la Pico y x86 alinean los structs de forma distinta.

### CRC-16/CCITT-FALSE

| Parámetro | Valor |
|---|---|
| Polinomio | `0x1021` |
| Valor inicial | `0xFFFF` |
| Reflexión entrada/salida | no |
| XOR final | `0x0000` |
| Valor de prueba | `"123456789"` → `0x29B1` |

## 3. Recepción y errores

El receptor procesa un byte a la vez (`FrameParserFeed`):

1. Descarta los bytes hasta encontrar `0x7E`. Estos bytes no se cuentan como error.
2. Si `type` no es un tipo conocido, **descarta** la trama.
3. Si `len > 32`, **descarta** la trama.
4. Lee el payload y el CRC. Si el CRC no coincide, **descarta** la trama.
5. Si todo es correcto, entrega la trama completa.

Cada descarte incrementa `badFrames` y el receptor vuelve al paso 1. El host reporta estas tramas descartadas en el reporte final (F8, F13). El nodo también las cuenta, pero no responde a ellas: el host las detecta porque no recibe la respuesta.

Si una lectura queda a medias (cable desconectado a mitad de una trama), el driver llama a `FrameParserReset` tras su timeout, para que el resto de esa trama no absorba la siguiente.

Un payload con la longitud equivocada o con un campo fuera de rango (algoritmo, tipo de tarea, estado de ACK) pasa el CRC, pero falla al desempaquetar con `ProtocolStatusBadPayload`. El nodo responde entonces `Ack` con estado `BadPayload`.

## 4. Tipos de mensaje

| `type` | Nombre | Dirección | `len` | Respuesta esperada |
|---|---|---|---|---|
| `0x01` | `CalibRequest` | host → nodo | 4 | `CalibResponse` con el mismo `seq` |
| `0x02` | `CalibResponse` | nodo → host | 4 | — |
| `0x03` | `DeployConfig` | host → nodo | 3 | `Ack` |
| `0x04` | `DeployTask` | host → nodo | 21 | `Ack` |
| `0x05` | `Start` | host → nodo | 0 | `Ack` |
| `0x06` | `Stop` | host → nodo | 0 | `Ack` |
| `0x07` | `Event` | nodo → host | 13 | ninguna |
| `0x08` | `TickReport` | nodo → host | 16 | ninguna |
| `0x09` | `SporadicTrigger` | host → nodo | 4 | `Ack` |
| `0x0A` | `Ack` | nodo → host | 2 | — |

`Event` y `TickReport` no se confirman: confirmarlos duplicaría el tráfico. El host detecta la pérdida de nodos con los `TickReport` que no llegan (F11).

## 5. Payloads

Desplazamientos en bytes dentro del payload. `u8`/`u16`/`u32` son enteros sin signo little-endian.

### `CalibRequest` (0x01), 4 bytes

| Offset | Tipo | Campo | Descripción |
|---|---|---|---|
| 0 | u32 | `tickUs` | Duración del tick en microsegundos (10 ms → 10000) |

El nodo acepta `tickUs` entre 1000 y 1 000 000 (1 ms a 1 s, el mismo rango que `tick_ms` del host). Fuera de ese rango responde `Ack` con `BadPayload` en lugar de `CalibResponse`. Si la medición falla, responde `Ack` con `BadState`. Desde que acepta la calibración, el timer del nodo late con `tickUs`.

### `CalibResponse` (0x02), 4 bytes

| Offset | Tipo | Campo | Descripción |
|---|---|---|---|
| 0 | u32 | `unitsPerTick` | sₙ: unidades de trabajo (`unidad_trabajo()`) por tick, medidas en esta ejecución y con margen para el scheduler y el envío de eventos (F2) |

### `DeployConfig` (0x03), 3 bytes

| Offset | Tipo | Campo | Descripción |
|---|---|---|---|
| 0 | u8 | `algorithm` | `0` = RMS, `1` = EDF (igual que `SchedulerAlgorithm` del nodo) |
| 1 | u16 | `tickReportEvery` | Enviar un `TickReport` cada N ticks; `0` = nunca |

> El host numera los algoritmos al revés (`AssignAlgorithmEdf = 0`), así que el driver los traduce; no copia el valor.

### `DeployTask` (0x04), 21 bytes

| Offset | Tipo | Campo | Descripción |
|---|---|---|---|
| 0 | u32 | `id` | Identificador de la tarea, **≥ 1** (el 0 significa "ninguna" en `TickReport`) |
| 4 | u8 | `type` | `0` = periódica, `1` = esporádica (igual que `TaskType`) |
| 5 | u32 | `cost` | C(i,n) = ⌈W(i) / sₙ⌉ en ticks, calculado por el host |
| 9 | u32 | `period` | Período en ticks (en las esporádicas, el intervalo mínimo entre llegadas) |
| 13 | u32 | `deadline` | Plazo relativo en ticks |
| 17 | u32 | `criticality` | Criticidad; mayor = más crítica |

### `Start` (0x05) y `Stop` (0x06), sin payload

`Start` fija el **tick cero**: el nodo arranca su contador de ticks en 0 al procesarlo, y todas las marcas de tiempo posteriores se cuentan desde ahí. El host envía `Start` a todos los nodos uno tras otro, lo más juntos posible (el desfase entre nodos se mide en F12).

`Stop` detiene el calendarizador. El nodo envía los eventos pendientes de su búfer y luego responde `Ack`.

### `Event` (0x07), 13 bytes

| Offset | Tipo | Campo | Descripción |
|---|---|---|---|
| 0 | u8 | `eventType` | Valor de `EventType` (tabla siguiente) |
| 1 | u32 | `tick` | Tick del evento, contado desde `Start` |
| 5 | u32 | `taskId` | Tarea a la que se refiere |
| 9 | u32 | `value` | Dato extra según el evento (por ejemplo, la latencia de una esporádica en ticks); 0 si no aplica |

| `eventType` | Nombre (`nodes/core/event_buffer.h`) |
|---|---|
| 0 | `Release`: liberación de un trabajo |
| 1 | `Start`: el trabajo empieza a ejecutar |
| 2 | `Preempt`: el trabajo es desalojado |
| 3 | `Complete`: el trabajo termina |
| 4 | `DeadlineMiss`: plazo perdido |
| 5 | `Abort`: el trabajo se aborta por plazo perdido |
| 6 | `SporadicRelease`: llegada aceptada de la esporádica |
| 7 | `SporadicIgnored`: llegada antes del intervalo mínimo, ignorada |

El protocolo transporta `eventType` sin validarlo, porque `common/` no depende del código del nodo. El host descarta y reporta los valores fuera de esta tabla.

### `TickReport` (0x08), 16 bytes

| Offset | Tipo | Campo | Descripción |
|---|---|---|---|
| 0 | u32 | `tick` | Tick actual desde `Start` |
| 4 | u32 | `runningTaskId` | Tarea en ejecución; `0` si el nodo está ocioso |
| 8 | u32 | `droppedEvents` | Eventos perdidos por búfer lleno desde `Start` |
| 12 | u32 | `missCount` | Plazos perdidos desde `Start` |

### `SporadicTrigger` (0x09), 4 bytes

| Offset | Tipo | Campo | Descripción |
|---|---|---|---|
| 0 | u32 | `taskId` | Esporádica que se libera, como si se hubiera presionado el botón |

### `Ack` (0x0A), 2 bytes

| Offset | Tipo | Campo | Descripción |
|---|---|---|---|
| 0 | u8 | `ackedSeq` | `seq` de la trama que se confirma |
| 1 | u8 | `status` | Ver tabla |

| `status` | Nombre | Significado |
|---|---|---|
| 0 | `Ok` | Mensaje aplicado |
| 1 | `BadPayload` | Trama válida, contenido inválido (longitud o campo fuera de rango, `id` 0, costo 0, etc.) |
| 2 | `Full` | El nodo no tiene espacio para otra tarea (máximo 8) o ya tiene una esporádica |
| 3 | `BadState` | El mensaje no está permitido en el estado actual (sección 6) |

## 6. Estados del nodo

```
          CalibRequest            DeployConfig / DeployTask           Start
 Idle ─────────────────▶ Calibrated ─────────────────────▶ Deploying ─────────▶ Running
  ▲                                                                              │
  └──────────────────────────────────── Stop ◀──────────────────────────────────┘
```

| Mensaje | Idle | Calibrated | Deploying | Running |
|---|---|---|---|---|
| `CalibRequest` | ✔ | ✔ (recalibra) | `BadState` | `BadState` |
| `DeployConfig` | `BadState` | ✔ | ✔ (reemplaza) | `BadState` |
| `DeployTask` | `BadState` | `BadState`¹ | ✔ | ✔ (modo manual, F10) |
| `Start` | `BadState` | `BadState`¹ | ✔ | `BadState` |
| `SporadicTrigger` | `BadState` | `BadState` | `BadState` | ✔ |
| `Stop` | ✔ (sin efecto) | ✔ | ✔ | ✔ → Idle |

¹ Primero hace falta un `DeployConfig`, porque el algoritmo tiene que estar definido antes de cargar tareas.

Si el host deja de comunicarse, el nodo **sigue ejecutando** en `Running` (F1). Solo `Stop` lo detiene.

## 7. Confirmación y reintentos

- Para cada mensaje host → nodo, el host espera la respuesta con el mismo `seq`. Si no llega en **200 ms**, reenvía la **misma trama (mismo `seq`)**, hasta **3 reintentos**. Si aun así no hay respuesta, considera el nodo no disponible (F11).
- El nodo recuerda el último `seq` procesado y su respuesta. Si recibe otra vez ese mismo `seq` con el mismo tipo, **reenvía la respuesta guardada sin volver a aplicar el mensaje**. Así, perder un `Ack` nunca despliega una tarea dos veces.
- El host incrementa `seq` en cada mensaje nuevo, con un contador por nodo. El primer `seq` de cada ejecución del host sale del reloj y del pid, para que el nodo no tome el primer mensaje de una ejecución nueva por una retransmisión de la anterior.
- El nodo lee el puerto una vez por tick, así que el host espera **200 ms + 2 ticks** por respuesta (220 ms con el tick de 10 ms). La calibración espera 1 s más, porque el nodo mide durante un tick (como máximo 100 ms) antes de responder.
- Al arrancar, el host envía primero `Stop` a cada nodo (hasta 10 intentos). Así un nodo que quedó en `Running` de una ejecución anterior vuelve a `Idle` antes de la calibración.
- El host descarta los `Event` y `TickReport` que llegan fuera de una corrida (antes de enviar `Start` o después del `Ack` de `Stop`): son restos de una ejecución anterior.

## 8. Secuencia típica

```
Host                                          Nodo
 │── CalibRequest(seq=1, tickUs=10000) ───────▶│  mide sₙ
 │◀────────────── CalibResponse(seq=1, sₙ) ────│
 │   (calcula C(i,n), admisión y asignación)    │
 │── DeployConfig(seq=2, EDF, every=10) ──────▶│
 │◀───────────────────── Ack(2, Ok) ───────────│
 │── DeployTask(seq=3, id=1, …) ──────────────▶│
 │◀───────────────────── Ack(3, Ok) ───────────│
 │   … una DeployTask por tarea asignada …      │
 │── Start(seq=k) ────────────────────────────▶│  tick = 0
 │◀───────────────────── Ack(k, Ok) ───────────│
 │◀──────────────── Event(Release, 0, 1, 0) ───│
 │◀──────────────── Event(Start, 0, 1, 0) ─────│
 │◀──────────────── TickReport(10, 1, 0, 0) ───│  cada 10 ticks
 │                    …                         │
 │── Stop(seq=k+1) ───────────────────────────▶│  vacía eventos
 │◀───────────────────── Ack(k+1, Ok) ─────────│
```

## 9. Ejemplos de tramas

Bytes en hexadecimal tal como viajan por el cable. Sirven para probar a mano o con un analizador serial.

| Mensaje | Trama |
|---|---|
| `CalibRequest`, `seq=5`, `tickUs=10000` | `7E 01 05 04 10 27 00 00 9B 59` |
| `Ack`, `seq=7`, `ackedSeq=5`, `Ok` | `7E 0A 07 02 05 00 97 1A` |
| `Start`, `seq=9` | `7E 05 09 00 9D F4` |

## 10. API en C (`common/protocol.h`)

| Para | Función |
|---|---|
| Construir un mensaje | `ProtocolPack<Mensaje>(&mensaje, &frame)` y después `frame.seq = …` |
| `Start` / `Stop` | `ProtocolPackEmpty(MessageTypeStart, &frame)` |
| Convertir a bytes | `ProtocolEncodeFrame(&frame, buf, sizeof(buf), &n)` (`buf` de `ProtocolMaxFrameSize`) |
| Recibir | `FrameParserInit` una vez y luego `FrameParserFeed(&parser, byte, &frame)` por cada byte leído; `FrameParserReady` indica que hay trama |
| Leer un mensaje | `ProtocolUnpack<Mensaje>(&frame, &mensaje)` según `frame.type` |
| Timeout a mitad de trama | `FrameParserReset(&parser)` |
| Contar tramas malas | `parser.badFrames` |

El código no usa memoria dinámica ni depende de la plataforma: compila igual en Linux, en ESP-IDF y en el Pico SDK.

## Historial

| Versión | Fecha | Cambio |
|---|---|---|
| 1.0 | 2026-10-20 | Versión inicial (M1) |
| 1.0.1 | 2026-10-24 | Aclaraciones de M3 sin cambios de formato: rango de `tickUs`, respuesta `Ack` cuando la calibración falla, tiempo de espera según el tick, `seq` inicial, saludo con `Stop` y descarte de eventos fuera de una corrida |
