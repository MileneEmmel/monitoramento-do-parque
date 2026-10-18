// Declares counting data and configuration, snapshot, and persistence operations

#pragma once
#include "who_detect.hpp"
#include "counting_core.hpp"
#include <string>

// Value copy shared with preview/history/status code, it does not expose the live counter or its mutex
struct CountSnapshot {
    uint64_t entries = 0, exits = 0;
    park::Config config;

    // Arrays follow detector order. ID zero means no assigned trajectory, states encodes the crossing overlay
    unsigned ids[park::capacity]   = {};
    uint8_t states[park::capacity] = {};
    unsigned created               = 0, expired = 0, resumed = 0, discarded = 0, frames = 0, empty_frames = 0;
    park::Event events[8];
    unsigned event_sequence = 0;

    // ready enables counting, storage_ok describes the last NVS save, pending means a newer revision is not saved yet
    bool ready              = false, storage_ok = false, pending = false;
};

// Validates core behavior and saved NVS state, then starts the checkpoint worker
bool visitor_counter_start();

// Pauses/resumes counting updates for final sleep persistence, reads remain available
void visitor_counter_freeze(bool frozen);

// Synchronously persists pending totals/calibration and checks that the saved revision is still current
bool visitor_counter_flush();

// Restore a persisted journal floor without discarding crossings since this boot
// Merges a larger persisted floor while preserving movement detected since this boot
void visitor_counter_restore(uint64_t entries, uint64_t exits);

// Processes one detector result and returns a snapshot with IDs mapped to the original detection order
CountSnapshot visitor_counter_update(const who::detect::WhoDetect::result_t &result);

// Returns a mutex-protected copy without changing tracking or totals
CountSnapshot visitor_counter_snapshot();

// Validates and applies new line settings, discarding trajectories measured with the old calibration
bool visitor_counter_configure(const park::Config &config);

// Serializes a snapshot, 64-bit totals remain strings to preserve browser precision
std::string visitor_counter_json(const CountSnapshot &snapshot);
