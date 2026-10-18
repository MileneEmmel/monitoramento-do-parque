// Declares cloud configuration, synchronization, and upload control

#pragma once
#include "cJSON.h"
#include <cstdint>

// Creates the synchronization worker after scheduling checks, connection/configuration may still be pending
bool park_cloud_start();

// Validates and saves ingestion credentials, then requests a fresh synchronization scan
bool park_cloud_configure(const cJSON *json);

// Returns owned diagnostic JSON without credentials, the caller must delete or transfer it
cJSON *park_cloud_status();

// Notifies the worker without bypassing its deadlines or configuration checks
void park_cloud_request_sync();

// Sets paused and waits for the active cycle to release its mutex, timeout does not clear the paused flag
bool park_cloud_pause(unsigned timeout_ms);

// Clears paused and notifies the worker
void park_cloud_resume();

// Begins bounded closing with an absolute deadline in monotonic milliseconds
void park_cloud_prepare_sleep(uint64_t deadline_ms);

// Returns the worker to normal scheduling after cancelled sleep
void park_cloud_cancel_sleep();

// True means closing has stopped, successfully or otherwise, inspect sleep_result for the outcome
bool park_cloud_sleep_ready();

// Returns a borrowed stable reason string such as drained, send_failed, or budget_exhausted
const char *park_cloud_sleep_result();
