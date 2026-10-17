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
SemaphoreHandle_t frame_mutex = nullptr;
char *latest = nullptr;
size_t latest_length = 0;
uint32_t sequence = 0;
int64_t updated = 0;
std::atomic<uint32_t> preview_until_ms{0};
char token[33] = {};
char authorization[48] = {};
bool configured = false;
std::atomic<bool> network_sleeping{false}, switching{false};
esp_timer_handle_t reconnect_timer = nullptr;
QueueHandle_t wifi_queue = nullptr;
bool clock_started = false;
extern const uint8_t index_start[] asm("_binary_park_index_start");
extern const uint8_t index_end[] asm("_binary_park_index_end");
extern const uint8_t app_start[] asm("_binary_park_app_start");
extern const uint8_t app_end[] asm("_binary_park_app_end");
extern const uint8_t style_start[] asm("_binary_park_style_start");
extern const uint8_t style_end[] asm("_binary_park_style_end");
extern const uint8_t foliage_start[] asm("_binary_park_foliage_start");
extern const uint8_t foliage_end[] asm("_binary_park_foliage_end");
extern const uint8_t font_start[] asm("_binary_park_font_start");
extern const uint8_t font_end[] asm("_binary_park_font_end");
esp_err_t service_handler(httpd_req_t *req)
{
    // The dashboard is embedded in the application: no notebook or external assets.
    const uint8_t *begin = index_start, *end = index_end;
    const char *type = "text/html; charset=utf-8";
    bool binary = false;
    if (!strcmp(req->uri, "/app.js")) {
        begin = app_start;
        end = app_end;
        type = "text/javascript; charset=utf-8";
    }
    if (!strcmp(req->uri, "/style.css")) {
        begin = style_start;
        end = style_end;
        type = "text/css; charset=utf-8";
    }
    if (!strcmp(req->uri, "/folhagem-v2.webp")) {
        begin = foliage_start;
        end = foliage_end;
        type = "image/webp";
        binary = true;
    }
    if (!strcmp(req->uri, "/source-sans-3-v1.woff2")) {
        begin = font_start;
        end = font_end;
        type = "font/woff2";
        binary = true;
    }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", binary ? "public, max-age=31536000, immutable" : "no-store");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req,
                       "Content-Security-Policy",
                       "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; connect-src "
                       "'self'; frame-ancestors 'none'; base-uri 'none'");
    return httpd_resp_send(req, reinterpret_cast<const char *>(begin), end - begin - (binary ? 0 : 1));
}

bool authorized(httpd_req_t *req)
{
    char supplied[64] = {};
    if (httpd_req_get_hdr_value_str(req, "Authorization", supplied, sizeof(supplied)) != ESP_OK ||
        strcmp(supplied, authorization) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Use o link completo exibido no terminal de configuracao.\"}");
        return false;
    }
    if (park_power_preparing() && req->method != HTTP_GET) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Preparando sono; tente novamente.\"}");
        return false;
    }
    return true;
}

esp_err_t counter_handler(httpd_req_t *req)
{
    if (!authorized(req))
        return ESP_OK;
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (req->method == HTTP_POST) {
        if (req->content_len <= 0 || req->content_len > 256)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
        char body[257] = {};
        int received = 0;
        while (received < req->content_len) {
            int n = httpd_req_recv(req, body + received, req->content_len - received);
            if (n <= 0)
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete body");
            received += n;
        }
        cJSON *json = cJSON_Parse(body);
        auto first = cJSON_GetObjectItemCaseSensitive(json, "first");
        auto second = cJSON_GetObjectItemCaseSensitive(json, "second");
        auto horizontal = cJSON_GetObjectItemCaseSensitive(json, "horizontal");
        auto reverse = cJSON_GetObjectItemCaseSensitive(json, "reverse");
        bool ok = cJSON_IsNumber(first) && cJSON_IsNumber(second) && cJSON_IsBool(horizontal) && cJSON_IsBool(reverse);
        if (ok)
            ok = visitor_counter_configure({static_cast<float>(first->valuedouble),
                                            static_cast<float>(second->valuedouble),
                                            cJSON_IsTrue(horizontal) != 0,
                                            cJSON_IsTrue(reverse) != 0});
        cJSON_Delete(json);
        if (!ok)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid lines or counter unavailable");
    }
    const auto json = visitor_counter_json(visitor_counter_snapshot());
    return httpd_resp_send(req, json.c_str(), json.size());
}


esp_err_t frame_handler(httpd_req_t *req)
{
    if (!authorized(req))
        return ESP_OK;
    preview_until_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000) + 3000,
                           std::memory_order_relaxed);
    char *snapshot = nullptr;
    size_t length;
    uint32_t count;
    int64_t timestamp;
    xSemaphoreTake(frame_mutex, portMAX_DELAY);
    length = latest_length;
    count = sequence;
    timestamp = updated;
    if (latest) {
        snapshot = static_cast<char *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM));
        if (snapshot)
            memcpy(snapshot, latest, length);
    }
    xSemaphoreGive(frame_mutex);
    if (length && !snapshot)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sem memoria");
    char prefix[256];
    const auto counter_json = visitor_counter_json(visitor_counter_snapshot());
    snprintf(prefix,
             sizeof(prefix),
             "{\"received\":%lu,\"age\":%.2f,\"status\":\"Recebendo imagens por Wi-Fi\",\"logs\":[],\"counter\":",
             static_cast<unsigned long>(count),
             timestamp ? (esp_timer_get_time() - timestamp) / 1000000.0 : 0.0);
    const std::string response_prefix = std::string(prefix) + counter_json + ",\"frame\":";
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t status = httpd_resp_send_chunk(req, response_prefix.c_str(), response_prefix.size());
    if (status == ESP_OK)
        status = httpd_resp_send_chunk(req, snapshot ? snapshot : "null", snapshot ? length : 4);
    if (status == ESP_OK)
        status = httpd_resp_send_chunk(req, "}", 1);
    if (status == ESP_OK)
        status = httpd_resp_send_chunk(req, nullptr, 0);
    heap_caps_free(snapshot);
    return status;
}

void reconnect(void *)
{
    if (configured && !network_sleeping.load())
        esp_wifi_connect();
}

esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *s = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!s)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sem memoria");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const auto status = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    return status;
}
esp_err_t api_error(httpd_req_t *req, const char *text)
{
    httpd_resp_set_status(req, "400 Bad Request");
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", text);
    return send_json(req, j);
}
cJSON *body_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512)
        return nullptr;
    char body[513] = {};
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0)
            return nullptr;
        received += n;
    }
    auto json = cJSON_Parse(body);
    memset(body, 0, sizeof(body));
    return json;
}
void wifi_switch_task(void *)
{
    wifi_config_t config;
    while (true) {
        if (xQueueReceive(wifi_queue, &config, portMAX_DELAY) != pdTRUE)
            continue;
        switching.store(true);
        if (network_sleeping.load()) {
            memset(config.sta.password, 0, sizeof(config.sta.password)); switching.store(false); continue;
        }
        vTaskDelay(pdMS_TO_TICKS(1200));
        configured = false;
        esp_timer_stop(reconnect_timer);
        esp_wifi_disconnect();
        const auto result = esp_wifi_set_config(WIFI_IF_STA, &config);
        memset(config.sta.password, 0, sizeof(config.sta.password));
        configured = result == ESP_OK;
        if (configured)
            esp_wifi_connect();
        switching.store(false);
    }
}
esp_err_t wifi_handler(httpd_req_t *req)
{
    if (!authorized(req))
        return ESP_OK;
    if (park_power_preparing()) return api_error(req, "Placa preparando sono; tente novamente.");
    auto j = body_json(req);
    auto ssid = cJSON_GetObjectItem(j, "ssid"), password = cJSON_GetObjectItem(j, "password");
    bool valid = cJSON_IsString(ssid) && cJSON_IsString(password) && strlen(ssid->valuestring) > 0 &&
        strlen(ssid->valuestring) <= 32 && strlen(password->valuestring) >= 8 && strlen(password->valuestring) <= 63;
    wifi_config_t config = {};
    if (valid) {
        memcpy(config.sta.ssid, ssid->valuestring, strlen(ssid->valuestring));
        memcpy(config.sta.password, password->valuestring, strlen(password->valuestring));
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    if (cJSON_IsString(password))
        memset(password->valuestring, 0, strlen(password->valuestring));
    cJSON_Delete(j);
    if (!valid)
        return api_error(req, "Use SSID de até 32 bytes e senha de 8 a 63 bytes, em rede 2,4 GHz WPA2.");
    bool queued = xQueueSend(wifi_queue, &config, 0) == pdTRUE;
    memset(config.sta.password, 0, sizeof(config.sta.password));
    if (!queued)
        return api_error(req, "Uma troca de rede já está em andamento. Aguarde.");
    auto response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "accepted", true);
    return send_json(req, response);
}
bool set_clock_if_missing(const cJSON *j)
{
    const auto seconds = cJSON_GetObjectItemCaseSensitive(j, "epoch");
    const bool valid = cJSON_IsNumber(seconds) && seconds->valuedouble >= 1704067200 && seconds->valuedouble < 4102444800;
    if (!valid) return false;
    if (park_today().empty()) {
        timeval tv = {};
        tv.tv_sec = static_cast<time_t>(seconds->valuedouble);
        return settimeofday(&tv, nullptr) == 0;
    }
    return true;
}
esp_err_t time_handler(httpd_req_t *req)
{
    if (!authorized(req))
        return ESP_OK;
    auto j = body_json(req);
    const bool valid = set_clock_if_missing(j);
    cJSON_Delete(j);
    if (!valid)
        return api_error(req, "Horário inválido.");
    auto response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    return send_json(req, response);
}
esp_err_t settings_handler(httpd_req_t *req)
{
    if (!authorized(req))
        return ESP_OK;
    auto j = body_json(req);
    auto name = cJSON_GetObjectItem(j, "name"), access = cJSON_GetObjectItem(j, "access");
    bool valid = cJSON_IsString(name) && cJSON_IsString(access) && strlen(name->valuestring) > 0 &&
        strlen(name->valuestring) <= 240 && strlen(access->valuestring) > 0 && strlen(access->valuestring) <= 240;
    nvs_handle_t handle;
    esp_err_t status = ESP_FAIL;
    if (valid && nvs_open("park_ui", NVS_READWRITE, &handle) == ESP_OK) {
        status = nvs_set_str(handle, "name", name->valuestring);
        if (status == ESP_OK)
            status = nvs_set_str(handle, "point", access->valuestring);
        if (status == ESP_OK)
            status = nvs_commit(handle);
        nvs_close(handle);
    }
    cJSON_Delete(j);
    if (status != ESP_OK)
        return api_error(req, "Não foi possível salvar a identificação na ESP.");
    auto response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    return send_json(req, response);
}
bool requested_range(httpd_req_t *req, std::string &start, std::string &end)
{
    start = end = park_today();
    char query[128] = {}, a[16] = {}, b[16] = {};
    const auto length = httpd_req_get_url_query_len(req);
    if (length >= sizeof(query))
        return false;
    if (length) {
        httpd_req_get_url_query_str(req, query, sizeof(query));
        if (httpd_query_key_value(query, "start", a, sizeof(a)) != ESP_OK ||
            httpd_query_key_value(query, "end", b, sizeof(b)) != ESP_OK)
            return false;
        start = a;
        end = b;
    }
    return (start.empty() && end.empty() && !length) || park_history_range(start, end);
}
esp_err_t cleanup_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    auto body = body_json(req);
    auto before = cJSON_GetObjectItem(body, "before");
    auto action = cJSON_GetObjectItem(body, "action");
    auto confirm = cJSON_GetObjectItem(body, "confirm");
    auto plan = cJSON_GetObjectItem(body, "plan");
    const bool preview = cJSON_IsString(action) && strcmp(action->valuestring, "preview") == 0;
    const bool erase = cJSON_IsString(action) && strcmp(action->valuestring, "delete") == 0 &&
        cJSON_IsString(confirm) && strcmp(confirm->valuestring, "APAGAR") == 0 && cJSON_IsString(plan);
    if (!cJSON_IsString(before) || (!preview && !erase)) {
        cJSON_Delete(body);
        return api_error(req, "Consulte o período e confirme com APAGAR antes de limpar.");
    }
    auto result = park_history_cleanup(before->valuestring, erase, erase ? plan->valuestring : "");
    cJSON_Delete(body);
    if (cJSON_GetObjectItem(result, "error")) httpd_resp_set_status(req, "400 Bad Request");
    return send_json(req, result);
}
esp_err_t state_response(httpd_req_t *req, bool include_report)
{
    if (!authorized(req))
        return ESP_OK;
    std::string start, end;
    if (include_report && !requested_range(req, start, end))
        return api_error(req, "Escolha até 366 dias válidos, sem datas futuras.");
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "today", park_today().c_str());
    cJSON_AddBoolToObject(j, "admin", true);
    auto device = cJSON_AddObjectToObject(j, "device");
    char name[241] = "Parque", point[241] = "Acesso principal";
    nvs_handle_t handle;
    if (nvs_open("park_ui", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(name);
        nvs_get_str(handle, "name", name, &size);
        size = sizeof(point);
        nvs_get_str(handle, "point", point, &size);
        nvs_close(handle);
    }
    cJSON_AddStringToObject(device, "name", name);
    cJSON_AddStringToObject(device, "access", point);
    cJSON_AddBoolToObject(device, "configured", true);
    cJSON_AddBoolToObject(device, "connected", true);
    cJSON_AddStringToObject(device, "last_seen", park_timestamp(time(nullptr)).c_str());
    cJSON_AddStringToObject(device, "error", "");
    auto counts = visitor_counter_json(visitor_counter_snapshot());
    cJSON_AddItemToObject(device, "counter", cJSON_Parse(counts.c_str()));
    esp_netif_ip_info_t ip = {};
    auto netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif)
        esp_netif_get_ip_info(netif, &ip);
    char address[48];
    snprintf(address, sizeof(address), "http://" IPSTR, IP2STR(&ip.ip));
    cJSON_AddStringToObject(device, "address", address);
    auto network = cJSON_AddObjectToObject(j, "network");
    auto wifi = cJSON_AddObjectToObject(network, "wifi");
    wifi_ap_record_t ap = {};
    bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    cJSON_AddBoolToObject(wifi, "connected", connected);
    cJSON_AddStringToObject(wifi, "ssid", connected ? reinterpret_cast<char *>(ap.ssid) : "");
    cJSON_AddNumberToObject(wifi, "rssi", connected ? ap.rssi : 0);
    cJSON_AddBoolToObject(wifi, "busy", wifi_queue && uxQueueMessagesWaiting(wifi_queue) > 0);
    cJSON_AddItemToObject(j, "storage", park_history_status());
    cJSON_AddItemToObject(j, "cloud", park_cloud_status());
    cJSON_AddItemToObject(j, "power", park_power_status());
    if (include_report) cJSON_AddItemToObject(j, "report", park_history_report(start, end));
    return send_json(req, j);
}
esp_err_t state_handler(httpd_req_t *req) { return state_response(req, true); }
esp_err_t local_handler(httpd_req_t *req) { return state_response(req, false); }
esp_err_t export_handler(httpd_req_t *req)
{
    if (!authorized(req))
        return ESP_OK;
    std::string start, end;
    if (!requested_range(req, start, end) || start.empty())
        return api_error(req, "Selecione um período válido.");
    auto csv = park_history_csv(start, end);
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=visitacao.csv");
    return httpd_resp_send(req, csv.c_str(), csv.size());
}

void network_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (network_sleeping.load()) return;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && configured)
        esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && configured) {
        auto event = static_cast<wifi_event_sta_disconnected_t *>(data);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %u); retry in 3 seconds", event->reason);
        esp_timer_stop(reconnect_timer);
        esp_timer_start_once(reconnect_timer, 3000000);
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        esp_timer_stop(reconnect_timer);
        auto event = static_cast<ip_event_got_ip_t *>(data);
        if (!clock_started) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            clock_started = true;
        }
        // This is an access key for this camera, never the router password.
        printf("\n@WIFI_STATUS {\"connected\":true,\"url\":\"http://" IPSTR "/#%s\"}\n",
               IP2STR(&event->ip_info.ip),
               token);
        fflush(stdout);
    }
}

void provision_task(void *)
{
    char line[1024];
    size_t used = 0;
    bool overflow = false;
    while (true) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(500)) != 1)
            continue;
        if (ch != '\n') {
            if (used < sizeof(line) - 1 && !overflow)
                line[used++] = static_cast<char>(ch);
            else
                overflow = true;
            continue;
        }
        line[used] = 0;
        if (!overflow && strcmp(line, "@POWER_STATUS") == 0) {
            auto json=park_power_status(); auto output=cJSON_PrintUnformatted(json);
            if(output) {printf("\n@POWER_STATUS %s\n",output);cJSON_free(output);}
            cJSON_Delete(json);fflush(stdout);
        } else if (!overflow && strncmp(line, "@POWER ", 7) == 0) {
            auto json=cJSON_Parse(line+7); const bool ok=park_power_configure(json); cJSON_Delete(json);
            printf("\n@POWER_CONFIG {\"ok\":%s}\n", ok?"true":"false");fflush(stdout);
        } else if (park_power_preparing()) {
            printf("\n@BUSY {\"retry\":true}\n");fflush(stdout);
        } else if (!overflow && strncmp(line, "@TIME ", 6) == 0) {
            auto json=cJSON_Parse(line+6); const bool ok=set_clock_if_missing(json); cJSON_Delete(json);
            printf("\n@TIME_CONFIG {\"ok\":%s}\n", ok?"true":"false");fflush(stdout);
        } else if (!overflow && strncmp(line, "@WIFI ", 6) == 0) {
            cJSON *json = cJSON_Parse(line + 6);
            const auto ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
            const auto password = cJSON_GetObjectItemCaseSensitive(json, "password");
            if (cJSON_IsString(ssid) && cJSON_IsString(password) && strlen(ssid->valuestring) > 0 &&
                strlen(ssid->valuestring) <= 32 && strlen(password->valuestring) >= 8 &&
                strlen(password->valuestring) <= 63) {
                wifi_config_t config = {};
                memcpy(config.sta.ssid, ssid->valuestring, strlen(ssid->valuestring));
                memcpy(config.sta.password, password->valuestring, strlen(password->valuestring));
                config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
                esp_timer_stop(reconnect_timer);
                configured = false;
                esp_wifi_disconnect();
                const esp_err_t status = esp_wifi_set_config(WIFI_IF_STA, &config);
                // Clear temporary copies; the Wi-Fi stack persists its own configuration in NVS.
                memset(config.sta.password, 0, sizeof(config.sta.password));
                if (status == ESP_OK) {
                    configured = true;
                    esp_wifi_connect();
                    printf("\n@WIFI_STATUS {\"configured\":true}\n");
                } else
                    printf("\n@WIFI_STATUS {\"error\":\"Falha ao configurar Wi-Fi\"}\n");
            } else
                printf("\n@WIFI_STATUS {\"error\":\"Use SSID de ate 32 bytes e senha WPA2 de 8 a 63 bytes\"}\n");
            if (cJSON_IsString(password))
                memset(password->valuestring, 0, strlen(password->valuestring));
            cJSON_Delete(json);
            fflush(stdout);
        } else if (!overflow && strncmp(line,"@CLOUD ",7)==0) {
            auto json=cJSON_Parse(line+7);
            const bool ok=park_cloud_configure(json);
            auto secret=cJSON_GetObjectItem(json,"token");
            if(cJSON_IsString(secret)) memset(secret->valuestring,0,strlen(secret->valuestring));
            cJSON_Delete(json);
            printf("\n@CLOUD_CONFIG {\"ok\":%s}\n",ok?"true":"false");fflush(stdout);
        } else if (!overflow && strcmp(line,"@PIR_STATUS")==0) {
            auto json=pir_sensor_status();auto output=cJSON_PrintUnformatted(json);
            if(output) {printf("\n@PIR_STATUS %s\n",output);cJSON_free(output);}
            cJSON_Delete(json);fflush(stdout);
        } else if (!overflow && strcmp(line,"@CLOUD_STATUS")==0) {
            auto json=park_cloud_status();auto output=cJSON_PrintUnformatted(json);
            if(output) {printf("\n@CLOUD_STATUS %s\n",output);cJSON_free(output);}
            cJSON_Delete(json);fflush(stdout);
        } else if (!overflow && strcmp(line, "@WIFI_STATUS") == 0) {
            esp_netif_ip_info_t ip = {};
            auto netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            wifi_ap_record_t ap;
            if (netif && esp_wifi_sta_get_ap_info(&ap) == ESP_OK && esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
                ip.ip.addr) {
                printf("\n@WIFI_STATUS {\"connected\":true,\"url\":\"http://" IPSTR "/#%s\"}\n", IP2STR(&ip.ip), token);
            } else
                printf("\n@WIFI_STATUS {\"connected\":false}\n");
            fflush(stdout);
        }
        memset(line, 0, sizeof(line));
        used = 0;
        overflow = false;
    }
}
} // namespace

bool wifi_preview_start()
{
    frame_mutex = xSemaphoreCreateMutex();
    if (!frame_mutex)
        return false;
    // Do not erase NVS on an initialization error: it can contain user settings.
    esp_err_t status = nvs_flash_init();
    if (status != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s; no data was erased", esp_err_to_name(status));
        return false;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (!esp_netif_create_default_wifi_sta())
        return false;
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Enable RF entropy before generating the camera's access key.
    ESP_ERROR_CHECK(esp_wifi_start());
    nvs_handle_t storage;
    if (nvs_open("preview", NVS_READWRITE, &storage) != ESP_OK)
        return false;
    size_t token_size = sizeof(token);
    if (nvs_get_str(storage, "access", token, &token_size) != ESP_OK || strlen(token) != 32) {
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        for (size_t i = 0; i < sizeof(random); ++i) snprintf(token + i * 2, 3, "%02x", random[i]);
        status = nvs_set_str(storage, "access", token);
        if (status == ESP_OK)
            status = nvs_commit(storage);
    }
    nvs_close(storage);
    if (status != ESP_OK)
        return false;
    snprintf(authorization, sizeof(authorization), "Bearer %s", token);
    wifi_config_t existing = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &existing) == ESP_OK)
        configured = existing.sta.ssid[0] != 0;
    memset(existing.sta.password, 0, sizeof(existing.sta.password));
    esp_timer_create_args_t timer = {};
    timer.callback = reconnect;
    timer.name = "wifi_reconnect";
    ESP_ERROR_CHECK(esp_timer_create(&timer, &reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, nullptr));
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 12288;
    http.max_uri_handlers = 16;
    http.lru_purge_enable = true;
    http.send_wait_timeout = 2;
    http.recv_wait_timeout = 2;
    httpd_handle_t server;
    if (httpd_start(&server, &http) != ESP_OK)
        return false;
    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = service_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    const struct {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    } routes[] = {{"/app.js", HTTP_GET, service_handler},
                  {"/style.css", HTTP_GET, service_handler},
                  {"/folhagem-v2.webp", HTTP_GET, service_handler},
                  {"/source-sans-3-v1.woff2", HTTP_GET, service_handler},
                  {"/api/state", HTTP_GET, state_handler},
                  {"/api/local", HTTP_GET, local_handler},
                  {"/api/frame", HTTP_GET, frame_handler},
                  {"/api/calibration", HTTP_POST, counter_handler},
                  {"/api/settings", HTTP_POST, settings_handler},
                  {"/api/wifi", HTTP_POST, wifi_handler},
                  {"/api/time", HTTP_POST, time_handler},
                  {"/api/history/cleanup", HTTP_POST, cleanup_handler},
                  {"/api/export.csv", HTTP_GET, export_handler}};
    for (const auto &route : routes) {
        httpd_uri_t item = {};
        item.uri = route.uri;
        item.method = route.method;
        item.handler = route.handler;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &item));
    }
    wifi_queue = xQueueCreate(1, sizeof(wifi_config_t));
    if (!wifi_queue || xTaskCreate(wifi_switch_task, "WiFiSwitch", 4096, nullptr, 2, nullptr) != pdPASS)
        return false;
    if (!uart_is_driver_installed(UART_NUM_0)) {
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 0, 0, nullptr, 0));
    }
    if (xTaskCreate(provision_task, "WiFiConfig", 4096, nullptr, 2, nullptr) != pdPASS)
        return false;
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    if (configured)
        esp_wifi_connect();
    ESP_LOGI(TAG, "Wi-Fi preview ready; configuration available through the local page or USB console (2.4 GHz WPA2)");
    return true;
}

bool wifi_preview_sleep()
{
    network_sleeping.store(true);
    if (switching.load() || (wifi_queue && uxQueueMessagesWaiting(wifi_queue))) {
        network_sleeping.store(false); return false;
    }
    if (reconnect_timer) esp_timer_stop(reconnect_timer);
    const auto result = esp_wifi_stop();
    if (result != ESP_OK) { network_sleeping.store(false); return false; }
    return true;
}
void wifi_preview_wake()
{
    network_sleeping.store(false);
    esp_wifi_start();
}
cJSON *wifi_preview_cloud_access()
{
    esp_netif_ip_info_t ip = {};
    auto netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    wifi_ap_record_t ap;
    if (!netif || strlen(token) != 32 || esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        esp_netif_get_ip_info(netif, &ip) != ESP_OK || !ip.ip.addr) return nullptr;
    // Match the backend's private IPv4 allowlist. Networks with a public/local
    // unsupported address still synchronize counts, just not camera discovery.
    const uint8_t a = esp_ip4_addr1(&ip.ip), b = esp_ip4_addr2(&ip.ip);
    if (!(a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168))) return nullptr;
    char address[16];
    snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
    auto result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "address", address);
    cJSON_AddStringToObject(result, "access_key", token);
    return result;
}

bool wifi_preview_requested()
{
    const auto deadline = preview_until_ms.load(std::memory_order_relaxed);
    const auto now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    return deadline != 0 && static_cast<int32_t>(deadline - now) > 0;
}

void wifi_preview_publish(char *packet, size_t length)
{
    if (!frame_mutex) {
        heap_caps_free(packet);
        return;
    }
    xSemaphoreTake(frame_mutex, portMAX_DELAY);
    auto previous = latest;
    latest = packet;
    latest_length = length;
    ++sequence;
    updated = esp_timer_get_time();
    xSemaphoreGive(frame_mutex);
    heap_caps_free(previous);
}
