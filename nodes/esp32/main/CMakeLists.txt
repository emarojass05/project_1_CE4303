# El nucleo del calendarizador vive en nodes/core y se comparte con la Pico;
# aqui solo se agrega la capa especifica del ESP32. ut.c/ut.h (del profesor,
# sin modificar) estan en nodes/ut y tambien los usara la Pico.
idf_component_register(
    SRCS "main.c"
         "board_esp32.c"
         "../../core/scheduler.c"
         "../../core/event_buffer.c"
         "../../core/indicators.c"
         "../../ut/ut.c"
    INCLUDE_DIRS "." "../../core" "../../ut" "../../../common"
    REQUIRES driver esp_driver_gptimer esp_driver_gpio)