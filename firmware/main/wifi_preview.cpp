// Manages Wi-Fi, serial configuration, and authenticated access to the camera and local dashboard

#include "wifi_preview.hpp"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "park_history.hpp"
#include "park_cloud.hpp"
#include "pir_sensor.hpp"
#include "park_power.hpp"
#include "visitor_counter.hpp"
#include <cstdio>
#include <atomic>
#include <cstring>
#include <sys/time.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
    const char *TAG = "WIFI_PREVIEW";

    // Only one encoded frame is retained. The mutex protects its pointer, length, sequence, and update timestamp
    SemaphoreHandle_t frame_mutex = nullptr;
    char *latest_frame_packet     = nullptr;
    size_t latest_frame_length    = 0;
    uint32_t sequence             = 0;
    int64_t last_frame_us         = 0;
    std::atomic<uint32_t> preview_until_ms{0};

    // Camera access is a separate random token, not the router password or the cloud ingestion token
    char camera_access_token[33] = {};
    char authorization[48]       = {};
    bool configured              = false;
    std::atomic<bool> network_sleeping{false}, switching{false};
    esp_timer_handle_t reconnect_timer = nullptr;
    QueueHandle_t wifi_queue           = nullptr;
    bool clock_started                 = false;

    // Linker symbols point into assets embedded by CMake, TEXT assets include a final zero, BINARY assets do not
    extern const uint8_t index_start[] asm("_binary_park_index_start");
    extern const uint8_t index_end[] asm("_binary_park_index_end");
    extern const uint8_t app_start[] asm("_binary_park_app_start");
    extern const uint8_t app_end[] asm("_binary_park_app_end");
    extern const uint8_t monitor_js_start[] asm("_binary_park_monitor_js_start");
    extern const uint8_t monitor_js_end[] asm("_binary_park_monitor_js_end");
    extern const uint8_t monitor_css_start[] asm("_binary_park_monitor_css_start");
    extern const uint8_t monitor_css_end[] asm("_binary_park_monitor_css_end");
    extern const uint8_t style_start[] asm("_binary_park_style_start");
    extern const uint8_t style_end[] asm("_binary_park_style_end");
    extern const uint8_t foliage_start[] asm("_binary_park_foliage_start");
    extern const uint8_t foliage_end[] asm("_binary_park_foliage_end");
    extern const uint8_t font_start[] asm("_binary_park_font_start");
    extern const uint8_t font_end[] asm("_binary_park_font_end");
    extern const uint8_t favicon_start[] asm("_binary_park_favicon_start");
    extern const uint8_t favicon_end[] asm("_binary_park_favicon_end");

    // Serves static dashboard assets embedded in the firmware. The API handlers separately enforce authorization
    esp_err_t service_handler(httpd_req_t *request) {
        // The dashboard is embedded in the application: no notebook or external assets
        const uint8_t *begin = index_start, *end = index_end;
        const char *type     = "text/html; charset=utf-8";
        bool binary          = false;

        if(!strcmp(request->uri, "/favicon.svg")) {
            begin = favicon_start;
            end   = favicon_end;
            type  = "image/svg+xml";
        }
        if(!strcmp(request->uri, "/app.js")) {
            begin = app_start;
            end   = app_end;
            type  = "text/javascript; charset=utf-8";
        }
        if(!strcmp(request->uri, "/style.css")) {
            begin = style_start;
            end   = style_end;
            type  = "text/css; charset=utf-8";
        }
        if(!strcmp(request->uri, "/monitor.js")) {
            begin = monitor_js_start;
            end   = monitor_js_end;
            type  = "text/javascript; charset=utf-8";
        }
        if(!strcmp(request->uri, "/monitor.css")) {
            begin = monitor_css_start;
            end   = monitor_css_end;
            type  = "text/css; charset=utf-8";
        }
        if(!strcmp(request->uri, "/folhagem-v2.webp")) {
            begin  = foliage_start;
            end    = foliage_end;
            type   = "image/webp";
            binary = true;
        }
        if(!strcmp(request->uri, "/source-sans-3-v1.woff2")) {
            begin  = font_start;
            end    = font_end;
            type   = "font/woff2";
            binary = true;
        }

        httpd_resp_set_type(request, type);
        httpd_resp_set_hdr(request, "Cache-Control", binary ? "public, max-age=31536000, immutable":"no-store");

        // Restrict asset behavior and referrer leakage, dynamic API responses separately disable caching
        httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
        httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
        httpd_resp_set_hdr(request, "Content-Security-Policy",
                           "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; connect-src "
                           "'self'; frame-ancestors 'none'; base-uri 'none'");

        // Remove the embedded terminator for text responses, binary assets must retain every byte
        return httpd_resp_send(request, reinterpret_cast<const char *>(begin), end - begin - (binary ? 0:1));
    }


    // Checks the camera Bearer token and rejects non-GET requests during sleep preparation
    // On failure it has already sent the HTTP response, so callers return without sending another
    bool authorized(httpd_req_t *request) {
        char supplied[64] = {};
        if(httpd_req_get_hdr_value_str(request, "Authorization", supplied, sizeof(supplied)) != ESP_OK ||
           strcmp(supplied, authorization) != 0) {
            httpd_resp_set_status(request, "401 Unauthorized");
            httpd_resp_set_type(request, "application/json");
            httpd_resp_sendstr(request, "{\"error\":\"Use o link completo exibido no terminal de configuracao.\"}");
            return false;
        }
        if(park_power_preparing() && request->method != HTTP_GET) {
            httpd_resp_set_status(request, "503 Service Unavailable");
            httpd_resp_set_type(request, "application/json");
            httpd_resp_sendstr(request, "{\"error\":\"Preparando sono; tente novamente.\"}");
            return false;
        }
        return true;
    }

    // Validates a calibration POST, applies line settings, and returns the current count snapshot
    esp_err_t counter_handler(httpd_req_t *request) {
        if(!authorized(request))
            return ESP_OK;

        httpd_resp_set_type(request, "application/json; charset=utf-8");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        if(request->method == HTTP_POST) {
            if(request->content_len <= 0 || request->content_len > 256)
                return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid body size");

            char body[257]           = {};
            int total_received_bytes = 0;

            while(total_received_bytes < request->content_len) {
                int received_bytes = httpd_req_recv(request, body + total_received_bytes, request->content_len - total_received_bytes);
                if(received_bytes <= 0)
                    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Incomplete body");
                total_received_bytes += received_bytes;
            }
            cJSON *json     = cJSON_Parse(body);
            auto first      = cJSON_GetObjectItemCaseSensitive(json, "first");
            auto second     = cJSON_GetObjectItemCaseSensitive(json, "second");
            auto horizontal = cJSON_GetObjectItemCaseSensitive(json, "horizontal");
            auto reverse    = cJSON_GetObjectItemCaseSensitive(json, "reverse");
            bool ok         = cJSON_IsNumber(first) && cJSON_IsNumber(second) && cJSON_IsBool(horizontal) && cJSON_IsBool(reverse);
            if(ok)
                ok = visitor_counter_configure({static_cast<float>(first->valuedouble),
                                                static_cast<float>(second->valuedouble), cJSON_IsTrue(horizontal) != 0,
                                                cJSON_IsTrue(reverse) != 0});
            cJSON_Delete(json);
            if(!ok)
                return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid lines or counter unavailable");
        }
        const auto json = visitor_counter_json(visitor_counter_snapshot());
        return httpd_resp_send(request, json.c_str(), json.size());
    }

    // Renews preview demand for three seconds and returns the latest encoded frame plus current count status
    // Copies shared frame data under the mutex, then sends over the network after releasing it
    esp_err_t frame_handler(httpd_req_t *request) {
        // 1. Authenticate the frame request
        if(!authorized(request))
            return ESP_OK;

        // 2. Renew the short viewer-demand deadline
        // Only this authenticated frame request renews viewer demand, status and monitor polling do not
        preview_until_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000) + 3000, std::memory_order_relaxed);

        // 3. Copy the latest frame and its metadata while holding the frame mutex
        char *frame_snapshot = nullptr;
        size_t frame_length;
        uint32_t frame_sequence_snapshot;
        int64_t frame_updated_us;
        xSemaphoreTake(frame_mutex, portMAX_DELAY);
        frame_length            = latest_frame_length;
        frame_sequence_snapshot = sequence;
        frame_updated_us        = last_frame_us;

        if(latest_frame_packet) {
            frame_snapshot = static_cast<char *>(heap_caps_malloc(frame_length, MALLOC_CAP_SPIRAM));
            if(frame_snapshot)
                memcpy(frame_snapshot, latest_frame_packet, frame_length);
        }
        xSemaphoreGive(frame_mutex);
        if(frame_length && !frame_snapshot)
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Sem memoria");

        // 4. Build the response prefix and send the frame in separate chunks
        char prefix[256];
        const auto counter_json = visitor_counter_json(visitor_counter_snapshot());
        snprintf(prefix, sizeof(prefix),
                 "{\"received\":%lu,\"age\":%.2f,\"status\":\"Recebendo imagens por Wi-Fi\",\"logs\":[],\"counter\":",
                 static_cast<unsigned long>(frame_sequence_snapshot), frame_updated_us ? (esp_timer_get_time() - frame_updated_us) / 1000000.0:0.0);

        const std::string response_prefix = std::string(prefix) + counter_json + ",\"frame\":";
        httpd_resp_set_type(request, "application/json; charset=utf-8");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");

        // Stream the prefix and frame separately to avoid allocating another full response-sized buffer
        esp_err_t send_status = httpd_resp_send_chunk(request, response_prefix.c_str(), response_prefix.size());

        if(send_status == ESP_OK)
            send_status = httpd_resp_send_chunk(request, frame_snapshot ? frame_snapshot:"null", frame_snapshot ? frame_length:4);
        if(send_status == ESP_OK)
            send_status = httpd_resp_send_chunk(request, "}", 1);
        if(send_status == ESP_OK)
            send_status = httpd_resp_send_chunk(request, nullptr, 0);

        // 5. Release the copied frame after sending or a transport failure
        heap_caps_free(frame_snapshot);
        return send_status;
    }

    // Timer callback that reconnects only when credentials exist and the network is not preparing for sleep
    void reconnect(void *) {
        if(configured && !network_sleeping.load())
            esp_wifi_connect();
    }

    // Takes ownership of the JSON response, sends its serialized content, and frees both the tree and temporary string
    esp_err_t send_json(httpd_req_t *request, cJSON *response_json) {
        char *serialized_json = cJSON_PrintUnformatted(response_json);
        cJSON_Delete(response_json);
        if(!serialized_json)
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Sem memoria");

        httpd_resp_set_type(request, "application/json; charset=utf-8");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        const auto status = httpd_resp_sendstr(request, serialized_json);
        cJSON_free(serialized_json);
        return status;
    }

    // Sends a consistent HTTP 400 JSON error through the shared ownership-aware response helper
    esp_err_t api_error(httpd_req_t *request, const char *text) {
        httpd_resp_set_status(request, "400 Bad Request");
        auto response_json = cJSON_CreateObject();
        cJSON_AddStringToObject(response_json, "error", text);
        return send_json(request, response_json);
    }

    // Reads a complete request of at most 512 bytes, parses it, and clears the temporary body
    // The caller owns the returned JSON tree, nullptr covers invalid size, incomplete reception, or invalid JSON
    cJSON *body_json(httpd_req_t *request) {
        if(request->content_len <= 0 || request->content_len > 512)
            return nullptr;
        char body[513]           = {};
        int total_received_bytes = 0;
        while(total_received_bytes < request->content_len) {
            int received_bytes = httpd_req_recv(request, body + total_received_bytes, request->content_len - total_received_bytes);
            if(received_bytes <= 0)
                return nullptr;
            total_received_bytes += received_bytes;
        }
        auto request_json = cJSON_Parse(body);
        memset(body, 0, sizeof(body));
        return request_json;
    }

    // Applies a queued Wi-Fi change after the HTTP acknowledgment has time to leave the old connection
    void wifi_switch_task(void *) {
        wifi_config_t wifi_config;
        while(true) {
            // 1. Wait for the next queued Wi-Fi configuration
            if(xQueueReceive(wifi_queue, &wifi_config, portMAX_DELAY) != pdTRUE)
                continue;

            // 2. Mark the switch active and discard it if network shutdown has started
            switching.store(true);
            if(network_sleeping.load()) {
                memset(wifi_config.sta.password, 0, sizeof(wifi_config.sta.password));
                switching.store(false);
                continue;
            }

            // 3. Allow the HTTP acknowledgment to leave before disconnecting
            // Delay disconnecting so the browser can receive the response acknowledging its Wi-Fi change
            vTaskDelay(pdMS_TO_TICKS(1200));

            // 4. Stop reconnection and apply the new Wi-Fi configuration
            configured = false;
            esp_timer_stop(reconnect_timer);
            esp_wifi_disconnect();
            const auto config_status = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

            // 5. Clear the temporary password, reconnect on success, and finish the switch
            memset(wifi_config.sta.password, 0, sizeof(wifi_config.sta.password));
            configured = config_status == ESP_OK;
            if(configured)
                esp_wifi_connect();
            switching.store(false);
        }
    }

    // Validates SSID/password, queues one network change, and clears temporary password copies before replying
    esp_err_t wifi_handler(httpd_req_t *request) {
        if(!authorized(request))
            return ESP_OK;
        if(park_power_preparing())
            return api_error(request, "Placa preparando sono; tente novamente.");

        auto request_json = body_json(request);
        auto ssid_json    = cJSON_GetObjectItem(request_json, "ssid"), password_json = cJSON_GetObjectItem(request_json, "password");
        bool valid        = cJSON_IsString(ssid_json) && cJSON_IsString(password_json) && strlen(ssid_json->valuestring) > 0 &&
                     strlen(ssid_json->valuestring) <= 32 && strlen(password_json->valuestring) >= 8 &&
                     strlen(password_json->valuestring) <= 63;

        wifi_config_t wifi_config = {};

        if(valid) {
            memcpy(wifi_config.sta.ssid, ssid_json->valuestring, strlen(ssid_json->valuestring));
            memcpy(wifi_config.sta.password, password_json->valuestring, strlen(password_json->valuestring));
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        }

        if(cJSON_IsString(password_json))
            memset(password_json->valuestring, 0, strlen(password_json->valuestring));
        cJSON_Delete(request_json);

        if(!valid)
            return api_error(request, "Use SSID de até 32 bytes e senha de 8 a 63 bytes, em rede 2,4 GHz WPA2.");

        bool queued = xQueueSend(wifi_queue, &wifi_config, 0) == pdTRUE;
        memset(wifi_config.sta.password, 0, sizeof(wifi_config.sta.password));
        if(!queued)
            return api_error(request, "Uma troca de rede já está em andamento. Aguarde.");

        auto response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "accepted", true);
        return send_json(request, response);
    }

    // Uses a valid supplied epoch only while the device clock is invalid, an existing clock is left unchanged
    bool set_clock_if_missing(const cJSON *request_json) {
        const auto seconds = cJSON_GetObjectItemCaseSensitive(request_json, "epoch");
        const bool valid   = cJSON_IsNumber(seconds) && seconds->valuedouble >= 1704067200 && seconds->valuedouble < 4102444800;

        if(!valid)
            return false;

        if(park_today().empty()) {
            timeval clock_time = {};
            clock_time.tv_sec  = static_cast<time_t>(seconds->valuedouble);
            return settimeofday(&clock_time, nullptr) == 0;
        }
        return true;
    }

    // Accepts an authenticated clock initialization request and returns its result
    esp_err_t time_handler(httpd_req_t *request) {
        if(!authorized(request))
            return ESP_OK;

        auto request_json = body_json(request);
        const bool valid  = set_clock_if_missing(request_json);
        cJSON_Delete(request_json);

        if(!valid)
            return api_error(request, "Horário inválido.");

        auto response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "ok", true);
        return send_json(request, response);
    }

    // Validates and commits the park name and entrance label to NVS, these are dashboard labels, not Wi-Fi credentials
    esp_err_t settings_handler(httpd_req_t *request) {
        if(!authorized(request))
            return ESP_OK;

        auto request_json   = body_json(request);
        auto park_name_json = cJSON_GetObjectItem(request_json, "name"), access_label_json = cJSON_GetObjectItem(request_json, "access");
        bool valid          = cJSON_IsString(park_name_json) && cJSON_IsString(access_label_json) && strlen(park_name_json->valuestring) > 0 &&
                     strlen(park_name_json->valuestring) <= 240 && strlen(access_label_json->valuestring) > 0 &&
                     strlen(access_label_json->valuestring) <= 240;

        nvs_handle_t nvs_handle;
        esp_err_t status = ESP_FAIL;

        if(valid && nvs_open("park_ui", NVS_READWRITE, &nvs_handle) == ESP_OK) {
            status = nvs_set_str(nvs_handle, "name", park_name_json->valuestring);

            if(status == ESP_OK)
                status = nvs_set_str(nvs_handle, "point", access_label_json->valuestring);
            if(status == ESP_OK)
                status = nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }
        cJSON_Delete(request_json);

        if(status != ESP_OK)
            return api_error(request, "Não foi possível salvar a identificação na ESP.");

        auto response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "ok", true);
        return send_json(request, response);
    }

    // Reads start/end query dates, defaults to today, and validates the report interval
    // An empty default range is allowed while the clock is not ready so status queries can still work
    bool requested_range(httpd_req_t *request, std::string &start, std::string &end) {
        start             = end = park_today();
        char query[128]   = {}, start_buffer[16] = {}, end_buffer[16] = {};
        const auto length = httpd_req_get_url_query_len(request);

        if(length >= sizeof(query))
            return false;
        if(length) {
            httpd_req_get_url_query_str(request, query, sizeof(query));
            if(httpd_query_key_value(query, "start", start_buffer, sizeof(start_buffer)) != ESP_OK ||
               httpd_query_key_value(query, "end", end_buffer, sizeof(end_buffer)) != ESP_OK)
                return false;
            start = start_buffer;
            end   = end_buffer;
        }
        return (start.empty() && end.empty() && !length) || park_history_range(start, end);
    }

    // Separates cleanup preview from confirmed deletion and passes the plan token to the history module
    esp_err_t cleanup_handler(httpd_req_t *request) {
        if(!authorized(request))
            return ESP_OK;

        auto body          = body_json(request);
        auto before        = cJSON_GetObjectItem(body, "before");
        auto action        = cJSON_GetObjectItem(body, "action");
        auto confirm       = cJSON_GetObjectItem(body, "confirm");
        auto plan          = cJSON_GetObjectItem(body, "plan");
        const bool preview = cJSON_IsString(action) && strcmp(action->valuestring, "preview") == 0;
        const bool erase   = cJSON_IsString(action) && strcmp(action->valuestring, "delete") == 0 &&
                             cJSON_IsString(confirm) && strcmp(confirm->valuestring, "APAGAR") == 0 &&
                             cJSON_IsString(plan);

        if(!cJSON_IsString(before) || (!preview && !erase)) {
            cJSON_Delete(body);
            return api_error(request, "Consulte o período e confirme com APAGAR antes de limpar.");
        }

        auto result = park_history_cleanup(before->valuestring, erase, erase ? plan->valuestring:"");
        cJSON_Delete(body);

        // A history-level validation/storage failure becomes an HTTP error while retaining its JSON details
        if(cJSON_GetObjectItem(result, "error"))
            httpd_resp_set_status(request, "400 Bad Request");
        return send_json(request, result);
    }


    // Combines identification, counter, network, storage, cloud, and power state, includes a report only when requested
    esp_err_t state_response(httpd_req_t *request, bool include_report) {
        if(!authorized(request))
            return ESP_OK;

        std::string start, end;

        if(include_report && !requested_range(request, start, end))
            return api_error(request, "Escolha até 366 dias válidos, sem datas futuras.");

        auto response_json = cJSON_CreateObject();
        cJSON_AddStringToObject(response_json, "today", park_today().c_str());
        cJSON_AddBoolToObject(response_json, "admin", true);
        auto device         = cJSON_AddObjectToObject(response_json, "device");
        char park_name[241] = "Parque", access_label[241] = "Acesso principal";
        nvs_handle_t nvs_handle;

        if(nvs_open("park_ui", NVS_READONLY, &nvs_handle) == ESP_OK) {
            size_t buffer_size = sizeof(park_name);
            nvs_get_str(nvs_handle, "name", park_name, &buffer_size);
            buffer_size = sizeof(access_label);
            nvs_get_str(nvs_handle, "point", access_label, &buffer_size);
            nvs_close(nvs_handle);
        }

        cJSON_AddStringToObject(device, "name", park_name);
        cJSON_AddStringToObject(device, "access", access_label);
        cJSON_AddBoolToObject(device, "configured", true);
        cJSON_AddBoolToObject(device, "connected", true);
        cJSON_AddStringToObject(device, "last_seen", park_timestamp(time(nullptr)).c_str());
        cJSON_AddStringToObject(device, "error", "");
        auto counter_json = visitor_counter_json(visitor_counter_snapshot());
        cJSON_AddItemToObject(device, "counter", cJSON_Parse(counter_json.c_str()));
        esp_netif_ip_info_t ip_info = {};

        auto network_interface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if(network_interface)
            esp_netif_get_ip_info(network_interface, &ip_info);

        char address[48];
        snprintf(address, sizeof(address), "http://" IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(device, "address", address);
        auto network                  = cJSON_AddObjectToObject(response_json, "network");
        auto wifi                     = cJSON_AddObjectToObject(network, "wifi");
        wifi_ap_record_t access_point = {};
        bool connected                = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
        cJSON_AddBoolToObject(wifi, "connected", connected);
        cJSON_AddStringToObject(wifi, "ssid", connected ? reinterpret_cast<char *>(access_point.ssid):"");
        cJSON_AddNumberToObject(wifi, "rssi", connected ? access_point.rssi:0);
        cJSON_AddBoolToObject(wifi, "busy", wifi_queue && uxQueueMessagesWaiting(wifi_queue) > 0);
        cJSON_AddItemToObject(response_json, "storage", park_history_status());
        cJSON_AddItemToObject(response_json, "cloud", park_cloud_status());
        cJSON_AddItemToObject(response_json, "power", park_power_status());

        if(include_report)
            cJSON_AddItemToObject(response_json, "report", park_history_report(start, end));
        return send_json(request, response_json);
    }

    // Returns full dashboard state, including the selected historical report
    esp_err_t state_handler(httpd_req_t *request) {
        return state_response(request, true);
    }

    // Returns operational dashboard state without scanning historical report files
    esp_err_t local_handler(httpd_req_t *request) {
        return state_response(request, false);
    }

    // Optionally changes the operating mode, then returns field-monitor telemetry without requesting camera frames
    esp_err_t monitor_handler(httpd_req_t *request) {
        if(!authorized(request))
            return ESP_OK;
        if(request->method == HTTP_POST) {
            auto body     = body_json(request);
            const bool ok = park_power_configure(body);
            cJSON_Delete(body);
            if(!ok)
                return api_error(request, "Não foi possível alterar o modo. Aguarde a ESP ficar disponível.");
        }
        // No frames, NVS reads or SD queries: monitoring must stay lightweight and
        // must never renew the presence timeout or start the camera
        return send_json(request, park_power_status());
    }

    // Validates a dated interval and returns the daily CSV as a browser download
    esp_err_t export_handler(httpd_req_t *request) {
        if(!authorized(request))
            return ESP_OK;

        std::string start, end;
        if(!requested_range(request, start, end) || start.empty())
            return api_error(request, "Selecione um período válido.");

        auto csv = park_history_csv(start, end);
        httpd_resp_set_type(request, "text/csv; charset=utf-8");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        httpd_resp_set_hdr(request, "Content-Disposition", "attachment; filename=visitacao.csv");
        return httpd_resp_send(request, csv.c_str(), csv.size());
    }

    // Handles connection events, schedules reconnects, and starts clock synchronization once an IP address is obtained
    void network_event(void *, esp_event_base_t event_base, int32_t event_id, void *event_data) {
        if(network_sleeping.load())
            return;
        if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START && configured)
            esp_wifi_connect();
        if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED && configured) {
            auto event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
            ESP_LOGW(TAG, "Wi-Fi disconnected (reason %u); retry in 3 seconds", event->reason);
            esp_timer_stop(reconnect_timer);
            esp_timer_start_once(reconnect_timer, 3000000);
        }
        if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            esp_timer_stop(reconnect_timer);
            auto event = static_cast<ip_event_got_ip_t *>(event_data);
            // Initialize SNTP once, subsequent reconnects reuse it instead of creating duplicate synchronization state
            if(!clock_started) {
                esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                esp_sntp_setservername(0, "pool.ntp.org");
                esp_sntp_init();
                clock_started = true;
            }
            // This is an access key for this camera, never the router password
            printf("\n@WIFI_STATUS {\"connected\":true,\"url\":\"http://" IPSTR "/#%s\"}\n", IP2STR(&event->ip_info.ip), camera_access_token);
            fflush(stdout);
        }
    }

    // Reads newline-delimited USB/UART commands for power, clock, Wi-Fi, cloud, and diagnostics
    // An oversized command is discarded through its newline, temporary command/secret buffers are cleared after use
    void provision_task(void *) {
        char command_line[1024];
        size_t command_length = 0;
        bool overflow         = false;
        while(true) {
            // 1. Read the next serial character
            uint8_t received_char;
            if(uart_read_bytes(UART_NUM_0, &received_char, 1, pdMS_TO_TICKS(500)) != 1)
                continue;

            // 2. Accumulate a full command and reject oversized input
            // Accumulate one complete line. Once full, ignore the rest of that line rather than accepting a truncated command
            if(received_char != '\n') {
                if(command_length < sizeof(command_line) - 1 && !overflow)
                    command_line[command_length++] = static_cast<char>(received_char);
                else
                    overflow = true;
                continue;
            }
            command_line[command_length] = 0;

            // 3. Dispatch the command and send its response, keeping power commands before the busy check
            // Dispatch complete commands. Power status/mode requests are handled before the preparation busy response
            if(!overflow && strcmp(command_line, "@POWER_STATUS") == 0) {
                auto json            = park_power_status();
                auto serialized_json = cJSON_PrintUnformatted(json);
                if(serialized_json) {
                    printf("\n@POWER_STATUS %s\n", serialized_json);
                    cJSON_free(serialized_json);
                }
                cJSON_Delete(json);
                fflush(stdout);
            } else if(!overflow && strncmp(command_line, "@POWER ", 7) == 0) {
                auto json     = cJSON_Parse(command_line + 7);
                const bool ok = park_power_configure(json);
                cJSON_Delete(json);
                printf("\n@POWER_CONFIG {\"ok\":%s}\n", ok ? "true":"false");
                fflush(stdout);
            } else if(park_power_preparing()) {
                printf("\n@BUSY {\"retry\":true}\n");
                fflush(stdout);
            } else if(!overflow && strncmp(command_line, "@TIME ", 6) == 0) {
                auto json     = cJSON_Parse(command_line + 6);
                const bool ok = set_clock_if_missing(json);
                cJSON_Delete(json);
                printf("\n@TIME_CONFIG {\"ok\":%s}\n", ok ? "true":"false");
                fflush(stdout);
            } else if(!overflow && strncmp(command_line, "@WIFI ", 6) == 0) {
                cJSON *json              = cJSON_Parse(command_line + 6);
                const auto ssid_json     = cJSON_GetObjectItemCaseSensitive(json, "ssid");
                const auto password_json = cJSON_GetObjectItemCaseSensitive(json, "password");

                if(cJSON_IsString(ssid_json) && cJSON_IsString(password_json) && strlen(ssid_json->valuestring) > 0 &&
                   strlen(ssid_json->valuestring) <= 32 && strlen(password_json->valuestring) >= 8 &&
                   strlen(password_json->valuestring) <= 63) {
                    wifi_config_t wifi_config = {};
                    memcpy(wifi_config.sta.ssid, ssid_json->valuestring, strlen(ssid_json->valuestring));
                    memcpy(wifi_config.sta.password, password_json->valuestring, strlen(password_json->valuestring));
                    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
                    esp_timer_stop(reconnect_timer);
                    configured = false;
                    esp_wifi_disconnect();
                    const esp_err_t status = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

                    // Clear temporary copies, the Wi-Fi stack persists its own configuration in NVS
                    memset(wifi_config.sta.password, 0, sizeof(wifi_config.sta.password));

                    if(status == ESP_OK) {
                        configured = true;
                        esp_wifi_connect();
                        printf("\n@WIFI_STATUS {\"configured\":true}\n");
                    } else
                        printf("\n@WIFI_STATUS {\"error\":\"Falha ao configurar Wi-Fi\"}\n");
                } else
                    printf("\n@WIFI_STATUS {\"error\":\"Use SSID de ate 32 bytes e senha WPA2 de 8 a 63 bytes\"}\n");

                if(cJSON_IsString(password_json))
                    memset(password_json->valuestring, 0, strlen(password_json->valuestring));

                cJSON_Delete(json);
                fflush(stdout);
            } else if(!overflow && strncmp(command_line, "@CLOUD ", 7) == 0) {
                auto json             = cJSON_Parse(command_line + 7);
                const bool ok         = park_cloud_configure(json);
                auto cloud_token_json = cJSON_GetObjectItem(json, "token");

                if(cJSON_IsString(cloud_token_json))
                    memset(cloud_token_json->valuestring, 0, strlen(cloud_token_json->valuestring));

                cJSON_Delete(json);
                printf("\n@CLOUD_CONFIG {\"ok\":%s}\n", ok ? "true":"false");
                fflush(stdout);
            } else if(!overflow && strcmp(command_line, "@PIR_STATUS") == 0) {
                auto json            = pir_sensor_status();
                auto serialized_json = cJSON_PrintUnformatted(json);
                if(serialized_json) {
                    printf("\n@PIR_STATUS %s\n", serialized_json);
                    cJSON_free(serialized_json);
                }

                cJSON_Delete(json);
                fflush(stdout);
            } else if(!overflow && strcmp(command_line, "@CLOUD_STATUS") == 0) {
                auto json            = park_cloud_status();
                auto serialized_json = cJSON_PrintUnformatted(json);
                if(serialized_json) {
                    printf("\n@CLOUD_STATUS %s\n", serialized_json);
                    cJSON_free(serialized_json);
                }
                cJSON_Delete(json);
                fflush(stdout);
            } else if(!overflow && strcmp(command_line, "@WIFI_STATUS") == 0) {
                esp_netif_ip_info_t ip_info = {};
                auto network_interface      = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                wifi_ap_record_t access_point;

                if(network_interface && esp_wifi_sta_get_ap_info(&access_point) == ESP_OK && esp_netif_get_ip_info(network_interface, &ip_info) == ESP_OK &&
                   ip_info.ip.addr) {
                    printf("\n@WIFI_STATUS {\"connected\":true,\"url\":\"http://" IPSTR "/#%s\"}\n", IP2STR(&ip_info.ip),
                           camera_access_token);
                } else
                    printf("\n@WIFI_STATUS {\"connected\":false}\n");
                fflush(stdout);
            }

            // 4. Clear the command buffer and reset reception state
            // Clear the command buffer, including any credentials, before receiving the next line
            memset(command_line, 0, sizeof(command_line));
            command_length = 0;
            overflow       = false;
        }
    }
} // namespace

// Initializes Wi-Fi/NVS, the camera access token, HTTP routes, network switching, and serial provisioning tasks
bool wifi_preview_start() {
    // 1. Create the frame mutex and initialize NVS without erasing saved settings
    frame_mutex = xSemaphoreCreateMutex();
    if(!frame_mutex)
        return false;

    // Do not erase NVS on an initialization error: it can contain user settings
    esp_err_t status = nvs_flash_init();
    if(status != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s; no data was erased", esp_err_to_name(status));
        return false;
    }

    // 2. Initialize the network stack, event loop, and station radio
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if(!esp_netif_create_default_wifi_sta())
        return false;

    // WIFI_INIT_CONFIG_DEFAULT is the standard initializer supplied by ESP-IDF, invoked unchanged
    // Station mode and the network configuration flow below are project-specific
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Enable RF entropy before generating the camera's access key
    ESP_ERROR_CHECK(esp_wifi_start());

    // 3. Restore or generate the persistent camera access key
    nvs_handle_t nvs_handle;
    if(nvs_open("preview", NVS_READWRITE, &nvs_handle) != ESP_OK)
        return false;

    size_t access_token_size = sizeof(camera_access_token);

    // Reuse the saved camera key so links survive reboot, generate and commit one only when no valid key is stored
    if(nvs_get_str(nvs_handle, "access", camera_access_token, &access_token_size) != ESP_OK || strlen(camera_access_token) != 32) {
        uint8_t random_bytes[16];
        esp_fill_random(random_bytes, sizeof(random_bytes));

        for(size_t byte_index = 0; byte_index < sizeof(random_bytes); ++byte_index)
            snprintf(camera_access_token + byte_index * 2, 3, "%02x", random_bytes[byte_index]);

        status = nvs_set_str(nvs_handle, "access", camera_access_token);
        if(status == ESP_OK)
            status = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    if(status != ESP_OK)
        return false;

    snprintf(authorization, sizeof(authorization), "Bearer %s", camera_access_token);

    // 4. Read saved Wi-Fi credentials and configure reconnection and network events
    wifi_config_t saved_wifi_config = {};

    if(esp_wifi_get_config(WIFI_IF_STA, &saved_wifi_config) == ESP_OK)
        configured = saved_wifi_config.sta.ssid[0] != 0;

    memset(saved_wifi_config.sta.password, 0, sizeof(saved_wifi_config.sta.password));
    esp_timer_create_args_t reconnect_timer_config = {};
    reconnect_timer_config.callback                = reconnect;
    reconnect_timer_config.name                    = "wifi_reconnect";
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_config, &reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, nullptr));

    // 5. Configure and start the local HTTP server
    // HTTPD_DEFAULT_CONFIG is the standard initializer supplied by ESP-IDF, invoked unchanged
    // Stack size, handler count, timeouts, and routes below are project-specific settings
    httpd_config_t http_config    = HTTPD_DEFAULT_CONFIG();
    http_config.stack_size        = 12288;
    http_config.max_uri_handlers  = 20;
    http_config.lru_purge_enable  = true;
    http_config.send_wait_timeout = 2;
    http_config.recv_wait_timeout = 2;
    httpd_handle_t server;

    if(httpd_start(&server, &http_config) != ESP_OK)
        return false;

    // 6. Register the root page, embedded assets, and protected API routes in order
    // Static files are public resources, each protected API handler checks its own authorization
    const struct {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    } routes[] = {{"/", HTTP_GET, service_handler},
                  {"/app.js", HTTP_GET, service_handler},
                  {"/favicon.svg", HTTP_GET, service_handler},
                  {"/monitor.js", HTTP_GET, service_handler},
                  {"/monitor.css", HTTP_GET, service_handler},
                  {"/style.css", HTTP_GET, service_handler},
                  {"/folhagem-v2.webp", HTTP_GET, service_handler},
                  {"/source-sans-3-v1.woff2", HTTP_GET, service_handler},
                  {"/api/state", HTTP_GET, state_handler},
                  {"/api/local", HTTP_GET, local_handler},
                  {"/api/monitor", HTTP_GET, monitor_handler},
                  {"/api/monitor", HTTP_POST, monitor_handler},
                  {"/api/frame", HTTP_GET, frame_handler},
                  {"/api/calibration", HTTP_POST, counter_handler},
                  {"/api/settings", HTTP_POST, settings_handler},
                  {"/api/wifi", HTTP_POST, wifi_handler},
                  {"/api/time", HTTP_POST, time_handler},
                  {"/api/history/cleanup", HTTP_POST, cleanup_handler},
                  {"/api/export.csv", HTTP_GET, export_handler}};

    for(const auto &route : routes) {
        httpd_uri_t route_config = {};
        route_config.uri         = route.uri;
        route_config.method      = route.method;
        route_config.handler     = route.handler;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route_config));
    }

    // 7. Start the Wi-Fi switch queue and serial provisioning tasks
    // A one-item queue separates HTTP request handling from changing the connection that carries that request
    wifi_queue = xQueueCreate(1, sizeof(wifi_config_t));
    if(!wifi_queue || xTaskCreate(wifi_switch_task, "WiFiSwitch", 4096, nullptr, 2, nullptr) != pdPASS)
        return false;

    if(!uart_is_driver_installed(UART_NUM_0)) {
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 0, 0, nullptr, 0));
    }

    if(xTaskCreate(provision_task, "WiFiConfig", 4096, nullptr, 2, nullptr) != pdPASS)
        return false;

    // 8. Apply the radio power setting and connect when credentials are available
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if(configured)
        esp_wifi_connect();

    ESP_LOGI(TAG, "Wi-Fi preview ready; configuration available through the local page or USB console (2.4 GHz WPA2)");
    return true;
}

// Marks network shutdown, rejects a concurrent/pending Wi-Fi change, and stops the radio
// Failure clears the shutdown flag so normal reconnection can continue
bool wifi_preview_sleep() {
    network_sleeping.store(true);
    if(switching.load() || (wifi_queue && uxQueueMessagesWaiting(wifi_queue))) {
        network_sleeping.store(false);
        return false;
    }

    if(reconnect_timer)
        esp_timer_stop(reconnect_timer);

    const auto result = esp_wifi_stop();
    if(result != ESP_OK) {
        network_sleeping.store(false);
        return false;
    }
    return true;
}

// Reenables radio startup after cancelled sleep, normal network events resume connection handling
void wifi_preview_wake() {
    network_sleeping.store(false);
    esp_wifi_start();
}

// Builds private camera discovery metadata only for a connected supported private IPv4 address
// This JSON contains an access key and must remain in the authenticated owner-only cloud path
cJSON *wifi_preview_cloud_access() {
    esp_netif_ip_info_t ip_info = {};
    auto network_interface      = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    wifi_ap_record_t access_point;
    if(!network_interface || strlen(camera_access_token) != 32 || esp_wifi_sta_get_ap_info(&access_point) != ESP_OK ||
       esp_netif_get_ip_info(network_interface, &ip_info) != ESP_OK || !ip_info.ip.addr)
        return nullptr;

    // Match the backend's private IPv4 allowlist. Networks with a public/local
    // unsupported address still synchronize counts, just not camera discovery
    const uint8_t first_octet = esp_ip4_addr1(&ip_info.ip), second_octet = esp_ip4_addr2(&ip_info.ip);
    if(!(first_octet == 10 || (first_octet == 172 && second_octet >= 16 && second_octet <= 31) || (first_octet == 192 && second_octet == 168)))
        return nullptr;

    char address[16];
    snprintf(address, sizeof(address), IPSTR, IP2STR(&ip_info.ip));
    auto result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "address", address);
    cJSON_AddStringToObject(result, "access_key", camera_access_token);
    return result;
}

// Tests the short viewer-demand deadline without starting capture or extending the inactivity window
bool wifi_preview_requested() {
    // Signed subtraction handles 32-bit millisecond wrap for this short-lived deadline
    const auto preview_deadline_ms = preview_until_ms.load(std::memory_order_relaxed);
    const auto now_ms              = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    return preview_deadline_ms != 0 && static_cast<int32_t>(preview_deadline_ms - now_ms) > 0;
}

// Takes ownership of a JSON frame packet, replaces the shared latest packet, and frees the previous one
void wifi_preview_publish(char *packet, size_t length) {
    if(!frame_mutex) {
        heap_caps_free(packet);
        return;
    }
    xSemaphoreTake(frame_mutex, portMAX_DELAY);
    auto previous_frame_packet = latest_frame_packet;
    latest_frame_packet        = packet;
    latest_frame_length        = length;
    ++sequence;
    last_frame_us = esp_timer_get_time();
    xSemaphoreGive(frame_mutex);
    // Readers copy the packet while holding the mutex, after replacement no new reader can acquire the old pointer
    heap_caps_free(previous_frame_packet);
}
