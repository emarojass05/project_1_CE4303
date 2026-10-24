#include "board.h"

#include <stdatomic.h>

#include "board_config.h"
#include "debounce.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The protocol uses the same UART as the USB console of the devkit */
#define BOARD_UART UART_NUM_0

static const gpio_num_t taskLedPins[] = BOARD_TASK_LED_PINS;

enum {
    TaskLedCount = sizeof(taskLedPins) / sizeof(taskLedPins[0])
};

_Static_assert(TaskLedCount > 0 && (int)TaskLedCount <= (int)IndicatorsMaxTaskBits,
               "BOARD_TASK_LED_PINS must list between 1 and 8 pins");

static TaskHandle_t tickTaskHandle;
static Debounce buttonDebounce;
static atomic_bool buttonPressed;

static void WriteLed(gpio_num_t pin, bool on)
{
    gpio_set_level(pin, on ? BOARD_LED_ON_LEVEL : !BOARD_LED_ON_LEVEL);
}

bool BoardLedsInit(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_MISS_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    for (unsigned int i = 0; i < TaskLedCount; i++) {
        config.pin_bit_mask |= 1ULL << taskLedPins[i];
    }
    if (gpio_config(&config) != ESP_OK) {
        return false;
    }
    BoardLedsShow((IndicatorState){ .taskPattern = 0, .missLed = false });
    return true;
}

unsigned int BoardTaskLedCount(void)
{
    return TaskLedCount;
}

void BoardLedsShow(IndicatorState state)
{
    for (unsigned int i = 0; i < TaskLedCount; i++) {
        WriteLed(taskLedPins[i], (state.taskPattern >> i) & 1u);
    }
    WriteLed(BOARD_MISS_LED_PIN, state.missLed);
}

/* ISR of the hardware timer: only wakes the main loop */
static bool IRAM_ATTR OnTimerAlarm(gptimer_handle_t timer,
                                   const gptimer_alarm_event_data_t *data, void *context)
{
    BaseType_t higherPriorityWoken = pdFALSE;

    (void)timer;
    (void)data;
    (void)context;
    vTaskNotifyGiveFromISR(tickTaskHandle, &higherPriorityWoken);
    return higherPriorityWoken == pdTRUE;
}

/* Hardware timer with automatic reload every tickUs */
bool BoardTickStart(uint32_t tickUs)
{
    gptimer_handle_t timer = NULL;
    gptimer_config_t timerConfig = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, /* 1 count = 1 us */
    };
    gptimer_alarm_config_t alarmConfig = {
        .alarm_count = tickUs,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_event_callbacks_t callbacks = {
        .on_alarm = OnTimerAlarm,
    };

    tickTaskHandle = xTaskGetCurrentTaskHandle();
    return gptimer_new_timer(&timerConfig, &timer) == ESP_OK &&
           gptimer_register_event_callbacks(timer, &callbacks, NULL) == ESP_OK &&
           gptimer_set_alarm_action(timer, &alarmConfig) == ESP_OK &&
           gptimer_enable(timer) == ESP_OK &&
           gptimer_start(timer) == ESP_OK;
}

void BoardTickWait(void)
{
    /* Blocks without using the CPU until the timer interrupt; each pending
       notification is one tick, so none is lost if a tick runs long */
    ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
}

uint64_t BoardMicros(void)
{
    return (uint64_t)esp_timer_get_time();
}

/* ISR of the button: only records the press */
static void OnButton(void *context)
{
    (void)context;
    if (DebounceAccept(&buttonDebounce, (uint32_t)esp_timer_get_time())) {
        atomic_store(&buttonPressed, true);
    }
}

bool BoardButtonInit(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE, /* Pressed = pin pulled to GND */
    };

    DebounceInit(&buttonDebounce, BOARD_DEBOUNCE_US);
    atomic_store(&buttonPressed, false);
    return gpio_config(&config) == ESP_OK &&
           gpio_install_isr_service(0) == ESP_OK &&
           gpio_isr_handler_add(BOARD_BUTTON_PIN, OnButton, NULL) == ESP_OK;
}

bool BoardButtonTakePress(void)
{
    return atomic_exchange(&buttonPressed, false);
}

bool BoardSerialInit(void)
{
#if !BOARD_TEXT_EVENTS
    /* Console text would corrupt the frames (docs/protocolo.md, section 1) */
    esp_log_level_set("*", ESP_LOG_NONE);
#endif
    /* The console already set 115200 8N1; the driver gives uart_tx_chars */
    return uart_driver_install(BOARD_UART, 256, 0, 0, NULL, 0) == ESP_OK;
}

size_t BoardSerialWrite(const uint8_t *bytes, size_t count)
{
    int written;

    if (bytes == NULL || count == 0) {
        return 0;
    }
    /* Fills only the free space of the hardware FIFO and returns at once */
    written = uart_tx_chars(BOARD_UART, (const char *)bytes, count);
    return written > 0 ? (size_t)written : 0;
}
