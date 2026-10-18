// Coordinates activity, camera recovery, and device preparation for deep sleep

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
    // Short policy updates use a critical section, control_mutex serializes configuration with the longer sleep transaction
    portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
    park::PowerPolicy policy;
    SemaphoreHandle_t control_mutex = nullptr;
    std::atomic<bool> enabled{false}, preparing{false};
    std::atomic<const char *> phase{"starting"};
    std::atomic<const char *> last_block{""};
    uint64_t boot_grace_ms = park::settings::cold_boot_ms;
    std::atomic<bool> timer_only{false}, camera_fault{false}, recovery_requested{false}, recovery_used{false};
    std::atomic<uint64_t> camera_started_ms{0}, recovery_deadline{0}, network_deadline{0};
    std::atomic<time_t> expected_wake{0};

    // Returns monotonic milliseconds, shared by activity, recovery, and closing deadlines
    uint64_t ms() {
        return esp_timer_get_time() / 1000;
    }

    // Copies the small policy under a critical section, then lets callers inspect it without holding the lock
    park::PowerPolicy snapshot() {
        portENTER_CRITICAL(&state_lock);
        auto policy_copy = policy;
        portEXIT_CRITICAL(&state_lock);
        return policy_copy;
    }

    // Renews activity safely from tasks that may race with detector and PIR updates
    void touch(const char *source) {
        portENTER_CRITICAL(&state_lock);
        policy.note_activity(ms(), source);
        portEXIT_CRITICAL(&state_lock);
    }

    // Combines operating mode, PIR availability/level, startup grace, and inactivity into one reason string
    const char *sleep_block(const park::PowerPolicy &policy_snapshot, uint64_t now_ms, int pir_level) {
        if(!enabled.load())
            return "disabled";
        if(pir_level != 0)
            return pir_level == 1 ? "pir_active":"gpio_unavailable";
        return policy_snapshot.idle_block(now_ms, boot_grace_ms, timer_only.load() ? 0:park::settings::idle_ms);
    }

    // Explains a cancelled preparation using current state, falling back to new_activity when no block remains
    const char *cancel_reason() {
        const auto policy_snapshot = snapshot();
        const auto reason          = sleep_block(policy_snapshot, ms(), pir_sensor_level());
        return reason[0] ? reason:"new_activity";
    }

    // Samples the PIR and reevaluates sleep eligibility. High PIR also ends timer-only synchronization mode
    bool quiet() {
        const int pir_level = pir_sensor_level();
        power_monitor_pir(pir_level, ms());
        if(pir_level == 1) {
            timer_only.store(false);
            touch("pir");
            return false;
        }
        const auto policy_snapshot = snapshot();
        return sleep_block(policy_snapshot, ms(), pir_level)[0] == '\0';
    }

    // Runs the closing sequence: checkpoint, bounded cloud drain, final freeze, Wi-Fi stop, and wake-source arming
    // Activity can cancel preparation, the rollback path restores normal operation. Successful deep sleep does not return
    void attempt_sleep() {
        // 1. Acquire the control lock and check whether preparation can begin
        if(xSemaphoreTake(control_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
            return;

        if(!quiet()) {
            xSemaphoreGive(control_mutex);
            return;
        }

        // 2. Announce preparation and save the initial counter state
        expected_wake.store(time(nullptr) + CONFIG_APP_PIR_SYNC_SECONDS);
        preparing.store(true);
        phase.store("saving");
        power_monitor_event("sleep_prepare");
        bool journal_locked = false, wifi_stopped = false;

        // Keep counting throughout the network window
        // Freeze only for the final durable snapshot, so returning motion can cancel sleep without an 8 s gap
        bool preparation_ok = visitor_counter_flush();
        if(!preparation_ok)
            last_block.store("checkpoint_failed");

        if(preparation_ok) {
            journal_locked = park_history_sleep_lock();
            preparation_ok = journal_locked;
            if(!preparation_ok)
                last_block.store("journal_busy");
        }

        if(journal_locked) {
            park_history_sleep_unlock();
            journal_locked = false;
        }

        // 3. Open the bounded synchronization window while counting remains active
        if(preparation_ok) {
            phase.store("sync_window");
            // This is the total closing network budget. Individual DNS/HTTPS attempts share its remaining time
            const auto network_deadline_ms = ms() + park::settings::shutdown_network_ms;
            network_deadline.store(network_deadline_ms);
            expected_wake.store(time(nullptr) + CONFIG_APP_PIR_SYNC_SECONDS);
            const auto closing_revision = park_history_revision();
            park_cloud_prepare_sleep(network_deadline_ms);

            while(ms() < network_deadline_ms && quiet() && !park_cloud_sleep_ready())
                vTaskDelay(pdMS_TO_TICKS(50));

            preparation_ok = quiet();
            if(!preparation_ok)
                last_block.store(cancel_reason());

            if(preparation_ok) {
                // 4. Pause cloud work and freeze counting for the final checkpoint
                const bool cloud_locked = park_cloud_pause(2000);
                // Abortable HTTP has a bounded deadline
                // Paused also prevents new requests/cursor writes if the worker has not returned yet
                if(!cloud_locked)
                    power_monitor_event("sleep_warning", "cloud_stop_timeout");

                visitor_counter_freeze(true);
                preparation_ok = visitor_counter_flush();
                if(!preparation_ok)
                    last_block.store("checkpoint_failed");

                if(preparation_ok) {
                    journal_locked = park_history_sleep_lock();
                    preparation_ok = journal_locked;
                    if(!preparation_ok)
                        last_block.store("journal_busy");
                }

                // 5. Give newly persisted passages another upload opportunity under the same deadline
                // A passage arrived during successful draining:
                // give its new row a chance too, under the original deadline, never a fresh 120 s
                if(preparation_ok && park_history_revision() != closing_revision && ms() < network_deadline_ms &&
                   !strcmp(park_cloud_sleep_result(), "drained")) {
                    park_history_sleep_unlock();
                    journal_locked = false;
                    visitor_counter_freeze(false);
                    park_cloud_resume();
                    park_cloud_prepare_sleep(network_deadline_ms);

                    while(ms() < network_deadline_ms && quiet() && !park_cloud_sleep_ready())
                        vTaskDelay(pdMS_TO_TICKS(50));

                    park_cloud_pause(2000);
                    visitor_counter_freeze(true);
                    preparation_ok = quiet() && visitor_counter_flush();
                    if(preparation_ok) {
                        journal_locked = park_history_sleep_lock();
                        preparation_ok = journal_locked;
                    }
                    if(!preparation_ok)
                        last_block.store(cancel_reason());
                }
            }
        }
        // 6. Recheck activity and report storage or clock warnings
        if(preparation_ok) {
            preparation_ok = quiet();
            if(!preparation_ok)
                last_block.store(cancel_reason());
        }

        // Storage/clock failures are visible warnings, but do not keep an idle device awake forever
        if(preparation_ok && !park_history_healthy())
            power_monitor_event("sleep_warning", "journal_failed");

        if(preparation_ok && park_today().empty())
            power_monitor_event("sleep_warning", "waiting_clock");

        // 7. Stop Wi-Fi and recheck PIR activity before arming wake sources
        if(preparation_ok) {
            // Stop radio activity before arming wake sources, recheck PIR afterward to catch motion during shutdown
            wifi_stopped   = wifi_preview_sleep();
            preparation_ok = wifi_stopped;
            if(!preparation_ok)
                last_block.store("wifi_stop_failed");
        }

        if(preparation_ok) {
            preparation_ok = quiet();
            if(!preparation_ok)
                last_block.store(cancel_reason());
        }

        // 8. Arm wake sources and enter deep sleep
        if(preparation_ok) {
            phase.store("sleeping");
            preparation_ok = esp_sleep_enable_timer_wakeup(park::settings::periodic_sleep_us) == ESP_OK && pir_sensor_arm_wakeup();
            if(!preparation_ok)
                last_block.store("wake_arm_or_new_activity");

            // A level-sensitive wake means motion during this final step wakes us immediately
            if(preparation_ok) {
                power_monitor_sleep();
                ESP_LOGI("POWER", "DEEP_SLEEP: GPIO1 high or timer=%ds; NVS saved; SD health=%d; sync=%s", CONFIG_APP_PIR_SYNC_SECONDS, park_history_healthy(), park_cloud_sleep_result());

                esp_deep_sleep_start();
            }
        }

        // 9. Restore services and release resources if preparation was cancelled or failed
        // Rollback is reached only if preparation failed or new activity cancelled it. Release held resources and resume services
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        pir_sensor_init();

        if(journal_locked)
            park_history_sleep_unlock();

        park_cloud_cancel_sleep();
        park_cloud_resume();
        expected_wake.store(0);
        network_deadline.store(0);

        if(wifi_stopped)
            wifi_preview_wake();

        visitor_counter_freeze(false);
        preparing.store(false);
        phase.store("sleep_cancelled");
        power_monitor_event("sleep_cancelled", last_block.load());

        // Restart the quiet interval after cancellation so the worker does not immediately repeat a failing shutdown
        touch("retry");
        xSemaphoreGive(control_mutex);
        ESP_LOGW("POWER", "Sleep cancelled: %s", last_block.load());
    }

    // Polls PIR, requests at most one camera recovery per boot, and initiates sleep when conditions permit
    void task(void *) {
        while(true) {
            // 1. Read PIR activity and update the power policy
            const int pir_level = pir_sensor_level();
            power_monitor_pir(pir_level, ms());
            portENTER_CRITICAL(&state_lock);
            policy.pir(pir_level, ms());
            portEXIT_CRITICAL(&state_lock);

            if(pir_level == 1)
                timer_only.store(false);

            // 2. Inspect the last frame and request at most one camera recovery per boot
            const auto policy_snapshot = snapshot();
            const auto last_frame      = policy_snapshot.frame ? policy_snapshot.frame:camera_started_ms.load();

            // A started camera with stale inference gets one cooperative recovery attempt for this boot
            if(policy_snapshot.camera && ms() - last_frame >= park::settings::inference_stale_ms && !recovery_used.exchange(true)) {
                recovery_deadline.store(ms() + park::settings::camera_recovery_ms);
                recovery_requested.store(true);
                power_monitor_event("camera_recovery");
            }

            // 3. Mark camera recovery as failed if its deadline expires
            // A recovery timeout becomes a camera fault instead of vetoing sleep indefinitely
            if(recovery_requested.load() && ms() >= recovery_deadline.load()) {
                recovery_requested.store(false);
                camera_fault.store(true);
                power_monitor_event("camera_failed");
            }

            // 4. Publish the current operating phase when sleep preparation is inactive
            if(!preparing.load())
                phase.store(!enabled.load() ? "continuous":recovery_requested.load() ? "camera_recovery":"watching");

            // 5. Attempt sleep when recovery is inactive and the device is quiet
            if(!recovery_requested.load() && quiet())
                attempt_sleep();

            // 6. Wait before the next monitoring cycle
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// Initializes monitoring/PIR, restores the saved mode, classifies this boot, and starts the power worker
bool park_power_start() {
    power_monitor_start();
    if(!park::power_policy_test() || !pir_sensor_init()) {
        phase.store("self_test_or_gpio_failed");
        return false;
    }

    ESP_LOGI("POWER", "Policy self-tests PASS; PIR GPIO1 input");
    control_mutex = xSemaphoreCreateMutex();
    if(!control_mutex)
        return false;

    const auto cause = esp_sleep_get_wakeup_cause();
    const bool wake  = esp_reset_reason() == ESP_RST_DEEPSLEEP;

    // A real timer wake can synchronize and return to sleep without starting the camera or waiting through cold-boot grace
    timer_only.store(wake && cause == ESP_SLEEP_WAKEUP_TIMER);
    boot_grace_ms = wake ? 0:park::settings::cold_boot_ms;
    nvs_handle_t handle;
#ifdef CONFIG_APP_PIR_SLEEP
    uint8_t value = 1;
#else
    uint8_t value = 0;
#endif
    if(nvs_open("park_power", NVS_READWRITE, &handle) != ESP_OK)
        return false;

    // A saved runtime mode overrides the build default, a missing key keeps the default
    auto result = nvs_get_u8(handle, "enabled", &value);
    nvs_close(handle);

    if(result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
        return false;

    enabled.store(value != 0);
    ESP_LOGI("POWER", "Boot: wake_cause=%d, enabled=%d, grace_ms=%llu, sensor_valid=%d", int(cause), int(enabled.load()), boot_grace_ms, int(policy.validated));

    if(xTaskCreate(task, "ParkPower", 6144, nullptr, 1, nullptr) != pdPASS) {
        enabled.store(false);
        return false;
    }
    return true;
}

// Allows camera startup in continuous mode, for active PIR, or for an authenticated local preview
bool park_power_should_start_camera() {
    return !enabled.load() || pir_sensor_level() == 1 || wifi_preview_requested();
}

// Starts the frame-health deadline and normal activity timing when capture is about to start
void park_power_camera_started() {
    timer_only.store(false);
    camera_started_ms.store(ms());
    portENTER_CRITICAL(&state_lock);
    policy.camera = true;
    policy.note_activity(ms(), "camera_start");
    portEXIT_CRITICAL(&state_lock);
}

// Reports a processed frame to telemetry and policy. Empty results keep frame health alive without renewing presence
void park_power_detection(bool people) {
    camera_fault.store(false);
    power_monitor_frame(people, ms());
    portENTER_CRITICAL(&state_lock);
    policy.detection(people, ms());
    portEXIT_CRITICAL(&state_lock);
}

// Lets request handlers avoid changing configuration during the final sleep transaction
bool park_power_preparing() {
    return preparing.load();
}

// Builds local power diagnostics and remaining deadlines without starting capture or renewing activity
cJSON *park_power_status() {
    const auto policy_snapshot = snapshot();
    const auto now_ms          = ms();
    const auto pir_level       = pir_sensor_level();
    auto json                  = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", enabled.load());
    cJSON_AddBoolToObject(json, "preparing", preparing.load());
    cJSON_AddBoolToObject(json, "sensor_validated", policy_snapshot.validated);
    cJSON_AddBoolToObject(json, "camera_started", policy_snapshot.camera);
    cJSON_AddBoolToObject(json, "detector_ready", policy_snapshot.frame != 0);
    cJSON_AddBoolToObject(json, "clock_ok", !park_today().empty());
    cJSON_AddStringToObject(json, "last_block", last_block.load());
    cJSON_AddStringToObject(json, "sleep_block", preparing.load() ? "preparing":recovery_requested.load() ? "camera_recovery":sleep_block(policy_snapshot, now_ms, pir_level));
    cJSON_AddStringToObject(json, "activity_source", policy_snapshot.activity_source);
    cJSON_AddNumberToObject(json, "idle_for_ms", now_ms >= policy_snapshot.activity ? now_ms - policy_snapshot.activity:0);
    cJSON_AddNumberToObject(json, "boot_grace_remaining_ms", now_ms < boot_grace_ms ? boot_grace_ms - now_ms:0);
    cJSON_AddBoolToObject(json, "preview_active", wifi_preview_requested());
    cJSON_AddBoolToObject(json, "preview_blocks_sleep", false);
    cJSON_AddNumberToObject(json, "frame_age_ms", policy_snapshot.frame ? static_cast<double>(ms() - policy_snapshot.frame):-1.0);
    cJSON_AddNumberToObject(json, "wake_cause", esp_sleep_get_wakeup_cause());
    cJSON_AddNumberToObject(json, "pir_level", pir_level);
    cJSON_AddStringToObject(json, "phase", phase.load());
    cJSON_AddNumberToObject(json, "idle_seconds", CONFIG_APP_PIR_IDLE_SECONDS);
    cJSON_AddNumberToObject(json, "sync_seconds", CONFIG_APP_PIR_SYNC_SECONDS);
    cJSON_AddNumberToObject(json, "upload_seconds", park::settings::collect_ms / 1000);
    cJSON_AddNumberToObject(json, "network_budget_seconds", park::settings::shutdown_network_ms / 1000);
    cJSON_AddNumberToObject(json, "network_remaining_ms", preparing.load() && network_deadline.load() > now_ms ? network_deadline.load() - now_ms:0);
    cJSON_AddBoolToObject(json, "camera_fault", camera_fault.load());
    cJSON_AddBoolToObject(json, "timer_only", timer_only.load());
    cJSON_AddBoolToObject(json, "storage_ok", park_history_healthy());
    cJSON_AddNumberToObject(json, "uptime_ms", ms());
    const auto idle_ms = timer_only.load() ? 0:park::settings::idle_ms;
    const auto elapsed = now_ms >= policy_snapshot.activity ? now_ms - policy_snapshot.activity:0;
    cJSON_AddNumberToObject(json, "quiet_remaining_ms", elapsed < idle_ms ? idle_ms - elapsed:0);
    cJSON_AddItemToObject(json, "monitor", power_monitor_status());
    cJSON_AddItemToObject(json, "cloud", park_cloud_status());
    return json;
}

// Announces awake/planned-sleep state to the cloud. Planned sleep is intent, not proof that the chip slept
cJSON *park_power_cloud_status() {
    auto status_json = cJSON_CreateObject();
    cJSON_AddBoolToObject(status_json, "sleep_enabled", enabled.load());
    cJSON_AddStringToObject(status_json, "state", preparing.load() ? "sleep_planned":"awake");
    cJSON_AddNumberToObject(status_json, "sleep_seconds", CONFIG_APP_PIR_SYNC_SECONDS);
    cJSON_AddBoolToObject(status_json, "camera_ok", !camera_fault.load());
    if(preparing.load() && expected_wake.load() >= 1704067200)
        cJSON_AddNumberToObject(status_json, "expected_wake_epoch", expected_wake.load());
    return status_json;
}

// Marks startup capture as unavailable, storage/network operation can still continue
void park_power_camera_failed() {
    camera_fault.store(true);
}

// Lets the camera worker check whether a bounded recovery attempt is still active
bool park_power_recovery_requested() {
    return recovery_requested.load();
}

// Completes an active recovery request once, a late result after timeout is ignored
void park_power_recovery_finished(bool success) {
    if(!recovery_requested.exchange(false))
        return;
    camera_fault.store(!success);
    power_monitor_event(success ? "camera_recovered":"camera_failed");
}

// Persists the requested operating mode before applying it, serialized against sleep preparation
bool park_power_configure(const cJSON *json) {
    const auto value = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if(!cJSON_IsBool(value) || !control_mutex || xSemaphoreTake(control_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
        return false;

    nvs_handle_t handle;
    auto result = nvs_open("park_power", NVS_READWRITE, &handle);

    if(result == ESP_OK) {
        result = nvs_set_u8(handle, "enabled", cJSON_IsTrue(value) ? 1:0);
        if(result == ESP_OK)
            result = nvs_commit(handle);
        nvs_close(handle);
    }

    if(result == ESP_OK) {
        enabled.store(cJSON_IsTrue(value));
        touch("configuration");
        power_monitor_event("mode_changed", enabled.load() ? "pir":"continuous");
    }
    xSemaphoreGive(control_mutex);
    return result == ESP_OK;
}
