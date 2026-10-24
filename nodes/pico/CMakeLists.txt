# Proyecto del nodo Raspberry Pi Pico (Pico SDK, C). Compilar desde la raiz con
#   make pico            (requiere PICO_SDK_PATH apuntando al pico-sdk)
# o desde esta carpeta con
#   cmake -S . -B build && cmake --build build
# El nucleo (nodes/core) y el programa del nodo (nodes/app) son los mismos del
# ESP32; aqui solo se agrega la capa especifica de la Pico.
cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
if(NOT DEFINED PICO_SDK_PATH)
    message(FATAL_ERROR "Defina PICO_SDK_PATH con la ruta del pico-sdk")
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(rt_node_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

set(NODES_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
set(COMMON_DIR ${CMAKE_CURRENT_LIST_DIR}/../../common)

add_executable(rt_node_pico
    main.c
    board_pico.c
    ${NODES_DIR}/app/node_app.c
    ${NODES_DIR}/core/scheduler.c
    ${NODES_DIR}/core/event_buffer.c
    ${NODES_DIR}/core/event_tx.c
    ${NODES_DIR}/core/node_link.c
    ${NODES_DIR}/core/indicators.c
    ${NODES_DIR}/core/calibration.c
    ${NODES_DIR}/core/debounce.c
    ${NODES_DIR}/ut/ut.c
    ${COMMON_DIR}/protocol.c
    ${COMMON_DIR}/crc16.c
)

target_include_directories(rt_node_pico PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${NODES_DIR}/app
    ${NODES_DIR}/core
    ${NODES_DIR}/ut
    ${COMMON_DIR}
)

target_compile_options(rt_node_pico PRIVATE -Wall -Wextra)

target_link_libraries(rt_node_pico pico_stdlib hardware_gpio hardware_sync)

# El protocolo va por el USB (aparece como /dev/ttyACM0); la UART queda libre
pico_enable_stdio_usb(rt_node_pico 1)
pico_enable_stdio_uart(rt_node_pico 0)

# Genera rt_node_pico.uf2 para arrastrar a la unidad RPI-RP2 (modo BOOTSEL)
pico_add_extra_outputs(rt_node_pico)
