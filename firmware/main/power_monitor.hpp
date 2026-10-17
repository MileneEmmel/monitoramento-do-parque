#pragma once
#include "cJSON.h"
#include <cstdint>

// Bounded runtime telemetry. No files, images or credentials are stored here.
void power_monitor_start();
void power_monitor_pir(int level, uint64_t now);
void power_monitor_frame(bool people, uint64_t now);
void power_monitor_event(const char *kind, const char *reason = "");
void power_monitor_sleep();
cJSON *power_monitor_status();
