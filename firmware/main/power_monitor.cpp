#include "power_monitor.hpp"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <ctime>

namespace {
constexpr uint32_t MAGIC = 0x504D3031;
constexpr unsigned CAPACITY = 32;
struct SleepReceipt {
    uint32_t magic, cycles, requests;
    int64_t requested_epoch;
    bool pending;
};
RTC_DATA_ATTR SleepReceipt receipt = {};
struct Event { uint32_t seq; uint64_t at; const char *kind; const char *reason; };
struct State {
    uint32_t boot = 0, sequence = 0, rises = 0, frames = 0;
    int level = -1;
    int people = -1;
    uint64_t high_since = 0, last_high_ms = 0, first_frame = 0;
    bool initialized = false, deep_wake = false, paired = false;
    Event events[CAPACITY] = {};
};
State state;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
uint64_t ms() { return esp_timer_get_time() / 1000; }
void emit(const char *kind, const char *reason, uint64_t now) {
    const auto seq = ++state.sequence;
    state.events[(seq - 1) % CAPACITY] = {seq, now, kind, reason};
}
const char *wake_source() {
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0: return "pir";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    default: return esp_reset_reason() == ESP_RST_POWERON ? "power_on" : "reset";
    }
}
}

void power_monitor_start() {
    const bool deep = esp_reset_reason() == ESP_RST_DEEPSLEEP;
    const uint32_t boot_id = esp_random();
    portENTER_CRITICAL(&lock);
    state = State{};
    state.boot = boot_id;
    state.deep_wake = deep;
    state.paired = deep && receipt.magic == MAGIC && receipt.pending;
    if (!deep || receipt.magic != MAGIC) receipt = {MAGIC, 0, 0, 0, false};
    if (state.paired) ++receipt.cycles;
    receipt.pending = false;
    state.initialized = true;
    emit("boot", wake_source(), ms());
    portEXIT_CRITICAL(&lock);
}
void power_monitor_pir(int level, uint64_t now) {
    portENTER_CRITICAL(&lock);
    if (state.initialized && state.level != level) {
        if (level == 1) {
            if (state.level == 0) ++state.rises;
            state.high_since = now;
        } else if (state.level == 1) state.last_high_ms = now - state.high_since;
        state.level = level;
        emit(level == 1 ? "pir_high" : level == 0 ? "pir_low" : "pir_error", "", now);
    }
    portEXIT_CRITICAL(&lock);
}
void power_monitor_frame(bool people, uint64_t now) {
    portENTER_CRITICAL(&lock);
    if (state.initialized) {
        if (!state.frames) { state.first_frame = now; emit("first_frame", "", now); }
        ++state.frames;
        if (state.people != int(people)) {
            state.people = people;
            emit(people ? "ai_people" : "ai_empty", "", now);
        }
    }
    portEXIT_CRITICAL(&lock);
}
void power_monitor_event(const char *kind, const char *reason) {
    portENTER_CRITICAL(&lock);
    if (state.initialized) emit(kind, reason, ms());
    portEXIT_CRITICAL(&lock);
}
void power_monitor_sleep() {
    // Written only after the wake sources have been armed. A following boot
    // must also report ESP_RST_DEEPSLEEP before this is a confirmed cycle.
    const auto epoch = time(nullptr);
    portENTER_CRITICAL(&lock);
    receipt.magic = MAGIC;
    ++receipt.requests;
    receipt.requested_epoch = epoch;
    receipt.pending = true;
    portEXIT_CRITICAL(&lock);
}
cJSON *power_monitor_status() {
    State s;
    SleepReceipt r;
    portENTER_CRITICAL(&lock);
    s = state; r = receipt;
    portEXIT_CRITICAL(&lock);
    auto json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "available", s.initialized);
    cJSON_AddNumberToObject(json, "boot_id", s.boot);
    cJSON_AddStringToObject(json, "wake_source", wake_source());
    cJSON_AddNumberToObject(json, "reset_reason", esp_reset_reason());
    cJSON_AddBoolToObject(json, "deep_sleep_wake", s.deep_wake);
    cJSON_AddBoolToObject(json, "paired_sleep", s.paired);
    cJSON_AddNumberToObject(json, "confirmed_cycles", r.cycles);
    cJSON_AddNumberToObject(json, "sleep_requests", r.requests);
    cJSON_AddNumberToObject(json, "last_sleep_epoch", r.requested_epoch);
    cJSON_AddNumberToObject(json, "pir_rises", s.rises);
    cJSON_AddNumberToObject(json, "pir_high_ms", s.level == 1 ? ms() - s.high_since : 0);
    cJSON_AddNumberToObject(json, "last_pulse_ms", s.last_high_ms);
    cJSON_AddNumberToObject(json, "frames", s.frames);
    cJSON_AddNumberToObject(json, "first_frame_ms", s.first_frame);
    cJSON_AddNumberToObject(json, "people_detected", s.people);
    auto events = cJSON_AddArrayToObject(json, "events");
    const unsigned count = s.sequence < CAPACITY ? s.sequence : CAPACITY;
    for (unsigned n = 0; n < count; ++n) {
        const auto &event = s.events[(s.sequence - count + n) % CAPACITY];
        auto item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "seq", event.seq);
        cJSON_AddNumberToObject(item, "ms", event.at);
        cJSON_AddStringToObject(item, "kind", event.kind);
        cJSON_AddStringToObject(item, "reason", event.reason);
        cJSON_AddItemToArray(events, item);
    }
    return json;
}
