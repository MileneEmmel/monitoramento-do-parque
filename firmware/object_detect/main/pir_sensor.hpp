#pragma once
#include "cJSON.h"

// Read-only diagnostic. Does not gate detection, counting or sleep.
cJSON *pir_sensor_status();
