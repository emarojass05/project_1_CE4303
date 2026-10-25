# Experimentos

Reportes completos de cada corrida en `docs/resultados/`.

## 1. RMS vs EDF con una carga que solo cumple EDF

**Objetivo.** Mostrar que EDF aprovecha el procesador hasta U = 1 y que RMS, con la misma carga, pierde plazos.

**Montaje.** Un solo nodo (ESP32, sₙ = 61 en la corrida), tick de 10 ms, `config/exp_rms_edf_*.conf`, tareas en `tasks/exp_rms_edf.csv`:

| Tarea | W | T = D (ticks) | C en el ESP32 (ticks) | U |
|---|---|---|---|---|
| 1 | 110 | 5 | 2 | 0.400 |
| 2 | 220 | 7 | 4 | 0.571 |
| Total | | | | **0.971** |

La cota de RMS para 2 tareas es 2(2^(1/2) − 1) = 0.828, así que la prueba de admisión de RMS no acepta la carga completa. EDF la acepta porque U ≤ 1. Para ver qué pasa si RMS corriera igual, `admission = off` salta la prueba de admisión (el host solo exige U ≤ 1) y despliega la carga completa.

**Resultados** (`docs/resultados/exp_rms_on.txt`, `exp_rms.txt`, `exp_edf.txt`):

| Corrida | Tareas aceptadas | Duración | Plazos perdidos | U medida | Desalojos |
|---|---|---|---|---|---|
| RMS, admisión activa | solo la 2 (la 1 se rechaza) | 44.2 s | 0 | 0.572 | 0 |
| RMS, admisión desactivada | 1 y 2 | 46.7 s | **134** (todos de la tarea 2) | 0.943 | 667 |
| EDF | 1 y 2 | 55.7 s | **0** | 0.971 | 159 |

**Análisis.**

- **RMS sin admisión.** La tarea 1 (T = 5) tiene mayor prioridad y nunca falla. La tarea 2 pierde un trabajo cada 35 ticks (tick 7, 42, 77, ...), es decir una vez por hiperperíodo mcm(5, 7) = 35: 134 plazos perdidos en 668 liberaciones (20 %). El Gantt muestra el `!` y el trabajo abortado. Coincide con el análisis de tiempo de respuesta: R₂ = 4 + ⌈R₂/5⌉·2 converge en 8, mayor que D = 7.
- **EDF.** Con la misma carga y U = 0.971 no hay plazos perdidos. El tiempo de respuesta máximo fue 4 ticks para la tarea 1 (D = 5) y 6 para la tarea 2 (D = 7). La utilización medida (0.971) coincide con la teórica.
- **RMS con admisión.** El host rechaza la tarea 1 con el motivo "fails the admission test on every node". La carga que sí se acepta (solo la tarea 2) no tiene plazos perdidos, pero se pierde una tarea completa.
- **Enlace.** En las tres corridas hubo 0 tramas corruptas, 0 retransmisiones y 0 eventos perdidos; los 134 "misses" del enlace en la corrida RMS son los plazos perdidos del nodo.

**Conclusión.** Con U = 0.971, EDF cumple todos los plazos y RMS pierde uno de cada cinco trabajos de la tarea 2. La cota de RMS es suficiente pero no necesaria; con esta carga la prueba de admisión de RMS acierta en no aceptarla.

**Cómo repetirlo:**

```bash
./build/bin/host config/exp_rms_edf_rms_on.conf | tee exp_rms_on.txt
./build/bin/host config/exp_rms_edf_rms.conf | tee exp_rms.txt
./build/bin/host config/exp_rms_edf_edf.conf | tee exp_edf.txt
```

Cada una se deja correr unos 30 s y se termina con `q`. La carga está calculada para sₙ entre 55 y 73 (ESP32); con la Pico (sₙ ≈ 86) la tarea 2 cuesta C = 3 y RMS sí cumple.

## 2. First-Fit vs Worst-Fit

Pendiente.
