#pragma once
#include "cJSON.h"
#include <cstdint>
bool park_cloud_start();
bool park_cloud_configure(const cJSON *json);
cJSON *park_cloud_status();
void park_cloud_request_sync();
bool park_cloud_pause(unsigned timeout_ms);
void park_cloud_resume();
void park_cloud_prepare_sleep(uint64_t deadline_ms);
void park_cloud_cancel_sleep();
bool park_cloud_sleep_ready();
const char *park_cloud_sleep_result();
