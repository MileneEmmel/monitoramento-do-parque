#include "park_power.hpp"
#include "app_settings.hpp"
#include "power_policy.hpp"
#include "power_monitor.hpp"
#include "pir_sensor.hpp"
#include "park_history.hpp"
#include "park_cloud.hpp"
#include "visitor_counter.hpp"
#include "wifi_preview.hpp"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <atomic>
#include <ctime>
#include <cstring>

namespace {
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
park::PowerPolicy policy;
SemaphoreHandle_t control = nullptr;
std::atomic<bool> enabled{false}, preparing{false};
std::atomic<const char *> phase{"starting"};
std::atomic<const char *> last_block{""};
uint64_t grace = park::settings::cold_boot_ms;
std::atomic<bool> timer_only{false}, camera_fault{false}, recovery_requested{false}, recovery_used{false};
std::atomic<uint64_t> camera_started_ms{0}, recovery_deadline{0}, network_deadline{0};
std::atomic<time_t> expected_wake{0};
uint64_t ms() { return esp_timer_get_time() / 1000; }
park::PowerPolicy snapshot() {
    portENTER_CRITICAL(&state_lock); auto p = policy; portEXIT_CRITICAL(&state_lock); return p;
}
void touch(const char *source) {
    portENTER_CRITICAL(&state_lock); policy.note_activity(ms(), source); portEXIT_CRITICAL(&state_lock);
}
const char *sleep_block(const park::PowerPolicy &p, uint64_t now, int level) {
    if (!enabled.load()) return "disabled";
    if (level != 0) return level == 1 ? "pir_active" : "gpio_unavailable";
    return p.idle_block(now, grace, timer_only.load() ? 0 : park::settings::idle_ms);
}
const char *cancel_reason() {
    const auto p = snapshot();
    const auto reason = sleep_block(p, ms(), pir_sensor_level());
    return reason[0] ? reason : "new_activity";
}
bool quiet() {
    const int level = pir_sensor_level();
    power_monitor_pir(level, ms());
    if (level == 1) { timer_only.store(false); touch("pir"); return false; }
    const auto p = snapshot();
    return sleep_block(p, ms(), level)[0] == '\0';
}
void attempt_sleep() {
    if (xSemaphoreTake(control, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (!quiet()) { xSemaphoreGive(control); return; }
    expected_wake.store(time(nullptr)+CONFIG_APP_PIR_SYNC_SECONDS);
    preparing.store(true);
    phase.store("saving");
    power_monitor_event("sleep_prepare");
    bool journal_locked = false, wifi_stopped = false;
    // Keep counting throughout the network window. Freeze only for the final
    // durable snapshot, so returning motion can cancel sleep without an 8 s gap.
    bool ok = visitor_counter_flush();
    if (!ok) last_block.store("checkpoint_failed");
    if (ok) {
        journal_locked = park_history_sleep_lock();
        ok = journal_locked;
        if (!ok) last_block.store("journal_busy");
    }
    if (journal_locked) { park_history_sleep_unlock(); journal_locked = false; }
    if (ok) {
        phase.store("sync_window");
        const auto deadline = ms() + park::settings::shutdown_network_ms;
        network_deadline.store(deadline);
        expected_wake.store(time(nullptr) + CONFIG_APP_PIR_SYNC_SECONDS);
        const auto closing_revision=park_history_revision();
        park_cloud_prepare_sleep(deadline);
        while (ms() < deadline && quiet() && !park_cloud_sleep_ready()) vTaskDelay(pdMS_TO_TICKS(50));
        ok = quiet();
        if (!ok) last_block.store(cancel_reason());
        if (ok) {
            const bool cloud_locked = park_cloud_pause(2000);
            // Abortable HTTP has a bounded deadline. Paused also prevents new
            // requests/cursor writes if the worker has not returned yet.
            if (!cloud_locked) power_monitor_event("sleep_warning", "cloud_stop_timeout");
            visitor_counter_freeze(true);
            ok = visitor_counter_flush();
            if (!ok) last_block.store("checkpoint_failed");
            if (ok) { journal_locked=park_history_sleep_lock(); ok=journal_locked; if(!ok) last_block.store("journal_busy"); }
            // A passage arrived during successful draining: give its new row a
            // chance too, under the original deadline, never a fresh 120 s.
            if (ok && park_history_revision()!=closing_revision && ms()<deadline &&
                !strcmp(park_cloud_sleep_result(),"drained")) {
                park_history_sleep_unlock(); journal_locked=false;
                visitor_counter_freeze(false); park_cloud_resume();
                park_cloud_prepare_sleep(deadline);
                while (ms()<deadline && quiet() && !park_cloud_sleep_ready()) vTaskDelay(pdMS_TO_TICKS(50));
                park_cloud_pause(2000); visitor_counter_freeze(true);
                ok=quiet() && visitor_counter_flush();
                if(ok) { journal_locked=park_history_sleep_lock(); ok=journal_locked; }
                if(!ok) last_block.store(cancel_reason());
            }
        }
    }
    if (ok) { ok = quiet(); if (!ok) last_block.store(cancel_reason()); }
    if (ok && !park_history_healthy()) power_monitor_event("sleep_warning", "journal_failed");
    if (ok && park_today().empty()) power_monitor_event("sleep_warning", "waiting_clock");
    if (ok) { wifi_stopped = wifi_preview_sleep(); ok = wifi_stopped; if (!ok) last_block.store("wifi_stop_failed"); }
    if (ok) { ok = quiet(); if (!ok) last_block.store(cancel_reason()); }
    if (ok) {
        phase.store("sleeping");
        ok = esp_sleep_enable_timer_wakeup(park::settings::periodic_sleep_us) == ESP_OK &&
             pir_sensor_arm_wakeup();
        if (!ok) last_block.store("wake_arm_or_new_activity");
        // A level-sensitive wake means motion during this final step wakes us immediately.
        if (ok) {
            power_monitor_sleep();
            ESP_LOGI("POWER", "DEEP_SLEEP: GPIO1 high or timer=%ds; NVS saved; SD health=%d; sync=%s",
                     CONFIG_APP_PIR_SYNC_SECONDS,park_history_healthy(),park_cloud_sleep_result());
            esp_deep_sleep_start();
        }
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    pir_sensor_init();
    if (journal_locked) park_history_sleep_unlock();
    park_cloud_cancel_sleep(); park_cloud_resume();
    expected_wake.store(0); network_deadline.store(0);
    if (wifi_stopped) wifi_preview_wake();
    visitor_counter_freeze(false);
    preparing.store(false);
    phase.store("sleep_cancelled");
    power_monitor_event("sleep_cancelled", last_block.load());
    touch("retry");
    xSemaphoreGive(control);
    ESP_LOGW("POWER", "Sleep cancelled: %s", last_block.load());
}
void task(void *) {
    while (true) {
        const int level = pir_sensor_level();
        power_monitor_pir(level, ms());
        portENTER_CRITICAL(&state_lock);
        policy.pir(level, ms());
        portEXIT_CRITICAL(&state_lock);
        if(level==1) timer_only.store(false);
        const auto p=snapshot();
        const auto last_frame=p.frame?p.frame:camera_started_ms.load();
        if(p.camera && ms()-last_frame>=park::settings::inference_stale_ms && !recovery_used.exchange(true)) {
            recovery_deadline.store(ms()+park::settings::camera_recovery_ms);
            recovery_requested.store(true); power_monitor_event("camera_recovery");
        }
        if(recovery_requested.load() && ms()>=recovery_deadline.load()) {
            recovery_requested.store(false); camera_fault.store(true); power_monitor_event("camera_failed");
        }
        if (!preparing.load()) phase.store(!enabled.load() ? "continuous" :
            recovery_requested.load()?"camera_recovery":"watching");
        if (!recovery_requested.load() && quiet()) attempt_sleep();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}
bool park_power_start() {
    power_monitor_start();
    if (!park::power_policy_test() || !pir_sensor_init()) {
        phase.store("self_test_or_gpio_failed"); return false;
    }
    ESP_LOGI("POWER", "Policy self-tests PASS; PIR GPIO1 input");
    control = xSemaphoreCreateMutex();
    if (!control) return false;
    const auto cause = esp_sleep_get_wakeup_cause();
    const bool wake = esp_reset_reason()==ESP_RST_DEEPSLEEP;
    timer_only.store(wake && cause==ESP_SLEEP_WAKEUP_TIMER);
    grace = wake ? 0 : park::settings::cold_boot_ms;
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
    timer_only.store(false); camera_started_ms.store(ms());
    portENTER_CRITICAL(&state_lock); policy.camera = true; policy.note_activity(ms(), "camera_start"); portEXIT_CRITICAL(&state_lock);
}
void park_power_detection(bool people) {
    camera_fault.store(false);
    power_monitor_frame(people, ms());
    portENTER_CRITICAL(&state_lock); policy.detection(people, ms()); portEXIT_CRITICAL(&state_lock);
}
bool park_power_preparing() { return preparing.load(); }
cJSON *park_power_status() {
    const auto p = snapshot();
    const auto now = ms();
    const auto level = pir_sensor_level();
    auto json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", enabled.load());
    cJSON_AddBoolToObject(json, "preparing", preparing.load());
    cJSON_AddBoolToObject(json, "sensor_validated", p.validated);
    cJSON_AddBoolToObject(json, "camera_started", p.camera);
    cJSON_AddBoolToObject(json, "detector_ready", p.frame != 0);
    cJSON_AddBoolToObject(json, "clock_ok", !park_today().empty());
    cJSON_AddStringToObject(json, "last_block", last_block.load());
    cJSON_AddStringToObject(json, "sleep_block", preparing.load() ? "preparing" : recovery_requested.load()?"camera_recovery":sleep_block(p, now, level));
    cJSON_AddStringToObject(json, "activity_source", p.activity_source);
    cJSON_AddNumberToObject(json, "idle_for_ms", now >= p.activity ? now - p.activity : 0);
    cJSON_AddNumberToObject(json, "boot_grace_remaining_ms", now < grace ? grace - now : 0);
    cJSON_AddBoolToObject(json, "preview_active", wifi_preview_requested());
    cJSON_AddBoolToObject(json, "preview_blocks_sleep", false);
    cJSON_AddNumberToObject(json, "frame_age_ms", p.frame ? static_cast<double>(ms() - p.frame) : -1.0);
    cJSON_AddNumberToObject(json, "wake_cause", esp_sleep_get_wakeup_cause());
    cJSON_AddNumberToObject(json, "pir_level", level);
    cJSON_AddStringToObject(json, "phase", phase.load());
    cJSON_AddNumberToObject(json, "idle_seconds", CONFIG_APP_PIR_IDLE_SECONDS);
    cJSON_AddNumberToObject(json, "sync_seconds", CONFIG_APP_PIR_SYNC_SECONDS);
    cJSON_AddNumberToObject(json, "upload_seconds", park::settings::collect_ms/1000);
    cJSON_AddNumberToObject(json, "network_budget_seconds", park::settings::shutdown_network_ms/1000);
    cJSON_AddNumberToObject(json, "network_remaining_ms", preparing.load() && network_deadline.load()>now?network_deadline.load()-now:0);
    cJSON_AddBoolToObject(json, "camera_fault", camera_fault.load());
    cJSON_AddBoolToObject(json, "timer_only", timer_only.load());
    cJSON_AddBoolToObject(json, "storage_ok", park_history_healthy());
    cJSON_AddNumberToObject(json, "uptime_ms", ms());
    const auto idle_ms = timer_only.load()?0:park::settings::idle_ms;
    const auto elapsed = now >= p.activity ? now - p.activity : 0;
    cJSON_AddNumberToObject(json, "quiet_remaining_ms", elapsed < idle_ms ? idle_ms - elapsed : 0);
    cJSON_AddItemToObject(json, "monitor", power_monitor_status());
    cJSON_AddItemToObject(json, "cloud", park_cloud_status());
    return json;
}
cJSON *park_power_cloud_status() {
    auto j=cJSON_CreateObject();
    cJSON_AddBoolToObject(j,"sleep_enabled",enabled.load());
    cJSON_AddStringToObject(j,"state",preparing.load()?"sleep_planned":"awake");
    cJSON_AddNumberToObject(j,"sleep_seconds",CONFIG_APP_PIR_SYNC_SECONDS);
    cJSON_AddBoolToObject(j,"camera_ok",!camera_fault.load());
    if(preparing.load() && expected_wake.load()>=1704067200) cJSON_AddNumberToObject(j,"expected_wake_epoch",expected_wake.load());
    return j;
}
void park_power_camera_failed() { camera_fault.store(true); }
bool park_power_recovery_requested() { return recovery_requested.load(); }
void park_power_recovery_finished(bool success) {
    if(!recovery_requested.exchange(false)) return;
    camera_fault.store(!success); power_monitor_event(success?"camera_recovered":"camera_failed");
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
    if (result == ESP_OK) {
        enabled.store(cJSON_IsTrue(value)); touch("configuration");
        power_monitor_event("mode_changed", enabled.load() ? "pir" : "continuous");
    }
    xSemaphoreGive(control);
    return result == ESP_OK;
}
