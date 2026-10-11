#pragma once
#include "cJSON.h"
bool park_cloud_start();
bool park_cloud_configure(const cJSON *json);
cJSON *park_cloud_status();
