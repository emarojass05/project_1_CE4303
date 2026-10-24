/* Demo del nodo ESP32 para el Milestone 2: corre tareas de prueba fijas con el
   calendarizador de nodes/core y muestra en los LEDs la tarea en ejecucion
   (en binario) y los plazos perdidos.

   Las tareas fijas y la impresion de eventos por consola son solo para esta
   demo; en M3 las tareas llegan del host y los eventos salen por el protocolo. */

#include <inttypes.h>
#include <stdio.h>

#include "board.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "indicators.h"
#include "scheduler.h"

/* Algoritmo de la demo: SchedulerAlgorithmRms o SchedulerAlgorithmEdf */
#define DEMO_ALGORITHM SchedulerAlgorithmRms

/* Tick lento para que los LEDs se puedan seguir a simple vista. El valor del
   proyecto es 10 ms y en M3 lo envia el host en la solicitud de calibracion. */
#define DEMO_TICK_US 250000u

/* Ticks que el LED de plazo perdido queda encendido (0 = hasta reiniciar) */
#define DEMO_MISS_HOLD_TICKS 4u

typedef struct {
    Task task;
    uint32_t cost; /* Ticks por trabajo; en M3 sale de C(i,n) = ceil(W / sn) */
} DemoTask;

/* U = 2/5 + 3/7 + 1/10 = 0.93. EDF cumple todos los plazos; RMS pierde uno
   cada 70 ticks aproximadamente. Los LEDs muestran 1, 2 o 3 (orden de la tabla). */
static const DemoTask demoTasks[] = {
    { { .id = 1, .type = TaskTypePeriodic, .work = 0, .period = 5,  .deadline = 5,  .criticality = 1 }, 2 },
    { { .id = 2, .type = TaskTypePeriodic, .work = 0, .period = 7,  .deadline = 7,  .criticality = 1 }, 3 },
    { { .id = 3, .type = TaskTypePeriodic, .work = 0, .period = 10, .deadline = 10, .criticality = 1 }, 1 },
};

static const char *logTag = "node";

static EventBuffer events;
static Scheduler scheduler;
static Indicators indicators;
static TaskHandle_t tickTaskHandle;

/* ISR del timer de hardware: solo despierta al ciclo principal */
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

/* Timer de hardware con recarga automatica cada DEMO_TICK_US */
static bool StartTickTimer(void)
{
    gptimer_handle_t timer = NULL;
    gptimer_config_t timerConfig = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, /* 1 cuenta = 1 us */
    };
    gptimer_alarm_config_t alarmConfig = {
        .alarm_count = DEMO_TICK_US,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_event_callbacks_t callbacks = {
        .on_alarm = OnTimerAlarm,
    };

    return gptimer_new_timer(&timerConfig, &timer) == ESP_OK &&
           gptimer_register_event_callbacks(timer, &callbacks, NULL) == ESP_OK &&
           gptimer_set_alarm_action(timer, &alarmConfig) == ESP_OK &&
           gptimer_enable(timer) == ESP_OK &&
           gptimer_start(timer) == ESP_OK;
}

static const char *EventName(EventType type)
{
    switch (type) {
    case EventTypeRelease:         return "release";
    case EventTypeStart:           return "start";
    case EventTypePreempt:         return "preempt";
    case EventTypeComplete:        return "complete";
    case EventTypeDeadlineMiss:    return "deadline-miss";
    case EventTypeAbort:           return "abort";
    case EventTypeSporadicRelease: return "sporadic-release";
    case EventTypeSporadicIgnored: return "sporadic-ignored";
    default:                       return "?";
    }
}

/* Solo para la demo: imprime los eventos pendientes por la consola USB */
static void PrintEvents(void)
{
    Event event;

    while (EventBufferPop(&events, &event)) {
        printf("tick %" PRIu32 " task %" PRIu32 " %s %" PRIu32 "\n",
               event.tick, event.taskId, EventName(event.type), event.value);
    }
}

void app_main(void)
{
    size_t i;

    tickTaskHandle = xTaskGetCurrentTaskHandle();
    EventBufferInit(&events);

    if (SchedulerInit(&scheduler, DEMO_ALGORITHM, &events) != SchedulerStatusOk ||
        !IndicatorsInit(&indicators, BoardTaskLedCount(), DEMO_MISS_HOLD_TICKS) ||
        !BoardLedsInit()) {
        ESP_LOGE(logTag, "init failed");
        return;
    }
    for (i = 0; i < sizeof(demoTasks) / sizeof(demoTasks[0]); i++) {
        if (SchedulerAddTask(&scheduler, &demoTasks[i].task, demoTasks[i].cost) !=
            SchedulerStatusOk) {
            ESP_LOGE(logTag, "task %" PRIu32 " rejected", demoTasks[i].task.id);
        }
    }
    if (!StartTickTimer()) {
        ESP_LOGE(logTag, "tick timer failed");
        return;
    }
    ESP_LOGI(logTag, "running %s, tick %u us",
             DEMO_ALGORITHM == SchedulerAlgorithmRms ? "RMS" : "EDF", DEMO_TICK_US);

    for (;;) {
        /* Bloquea sin consumir CPU hasta la siguiente interrupcion del timer;
           cada notificacion pendiente es un tick, asi que no se pierde ninguno */
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        SchedulerTick(&scheduler);
        BoardLedsShow(IndicatorsUpdate(&indicators, &scheduler));
        PrintEvents();
    }
}
