#include "indicators.h"

bool IndicatorsInit(Indicators *indicators, uint32_t taskBits, uint32_t missHoldTicks)
{
    if (indicators == NULL || taskBits == 0 || taskBits > IndicatorsMaxTaskBits) {
        return false;
    }
    indicators->taskBits = taskBits;
    indicators->missHoldTicks = missHoldTicks;
    indicators->seenMisses = 0;
    indicators->missTicksLeft = 0;
    indicators->missLatched = false;
    return true;
}

IndicatorState IndicatorsUpdate(Indicators *indicators, const Scheduler *scheduler)
{
    IndicatorState state = { .taskPattern = 0, .missLed = false };

    if (indicators == NULL || scheduler == NULL) {
        return state;
    }

    if (scheduler->lastRunIndex != SchedulerNoTask) {
        uint32_t allOn = (1u << indicators->taskBits) - 1u;
        uint32_t number = (uint32_t)scheduler->lastRunIndex + 1u;

        state.taskPattern = (number > allOn) ? allOn : number;
    }

    if (scheduler->missCount != indicators->seenMisses) {
        indicators->seenMisses = scheduler->missCount;
        if (indicators->missHoldTicks == 0) {
            indicators->missLatched = true;
        } else {
            indicators->missTicksLeft = indicators->missHoldTicks;
        }
    }
    if (indicators->missLatched) {
        state.missLed = true;
    } else if (indicators->missTicksLeft > 0) {
        state.missLed = true;
        indicators->missTicksLeft--;
    }
    return state;
}