# Makefile raiz - Proyecto 1 (CE 4303)
#
# Uso:
#   make         compila el host (build/bin/host) y corre todas las pruebas
#   make host    compila solo el host
#   make run     corre el host con config/ejemplo.conf (CONFIG=otro.conf para cambiarlo)
#   make help    lista todos los atajos de corrida (run-edf, run-ff, run-profesor, ...)
#   make fake    compila el nodo simulado (build/bin/fake_node) para probar sin placas
#   make test    compila y corre todas las pruebas
#   make asan    igual, pero con AddressSanitizer + UBSan (detecta segfaults)
#   make valgrind  corre todas las pruebas bajo Valgrind (fugas y memoria sin inicializar)
#   make clean   borra las carpetas de compilacion
#
# Nodo ESP32 (requiere ESP-IDF instalado en IDF_PATH):
#   make esp32          compila el firmware
#   make esp32-flash    compila y lo carga en la placa
#   make esp32-monitor  abre la consola serial (salir con Ctrl+])
#   make esp32-run      carga y abre la consola
#   Otro puerto: make esp32-run PORT=/dev/ttyACM0
#
# Nodo Raspberry Pi Pico (requiere el Pico SDK en PICO_SDK_PATH y gcc-arm-none-eabi):
#   make pico           compila el firmware (nodes/pico/build/rt_node_pico.uf2)
#   make pico-flash     lo carga con picotool (la Pico en modo BOOTSEL)
#   Sin picotool: copiar el .uf2 a la unidad RPI-RP2 que aparece en modo BOOTSEL

CC       ?= gcc
CFLAGS   := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -g -O0
INCLUDES := -Icommon -Ihost/src -Inodes/core -Itools
BUILD    := build

# Con "make asan" se activa SANITIZE y se usa otra carpeta, para no
# mezclar binarios normales con binarios instrumentados.
ifdef SANITIZE
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD  := build-asan
endif

# Codigo compartido por el host y los nodos (CRC, formato de trama)
COMMON_LIB := $(wildcard common/*.c)

# Codigo reutilizable (todo menos main.c, que se agrega mas adelante)
HOST_LIB := $(filter-out host/src/main.c,$(wildcard host/src/*.c)) $(COMMON_LIB)
NODE_LIB := $(wildcard nodes/core/*.c) $(COMMON_LIB)

# Nodo simulado sobre una pseudo terminal: usa el mismo codigo de nodes/core
FAKE_NODE_LIB := tools/fake_node.c $(wildcard nodes/core/*.c)

# Cada archivo test_*.c es un programa de prueba independiente
COMMON_TESTS := $(wildcard common/tests/test_*.c)
HOST_TESTS   := $(wildcard host/tests/test_*.c)
NODE_TESTS   := $(wildcard nodes/core/tests/test_*.c)
TEST_BINS    := $(patsubst %.c,$(BUILD)/%,$(COMMON_TESTS) $(HOST_TESTS) $(NODE_TESTS))

.PHONY: all test asan valgrind host run fake clean help fuzz run-rms-on run-rms run-edf run-ff run-wf run-estres run-profesor esp32 esp32-flash esp32-monitor esp32-run pico pico-flash

all: test host fake

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "== $$t"; ./$$t || exit 1; done
	@echo "OK: $(words $(TEST_BINS)) programa(s) de prueba"

$(BUILD)/common/tests/%: common/tests/%.c $(COMMON_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm -pthread -lrt

$(BUILD)/host/tests/%: host/tests/%.c $(HOST_LIB) $(FAKE_NODE_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm -pthread -lrt

$(BUILD)/nodes/core/tests/%: nodes/core/tests/%.c $(NODE_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm -pthread -lrt

asan:
	$(MAKE) test SANITIZE=1

# Cada prueba bajo Valgrind; falla si hay errores de memoria o fugas
valgrind: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "== valgrind $$t"; \
		valgrind -q --leak-check=full --error-exitcode=1 ./$$t || exit 1; done
	@echo "OK: valgrind sin errores en $(words $(TEST_BINS)) programa(s) de prueba"

# Programa del host: ./build/bin/host [-v] [config]
host: $(BUILD)/bin/host

$(BUILD)/bin/host: host/src/main.c $(HOST_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm -pthread -lrt

CONFIG ?= config/ejemplo.conf
# Corre el host. Si la sesion no tiene el grupo dialout, usa "sg dialout -c".
# HOSTFLAGS=-v imprime cada evento al llegar.
HOSTFLAGS ?=
RUNHOST = if command -v sg >/dev/null 2>&1 && ! id -nG | grep -qw dialout; then sg dialout -c "./$(BUILD)/bin/host $(HOSTFLAGS) $(1)"; else ./$(BUILD)/bin/host $(HOSTFLAGS) $(1); fi

run: $(BUILD)/bin/host
	@$(call RUNHOST,$(CONFIG))

# Atajos de corrida (cada uno termina con q)
run-rms-on: $(BUILD)/bin/host   # RMS con admision activa (rechaza la tarea 1)
	@$(call RUNHOST,config/exp_rms_edf_rms_on.conf)

run-rms: $(BUILD)/bin/host      # RMS sin admision: pierde plazos
	@$(call RUNHOST,config/exp_rms_edf_rms.conf)

run-edf: $(BUILD)/bin/host      # EDF con la misma carga: 0 plazos perdidos
	@$(call RUNHOST,config/exp_rms_edf_edf.conf)

run-ff: $(BUILD)/bin/host       # First-Fit con las dos placas
	@$(call RUNHOST,config/exp_ff_wf_ff.conf)

run-wf: $(BUILD)/bin/host       # Worst-Fit con las dos placas
	@$(call RUNHOST,config/exp_ff_wf_wf.conf)

run-estres: $(BUILD)/bin/host   # 8 tareas en el ESP32
	@$(call RUNHOST,config/estres_8tareas.conf)

run-profesor: $(BUILD)/bin/host # tareas del profesor (config/profesor.conf)
	@$(call RUNHOST,config/profesor.conf)

# Prueba de comandos al azar con nodos simulados (N corridas: make fuzz N=20)
N ?= 10
fuzz: $(BUILD)/bin/host $(BUILD)/bin/fake_node
	tools/fuzz_comandos.sh $(N)

help:
	@echo "Compilar y probar:  make | make test | make asan | make valgrind | make fuzz"
	@echo "Correr el host (placas conectadas; HOSTFLAGS=-v para ver eventos):"
	@echo "  make run           config/ejemplo.conf (o CONFIG=otro.conf)"
	@echo "  make run-rms-on    RMS con admision activa"
	@echo "  make run-rms       RMS sin admision (pierde plazos)"
	@echo "  make run-edf       EDF con la misma carga (0 plazos perdidos)"
	@echo "  make run-ff        First-Fit"
	@echo "  make run-wf        Worst-Fit"
	@echo "  make run-estres    8 tareas en el ESP32"
	@echo "  make run-profesor  tareas del profesor (config/profesor.conf)"
	@echo "Cargar nodos: make esp32-run | make pico | make pico-flash"

# Nodo simulado: ./build/bin/fake_node [sn] imprime el puerto que hay que poner en "ports"
fake: $(BUILD)/bin/fake_node

$(BUILD)/bin/fake_node: tools/fake_node_main.c $(FAKE_NODE_LIB) $(COMMON_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm -pthread -lrt

clean:
	rm -rf build build-asan

# ---- Nodo ESP32 ----
IDF_PATH ?= $(HOME)/esp/esp-idf
PORT     ?= /dev/ttyUSB0
# Carga el entorno de ESP-IDF en un bash aparte y corre idf.py en nodes/esp32
IDF_PY := bash -c '. "$(IDF_PATH)/export.sh" > /dev/null && cd nodes/esp32 && idf.py "$$@"' idf.py

esp32:
	$(IDF_PY) build

esp32-flash:
	$(IDF_PY) -p $(PORT) flash

esp32-monitor:
	$(IDF_PY) -p $(PORT) monitor

esp32-run:
	$(IDF_PY) -p $(PORT) flash monitor

# ---- Nodo Raspberry Pi Pico ----
PICO_SDK_PATH ?= $(HOME)/pico/pico-sdk
PICO_BUILD    := nodes/pico/build

pico:
	cmake -S nodes/pico -B $(PICO_BUILD) -DPICO_SDK_PATH="$(PICO_SDK_PATH)"
	cmake --build $(PICO_BUILD)

pico-flash: pico
	picotool load -x $(PICO_BUILD)/rt_node_pico.uf2
