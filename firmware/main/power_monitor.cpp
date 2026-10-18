// Records PIR and detection activity and confirms deep sleep cycles

#include "power_monitor.hpp"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <ctime>

namespace {
    constexpr uint32_t MAGIC    = 0x504D3031;
    constexpr unsigned CAPACITY = 32;

    // Small receipt retained across deep sleep, ordinary resets start a fresh confirmation session
    struct SleepReceipt {
        uint32_t magic, cycles, requests;
        int64_t requested_epoch;
        bool pending;
    };

    RTC_DATA_ATTR SleepReceipt receipt = {};

    struct Event {
        uint32_t seq;
        uint64_t at;
        const char *kind;
        const char *reason;
    };

    // Per-boot diagnostics stay in RAM and are not the visitor journal. Events overwrite the oldest of 32 slots
    struct State {
        uint32_t boot          = 0, sequence = 0, rises = 0, frames = 0;
        int level              = -1, people = -1;
        uint64_t high_since    = 0, last_high_ms = 0, first_frame = 0;
        bool initialized       = false, deep_wake = false, paired = false;
        Event events[CAPACITY] = {};
    };

    State state;
    portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;

    // Returns monotonic milliseconds used by events and pulse durations in this boot
    uint64_t ms() {
        return esp_timer_get_time() / 1000;
    }

    // Stores a bounded event while the caller holds state_lock, kind/reason must point to long-lived strings
    void emit(const char *kind, const char *reason, uint64_t now_ms) {
        const auto event_sequence = ++state.sequence;
        state.events[(event_sequence - 1) % CAPACITY] = {event_sequence, now_ms, kind, reason};
    }

    // Translates hardware wake/reset information into the names consumed by the local monitor
    const char *wake_source() {
        switch(esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT0:
            return "pir";
        case ESP_SLEEP_WAKEUP_TIMER:
            return "timer";
        default:
            return esp_reset_reason() == ESP_RST_POWERON ? "power_on":"reset";
        }
    }
}

// Resets per-boot telemetry and pairs any retained sleep request with the hardware deep-sleep reset reason
void power_monitor_start() {
    // 1. Read the hardware reset reason and generate this boot ID
    const bool woke_from_deep_sleep = esp_reset_reason() == ESP_RST_DEEPSLEEP;
    const uint32_t boot_id          = esp_random();

    // 2. Reset per-boot diagnostics and validate the retained sleep receipt
    portENTER_CRITICAL(&state_lock);
    state           = State{};
    state.boot      = boot_id;
    state.deep_wake = woke_from_deep_sleep;
    state.paired    = woke_from_deep_sleep && receipt.magic == MAGIC && receipt.pending;

    if(!woke_from_deep_sleep || receipt.magic != MAGIC)
        receipt = {MAGIC, 0, 0, 0, false};

    // 3. Confirm a sleep cycle only when the retained request matches a deep-sleep wake
    // A retained pending marker counts as a confirmed cycle only when hardware also reports deep sleep
    if(state.paired)
        ++receipt.cycles;

    // 4. Clear the pending marker and record the boot event before releasing the lock
    receipt.pending   = false;
    state.initialized = true;
    emit("boot", wake_source(), ms());
    portEXIT_CRITICAL(&state_lock);
}

// Records level transitions, rising edges, and completed high-pulse duration, repeated levels add no event
void power_monitor_pir(int pir_level, uint64_t now_ms) {
    // 1. Inspect initialized state and detect a change in PIR level
    portENTER_CRITICAL(&state_lock);

    if(state.initialized && state.level != pir_level) {
        // 2. Update rising-edge count and high-pulse timing
        if(pir_level == 1) {
            if(state.level == 0)
                ++state.rises;
            state.high_since = now_ms;
        } else if(state.level == 1)
            state.last_high_ms = now_ms - state.high_since;

        // 3. Save the new level and append the corresponding event
        state.level = pir_level;
        emit(pir_level == 1 ? "pir_high":pir_level == 0 ? "pir_low":"pir_error", "", now_ms);
    }
    portEXIT_CRITICAL(&state_lock);
}


// Counts processed frames, records first-frame timing, and emits only changes between people/empty results
void power_monitor_frame(bool people_detected, uint64_t now_ms) {
    portENTER_CRITICAL(&state_lock);
    if(state.initialized) {
        // 1. Record the first completed detection after monitoring starts
        // Time to the first completed detection helps check whether startup is fast enough for the first visitor
        if(!state.frames) {
            state.first_frame = now_ms;
            emit("first_frame", "", now_ms);
        }

        // 2. Count every processed frame, including empty detections
        ++state.frames;

        // 3. Append an event only when people detection changes
        if(state.people != int(people_detected)) {
            state.people = people_detected;
            emit(people_detected ? "ai_people":"ai_empty", "", now_ms);
        }
    }
    portEXIT_CRITICAL(&state_lock);
}

// Adds a named application event to the in-memory ring after monitoring has initialized
void power_monitor_event(const char *kind, const char *reason) {
    portENTER_CRITICAL(&state_lock);
    if(state.initialized)
        emit(kind, reason, ms());
    portEXIT_CRITICAL(&state_lock);
}

// Stores an intention receipt in RTC memory just before deep sleep, confirmation happens on the next boot
void power_monitor_sleep() {
    // Written only after the wake sources have been armed. A following boot
    // must also report ESP_RST_DEEPSLEEP before this is a confirmed cycle
    const auto epoch = time(nullptr);
    portENTER_CRITICAL(&state_lock);
    receipt.magic = MAGIC;
    ++receipt.requests;
    receipt.requested_epoch = epoch;
    receipt.pending         = true;
    portEXIT_CRITICAL(&state_lock);
}

// Copies state under the lock, then allocates JSON outside it. Events are returned oldest to newest
cJSON *power_monitor_status() {
    // 1. Copy diagnostic state and the sleep receipt under the lock
    State state_snapshot;
    SleepReceipt receipt_snapshot;
    portENTER_CRITICAL(&state_lock);
    state_snapshot   = state;
    receipt_snapshot = receipt;
    portEXIT_CRITICAL(&state_lock);

    // 2. Build diagnostic JSON after releasing the lock
    auto json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "available", state_snapshot.initialized);
    cJSON_AddNumberToObject(json, "boot_id", state_snapshot.boot);
    cJSON_AddStringToObject(json, "wake_source", wake_source());
    cJSON_AddNumberToObject(json, "reset_reason", esp_reset_reason());
    cJSON_AddBoolToObject(json, "deep_sleep_wake", state_snapshot.deep_wake);
    cJSON_AddBoolToObject(json, "paired_sleep", state_snapshot.paired);
    cJSON_AddNumberToObject(json, "confirmed_cycles", receipt_snapshot.cycles);
    cJSON_AddNumberToObject(json, "sleep_requests", receipt_snapshot.requests);
    cJSON_AddNumberToObject(json, "last_sleep_epoch", receipt_snapshot.requested_epoch);
    cJSON_AddNumberToObject(json, "pir_rises", state_snapshot.rises);
    cJSON_AddNumberToObject(json, "pir_high_ms", state_snapshot.level == 1 ? ms() - state_snapshot.high_since:0);
    cJSON_AddNumberToObject(json, "last_pulse_ms", state_snapshot.last_high_ms);
    cJSON_AddNumberToObject(json, "frames", state_snapshot.frames);
    cJSON_AddNumberToObject(json, "first_frame_ms", state_snapshot.first_frame);
    cJSON_AddNumberToObject(json, "people_detected", state_snapshot.people);

    // 3. Append retained events from oldest to newest
    auto events                = cJSON_AddArrayToObject(json, "events");
    const unsigned event_count = state_snapshot.sequence < CAPACITY ? state_snapshot.sequence:CAPACITY;

    for(unsigned event_offset = 0; event_offset < event_count; ++event_offset) {
        const auto &event = state_snapshot.events[(state_snapshot.sequence - event_count + event_offset) % CAPACITY];
        auto item         = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "seq", event.seq);
        cJSON_AddNumberToObject(item, "ms", event.at);
        cJSON_AddStringToObject(item, "kind", event.kind);
        cJSON_AddStringToObject(item, "reason", event.reason);
        cJSON_AddItemToArray(events, item);
    }
    return json;
}
