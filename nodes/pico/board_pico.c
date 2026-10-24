#include "board.h"

#include <stdatomic.h>

#include "board_config.h"
#include "debounce.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "tusb.h"

static const uint taskLedPins[] = BOARD_TASK_LED_PINS;

enum {
    TaskLedCount = sizeof(taskLedPins) / sizeof(taskLedPins[0])
};

_Static_assert(TaskLedCount > 0 && (int)TaskLedCount <= (int)IndicatorsMaxTaskBits,
               "BOARD_TASK_LED_PINS must list between 1 and 8 pins");

static repeating_timer_t tickTimer;
static volatile uint32_t pendingTicks; /* Ticks raised by the timer, not yet run */
static Debounce buttonDebounce;
static atomic_bool buttonPressed;

static void WriteLed(uint pin, bool on)
{
    gpio_put(pin, on ? BOARD_LED_ON_LEVEL : !BOARD_LED_ON_LEVEL);
}

static void InitOutput(uint pin)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
}

bool BoardLedsInit(void)
{
    for (unsigned int i = 0; i < TaskLedCount; i++) {
        InitOutput(taskLedPins[i]);
    }
    InitOutput(BOARD_MISS_LED_PIN);
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

/* Runs in the hardware alarm interrupt: only counts the tick */
static bool OnTick(repeating_timer_t *timer)
{
    (void)timer;
    pendingTicks++;
    return true; /* Keep repeating */
}

bool BoardTickStart(uint32_t tickUs)
{
    /* A negative delay keeps a fixed period from start to start */
    return add_repeating_timer_us(-(int64_t)tickUs, OnTick, NULL, &tickTimer);
}

void BoardTickWait(void)
{
    for (;;) {
        /* Interrupts are off while checking, so a tick that arrives between
           the check and __wfi still wakes the core: no tick is missed */
        uint32_t saved = save_and_disable_interrupts();

        if (pendingTicks > 0) {
            pendingTicks--;
            restore_interrupts(saved);
            return;
        }
        __wfi(); /* Sleeps until any interrupt is pending */
        restore_interrupts(saved);
    }
}

uint64_t BoardMicros(void)
{
    return time_us_64();
}

/* Runs in the GPIO interrupt: only records the press */
static void OnButton(uint gpio, uint32_t events)
{
    (void)events;
    if (gpio == BOARD_BUTTON_PIN && DebounceAccept(&buttonDebounce, time_us_32())) {
        atomic_store(&buttonPressed, true);
    }
}

bool BoardButtonInit(void)
{
    DebounceInit(&buttonDebounce, BOARD_DEBOUNCE_US);
    atomic_store(&buttonPressed, false);
    gpio_init(BOARD_BUTTON_PIN);
    gpio_set_dir(BOARD_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BOARD_BUTTON_PIN);
    /* Pressed = pin pulled to GND */
    gpio_set_irq_enabled_with_callback(BOARD_BUTTON_PIN, GPIO_IRQ_EDGE_FALL, true, OnButton);
    return true;
}

bool BoardButtonTakePress(void)
{
    return atomic_exchange(&buttonPressed, false);
}

bool BoardSerialInit(void)
{
    if (!stdio_usb_init()) {
        return false;
    }
    /* Frames are binary: a 0x0A byte must not become 0x0D 0x0A */
    stdio_set_translate_crlf(&stdio_usb, false);
    return true;
}

size_t BoardSerialWrite(const uint8_t *bytes, size_t count)
{
    size_t room;
    size_t i;

    if (bytes == NULL || count == 0 || !stdio_usb_connected()) {
        return 0; /* Host not connected: the frame waits, the node keeps running */
    }
    /* Writing no more than the free space of the USB buffer never waits */
    room = tud_cdc_write_available();
    if (count > room) {
        count = room;
    }
    for (i = 0; i < count; i++) {
        putchar_raw(bytes[i]);
    }
    return count;
}
