// Declares event collection and power monitor status queries

#pragma once
#include "cJSON.h"
#include <cstdint>

// Bounded runtime telemetry. No files, images or credentials are stored here
// Resets boot-local telemetry and checks whether a retained request proves a completed deep-sleep cycle
void power_monitor_start();

// Samples PIR transitions and pulse durations using monotonic milliseconds
void power_monitor_pir(int level, uint64_t now);

// Reports frame timing and people/empty transitions, not individual visitor passages
void power_monitor_frame(bool people, uint64_t now);

// Stores an event, kind and reason must remain valid for the lifetime of the retained RAM event
void power_monitor_event(const char *kind, const char *reason = "");

// Writes a pending RTC receipt just before sleep, the next hardware boot confirms or rejects it
void power_monitor_sleep();

// Returns owned JSON containing counters and the recent event ring
cJSON *power_monitor_status();
