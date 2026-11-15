/**
 * @file    battery.h
 * @brief   Battery sample type and BatteryTask for OpenVIBE BATT_INFO frames.
 *
 * The Nicla Voice reads a BQ25120A voltage-monitor percent, not an iES
 * MAX17043 state of charge. Modern mode (on request, or when low) is not
 * implemented here.
 */

#pragma once

#include "task.h"
#include "config.h"
#include <stdint.h>


/**
 * @brief One battery reading handed to PacketiserTask.
 *
 * percent is nicla::getBatteryVoltagePercentage(): percent of the regulated
 * full voltage, about 60 to 100, not fuel-gauge state of charge.
 * sizeof == 1. RAM_USAGE_FIFOS counts FIFO_DEPTH_BATTERY_QUEUE * 1.
 */
struct BatterySample {
    int8_t percent;
};
static_assert(sizeof(BatterySample) == 1, "BatterySample size mismatch — update RAM_USAGE_FIFOS");


class BatteryTask : public ProducerTask<BatterySample> {
protected:
    void run() override;

public:
    BatteryTask();
};


extern BatteryTask batteryTask;
