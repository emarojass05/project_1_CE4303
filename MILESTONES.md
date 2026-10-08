# Plan por milestones — Proyecto 1 (CE 4303)

Entrega: **jueves 22 de octubre 2026** (límite 23:59; meta: subir antes del mediodía).
Detalle de cada feat (F0–F13) en [`FEATS.md`](FEATS.md).

> La documentación (documento, Atributo TE, README, instrucciones) **no está incluida** en este reparto; se coordina aparte.

Roles provisionales (cambiar por nombres reales): **P1**, **P2**, **P3**, **P4**.
**P1 es la única persona que no necesita Linux** (todo su trabajo es firmware del nodo), por eso es la recomendada para quien no tiene Linux.

## Reparto de carga (equitativo)

Peso estimado en "puntos" (≈ días de trabajo de una persona). Total ≈ 57.5 → ≈ 14.4 por persona.

| Persona | Foco | Feats / tareas | Puntos |
|---|---|---|---|
| **P1** | Nodo: calendarizador | F1 (8), F2 (2), F4 (3), experimento RMS vs EDF (2) | 15 |
| **P2** | Protocolo, hardware y driver serial | F0 (3), F3 (3), F5 (2), F8 (5), F12 (1.5) | 14.5 |
| **P3** | Host: planificación y tolerancia a fallas | F6 (2), F7 (5), F10 (2), F11 (5) | 14 |
| **P4** | Host: procesos, reporte e integración | F9 (6), F13 (5), experimento First-Fit vs Worst-Fit (2), conjuntos de tareas de ejemplo (1) | 14 |

Puntos por feat: F0 3 · F1 8 · F2 2 · F3 3 · F4 3 · F5 2 · F6 2 · F7 5 · F8 5 · F9 6 · F10 2 · F11 5 · F12 1.5 · F13 5.

Reglas de trabajo:
- Cada milestone termina con un **gate** (demo/verificación). No se arranca el siguiente sin cumplirlo, salvo acuerdo del grupo.
- El protocolo (F0) se congela al cerrar M1; cambios posteriores requieren acuerdo de los 4.
- El núcleo del calendarizador (F1) se escribe independiente del hardware, con una capa fina por placa (ESP32 / Pico), para no duplicar lógica.
- Antes del M3, las partes que dependen de otras usan datos simulados (por ejemplo, sₙ fijo solo para pruebas unitarias del host y tareas hardcodeadas solo para probar el nodo). En la integración se eliminan.
- Todos deben poder explicar cualquier parte en la defensa: revisión cruzada de código en cada gate.
- Las pruebas de integración (M3 en adelante) requieren correr el host en Linux. P1 puede avanzar todo el firmware sin Linux y probar la integración en la máquina Linux de P2, P3 o P4.

---

## M1 — Fundación y contratos (8–10 oct)

- [ ] **P1** F1 (parte 1): timer de hardware para el tick, integrar `ut.c`/`ut.h`, estructura de tarea como máquina de estados (API del scheduler)
- [ ] **P2** F0: especificar trama y mensajes (en `docs/protocolo.md`) + librería CRC-16 compartida host/nodo con pruebas
- [ ] **P3** F6: parser de configuración y de archivo de tareas con validación y pruebas de entradas inválidas
- [ ] **P4** Estructura del repo (host/, nodes/, docs/, config/, tasks/), Makefile base, F9 (parte 1): prototipo de `shm_open`/`mmap` + semáforos POSIX entre procesos

**Gate M1:** protocolo aprobado por los 4 · el tick corre en un ESP32 y en una Pico · parser pasa pruebas · el host compila con `make`.

## M2 — Componentes aislados (11–13 oct)

- [ ] **P1** F1 (parte 2): RMS, EDF, desempate, 8 tareas, plazo perdido con aborto del trabajo (con tareas de prueba)
- [ ] **P2** F5: LEDs en binario, LED de plazo perdido y botón · F3: búfer circular de eventos y transmisión en segundo plano
- [ ] **P3** F7: costo C(i,n), orden por utilización, admisión EDF/RMS, First-Fit y Worst-Fit, rechazo con motivo (pruebas unitarias con sₙ simulado)
- [ ] **P4** F9 (parte 2): cola de eventos acotada productor/consumidor en shm + semáforos, proceso de reporte que consume eventos de prueba, terminación con `q` sin fugas

**Gate M2:** el nodo ejecuta tareas de prueba y cumple/pierde plazos según lo esperado · las pruebas de admisión pasan · la cola IPC funciona sin busy waiting.

## M3 — Primer end-to-end (14–16 oct)

- [ ] **P1** F2: calibración en el nodo (sₙ medido en cada ejecución) · recepción de tareas desplegadas y mensaje de inicio (tick cero)
- [ ] **P2** F8: driver serial del host (puerto, tramas, CRC, ACK) y el lado receptor de tramas del nodo
- [ ] **P3** F10 (modo automático): flujo completo calibración → costo → asignación → despliegue, integrando planificador y drivers
- [ ] **P4** F9 (parte 3): planificador + un driver por nodo + proceso de reporte corriendo juntos, reporte básico en texto · conjuntos de tareas de ejemplo

**Gate M3:** demo con 2 nodos reales: el host calibra, asigna, despliega, inicia y el reporte muestra eventos.

## M4 — Funcionalidades completas (17–19 oct)

- [ ] **P1** F4: tarea esporádica (botón, debounce, ISR mínima, intervalo mínimo, latencia)
- [ ] **P2** F3: reporte de tick cada N ticks · F12: medición del desfase entre nodos · tramas corruptas descartadas y reportadas · el nodo sigue corriendo si el host cae
- [ ] **P3** F11: detección de nodo caído, replanificación, rechazo por criticidad · F10 (modo manual): agregar tareas en ejecución
- [ ] **P4** F13: reporte completo (Gantt por nodo, estadísticas por tarea, utilización teórica y medida, cambios de tarea, aceptadas/rechazadas, latencia esporádicas)

**Gate M4:** todas las secciones de la rúbrica funcionan al menos una vez de punta a punta.

## M5 — Pruebas, experimentos y robustez (20–21 oct)

- [ ] **P1** Experimento RMS vs EDF (carga que solo cumple EDF) · prueba de estrés del nodo (8 tareas)
- [ ] **P2** Pruebas de robustez del protocolo (cable desconectado, tramas corruptas)
- [ ] **P3** Pruebas de entradas inválidas · prueba de falla de nodo por cable y por comando
- [ ] **P4** Experimento First-Fit vs Worst-Fit (carga con asignaciones distintas) · Valgrind/ASan, liberación de recursos y cierre de puertos · Makefile final

**Gate M5:** cero segfaults en 3 corridas completas seguidas · ambos experimentos con resultados · tecla `q` genera el reporte y cierra limpio.

## M6 — Cierre y entrega (22 oct)

- [ ] **Todos** Congelar el código y hacer una corrida final completa con los 2 nodos
- [ ] **Todos** Ensayo de defensa: cada integrante explica una parte que no escribió
- [ ] **Todos** Verificar entregables de código: fuentes, configs, tareas de ejemplo, Makefile
- [ ] Subir a TEC-Digital (meta antes del mediodía; límite 23:59, sin entregas tardías)
