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

**Objetivo.** Mostrar que las dos heurísticas de asignación producen repartos distintos con la misma carga, y qué consecuencias tiene cada una.

**Montaje.** Los dos nodos juntos (ESP32 = nodo 1, sₙ = 61; Pico = nodo 2, sₙ = 86), EDF, tick de 10 ms, `config/exp_ff_wf_ff.conf` y `config/exp_ff_wf_wf.conf`, tareas en `tasks/exp_ff_wf.csv` (todas periódicas, D = T, criticidad 1):

| Tarea | W | T = D | C en ESP32 | C en Pico |
|---|---|---|---|---|
| 1 | 400 | 20 | 7 | 5 |
| 2 | 380 | 20 | 7 | 5 |
| 3 | 330 | 20 | 6 | 4 |
| 4 | 270 | 25 | 5 | 4 |
| 5 | 150 | 25 | 3 | 2 |
| 6 | 150 | 30 | 3 | 2 |

First-Fit revisa los nodos del más rápido al más lento y deja la tarea en el primero que la acepta. Worst-Fit la deja en el nodo que queda con más capacidad libre.

**Resultados** (`docs/resultados/exp_ff.txt` y `exp_wf.txt`):

| | First-Fit | Worst-Fit |
|---|---|---|
| Tareas en el ESP32 | 6 | 2 y 4 |
| Tareas en la Pico | 1, 2, 3, 4 y 5 | 1, 3, 5 y 6 |
| U del ESP32 (teórica / medida) | 0.100 / 0.100 | 0.550 / 0.551 |
| U de la Pico (teórica / medida) | 0.940 / 0.940 | 0.597 / 0.597 |
| Tareas aceptadas | 6 de 6 | 6 de 6 |
| Plazos perdidos | 0 | 0 |
| Tiempo de respuesta máximo | 20 ticks (tarea 5, D = 25) | 13 ticks (tarea 6, D = 30) |
| Holgura mínima (D − respuesta máxima) | 5 ticks | 11 ticks |
| Desalojos | 0 | 0 |
| Duración de la corrida | 107.3 s | 55.6 s |

**Análisis.**

- **First-Fit** concentra la carga en el nodo más rápido: la Pico queda con U = 0.94 y el ESP32 casi vacío (0.10). Todo se cumple, pero con poco margen: las tareas 3 y 5 terminan a 5 ticks de su plazo, y a la Pico le quedan 0.06 de capacidad libre.
- **Worst-Fit** reparte la carga y deja los dos nodos entre 0.55 y 0.60. Los tiempos de respuesta bajan (máximo 13 ticks contra 20) y la holgura mínima sube a 11 ticks. Cada nodo conserva capacidad libre (0.45 en el ESP32 y 0.40 en la Pico) para tareas nuevas en modo manual o para recibir las de un nodo caído.
- **Qué se paga.** Worst-Fit usa los dos nodos para una carga que cabría casi toda en la Pico, y manda trabajo al nodo más lento (las tareas 2 y 4 cuestan 7 y 5 ticks en el ESP32, contra 5 y 4 en la Pico).
- **Enlace.** En ambas corridas hubo 0 tramas corruptas, 0 retransmisiones y 0 eventos perdidos.
- **Desfase entre nodos.** −5.06 ms al inicio y −11.59 ms a los 106 s (deriva −62 ppm) en First-Fit; −2.83 ms y −6.12 ms a los 54 s (−61 ppm) en Worst-Fit. Coincide con las mediciones de `docs/host.md`.

**Conclusión.** Con la misma carga, First-Fit deja un nodo casi saturado y otro casi vacío, y Worst-Fit reparte la carga y gana holgura en los plazos. Ninguna perdió plazos, porque ambas respetan U ≤ 1 en cada nodo. La diferencia está en el margen, no en que una cumpla y la otra no.

**Cómo repetirlo:**

```bash
./build/bin/host config/exp_ff_wf_ff.conf | tee exp_ff.txt
./build/bin/host config/exp_ff_wf_wf.conf | tee exp_wf.txt
```

Cada una se deja correr unos 30 s y se termina con `q`. La asignación se mantiene para sₙ del ESP32 entre 57 y 65 y de la Pico entre 82 y 90.
