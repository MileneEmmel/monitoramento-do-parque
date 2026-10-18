// Synchronizes history and device status with the cloud, handling acknowledgments and retries

#include "park_cloud.hpp"
#include "cloud_policy.hpp"
#include "park_history.hpp"
#include "park_power.hpp"
#include "power_monitor.hpp"
#include "wifi_preview.hpp"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <map>
#include <cstring>
#include <string>

namespace {
    constexpr const char *HOST     = "pcxeestzvcpbfkcbuqpa.supabase.co";
    constexpr const char *ENDPOINT = "https://pcxeestzvcpbfkcbuqpa.supabase.co/rest/v1/rpc/park_ingest";

    // Shared diagnostics and control flags are read by HTTP/power tasks while this module's worker updates them
    std::atomic<bool> configured{false}, syncing{false}, closing{false}, close_ready{false}, paused{false};
    std::atomic<bool> pending{true}, permanent_error{false}, reset_schedule{false};
    std::atomic<int> last_http{0}, last_error{ESP_OK};
    std::atomic<time_t> last_success{0};
    std::atomic<uint64_t> close_deadline{0}, next_attempt{0};
    std::atomic<unsigned> batches{0}, failures{0};
    std::atomic<const char *> phase{"starting"}, server_error{""}, close_result{""};
    SemaphoreHandle_t cycle_mutex = nullptr;
    TaskHandle_t worker           = nullptr;

    // Returns monotonic milliseconds for deadlines, network time changes cannot extend an upload attempt
    uint64_t ms() {
        return esp_timer_get_time() / 1000;
    }

    // Reads a bounded string from the already-open NVS namespace
    bool read_nvs_string(nvs_handle_t nvs_handle, const char *key, char *buffer, size_t buffer_size) {
        return nvs_get_str(nvs_handle, key, buffer, &buffer_size) == ESP_OK;
    }

    // Publishes the closing result for the power manager, readiness does not necessarily mean all rows were uploaded
    void finish(const char *reason) {
        close_result.store(reason);
        close_ready.store(true);
    }

    // DNS runs on lwIP's thread. Static storage remains valid if its callback is delivered after our monotonic deadline, no task is killed holding a mutex
    std::atomic<bool> dns_pending{false}, dns_done{false}, dns_ok{false};

    // Records an asynchronous DNS result in storage that remains valid after a waiting attempt times out
    void dns_result(const char *, const ip_addr_t *ip, void *) {
        dns_ok.store(ip != nullptr);
        dns_done.store(true);
        dns_pending.store(false);
    }


    // Starts IPv4 lookup on the lwIP thread and handles cached/immediate results through the same callback
    void dns_start(void *) {
        ip_addr_t address;
        const auto dns_status = dns_gethostbyname_addrtype(HOST, &address, dns_result, nullptr, LWIP_DNS_ADDRTYPE_IPV4);
        if(dns_status != ERR_INPROGRESS)
            dns_result(HOST, dns_status == ERR_OK ? &address:nullptr, nullptr);
    }

    // Starts at most one outstanding lookup and waits cooperatively until it completes, times out, or is paused
    bool resolve(uint64_t deadline) {
        if(!dns_pending.exchange(true)) {
            dns_done.store(false);
            dns_ok.store(false);
            if(tcpip_callback(dns_start, nullptr) != ERR_OK) {
                dns_pending.store(false);
                return false;
            }
        }
        while(!dns_done.load() && ms() < deadline && !paused.load())
            vTaskDelay(pdMS_TO_TICKS(20));
        return dns_done.load() && dns_ok.load();
    }

    struct Response {
        std::string body;
        bool overflow = false;
    };

    // Collects HTTP response chunks up to 1024 bytes, an oversized response is rejected instead of growing memory use
    esp_err_t response_event(esp_http_client_event_t *event) {
        auto *response = static_cast<Response *>(event->user_data);
        if(event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
            if(response->body.size() + event->data_len > 1024) {
                response->overflow = true;
                return ESP_FAIL;
            }
            response->body.append(static_cast<const char *>(event->data), event->data_len);
        }
        return ESP_OK;
    }

    // Separates transport errors, HTTP status, acknowledgment validity, and whether a retry can help
    struct Attempt {
        bool ok             = false, transient = true;
        int code            = 0;
        esp_err_t error     = ESP_FAIL;
        const char *message = "";
    };

    // Sends one HTTPS request within the shared DNS/HTTP deadline
    // An HTTP 200 alone is insufficient: the response must acknowledge exactly the submitted number of records
    Attempt send(const char *request_body, const char *publishable_key, uint64_t deadline) {
        // 1. Resolve the server address within the shared deadline
        Attempt result;
        if(!resolve(deadline)) {
            result.error = ESP_ERR_TIMEOUT;
            return result;
        }

        // 2. Configure the HTTPS client and its response buffer
        Response response;
        esp_http_client_config_t http_config = {};
        http_config.url                      = ENDPOINT;
        http_config.method                   = HTTP_METHOD_POST;
        http_config.timeout_ms               = std::max(1, int(deadline > ms() ? deadline - ms():1));

        // Use the ESP-IDF certificate bundle for server verification and keep redirects disabled for this fixed endpoint
        http_config.crt_bundle_attach     = esp_crt_bundle_attach;
        http_config.disable_auto_redirect = true;
        http_config.is_async              = true;
        http_config.buffer_size_tx        = 512;
        http_config.buffer_size           = 512;
        http_config.event_handler         = response_event;
        http_config.user_data             = &response;

        auto http_client = esp_http_client_init(&http_config);
        if(!http_client) {
            result.error = ESP_ERR_NO_MEM;
            return result;
        }

        // 3. Set the headers and request body
        esp_http_client_set_header(http_client, "apikey", publishable_key);
        esp_http_client_set_header(http_client, "Content-Type", "application/json");
        esp_http_client_set_post_field(http_client, request_body, strlen(request_body));

        // 4. Perform the asynchronous request until completion, timeout, or pause
        do {
            if(ms() >= deadline || paused.load()) {
                result.error = ESP_ERR_TIMEOUT;
                break;
            }

            result.error = esp_http_client_perform(http_client);
            if(result.error == ESP_ERR_HTTP_EAGAIN)
                vTaskDelay(pdMS_TO_TICKS(20));
        } while(result.error == ESP_ERR_HTTP_EAGAIN);

        // 5. Validate the acknowledgment against the submitted record count
        result.code         = esp_http_client_get_status_code(http_client);
        auto acknowledgment = cJSON_Parse(response.body.c_str());
        auto request_json   = cJSON_Parse(request_body);

        // Compare acknowledged rows with the request itself, including an empty array for status-only requests
        const int record_count    = cJSON_GetArraySize(cJSON_GetObjectItem(request_json, "p_records"));
        const auto accepted_count = cJSON_GetObjectItem(acknowledgment, "accepted");
        result.ok = result.error == ESP_OK && !response.overflow && result.code == 200 && cJSON_IsNumber(accepted_count) && accepted_count->valuedouble == record_count;

        // 6. Classify retryable failures and retain recognized server messages
        // Retry timeouts, throttling, server failures, and malformed success acknowledgments, reject other errors persistently
        result.transient = result.code == 0 || result.code == 408 || result.code == 429 || result.code >= 500 || (result.code == 200 && !result.ok);

        auto server_message = cJSON_GetObjectItem(acknowledgment, "message");
        if(cJSON_IsString(server_message))
            for(const char *known : {"Dispositivo não autorizado", "Lote inválido", "Horário inválido", "Contagem inválida", "Endereço local inválido"})
                if(!strcmp(server_message->valuestring, known))
                    result.message = known;

        // 7. Release the parsed JSON objects and HTTP client
        cJSON_Delete(request_json);
        cJSON_Delete(acknowledgment);
        esp_http_client_cleanup(http_client);
        return result;
    }


    using Digest        = std::array<unsigned char, 32>;
    using StatusVersion = std::map<std::string, Digest>;

    // Hashes a serialized status field so unchanged values can be omitted from later requests
    Digest digest(cJSON *item) {
        Digest hash{};
        char *text = cJSON_PrintUnformatted(item);
        if(text) {
            mbedtls_sha256(reinterpret_cast<unsigned char *>(text), strlen(text), hash.data(), 0);
            memset(text, 0, strlen(text));
            cJSON_free(text);
        }
        return hash;
    }

    // Builds only fields differing from the last acknowledged status candidate_status records candidate hashes, not confirmations
    cJSON *status_delta(const StatusVersion &acknowledged_status, StatusVersion &candidate_status) {
        auto full_status = cJSON_CreateObject();
        cJSON_AddBoolToObject(full_status, "storage_ok", park_history_healthy());
        cJSON_AddBoolToObject(full_status, "clock_ok", !park_today().empty());
        cJSON_AddItemToObject(full_status, "operation", park_power_cloud_status());

        if(auto camera = wifi_preview_cloud_access())
            cJSON_AddItemToObject(full_status, "camera", camera);

        auto status_changes = cJSON_CreateObject();

        for(auto *item = full_status->child; item; item = item->next) {
            const auto hash     = digest(item);
            const auto previous = acknowledged_status.find(item->string);
            if(previous == acknowledged_status.end() || previous->second != hash) {
                cJSON_AddItemToObject(status_changes, item->string, cJSON_Duplicate(item, true));
                candidate_status[item->string] = hash;
            }
        }
        cJSON_Delete(full_status);
        return status_changes;
    }

    // Runs the upload loop: detect pending work, respect scheduling, load credentials, send, then acknowledge locally
    // The cycle mutex lets sleep preparation wait for an upload/cursor update to finish
    void task(void *) {
        park::CloudSchedule schedule;
        schedule.collect_at = 1; // One backlog scan per boot

        // Only a successful request advances these status hashes, failed requests must resend changed fields
        StatusVersion acknowledged_status;
        uint32_t last_seen_revision = park_history_revision();
        bool scan_pending_records   = true, failed_while_closing = false, was_closing = false;
        uint64_t next_scan_ms       = 0, wifi_wait_started_ms = 0;

        while(true) {
            // 1. Handle pauses and scheduling resets after configuration changes
            if(paused.load()) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                continue;
            }

            // New credentials require a fresh backlog scan and a complete status announcement
            if(reset_schedule.exchange(false)) {
                schedule.success();
                schedule.collect_at = 1;
                acknowledged_status.clear();
                scan_pending_records = true;
            }

            // 2. Track closing mode and enforce its deadline
            const auto now_ms   = ms();
            const bool shutdown = closing.load();
            if(shutdown && !was_closing)
                scan_pending_records = true;

            was_closing = shutdown;
            if(!shutdown) {
                wifi_wait_started_ms = 0;
                failed_while_closing = false;
            }

            if(shutdown && (close_ready.load() || now_ms >= close_deadline.load())) {
                if(!close_ready.load())
                    finish("budget_exhausted");
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // 3. Check persisted changes, status updates, and scheduling limits
            // Movement/reset records open a collection window, idle samples alone do not trigger periodic uploads
            if(park_history_revision() != last_seen_revision) {
                schedule.pending(now_ms);
                scan_pending_records = true;
                pending.store(true);
                last_seen_revision = park_history_revision();
            }

            // Give the background card mount an initial chance to finish without waiting indefinitely on storage
            if(!park_history_initialized() && now_ms < park::settings::request_ms) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            StatusVersion candidate_status;
            auto status_changes       = status_delta(acknowledged_status, candidate_status);
            const bool status_changed = status_changes->child != nullptr;

            if(schedule.exhausted(shutdown) || (shutdown && schedule.failures && !failed_while_closing)) {
                if(shutdown)
                    finish(schedule.permanent ? "permanent_error":"send_failed");
                cJSON_Delete(status_changes);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            const bool upload_due = schedule.due(now_ms, shutdown);
            if((!upload_due && !(status_changed && !schedule.failures)) || now_ms < next_scan_ms) {
                cJSON_Delete(status_changes);
                next_attempt.store(schedule.failures ? schedule.retry_at:schedule.collect_at);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // 4. Serialize the cycle, load credentials, and verify Wi-Fi
            xSemaphoreTake(cycle_mutex, portMAX_DELAY);
            if(paused.load()) {
                cJSON_Delete(status_changes);
                xSemaphoreGive(cycle_mutex);
                continue;
            }

            // Load credentials inside the serialized cycle. Temporary token/body buffers are cleared after use
            char device_id[40] = {}, device_token[65] = {}, publishable_key[192] = {};
            nvs_handle_t nvs_handle;
            bool credentials_ready = nvs_open("park_cloud", NVS_READONLY, &nvs_handle) == ESP_OK;

            if(credentials_ready) {
                credentials_ready = read_nvs_string(nvs_handle, "device", device_id, sizeof(device_id)) && read_nvs_string(nvs_handle, "token", device_token, sizeof(device_token)) && read_nvs_string(nvs_handle, "apikey", publishable_key, sizeof(publishable_key));
                nvs_close(nvs_handle);
            }

            configured.store(credentials_ready);
            wifi_ap_record_t access_point;
            const bool wifi_connected = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;

            if(!credentials_ready || !wifi_connected) {
                phase.store(!credentials_ready ? "not_configured":"waiting_wifi");
                if(shutdown && !credentials_ready)
                    finish("not_configured");
                else if(shutdown) {
                    if(!wifi_wait_started_ms)
                        wifi_wait_started_ms = now_ms;
                    if(now_ms - wifi_wait_started_ms >= park::settings::request_ms)
                        finish("wifi_unavailable");
                }

                cJSON_Delete(status_changes);
                memset(device_token, 0, sizeof(device_token));
                xSemaphoreGive(cycle_mutex);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            wifi_wait_started_ms = 0;

            // 5. Read pending records and handle busy or idle storage
            ParkCloudBatch batch;

            if(scan_pending_records && upload_due)
                batch = park_history_cloud_batch();
            if(!batch.ok) {
                cJSON_Delete(batch.records);
                batch.records = nullptr;
            }
            const int record_count = cJSON_GetArraySize(batch.records);

            // A busy journal is retried later, it does not mean the queue is empty or successfully drained
            if(!strcmp(batch.error, "sd_busy")) {
                cJSON_Delete(batch.records);
                cJSON_Delete(status_changes);
                xSemaphoreGive(cycle_mutex);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // No records and no status changes means this scan has no request to send
            if(!record_count && !status_changed) {
                phase.store(batch.ok ? "idle":"storage_unavailable");

                if(shutdown)
                    finish(batch.ok ? "drained":"storage_unavailable");

                scan_pending_records = false;
                pending.store(!batch.ok);
                schedule.success();
                cJSON_Delete(batch.records);
                cJSON_Delete(status_changes);
                memset(device_token, 0, sizeof(device_token));
                xSemaphoreGive(cycle_mutex);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // 6. Build and send the request within the available time budget
            auto request = cJSON_CreateObject();
            cJSON_AddStringToObject(request, "p_device", device_id);
            cJSON_AddStringToObject(request, "p_token", device_token);

            // Transfer the records array to request ownership, then clear the batch pointer to avoid double ownership
            cJSON_AddItemToObject(request, "p_records", batch.records ? batch.records:cJSON_CreateArray());
            batch.records = nullptr;
            cJSON_AddItemToObject(request, "p_status", status_changes);
            char *request_body = cJSON_PrintUnformatted(request);
            cJSON_Delete(request);
            phase.store("sending_https");
            syncing.store(true);
            power_monitor_event("cloud_attempt");
            const auto request_deadline_ms = std::min(ms() + park::settings::request_ms, shutdown ? close_deadline.load():UINT64_MAX);

            auto result = request_body ? send(request_body, publishable_key, request_deadline_ms):Attempt{};
            if(request_body) {
                memset(request_body, 0, strlen(request_body));
                cJSON_free(request_body);
            }

            memset(device_token, 0, sizeof(device_token));
            last_http.store(result.code);
            last_error.store(result.error);
            server_error.store(result.message);

            // 7. Acknowledge persisted records only after server confirmation
            // Advance the SD cursor only after server acknowledgment, and only while cursor updates are allowed
            if(result.ok && record_count && !paused.load()) {
                result.ok = park_history_cloud_ack(batch);
                if(!result.ok) {
                    result.transient = false;
                    phase.store("sd_ack_failed");
                }
            }

            // 8. Publish the outcome, update scheduling, and release the cycle lock
            if(result.ok) {
                // Status hashes become acknowledged only after the request and any required SD cursor write both succeed
                for(const auto &field : candidate_status)
                    acknowledged_status[field.first] = field.second;

                last_success.store(time(nullptr));
                ++batches;
                failures.store(0);
                permanent_error.store(false);

                // A status-only ACK must not cancel a pending 10 s collection window
                if(record_count) {
                    schedule.success();
                    schedule.collect_at  = ms() + park::settings::batch_gap_ms;
                    scan_pending_records = true;
                    pending.store(true);
                } else {
                    schedule.failures  = 0;
                    schedule.retry_at  = 0;
                    schedule.permanent = false;
                }

                phase.store("acknowledged");
                power_monitor_event("cloud_ack");
                next_scan_ms = record_count ? ms() + park::settings::batch_gap_ms:0;
            } else {
                schedule.failure(ms(), shutdown, result.transient);
                failed_while_closing = shutdown;
                failures.store(schedule.failures);
                permanent_error.store(schedule.permanent);
                phase.store(result.transient ? "retry_wait":"permanent_error");
                pending.store(true);
                power_monitor_event("cloud_failed", result.transient ? "temporary":"permanent");
            }

            syncing.store(false);
            next_attempt.store(schedule.failures ? schedule.retry_at:schedule.collect_at);
            xSemaphoreGive(cycle_mutex);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// Checks scheduling policy, protects pending SD records from cleanup, and creates the upload worker
bool park_cloud_start() {
    if(!park::cloud_policy_test()) {
        ESP_LOGE("CLOUD", "Scheduling self-check failed");
        return false;
    }
    ESP_LOGI("CLOUD", "Scheduling self-check PASS; collection=10 s, batch=8, request=10 s, close=120 s");
    park_history_cloud_protect();
    cycle_mutex = xSemaphoreCreateMutex();
    return cycle_mutex && xTaskCreate(task, "ParkCloud", 12288, nullptr, 1, &worker) == pdPASS;
}


// Notifies the worker, scheduling rules and network readiness still govern when it can send
void park_cloud_request_sync() {
    if(worker)
        xTaskNotifyGive(worker);
}


// Switches the worker to closing mode using the power manager's absolute monotonic deadline
void park_cloud_prepare_sleep(uint64_t deadline) {
    close_deadline.store(deadline);
    close_result.store("");
    close_ready.store(false);
    closing.store(true);
    if(!worker)
        finish("not_configured");
    else
        park_cloud_request_sync();
}


// Leaves closing mode when activity cancels sleep and wakes the worker for normal operation
void park_cloud_cancel_sleep() {
    closing.store(false);
    close_ready.store(false);
    park_cloud_request_sync();
}


// Reports that closing finished or stopped, inspect sleep_result to distinguish drained data from a failure
bool park_cloud_sleep_ready() {
    return close_ready.load();
}


// Returns the stable reason string describing the latest closing outcome
const char *park_cloud_sleep_result() {
    return close_result.load();
}


// Prevents new requests and asks an in-flight asynchronous attempt to stop
// True means the cycle mutex became available within timeout_ms, paused remains set even on timeout
bool park_cloud_pause(unsigned timeout_ms) {
    paused.store(true);
    if(cycle_mutex && xSemaphoreTake(cycle_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        xSemaphoreGive(cycle_mutex);
        return true;
    }
    return false;
}


// Allows the worker to run again after a cancelled sleep or temporary pause
void park_cloud_resume() {
    paused.store(false);
    park_cloud_request_sync();
}


// Validates device ID/token/key shape and persists credentials. Successful changes reset scheduling and status
// hashes
bool park_cloud_configure(const cJSON *json) {
    auto device_id_json       = cJSON_GetObjectItemCaseSensitive(json, "device_id"),
         token_json           = cJSON_GetObjectItemCaseSensitive(json, "token"),
         publishable_key_json = cJSON_GetObjectItemCaseSensitive(json, "publishable_key");

    if(!cJSON_IsString(device_id_json) || strlen(device_id_json->valuestring) != 36 || !cJSON_IsString(token_json) ||
       strlen(token_json->valuestring) != 64 || !cJSON_IsString(publishable_key_json) || strlen(publishable_key_json->valuestring) >= 192 ||
       strncmp(publishable_key_json->valuestring, "sb_publishable_", 15))
        return false;

    for(const char *character = token_json->valuestring; *character; ++character)
        if(!strchr("0123456789abcdef", *character))
            return false;

    for(const char *character = device_id_json->valuestring; *character; ++character)
        if(!strchr("0123456789abcdef-", *character))
            return false;

    nvs_handle_t nvs_handle;
    if(nvs_open("park_cloud", NVS_READWRITE, &nvs_handle) != ESP_OK)
        return false;

    const bool ok = nvs_set_str(nvs_handle, "device", device_id_json->valuestring) == ESP_OK &&
                    nvs_set_str(nvs_handle, "token", token_json->valuestring) == ESP_OK &&
                    nvs_set_str(nvs_handle, "apikey", publishable_key_json->valuestring) == ESP_OK && nvs_commit(nvs_handle) == ESP_OK;

    nvs_close(nvs_handle);
    if(ok) {
        reset_schedule.store(true);
        park_cloud_request_sync();
    }
    return ok;
}


// Builds local upload diagnostics without exposing stored credentials. The caller owns the returned JSON object
cJSON *park_cloud_status() {
    auto status_json = cJSON_CreateObject();
    cJSON_AddBoolToObject(status_json, "configured", configured.load());
    cJSON_AddBoolToObject(status_json, "syncing", syncing.load());
    cJSON_AddBoolToObject(status_json, "pending", pending.load());
    cJSON_AddBoolToObject(status_json, "permanent_error", permanent_error.load());
    cJSON_AddNumberToObject(status_json, "last_http", last_http.load());
    cJSON_AddStringToObject(status_json, "last_success", park_timestamp(last_success.load()).c_str());
    cJSON_AddStringToObject(status_json, "phase", phase.load());
    cJSON_AddStringToObject(status_json, "error", esp_err_to_name(last_error.load()));
    cJSON_AddStringToObject(status_json, "server_error", server_error.load());
    cJSON_AddStringToObject(status_json, "sleep_result", close_result.load());
    cJSON_AddNumberToObject(status_json, "batches", batches.load());
    cJSON_AddNumberToObject(status_json, "failures", failures.load());
    cJSON_AddNumberToObject(status_json, "next_attempt_ms", next_attempt.load() > ms() ? next_attempt.load() - ms():0);
    cJSON_AddNumberToObject(status_json, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(status_json, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return status_json;
}
