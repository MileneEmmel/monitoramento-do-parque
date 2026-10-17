#pragma once
#include "cJSON.h"

bool park_power_start();
bool park_power_should_start_camera();
void park_power_camera_started();
void park_power_detection(bool people);
bool park_power_preparing();
cJSON *park_power_status();
bool park_power_configure(const cJSON *json);
cJSON *park_power_cloud_status();
void park_power_camera_failed();
bool park_power_recovery_requested();
void park_power_recovery_finished(bool success);
