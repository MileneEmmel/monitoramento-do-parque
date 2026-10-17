#pragma once
#include "cJSON.h"

bool pir_sensor_init();
int pir_sensor_level();
bool pir_sensor_arm_wakeup();
cJSON *pir_sensor_status();
