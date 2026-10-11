#include "park_cloud.hpp"
#include "park_history.hpp"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <string>

namespace {
constexpr const char *ENDPOINT="https://pcxeestzvcpbfkcbuqpa.supabase.co/rest/v1/rpc/park_ingest";
std::atomic<bool> configured{false}, syncing{false};
std::atomic<int> last_http{0};
std::atomic<time_t> last_success{0};
bool read(nvs_handle_t nvs,const char *key,char *out,size_t size) {
    return nvs_get_str(nvs,key,out,&size)==ESP_OK;
}
esp_err_t receive(esp_http_client_event_t *event) {
    if (event->event_id==HTTP_EVENT_ON_DATA) {
        auto body=static_cast<std::string *>(event->user_data);
        if (body->size()+event->data_len>1024) return ESP_FAIL;
        body->append(static_cast<const char *>(event->data),event->data_len);
    }
    return ESP_OK;
}
void task(void *) {
    unsigned retry_seconds=10;
    while (true) {
        char device[40]={},token[65]={},key[192]={};
        nvs_handle_t nvs;
        bool ready=nvs_open("park_cloud",NVS_READONLY,&nvs)==ESP_OK;
        if (ready) {
            ready=read(nvs,"device",device,sizeof(device)) && read(nvs,"token",token,sizeof(token)) && read(nvs,"apikey",key,sizeof(key));
            nvs_close(nvs);
        }
        configured.store(ready);
        wifi_ap_record_t ap;
        if (!ready || park_today().empty() || esp_wifi_sta_get_ap_info(&ap)!=ESP_OK) {
            memset(token,0,sizeof(token)); vTaskDelay(pdMS_TO_TICKS(10000)); continue;
        }
        auto batch=park_history_cloud_batch();
        bool ok=false;
        if (batch.ok) {
            syncing.store(true);
            auto request=cJSON_CreateObject();
            cJSON_AddStringToObject(request,"p_device",device);
            cJSON_AddStringToObject(request,"p_token",token);
            cJSON_AddItemToObject(request,"p_records",batch.records?batch.records:cJSON_CreateArray());
            batch.records=nullptr;
            const int sent=cJSON_GetArraySize(cJSON_GetObjectItem(request,"p_records"));
            auto status=park_history_status();
            auto health=cJSON_AddObjectToObject(request,"p_status");
            cJSON_AddBoolToObject(health,"storage_ok",cJSON_IsTrue(cJSON_GetObjectItem(status,"ok")));
            cJSON_AddBoolToObject(health,"clock_ok",cJSON_IsTrue(cJSON_GetObjectItem(status,"clock_ok")));
            cJSON_Delete(status);
            char *body=cJSON_PrintUnformatted(request);
            cJSON_Delete(request);
            std::string response;
            esp_http_client_config_t config={};
            config.url=ENDPOINT; config.method=HTTP_METHOD_POST; config.timeout_ms=15000;
            config.crt_bundle_attach=esp_crt_bundle_attach;
            config.disable_auto_redirect=true;
            config.event_handler=receive; config.user_data=&response;
            auto client=body?esp_http_client_init(&config):nullptr;
            if (client) {
                esp_http_client_set_header(client,"apikey",key);
                esp_http_client_set_header(client,"Content-Type","application/json");
                esp_http_client_set_post_field(client,body,strlen(body));
                const auto result=esp_http_client_perform(client);
                const auto code=esp_http_client_get_status_code(client);last_http.store(code);
                auto ack=cJSON_Parse(response.c_str());
                ok=result==ESP_OK && code==200 && cJSON_IsNumber(cJSON_GetObjectItem(ack,"accepted")) && cJSON_GetNumberValue(cJSON_GetObjectItem(ack,"accepted"))==sent;
                cJSON_Delete(ack);esp_http_client_cleanup(client);
            }
            if (body) {memset(body,0,strlen(body));cJSON_free(body);}
            if (ok && !batch.file.empty()) ok=park_history_cloud_ack(batch);
            if (ok) last_success.store(time(nullptr));
            syncing.store(false);
        }
        cJSON_Delete(batch.records);memset(token,0,sizeof(token));
        retry_seconds=ok?10:std::min(300u,retry_seconds*2);
        // Drain backlog in bounded batches, otherwise heartbeat every 30 s.
        vTaskDelay(pdMS_TO_TICKS(ok && !batch.file.empty()?2000:ok?30000:retry_seconds*1000));
    }
}
}
bool park_cloud_start() {
    park_history_cloud_protect();
    return xTaskCreate(task,"ParkCloud",12288,nullptr,1,nullptr)==pdPASS;
}
bool park_cloud_configure(const cJSON *json) {
    auto device=cJSON_GetObjectItemCaseSensitive(json,"device_id");
    auto token=cJSON_GetObjectItemCaseSensitive(json,"token");
    auto key=cJSON_GetObjectItemCaseSensitive(json,"publishable_key");
    if (!cJSON_IsString(device)||strlen(device->valuestring)!=36 || !cJSON_IsString(token)||strlen(token->valuestring)!=64 || !cJSON_IsString(key)||strlen(key->valuestring)>=192 || strncmp(key->valuestring,"sb_publishable_",15)) return false;
    for (const char *p=token->valuestring;*p;++p) if (!strchr("0123456789abcdef",*p)) return false;
    for (const char *p=device->valuestring;*p;++p) if (!strchr("0123456789abcdef-",*p)) return false;
    nvs_handle_t nvs;
    if(nvs_open("park_cloud",NVS_READWRITE,&nvs)!=ESP_OK) return false;
    bool ok=nvs_set_str(nvs,"device",device->valuestring)==ESP_OK && nvs_set_str(nvs,"token",token->valuestring)==ESP_OK && nvs_set_str(nvs,"apikey",key->valuestring)==ESP_OK && nvs_commit(nvs)==ESP_OK;
    nvs_close(nvs);return ok;
}
cJSON *park_cloud_status() {
    auto json=cJSON_CreateObject();
    cJSON_AddBoolToObject(json,"configured",configured.load());
    cJSON_AddBoolToObject(json,"syncing",syncing.load());
    cJSON_AddNumberToObject(json,"last_http",last_http.load());
    cJSON_AddStringToObject(json,"last_success",park_timestamp(last_success.load()).c_str());
    return json;
}
