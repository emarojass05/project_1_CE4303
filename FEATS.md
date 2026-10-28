# Proyecto 1 — Calendarizador de Tiempo Real Heterogéneo (CE 4303)

Entrega: **22 de octubre de 2026, 23:59** (TEC-Digital).
Grupo de 4 personas. Penalización: **-5 pts por cada segfault / core dumped**.

Leyenda: `[ ]` pendiente · `[x]` listo · **Resp.:** responsable · **Dep.:** dependencias

---

## Base compartida

### F0 — Protocolo serial (Comunicación, 15%)
**Resp.:** P2  **Dep.:** ninguna (bloquea a casi todo)
- [x] Definir formato de trama: marca de inicio, tipo, longitud, payload, CRC-16
- [x] Definir tipos de mensaje: solicitud/respuesta de calibración, despliegue de tareas, inicio, evento, reporte de tick, disparo de tarea esporádica, detención, ACK
- [x] Definir la estructura del payload de cada mensaje (incluye duración del tick en la solicitud de calibración)
- [x] Implementar CRC-16 (misma implementación en host y nodos)
- [x] Documentar el formato exacto
- [x] Probar que las tramas corruptas se descartan y se reportan

---

## Nodos (ESP32 / Raspberry Pi Pico o similar — no Arduino)

### F1 — Calendarizador local (20%)
**Resp.:** P1  **Dep.:** F0 (para desplegar tareas)
- [x] Tick generado por timer de hardware (duración configurable, 10 ms por defecto)
- [x] Integrar `ut.c` / `ut.h` sin modificarlos
- [x] Tareas como máquinas de estados (avance de un tick a la vez)
- [x] Implementar RMS (prioridades fijas, menor período = mayor prioridad)
- [x] Implementar EDF
- [x] Selección de algoritmo por configuración al desplegar
- [x] Definir y documentar criterio de desempate (consistente en toda la ejecución)
- [x] Soportar al menos 8 tareas simultáneas (periódicas y esporádicas)
- [x] Plazo perdido: registrar evento, activar indicador y abortar el trabajo, continuar con los demás
- [x] Los nodos siguen ejecutando si el host se detiene o desconecta
- [ ] Verificar: sin `delay()`, `sleep` ni busy waiting; sin RTOS decidiendo el orden

### F2 — Calibración
**Resp.:** P1  **Dep.:** F0
- [x] Medir UT por tick (sₙ) dejando margen para scheduler y envío de eventos
- [x] Responder al host con el valor medido
- [x] Repetir la medición en cada ejecución (nada fijo en el código)

### F3 — Eventos y reporte de tick
**Resp.:** P2  **Dep.:** F0, F1
- [x] Búfer circular en RAM para eventos
- [x] Transmisión en segundo plano (sin provocar pérdida de plazos)
- [x] Marca de tiempo en ticks desde el mensaje de inicio
- [x] Reporte de tick cada N ticks (N configurable)
- [x] Tick cero establecido por el mensaje de inicio

### F4 — Tarea esporádica (5%)
**Resp.:** P1  **Dep.:** F1, F5
- [x] Botón en pin con interrupción + debounce
- [x] ISR solo registra la liberación; el scheduler hace el resto
- [x] Máximo una tarea esporádica por nodo
- [x] Ignorar y contabilizar eventos antes del intervalo mínimo
- [x] Medir latencia de respuesta para el reporte

### F5 — Hardware (Hardware e integración, 10%)
**Resp.:** P2  **Dep.:** ninguna
- [x] LEDs con la tarea en ejecución codificada en binario
- [x] LED de plazo perdido
- [x] Botón de la tarea esporádica
- [x] Verificar que todo refleja físicamente lo que ocurre (sin soluciones alambradas)

---

## Host (C sobre Linux, con Makefile)

### F6 — Configuración y carga de tareas
**Resp.:** P3  **Dep.:** ninguna
- [x] Archivo de configuración: algoritmo (RMS/EDF), heurística, duración del tick, puertos seriales, período del reporte de tick, reportes perdidos para declarar falla, ruta del archivo de tareas
- [x] Parser del archivo de tareas (`id,tipo,W,T,D,criticidad`; tipo P o S)
- [x] Validación robusta: ninguna entrada inválida causa segfault ni terminación abrupta
- [x] Interacción simple (línea de comandos o interfaz mínima)

### F7 — Planificación y admisión (15%)
**Resp.:** P3  **Dep.:** F2, F6
- [x] Costo por nodo: C(i,n) = ⌈W(i) / sₙ⌉
- [x] Ordenar tareas de mayor a menor utilización (medida en el nodo de mayor sₙ; desempate del grupo)
- [x] Prueba de admisión EDF: ΣC/T ≤ 1
- [x] Prueba de admisión RMS: ΣC/T ≤ n(2^(1/n) − 1)
- [x] Heurística First-Fit (nodos en orden decreciente de sₙ)
- [x] Heurística Worst-Fit (mayor capacidad libre tras asignar)
- [x] Rechazar tarea que no cabe en ningún nodo e informar el motivo
- [x] Tarea esporádica admitida como periódica con período = intervalo mínimo

### F8 — Driver por nodo
**Resp.:** P2  **Dep.:** F0
- [x] Apertura y configuración del puerto serial
- [x] Envío/recepción de tramas con verificación de CRC
- [x] Solicitud de calibración (con duración del tick) y recepción de sₙ
- [x] Despliegue de tareas y manejo de ACK
- [x] Mensaje de inicio (tick cero en todos los nodos)
- [x] Mensaje de detención
- [x] Descartar y reportar tramas corruptas

### F9 — Arquitectura multiproceso y terminación
**Resp.:** P4  **Dep.:** F8
- [x] Al menos 4 procesos: planificador, un driver por nodo, proceso de reporte
- [x] Memoria compartida POSIX (`shm_open`, `mmap`) y semáforos POSIX
- [x] Cola de eventos acotada y sincronizada (drivers productores, reporte consumidor)
- [x] Sin busy waiting en el host
- [x] Tecla `q`: terminación elegante (detener nodos, finalizar procesos, generar reporte)
- [x] Liberar todos los recursos y cerrar puertos seriales

### F10 — Modo automático y manual
**Resp.:** P3  **Dep.:** F7, F9
- [x] Modo automático: carga inicial desde el archivo de tareas
- [x] Modo manual: agregar tareas durante la ejecución
- [x] Nueva admisión y envío al nodo correspondiente sin detener el sistema

### F11 — Caída de nodos y replanificación (10%)
**Resp.:** P3  **Dep.:** F3, F7, F9
- [x] Declarar nodo caído tras N reportes consecutivos perdidos
- [x] Replanificar con la misma heurística y criterio de admisión
- [x] Las tareas del nodo sobreviviente se mantienen; solo se reubican las del caído
- [x] Si no caben todas, rechazar primero las de menor criticidad
- [x] Tareas reubicadas reinician su ejecución en el nuevo nodo
- [x] Tareas esporádicas del nodo caído: no se reubican, se reportan como rechazadas
- [x] Probar falla por desconexión de cable y por comando del host

### F12 — Sincronización temporal
**Resp.:** P2  **Dep.:** F8
- [x] Medir el desfase entre nodos
- [x] Documentar el desfase medido

---

## Salidas

### F13 — Reporte (Reporte y experimentos, 10%)
**Resp.:** P4  **Dep.:** F9 (cola de eventos)
- [x] Recibir datos únicamente por la cola de eventos en memoria compartida
- [x] Diagrama de Gantt por nodo
- [x] Liberaciones, ejecución, plazos y fallas
- [x] Estadísticas por tarea
- [x] Utilización teórica y medida por nodo
- [x] Cambios de tarea
- [x] Tareas aceptadas y rechazadas con su motivo
- [x] Latencia de las tareas esporádicas

### F14 — Documentación y entregables (15%)
**Resp.:** fuera del reparto (la documentación se coordina aparte). Los experimentos sí están asignados: RMS vs EDF → P1, First-Fit vs Worst-Fit → P4; conjuntos de tareas de ejemplo → P4  **Dep.:** todo lo anterior
- [x] Experimento: RMS vs EDF con una carga que solo cumpla EDF
- [x] Experimento: First-Fit vs Worst-Fit con una carga que produzca asignaciones distintas
- [ ] Documento del atributo Trabajo en Equipo: los 7 puntos (a–g), cada uno con pregunta y respuesta
- [ ] Código fuente con documentación interna
- [x] Makefile
- [ ] Instrucciones de compilación y carga
- [x] Archivos de configuración de ejemplo
- [x] Conjuntos de tareas de ejemplo
- [ ] Revisar que todos los integrantes puedan explicar cualquier parte (defensa con todos presentes)

---

## Orden sugerido (el plan detallado por milestones y personas está en [`MILESTONES.md`](MILESTONES.md))

1. F0
2. En paralelo: F1 + F2 (nodo) y F6 + F7 (host); F5 (hardware) puede avanzar desde ya
3. F8 para unir host y nodos
4. F3, F4, F9, F12
5. F10, F11, F13
6. F14 (los experimentos requieren el sistema funcionando)
