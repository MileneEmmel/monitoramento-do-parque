// Declares microSD history operations, reports, and cloud upload batches

#pragma once
#include "cJSON.h"
#include <ctime>
#include <string>
#include <cstdint>

// Establishes a baseline and starts a background mount/capture task, true does not mean mounting has finished
bool park_history_start();

// Captures changed totals or a due idle sample while holding the journal mutex internally
void park_history_capture();

// Checks mounted/healthy/baseline state with a short lock timeout
bool park_history_ready();

// Reports completion of the mount attempt, including failed attempts
bool park_history_initialized();

// Returns the last published write health without accessing the card
bool park_history_healthy();

// Returns the movement/reset revision used to notify cloud scheduling of new persisted work
uint32_t park_history_revision();

// Returns with the journal mutex held on success, release on an aborted sleep
// Forces capture and retains the journal lock for the final sleep checkpoint, health is reported separately
bool park_history_sleep_lock();

// Releases a successfully retained sleep lock when closing is cancelled or restarted
void park_history_sleep_unlock();

// Returns owned JSON with card/clock status, the caller must delete or transfer it
cJSON *park_history_status();

// Returns a fixed UTC-3 date, or an empty string when wall-clock time is invalid
std::string park_today();

// Returns an ISO UTC timestamp, or an empty string for an invalid epoch
std::string park_timestamp(time_t value);

// Returns owned JSON aggregates for an inclusive date range, may reuse a short-lived serialized cache
cJSON *park_history_report(const std::string &start, const std::string &end);

// Validates ordered dates, maximum span, and the absence of future days
bool park_history_range(const std::string &start, const std::string &end);

// Exports daily report aggregates for the selected interval
std::string park_history_csv(const std::string &start, const std::string &end);

// Returns a preview plan when erase is false, deletion requires the matching plan and all protections to pass
cJSON *park_history_cleanup(const std::string &before, bool erase, const std::string &plan);

// One file's pending byte range [from, to), its owned JSON records, and read outcome
// ok describes the batch read, not server acceptance, only cloud_ack advances the persisted cursor
struct ParkCloudBatch {
    std::string file;
    long from         = 0, to = 0;
    cJSON *records    = nullptr;
    bool ok           = false;
    const char *error = "sd_not_ready";
};

// Reads pending rows without acknowledging them. The caller owns records, even when the batch fails
ParkCloudBatch park_history_cloud_batch(unsigned timeout_ms = 200);

// Persists the batch end offset after server confirmation, provided the starting cursor still matches
bool park_history_cloud_ack(const ParkCloudBatch &batch);

// Prevents cleanup from deleting dated files that still contain unacknowledged cloud records
void park_history_cloud_protect();
