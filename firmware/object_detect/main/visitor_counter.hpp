#pragma once
#include "who_detect.hpp"
#include "counting_core.hpp"
#include <string>
struct CountSnapshot {
    uint64_t entries = 0, exits = 0;
    park::Config config;
    unsigned ids[park::capacity] = {};
    uint8_t states[park::capacity] = {};
    unsigned created=0, expired=0, resumed=0, discarded=0, frames=0, empty_frames=0;
    park::Event events[8];
    unsigned event_sequence=0;
    bool ready = false, storage_ok = false, pending = false;
};
bool visitor_counter_start();
CountSnapshot visitor_counter_update(const who::detect::WhoDetect::result_t &result);
CountSnapshot visitor_counter_snapshot();
bool visitor_counter_configure(const park::Config &config);
bool visitor_counter_reset_totals();
std::string visitor_counter_json(const CountSnapshot &snapshot);
