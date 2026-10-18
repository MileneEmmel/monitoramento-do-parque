// Declares power control, camera states, and operating mode configuration

#pragma once
#include "cJSON.h"

// Restores the operating mode and starts PIR monitoring and sleep coordination
bool park_power_start();

// Tests continuous mode, PIR activity, or viewer demand before initial camera startup
bool park_power_should_start_camera();

// Marks capture startup and starts the first-frame health deadline
void park_power_camera_started();

// Reports every processed frame, people renew activity, while empty frames only update frame health
void park_power_detection(bool people);

// Indicates that sleep preparation is active so request handlers can reject conflicting writes
bool park_power_preparing();

// Returns owned JSON for the local field monitor, querying it does not renew activity
cJSON *park_power_status();

// Persists a JSON enabled boolean and applies the operating mode when no closing transaction holds control
bool park_power_configure(const cJSON *json);

// Returns owned compact cloud metadata, planned sleep is an announcement, not confirmation
cJSON *park_power_cloud_status();

// Reports camera startup failure without stopping storage/network services
void park_power_camera_failed();

// Lets the camera worker poll the active recovery request
bool park_power_recovery_requested();

// Completes an active recovery once and publishes success/failure diagnostics
void park_power_recovery_finished(bool success);
