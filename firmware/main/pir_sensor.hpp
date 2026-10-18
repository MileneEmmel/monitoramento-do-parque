// Declares PIR sensor reading, configuration, and status operations

#pragma once
#include "cJSON.h"

// Configures normal polled GPIO input, also restores the pin after an aborted RTC wake setup
bool pir_sensor_init();

// Returns raw 0/1, or -1 if normal GPIO input is not initialized
int pir_sensor_level();

// Arms RTC wake on a high signal only if the pin is still low through the transition
bool pir_sensor_arm_wakeup();

// Returns owned raw-sensor diagnostic JSON, this does not count visitors
cJSON *pir_sensor_status();
