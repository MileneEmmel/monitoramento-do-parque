// Connects detection to the counter and preserves totals and calibration in NVS

#include "visitor_counter.hpp"
#include "app_settings.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <cstdio>

namespace {
    // NVS is the chip's nonvolatile key/value storage. It keeps a checkpoint, not the detailed SD event history
    park::Counter counter;

    // counter_mutex protects counter state, save_mutex orders flash writes. revision changes when persistent data changes
    SemaphoreHandle_t counter_mutex = nullptr;
    SemaphoreHandle_t save_mutex    = nullptr;
    bool ready  = false, storage_ok = false;
    bool frozen = false;
    uint32_t revision       = 0, saved_revision = 0;
    uint64_t loaded_entries = 0, loaded_exits   = 0;

    // Single versioned blob keeps totals and calibration in the same NVS transaction
    struct SavedCounterState {
        uint32_t version;
        float first, second;
        uint32_t horizontal, reverse;
        uint64_t entries, exits;
    };

    // Accepts finite line positions within the image, in order, with approximately 5% minimum separation
    bool valid(const park::Config &c) {
        return std::isfinite(c.first) && std::isfinite(c.second) && c.first >= .05f && c.second <= .95f && c.second - c.first >= .049999f;
    }

    // Writes one complete checkpoint to NVS and commits it. The caller serializes writes with save_mutex
    esp_err_t write_state(const SavedCounterState &state) {
        nvs_handle_t handle;
        esp_err_t error = nvs_open("park_counter", NVS_READWRITE, &handle);
        if(error == ESP_OK) {
            error = nvs_set_blob(handle, "state", &state, sizeof(state));
            if(error == ESP_OK)
                error = nvs_commit(handle);
            nvs_close(handle);
        }
        return error;
    }

    // Copies the counter and persistence flags. The caller must already hold counter_mutex
    CountSnapshot snapshot_locked() {
        CountSnapshot snapshot;
        snapshot.entries        = counter.entries;
        snapshot.exits          = counter.exits;
        snapshot.config         = counter.config;
        snapshot.ready          = ready;
        snapshot.storage_ok     = storage_ok;
        snapshot.pending        = revision != saved_revision;
        snapshot.created        = counter.created;
        snapshot.expired        = counter.expired;
        snapshot.resumed        = counter.resumed;
        snapshot.discarded      = counter.discarded;
        snapshot.frames         = counter.frames;
        snapshot.empty_frames   = counter.empty_frames;
        snapshot.event_sequence = counter.event_sequence;

        for(int i = 0; i < 8; ++i)
            snapshot.events[i] = counter.events[i];
        return snapshot;
    }


    // Periodically saves changed totals/calibration. Counter access is unlocked during slow flash I/O
    void persistence_task(void *) {
        while(true) {
            vTaskDelay(pdMS_TO_TICKS(park::settings::checkpoint_poll_ms));

            // Always acquire save_mutex before counter_mutex
            // Prevent an older background snapshot from overwriting a reset that was already acknowledged to the browser
            xSemaphoreTake(save_mutex, portMAX_DELAY);
            xSemaphoreTake(counter_mutex, portMAX_DELAY);
            const uint32_t revision_to_save = revision;
            bool has_pending_changes        = revision_to_save != saved_revision;
            SavedCounterState state         = {1,
                                               counter.config.first,
                                               counter.config.second,
                                               counter.config.horizontal,
                                               counter.config.reverse,
                                               counter.entries,
                                               counter.exits};

            xSemaphoreGive(counter_mutex);
            // Skip unchanged checkpoints to avoid unnecessary flash writes
            if(!has_pending_changes) {
                xSemaphoreGive(save_mutex);
                continue;
            }

            esp_err_t error = write_state(state);
            xSemaphoreTake(counter_mutex, portMAX_DELAY);
            storage_ok = error == ESP_OK;

            if(storage_ok)
                saved_revision = revision_to_save;

            xSemaphoreGive(counter_mutex);
            xSemaphoreGive(save_mutex);

            if(error != ESP_OK)
                ESP_LOGE("COUNTER", "Cannot save counts: %s", esp_err_to_name(error));
        }
    }
}

// Runs core checks, validates any saved checkpoint, restores it, and starts background persistence
// Unexpected NVS data/errors disable startup instead of erasing existing state
bool visitor_counter_start() {
    if(!park::self_test()) {
        ESP_LOGE("COUNTER", "Core self-tests FAILED; counting disabled");
        return false;
    }

    ESP_LOGI("COUNTER", "Core self-tests PASS (crossing scenario suite)");
    counter_mutex = xSemaphoreCreateMutex();
    save_mutex    = xSemaphoreCreateMutex();

    if(!counter_mutex || !save_mutex)
        return false;

    esp_err_t error = nvs_flash_init();
    if(error != ESP_OK) {
        ESP_LOGE("COUNTER", "NVS unavailable; no data erased");
        return false;
    }

    nvs_handle_t handle;
    error = nvs_open("park_counter", NVS_READWRITE, &handle);
    if(error != ESP_OK)
        return false;

    SavedCounterState state = {};
    size_t size = sizeof(state);
    error       = nvs_get_blob(handle, "state", &state, &size);
    nvs_close(handle);

    if(error == ESP_OK) {
        park::Config c{state.first, state.second, state.horizontal != 0, state.reverse != 0};
        if(size != sizeof(state) || state.version != 1 || state.horizontal > 1 || state.reverse > 1 || !valid(c)) {
            ESP_LOGE("COUNTER", "Invalid saved state; preserved, counting disabled");
            return false;
        }

        counter.config  = c;
        counter.entries = state.entries;
        counter.exits   = state.exits;
    } else if(error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE("COUNTER", "Cannot load counts: %s; preserved", esp_err_to_name(error));
        return false;
    }

    loaded_entries = counter.entries;
    loaded_exits   = counter.exits;
    storage_ok     = true;

    if(xTaskCreate(persistence_task, "CountSave", 3072, nullptr, 1, nullptr) != pdPASS) {
        storage_ok = false;
        return false;
    }

    ready = true;
    ESP_LOGI("COUNTER", "Ready: entries=%llu exits=%llu; checkpoint every 10 s", counter.entries, counter.exits);
    return true;
}


// Returns a consistent copy for other modules, an empty snapshot means the counter has not initialized
CountSnapshot visitor_counter_snapshot() {
    if(!counter_mutex)
        return CountSnapshot{};
    xSemaphoreTake(counter_mutex, portMAX_DELAY);
    auto snapshot = snapshot_locked();
    xSemaphoreGive(counter_mutex);
    return snapshot;
}


// Adds the journal totals missing from the loaded NVS floor, preserving passages already counted this boot
void visitor_counter_restore(uint64_t entries, uint64_t exits) {
    if(!counter_mutex)
        return;

    xSemaphoreTake(counter_mutex, portMAX_DELAY);
    const auto missing_entries = entries > loaded_entries ? entries - loaded_entries:0;
    const auto missing_exits   = exits > loaded_exits ? exits - loaded_exits:0;

    // Applied once, after mount. Do not replace totals containing new detections
    counter.entries += missing_entries;
    counter.exits   += missing_exits;

    if(missing_entries || missing_exits)
        ++revision;

    loaded_entries = counter.entries;
    loaded_exits   = counter.exits;
    xSemaphoreGive(counter_mutex);
}


// Filters detector boxes, updates tracking under counter_mutex, and maps IDs back to detector order
CountSnapshot visitor_counter_update(const who::detect::WhoDetect::result_t &result) {
    if(!counter_mutex)
        return CountSnapshot{};

    park::Observation observations[park::capacity];
    int detection_indices[park::capacity];
    int observation_count = 0, detection_index = 0;

    // 1. Filter detections by capacity, confidence, image dimensions, and valid box coordinates
    for(const auto &detection : result.det_res) {
        if(detection_index >= park::capacity)
            break;

        if(detection.box.size() >= 4 && detection.score >= .5f && result.img.width > 0 && result.img.height > 0 &&
           detection.box[2] > detection.box[0] && detection.box[3] > detection.box[1]) {
            // 2. Convert each accepted box to normalized center coordinates and dimensions
            observations[observation_count] = {
                std::clamp((detection.box[0] + detection.box[2]) / (2.f * result.img.width), 0.f, 1.f),
                std::clamp((detection.box[1] + detection.box[3]) / (2.f * result.img.height), 0.f, 1.f),
                std::clamp((detection.box[2] - detection.box[0]) / static_cast<float>(result.img.width), .001f, 1.f),
                std::clamp((detection.box[3] - detection.box[1]) / static_cast<float>(result.img.height), .001f, 1.f)};

            detection_indices[observation_count++] = detection_index;
        }

        ++detection_index;
    }

    // 3. Update tracking and passage totals while holding the counter mutex
    xSemaphoreTake(counter_mutex, portMAX_DELAY);
    unsigned ids[park::capacity] = {};

    if(ready && !frozen) {
        auto previous_total_passages = counter.entries + counter.exits;
        counter.update(observations, observation_count, esp_timer_get_time(), ids);

        if(previous_total_passages != counter.entries + counter.exits) {
            ++revision;
            ESP_LOGI("COUNTER", "Crossing: entries=%llu exits=%llu", counter.entries, counter.exits);
        }
    }

    // 4. Copy the current state and map track IDs and display states back to the original detection order
    auto snapshot = snapshot_locked();

    for(int i = 0; i < observation_count; ++i) {
        snapshot.ids[detection_indices[i]]    = ids[i];
        snapshot.states[detection_indices[i]] = counter.state(ids[i], esp_timer_get_time());
    }

    xSemaphoreGive(counter_mutex);
    return snapshot;
}


// Updates calibration only while counting is ready and unfrozen. Clears tracks so old paths cannot cross new lines
bool visitor_counter_configure(const park::Config &c) {
    if(!counter_mutex || !valid(c))
        return false;

    xSemaphoreTake(counter_mutex, portMAX_DELAY);
    bool ok = ready && !frozen;

    if(ok) {
        counter.config = c;
        counter.clear_tracks();
        ++revision;
    }
    xSemaphoreGive(counter_mutex);
    return ok;
}


// Temporarily blocks tracking/calibration updates while the final sleep checkpoint is being captured
void visitor_counter_freeze(bool value) {
    if(!counter_mutex)
        return;

    xSemaphoreTake(counter_mutex, portMAX_DELAY);
    frozen = value;
    xSemaphoreGive(counter_mutex);
}


// Saves pending state synchronously with a bounded wait for the writer lock
// Success also requires that no newer counter revision appeared while flash was being written
bool visitor_counter_flush() {
    if(!counter_mutex || !save_mutex || !ready)
        return false;

    if(xSemaphoreTake(save_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
        return false;

    xSemaphoreTake(counter_mutex, portMAX_DELAY);
    const auto revision_to_save    = revision;
    const bool has_pending_changes = revision_to_save != saved_revision;
    SavedCounterState state        = {1,
                                      counter.config.first,
                                      counter.config.second,
                                      counter.config.horizontal,
                                      counter.config.reverse,
                                      counter.entries,
                                      counter.exits};

    xSemaphoreGive(counter_mutex);
    const auto result = has_pending_changes ? write_state(state):ESP_OK;
    xSemaphoreTake(counter_mutex, portMAX_DELAY);
    storage_ok = result == ESP_OK;
    if(storage_ok)
        saved_revision = revision_to_save;

    const bool ok = storage_ok && saved_revision == revision;
    xSemaphoreGive(counter_mutex);
    xSemaphoreGive(save_mutex);
    return ok;
}


// Serializes one snapshot for the local page, including the latest diagnostic events in chronological order
std::string visitor_counter_json(const CountSnapshot &snapshot) {
    char json[768];

    // Totals are strings to preserve uint64 precision in browser/exports
    snprintf(
        json, sizeof(json),
        "{\"entries\":\"%llu\",\"exits\":\"%llu\",\"ready\":%s,\"storage_ok\":%s,\"pending\":%s,\"first\":%.4f,\"second\":%.4f,\"horizontal\":%s,\"reverse\":%s,\"created\":%u,\"expired\":%u,\"resumed\":%u,\"discarded\":%u,\"frames\":%u,\"empty_frames\":%u,\"events\":[",
        snapshot.entries, snapshot.exits, snapshot.ready ? "true":"false", snapshot.storage_ok ? "true":"false", snapshot.pending ? "true":"false",
        static_cast<double>(snapshot.config.first), static_cast<double>(snapshot.config.second),
        snapshot.config.horizontal ? "true":"false", snapshot.config.reverse ? "true":"false", snapshot.created, snapshot.expired, snapshot.resumed,
        snapshot.discarded, snapshot.frames, snapshot.empty_frames);

    std::string output   = json;
    const unsigned count = std::min(snapshot.event_sequence, 8u);

    for(unsigned n = 0; n < count; ++n) {
        const auto &e = snapshot.events[(snapshot.event_sequence - count + n) % 8];
        char event[128];
        snprintf(event, sizeof(event), "%s{\"seq\":%u,\"id\":%u,\"code\":%d,\"ms\":%lld}", n ? ",":"", e.sequence, e.id, e.code, e.at / 1000);
        output += event;
    }
    output += "]}";
    return output;
}
