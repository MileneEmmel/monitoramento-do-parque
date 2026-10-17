#pragma once
#include "cJSON.h"
bool park_cloud_start();
bool park_cloud_configure(const cJSON *json);
cJSON *park_cloud_status();
void park_cloud_request_sync();
bool park_cloud_pause(unsigned timeout_ms);
void park_cloud_resume();
