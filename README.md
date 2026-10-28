# Proyecto 1 — Calendarizador de Tiempo Real Heterogéneo (CE 4303)

Un host Linux reparte tareas entre microcontroladores (ESP32 y Raspberry Pi Pico) que las ejecutan con un calendarizador local RMS o EDF. El host calibra cada nodo, asigna las tareas con una heurística (First-Fit o Worst-Fit) y una prueba de admisión, detecta nodos caídos, replanifica y al final genera un reporte con diagrama de Gantt, utilización y desfase entre nodos.

## Documentación

| Documento | Contenido |
|---|---|
| [`docs/nodos.md`](docs/nodos.md) | Conexiones, compilación y carga de los nodos (ESP32 y Pico) |
| [`docs/host.md`](docs/host.md) | Host: procesos, driver serial, modos automático y manual, caída de nodos, reporte y desfase |
| [`docs/calendarizador.md`](docs/calendarizador.md) | Calendarizador local de los nodos, desempate y calibración |
| [`docs/protocolo.md`](docs/protocolo.md) | Protocolo serial y formato de las tramas |
| [`docs/experimentos.md`](docs/experimentos.md) | RMS vs EDF, First-Fit vs Worst-Fit y prueba de estrés de 8 tareas |
| [`docs/pruebas.md`](docs/pruebas.md) | Qué pruebas existen, dónde están y cómo correrlas |
| [`FEATS.md`](FEATS.md), [`MILESTONES.md`](MILESTONES.md) | Requisitos, reparto de trabajo y plan por milestones |

## Estructura

| Carpeta | Contenido |
|---|---|
| `common/` | Código en C portable compartido por host y nodos (CRC-16, formato de trama) |
| `common/tests/` | Pruebas del código compartido |
| `host/src/` | Código del host (planificador, drivers, reporte) |
| `host/tests/` | Pruebas del host |
| `nodes/core/` | Núcleo del calendarizador, independiente de la placa |
| `nodes/core/tests/` | Pruebas del núcleo (corren en la PC) |
| `nodes/app/` | Programa del nodo, común a las dos placas |
| `nodes/ut/` | `ut.c` y `ut.h` del profesor, sin modificar |
| `nodes/esp32/` | Capa específica del ESP32 (timer, GPIO, serial) |
| `nodes/pico/` | Capa específica de la Raspberry Pi Pico |
| `tools/` | Nodo simulado (`fake_node`) y script de fuzzing (`fuzz_comandos.sh`) |
| `docs/` | Documentación y resultados de los experimentos (`docs/resultados/`) |
| `config/` | Archivos de configuración de ejemplo y de los experimentos |
| `tasks/` | Conjuntos de tareas de ejemplo y de los experimentos |

## Requisitos

- Linux con `gcc` y `make`.
- `valgrind` (opcional, solo para `make valgrind`).
- Para las placas reales: ESP-IDF 5.3 o posterior (ESP32) y el Pico SDK 2.x con `gcc-arm-none-eabi` (Pico). Ver [`docs/nodos.md`](docs/nodos.md).
- Para abrir los puertos seriales hay que estar en el grupo `dialout` (`sudo usermod -aG dialout $USER` y volver a iniciar sesión).

## Compilar y probar

```
make            # compila el host, el nodo simulado y corre todas las pruebas
make host       # compila solo el host (build/bin/host)
make fake       # compila el nodo simulado (build/bin/fake_node)
make test       # compila y corre todas las pruebas (archivos test_*.c)
make asan       # igual, con AddressSanitizer y UBSan: detecta accesos inválidos a memoria
make valgrind   # corre las pruebas bajo Valgrind: fugas y memoria sin inicializar
make clean      # borra build/ y build-asan/
```

Cada archivo `test_*.c` dentro de una carpeta `tests/` es un programa de prueba independiente: devuelve 0 si pasa y distinto de 0 si falla. El detalle está en [`docs/pruebas.md`](docs/pruebas.md).

Para probar los comandos del host con entradas válidas, inválidas y al azar:

```
tools/fuzz_comandos.sh 10               # 10 corridas con nodos simulados
VALGRIND=1 tools/fuzz_comandos.sh 3     # el host corre bajo Valgrind
```

## Cargar los nodos

```
make esp32-run                 # compila, carga el ESP32 y abre el monitor (salir con Ctrl+])
make esp32-run PORT=/dev/ttyUSB1
make pico                      # genera nodes/pico/build/rt_node_pico.uf2
make pico-flash                # carga con picotool (la Pico en modo BOOTSEL)
```

Sin `picotool`: mantener presionado BOOTSEL, conectar el USB, y copiar el `.uf2` a la unidad `RPI-RP2`. Las conexiones de LEDs y botón están en [`docs/nodos.md`](docs/nodos.md). Antes de correr el host hay que cerrar el monitor del ESP32: dos programas no pueden usar el mismo puerto.

## Correr el host

### Con las placas

Conectar el ESP32 y la Pico y revisar sus puertos (`ls /dev/ttyUSB* /dev/ttyACM*`). Ajustar `ports` en la configuración y correr:

```
make run                              # usa config/ejemplo.conf
./build/bin/host config/ejemplo.conf  # igual, directo
./build/bin/host -v config/ejemplo.conf   # -v imprime cada evento al llegar
```

Si el usuario no tiene el grupo `dialout` activo en la sesión: `sg dialout -c "./build/bin/host config/ejemplo.conf"`.

### Sin placas (nodos simulados)

En una terminal por cada nodo simulado:

```
./build/bin/fake_node 61     # imprime el puerto, por ejemplo /dev/pts/5
./build/bin/fake_node 86
```

Copiar esos puertos en `ports` de un archivo de configuración y correr el host con él.

### Comandos durante la ejecución

| Tecla | Dato | Qué hace |
|---|---|---|
| `q` | — | Detiene los nodos, termina los procesos y genera el reporte |
| `a` | `id,tipo,W,T,D,criticidad` | Modo manual: agrega una tarea sin detener nada |
| `k` | número de nodo | Tumba un nodo: el host lo detecta como caído y replanifica |
| `s` | número de nodo | Libera la tarea esporádica de ese nodo, igual que su botón |
| `h` | — | Ayuda |

En una terminal basta la tecla; `a`, `k` y `s` piden el dato y se confirman con Enter. Si la entrada es un pipe, cada comando va en una línea (`a 9,P,200,40,40,2`, `k 2`, `s 1`, `q`). También se termina con Ctrl+C. Si un nodo se desconecta por cable el host lo declara caído solo.

## Configuración

Archivo de texto con `clave = valor` (ver `config/ejemplo.conf`):

| Clave | Valores | Por defecto |
|---|---|---|
| `algorithm` | `RMS` o `EDF` | obligatoria |
| `heuristic` | `first-fit` o `worst-fit` | obligatoria |
| `tick_ms` | 1 a 1000 | 10 |
| `ports` | puertos seriales separados por comas, uno por nodo | obligatoria |
| `tick_report_every` | 1 a 65535 | 10 |
| `missed_reports_for_failure` | reportes seguidos perdidos para declarar caído un nodo | 3 |
| `admission` | `on` u `off` (solo para experimentos: `off` asigna con U ≤ 1 sin la cota de RMS) | on |
| `tasks_file` | ruta del archivo de tareas | obligatoria |

El archivo de tareas es un CSV `id,tipo,W,T,D,criticidad` donde `tipo` es `P` (periódica) o `S` (esporádica). Ver `tasks/ejemplo.csv`.

## Flujo de Git

- `main`: versión entregable. `develop`: base de integración.
- Cada milestone o feature va en una rama `feat/<nombre>` (por ejemplo `feat/M4`) y se une a `develop` cuando pasan sus pruebas (`make`, `make asan`, `make valgrind`).
- Mensajes de commit cortos, con etiqueta entre corchetes y en minúsculas: `[add] ...`, `[fix] ...`.
