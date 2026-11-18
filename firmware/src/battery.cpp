/**
 * @file    battery.cpp
 * @brief   OpenVIBE battery level task for Arduino Nicla Voice.
 */

#include "battery.h"
#include "runtime_state.h"
#include "Nicla_System.h"

BatteryTask batteryTask;

BatteryTask::BatteryTask()
    : ProducerTask<BatterySample>(TASK_PRIORITY_BATTERY, STACK_SIZE_BATTERY)
{
}

void BatteryTask::run() {
    while (!_stopRequested) {
        // OpenVIBE battery send. Comment out this block if a type-5 frame
        // desyncs the host. The sleep below stays so the task does not spin.
        // Modern mode (on request / low battery) is not implemented.
        if (g_runtimeState.isOpenVibeMode()) {
            int8_t percent = nicla::getBatteryVoltagePercentage();
            if (percent >= 0) {
                // VBMON only covers 60–100% of VBATREG. Stretch that onto 0–100.
                // map() does not clamp, so a reading outside that window is held
                // at the ends.
                long level = map(percent, 60, 100, 0, 100);
                level = constrain(level, 0, 100);
                BatterySample sample;
                sample.percent = (int8_t)level;
                distribute(sample);
            }
        }

        rtos::ThisThread::sleep_for(std::chrono::milliseconds(BATTERY_PERIOD_MS));
    }
}
