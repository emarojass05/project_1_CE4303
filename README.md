# Proyecto 1 — Calendarizador de Tiempo Real Heterogéneo (CE 4303)

Un host Linux reparte tareas entre microcontroladores (ESP32, Raspberry Pi Pico)
que las ejecutan con un calendarizador local RMS o EDF.

- Requisitos y reparto de trabajo: [`FEATS.md`](FEATS.md)
- Plan por milestones: [`MILESTONES.md`](MILESTONES.md)

## Estructura

| Carpeta | Contenido |
|---|---|
| `common/` | Código en C portable compartido por host y nodos (CRC-16, formato de trama) |
| `common/tests/` | Pruebas del código compartido |
| `host/src/` | Código del host (planificador, drivers, reporte) |
| `host/tests/` | Pruebas del host |
| `nodes/core/` | Núcleo del calendarizador, independiente de la placa |
| `nodes/core/tests/` | Pruebas del núcleo (corren en la PC) |
| `nodes/esp32/` | Capa específica del ESP32 (timer, GPIO, serial) |
| `nodes/pico/` | Capa específica de la Raspberry Pi Pico |
| `docs/` | Documentación (protocolo, experimentos, atributo TE) |
| `config/` | Archivos de configuración de ejemplo |
| `tasks/` | Conjuntos de tareas de ejemplo |

## Compilar y probar

```
make test    # compila y corre todas las pruebas (archivos test_*.c)
make asan    # igual, con AddressSanitizer: detecta accesos inválidos a memoria
make clean   # borra build/ y build-asan/
```

Cada archivo `test_*.c` dentro de una carpeta `tests/` es un programa de prueba
independiente: devuelve 0 si pasa y distinto de 0 si falla.

## Flujo de Git

- `main`: versión entregable. `develop`: base de integración.
- Cada feature va en `feature/<nombre>` y se une a `develop` al pasar sus pruebas.
- Mensajes de commit cortos, con prefijo: `feat(host): ...`, `feat(node): ...`,
  `chore: ...`, `docs: ...`, `test: ...`.
