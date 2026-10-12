#include "park_cloud.hpp"
#include "park_history.hpp"
#include "wifi_preview.hpp"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
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
std::atomic<const char *> phase{"starting"};
std::atomic<int> last_error{ESP_OK};
std::atomic<const char *> server_error{""};
bool read(nvs_handle_t nvs,const char *key,char *out,size_t size) {
    return nvs_get_str(nvs,key,out,&size)==ESP_OK;
}
esp_err_t send_bounded(esp_http_client_handle_t client,const char *body,std::string &response) {
    const size_t length=strlen(body);
    auto result=esp_http_client_open(client,length);
    if(result!=ESP_OK) return result;
    // Short TLS records avoid exhausting internal TCP/Wi-Fi buffers during AI.
    for(size_t offset=0;offset<length;) {
        const int written=esp_http_client_write(client,body+offset,std::min(size_t(512),length-offset));
        if(written<=0) return ESP_FAIL;
        offset+=written;
    }
    if(esp_http_client_fetch_headers(client)<0) return ESP_FAIL;
    char data[1025]={};
    const int received=esp_http_client_read_response(client,data,sizeof(data)-1);
    if(received<0 || !esp_http_client_is_complete_data_received(client)) return ESP_ERR_INVALID_RESPONSE;
    response.assign(data,received);
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
        const bool clock_ok=!park_today().empty();
        const bool wifi_ok=esp_wifi_sta_get_ap_info(&ap)==ESP_OK;
        if (!ready || !clock_ok || !wifi_ok) {
            phase.store(!ready?"not_configured":!clock_ok?"waiting_clock":"waiting_wifi");
            memset(token,0,sizeof(token)); vTaskDelay(pdMS_TO_TICKS(10000)); continue;
        }
        phase.store("reading_sd");
        auto batch=park_history_cloud_batch();
        phase.store(batch.error);
        // Device reachability must not depend on SD availability. Never send a
        // partial/invalid batch or move its cursor when only sending health.
        const bool records_ready=batch.ok;
        if (!records_ready) {
            cJSON_Delete(batch.records);
            batch.records=nullptr;
        }
        bool ok=false;
        {
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
            auto camera=wifi_preview_cloud_access();
            if(camera) cJSON_AddItemToObject(health,"camera",camera);
            char *body=cJSON_PrintUnformatted(request);
            auto camera_key=cJSON_GetObjectItem(camera,"access_key");
            if(cJSON_IsString(camera_key)) memset(camera_key->valuestring,0,strlen(camera_key->valuestring));
            cJSON_Delete(request);
            std::string response;
            esp_http_client_config_t config={};
            config.url=ENDPOINT; config.method=HTTP_METHOD_POST; config.timeout_ms=15000;
            config.crt_bundle_attach=esp_crt_bundle_attach;
            config.disable_auto_redirect=true;
            auto client=body?esp_http_client_init(&config):nullptr;
            if (client) {
                phase.store("sending_https");
                esp_http_client_set_header(client,"apikey",key);
                esp_http_client_set_header(client,"Content-Type","application/json");
                const auto result=send_bounded(client,body,response);
                last_error.store(result);
                const auto code=esp_http_client_get_status_code(client);last_http.store(code);
                auto ack=cJSON_Parse(response.c_str());
                auto message=cJSON_GetObjectItemCaseSensitive(ack,"message");
                const char *safe_error="";
                if (cJSON_IsString(message)) {
                    // Only known application errors; never echo a server response or token.
                    for (const auto text : {"Dispositivo não autorizado","Lote inválido","Horário inválido","Contagem inválida","Endereço local inválido"})
                        if (!strcmp(message->valuestring,text)) safe_error=text;
                }
                server_error.store(safe_error);
                ok=result==ESP_OK && code==200 && cJSON_IsNumber(cJSON_GetObjectItem(ack,"accepted")) && cJSON_GetNumberValue(cJSON_GetObjectItem(ack,"accepted"))==sent;
                cJSON_Delete(ack);esp_http_client_cleanup(client);
                phase.store(ok?(records_ready?"acknowledged":"heartbeat_sd_unavailable"):result!=ESP_OK?"https_failed":"server_rejected");
            }
            else phase.store("http_allocation_failed");
            if (body) {memset(body,0,strlen(body));cJSON_free(body);}
            if (ok && records_ready && !batch.file.empty()) {ok=park_history_cloud_ack(batch);if(!ok)phase.store("sd_ack_failed");}
            if (ok) last_success.store(time(nullptr));
            syncing.store(false);
        }
        cJSON_Delete(batch.records);memset(token,0,sizeof(token));
        retry_seconds=ok?10:std::min(300u,retry_seconds*2);
        // Drain backlog in bounded batches, otherwise heartbeat every 30 s.
        vTaskDelay(pdMS_TO_TICKS(ok && records_ready && !batch.file.empty()?2000:ok?30000:retry_seconds*1000));
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
    cJSON_AddStringToObject(json,"phase",phase.load());
    cJSON_AddStringToObject(json,"error",esp_err_to_name(last_error.load()));
    cJSON_AddStringToObject(json,"server_error",server_error.load());
    nvs_handle_t nvs;
    char device[40]={};
    if(nvs_open("park_cloud",NVS_READONLY,&nvs)==ESP_OK) {
        read(nvs,"device",device,sizeof(device));nvs_close(nvs);
    }
    cJSON_AddStringToObject(json,"device_id",device);
    cJSON_AddNumberToObject(json,"free_heap",esp_get_free_heap_size());
    cJSON_AddNumberToObject(json,"internal_largest",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    return json;
}
