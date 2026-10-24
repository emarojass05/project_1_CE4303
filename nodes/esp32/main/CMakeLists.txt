# El nucleo del calendarizador (nodes/core) y el programa del nodo (nodes/app)
# se comparten con la Pico; aqui solo se agrega la capa especifica del ESP32.
# ut.c/ut.h (del profesor, sin modificar) estan en nodes/ut.
idf_component_register(
    SRCS "main.c"
         "board_esp32.c"
         "../../app/node_app.c"
         "../../core/scheduler.c"
         "../../core/event_buffer.c"
         "../../core/event_tx.c"
         "../../core/indicators.c"
         "../../core/calibration.c"
         "../../core/debounce.c"
         "../../ut/ut.c"
         "../../../common/protocol.c"
         "../../../common/crc16.c"
    INCLUDE_DIRS "." "../../app" "../../core" "../../ut" "../../../common"
    REQUIRES driver esp_driver_gptimer esp_driver_gpio esp_driver_uart esp_timer)
