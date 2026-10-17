#pragma once
#include "cJSON.h"
#include <ctime>
#include <string>
#include <cstdint>
bool park_history_start();
void park_history_capture();
bool park_history_ready();
bool park_history_initialized();
bool park_history_healthy();
uint32_t park_history_revision();
// Returns with the journal mutex held on success; release on an aborted sleep.
bool park_history_sleep_lock();
void park_history_sleep_unlock();
cJSON *park_history_status();
std::string park_today();
std::string park_timestamp(time_t value);
cJSON *park_history_report(const std::string &start, const std::string &end);
bool park_history_range(const std::string &start, const std::string &end);
std::string park_history_csv(const std::string &start, const std::string &end);
cJSON *park_history_cleanup(const std::string &before, bool erase, const std::string &plan);
struct ParkCloudBatch {
    std::string file;
    long from = 0, to = 0;
    cJSON *records = nullptr;
    bool ok = false;
    const char *error = "sd_not_ready";
};
ParkCloudBatch park_history_cloud_batch(unsigned timeout_ms = 200);
bool park_history_cloud_ack(const ParkCloudBatch &batch);
void park_history_cloud_protect();
