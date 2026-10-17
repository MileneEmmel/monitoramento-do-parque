#include "park_power.hpp"
#include "power_policy.hpp"
#include "pir_sensor.hpp"
#include "park_history.hpp"
#include "park_cloud.hpp"
#include "visitor_counter.hpp"
#include "wifi_preview.hpp"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <atomic>

namespace {
constexpr uint32_t SENSOR_VALID = 0x50495231;
RTC_DATA_ATTR uint32_t sensor_valid = 0;
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
park::PowerPolicy policy;
SemaphoreHandle_t control = nullptr;
std::atomic<bool> enabled{false}, preparing{false};
std::atomic<const char *> phase{"starting"};
std::atomic<const char *> last_block{""};
uint64_t grace = 120000;
uint64_t ms() { return esp_timer_get_time() / 1000; }
park::PowerPolicy snapshot() {
    portENTER_CRITICAL(&state_lock); auto p = policy; portEXIT_CRITICAL(&state_lock); return p;
}
void touch() {
    portENTER_CRITICAL(&state_lock); policy.activity = ms(); portEXIT_CRITICAL(&state_lock);
}
bool quiet() {
    if (pir_sensor_level() == 1) { touch(); return false; }
    return enabled.load() && pir_sensor_level() == 0 &&
        snapshot().idle(ms(), grace, CONFIG_APP_PIR_IDLE_SECONDS * 1000ULL, wifi_preview_requested());
}
void attempt_sleep() {
    if (xSemaphoreTake(control, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (!quiet()) { xSemaphoreGive(control); return; }
    preparing.store(true);
    phase.store("saving");
    visitor_counter_freeze(true);
    bool cloud_locked = false, journal_locked = false, wifi_stopped = false;
    // Freeze before taking the durable snapshot. A failed save cancels sleep.
    bool ok = visitor_counter_flush();
    if (!ok) last_block.store("checkpoint_failed");
    if (ok) {
        journal_locked = park_history_sleep_lock();
        ok = journal_locked;
        if (!ok) last_block.store(park_today().empty() ? "waiting_clock" : "journal_failed");
    }
    if (journal_locked) { park_history_sleep_unlock(); journal_locked = false; }
    if (ok) {
        phase.store("sync_window");
        park_cloud_request_sync();
        const auto deadline = ms() + 8000;
        while (ms() < deadline && quiet()) vTaskDelay(pdMS_TO_TICKS(100));
        ok = quiet();
        if (!ok) last_block.store("new_activity");
    }
    if (ok) { cloud_locked = park_cloud_pause(2000); ok = cloud_locked; if (!ok) last_block.store("cloud_busy"); }
    if (ok) { journal_locked = park_history_sleep_lock(); ok = journal_locked; if (!ok) last_block.store("journal_failed"); }
    if (ok) { wifi_stopped = wifi_preview_sleep(); ok = wifi_stopped; if (!ok) last_block.store("wifi_stop_failed"); }
    if (ok) { ok = quiet(); if (!ok) last_block.store("new_activity"); }
    if (ok) {
        phase.store("sleeping");
        ok = esp_sleep_enable_timer_wakeup(CONFIG_APP_PIR_SYNC_SECONDS * 1000000ULL) == ESP_OK &&
             pir_sensor_arm_wakeup();
        if (!ok) last_block.store("wake_arm_or_new_activity");
        // A level-sensitive wake means motion during this final step wakes us immediately.
        if (ok) {
            ESP_LOGI("POWER", "DEEP_SLEEP: GPIO1 high or timer=%ds; counts and SD flushed",
                     CONFIG_APP_PIR_SYNC_SECONDS);
            esp_deep_sleep_start();
        }
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    pir_sensor_init();
    if (journal_locked) park_history_sleep_unlock();
    if (cloud_locked) park_cloud_resume();
    if (wifi_stopped) wifi_preview_wake();
    visitor_counter_freeze(false);
    preparing.store(false);
    phase.store("sleep_cancelled");
    touch();
    xSemaphoreGive(control);
    ESP_LOGW("POWER", "Sleep cancelled: %s", last_block.load());
}
void task(void *) {
    while (true) {
        const int level = pir_sensor_level();
        portENTER_CRITICAL(&state_lock);
        policy.pir(level, ms());
        if (policy.validated) sensor_valid = SENSOR_VALID;
        portEXIT_CRITICAL(&state_lock);
        if (!preparing.load()) phase.store(!enabled.load() ? "continuous" :
            !snapshot().validated ? "validate_pir" : "watching");
        if (quiet()) attempt_sleep();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}
bool park_power_start() {
    if (!park::power_policy_test() || !pir_sensor_init()) {
        phase.store("self_test_or_gpio_failed"); return false;
    }
    ESP_LOGI("POWER", "Policy self-tests PASS; PIR GPIO1 input");
    control = xSemaphoreCreateMutex();
    if (!control) return false;
    const auto cause = esp_sleep_get_wakeup_cause();
    const bool wake = cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_TIMER;
    policy.validated = wake && sensor_valid == SENSOR_VALID;
    if (!wake) sensor_valid = 0;
    grace = !wake ? 120000 : cause == ESP_SLEEP_WAKEUP_EXT0 ? 60000 : 20000;
    nvs_handle_t handle;
#ifdef CONFIG_APP_PIR_SLEEP
    uint8_t value = 1;
#else
    uint8_t value = 0;
#endif
    if (nvs_open("park_power", NVS_READWRITE, &handle) != ESP_OK) return false;
    auto result = nvs_get_u8(handle, "enabled", &value);
    nvs_close(handle);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) return false;
    enabled.store(value != 0);
    ESP_LOGI("POWER", "Boot: wake_cause=%d, enabled=%d, grace_ms=%llu, sensor_valid=%d",
             int(cause), int(enabled.load()), grace, int(policy.validated));
    if (xTaskCreate(task, "ParkPower", 6144, nullptr, 1, nullptr) != pdPASS) {
        enabled.store(false); return false;
    }
    return true;
}
bool park_power_should_start_camera() {
    return !enabled.load() || pir_sensor_level() == 1 || wifi_preview_requested();
}
void park_power_camera_started() {
    portENTER_CRITICAL(&state_lock); policy.camera = true; policy.activity = ms(); portEXIT_CRITICAL(&state_lock);
}
void park_power_detection(bool people) {
    portENTER_CRITICAL(&state_lock); policy.detection(people, ms()); portEXIT_CRITICAL(&state_lock);
}
bool park_power_preparing() { return preparing.load(); }
cJSON *park_power_status() {
    const auto p = snapshot();
    auto json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", enabled.load());
    cJSON_AddBoolToObject(json, "preparing", preparing.load());
    cJSON_AddBoolToObject(json, "sensor_validated", p.validated);
    cJSON_AddBoolToObject(json, "camera_started", p.camera);
    cJSON_AddBoolToObject(json, "detector_ready", p.frame != 0);
    cJSON_AddBoolToObject(json, "clock_ok", !park_today().empty());
    cJSON_AddStringToObject(json, "last_block", last_block.load());
    cJSON_AddNumberToObject(json, "frame_age_ms", p.frame ? static_cast<double>(ms() - p.frame) : -1.0);
    cJSON_AddNumberToObject(json, "wake_cause", esp_sleep_get_wakeup_cause());
    cJSON_AddNumberToObject(json, "pir_level", pir_sensor_level());
    cJSON_AddStringToObject(json, "phase", phase.load());
    cJSON_AddNumberToObject(json, "idle_seconds", CONFIG_APP_PIR_IDLE_SECONDS);
    cJSON_AddNumberToObject(json, "sync_seconds", CONFIG_APP_PIR_SYNC_SECONDS);
    cJSON_AddNumberToObject(json, "uptime_ms", ms());
    return json;
}
bool park_power_configure(const cJSON *json) {
    const auto value = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (!cJSON_IsBool(value) || !control || xSemaphoreTake(control, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    nvs_handle_t handle;
    auto result = nvs_open("park_power", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, "enabled", cJSON_IsTrue(value) ? 1 : 0);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result == ESP_OK) { enabled.store(cJSON_IsTrue(value)); touch(); }
    xSemaphoreGive(control);
    return result == ESP_OK;
}
