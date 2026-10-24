# Makefile raiz - Proyecto 1 (CE 4303)
#
# Uso:
#   make         compila el host (build/host_demo) y corre todas las pruebas
#   make test    compila y corre todas las pruebas
#   make asan    igual, pero con AddressSanitizer + UBSan (detecta segfaults)
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
INCLUDES := -Icommon -Ihost/src -Inodes/core
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

# Cada archivo test_*.c es un programa de prueba independiente
COMMON_TESTS := $(wildcard common/tests/test_*.c)
HOST_TESTS   := $(wildcard host/tests/test_*.c)
NODE_TESTS   := $(wildcard nodes/core/tests/test_*.c)
TEST_BINS    := $(patsubst %.c,$(BUILD)/%,$(COMMON_TESTS) $(HOST_TESTS) $(NODE_TESTS))

.PHONY: all test asan demo clean esp32 esp32-flash esp32-monitor esp32-run pico pico-flash

all: test demo

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "== $$t"; ./$$t || exit 1; done
	@echo "OK: $(words $(TEST_BINS)) programa(s) de prueba"

$(BUILD)/common/tests/%: common/tests/%.c $(COMMON_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm -pthread -lrt

$(BUILD)/host/tests/%: host/tests/%.c $(HOST_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm -pthread -lrt

$(BUILD)/nodes/core/tests/%: nodes/core/tests/%.c $(NODE_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ -lm -pthread -lrt

asan:
	$(MAKE) test SANITIZE=1

# Host program: run it with ./build/host_demo
demo: $(BUILD)/host_demo

$(BUILD)/host_demo: host/src/main.c $(HOST_LIB)
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
