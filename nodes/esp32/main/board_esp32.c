#include "board.h"

#include "board_config.h"
#include "driver/gpio.h"

static const gpio_num_t taskLedPins[] = BOARD_TASK_LED_PINS;

enum {
    TaskLedCount = sizeof(taskLedPins) / sizeof(taskLedPins[0])
};

_Static_assert(TaskLedCount > 0 && (int)TaskLedCount <= (int)IndicatorsMaxTaskBits,
               "BOARD_TASK_LED_PINS must list between 1 and 8 pins");

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