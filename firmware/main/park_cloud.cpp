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
constexpr const char *HOST="pcxeestzvcpbfkcbuqpa.supabase.co";
constexpr const char *ENDPOINT="https://pcxeestzvcpbfkcbuqpa.supabase.co/rest/v1/rpc/park_ingest";
std::atomic<bool> configured{false}, syncing{false}, closing{false}, close_ready{false}, paused{false};
std::atomic<bool> pending{true}, permanent_error{false}, reset_schedule{false};
std::atomic<int> last_http{0}, last_error{ESP_OK};
std::atomic<time_t> last_success{0};
std::atomic<uint64_t> close_deadline{0}, next_attempt{0};
std::atomic<unsigned> batches{0}, failures{0};
std::atomic<const char *> phase{"starting"}, server_error{""}, close_result{""};
SemaphoreHandle_t cycle_mutex=nullptr;
TaskHandle_t worker=nullptr;
uint64_t ms() { return esp_timer_get_time()/1000; }
bool read(nvs_handle_t nvs,const char *key,char *out,size_t size) { return nvs_get_str(nvs,key,out,&size)==ESP_OK; }
void finish(const char *reason) { close_result.store(reason); close_ready.store(true); }

// DNS runs on lwIP's thread. Static storage remains valid if its callback is
// delivered after our monotonic deadline; no task is killed holding a mutex.
std::atomic<bool> dns_pending{false}, dns_done{false}, dns_ok{false};
void dns_result(const char *,const ip_addr_t *ip,void *) { dns_ok.store(ip!=nullptr); dns_done.store(true); dns_pending.store(false); }
void dns_start(void *) {
    ip_addr_t address;
    const auto r=dns_gethostbyname_addrtype(HOST,&address,dns_result,nullptr,LWIP_DNS_ADDRTYPE_IPV4);
    if(r!=ERR_INPROGRESS) dns_result(HOST,r==ERR_OK?&address:nullptr,nullptr);
}
bool resolve(uint64_t deadline) {
    if(!dns_pending.exchange(true)) {
        dns_done.store(false); dns_ok.store(false);
        if(tcpip_callback(dns_start,nullptr)!=ERR_OK) { dns_pending.store(false); return false; }
    }
    while(!dns_done.load() && ms()<deadline && !paused.load()) vTaskDelay(pdMS_TO_TICKS(20));
    return dns_done.load() && dns_ok.load();
}
struct Response { std::string body; bool overflow=false; };
esp_err_t response_event(esp_http_client_event_t *event) {
    auto *r=static_cast<Response *>(event->user_data);
    if(event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) {
        if(r->body.size()+event->data_len>1024) { r->overflow=true; return ESP_FAIL; }
        r->body.append(static_cast<const char *>(event->data),event->data_len);
    }
    return ESP_OK;
}
struct Attempt { bool ok=false, transient=true; int code=0; esp_err_t error=ESP_FAIL; const char *message=""; };
Attempt send(const char *body,const char *key,uint64_t deadline) {
    Attempt result;
    if(!resolve(deadline)) { result.error=ESP_ERR_TIMEOUT; return result; }
    Response response;
    esp_http_client_config_t config={};
    config.url=ENDPOINT; config.method=HTTP_METHOD_POST;
    config.timeout_ms=std::max(1,int(deadline>ms()?deadline-ms():1));
    config.crt_bundle_attach=esp_crt_bundle_attach; config.disable_auto_redirect=true;
    config.is_async=true; config.buffer_size_tx=512; config.buffer_size=512;
    config.event_handler=response_event; config.user_data=&response;
    auto client=esp_http_client_init(&config);
    if(!client) { result.error=ESP_ERR_NO_MEM; return result; }
    esp_http_client_set_header(client,"apikey",key);
    esp_http_client_set_header(client,"Content-Type","application/json");
    esp_http_client_set_post_field(client,body,strlen(body));
    do {
        if(ms()>=deadline || paused.load()) { result.error=ESP_ERR_TIMEOUT; break; }
        result.error=esp_http_client_perform(client);
        if(result.error==ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(20));
    } while(result.error==ESP_ERR_HTTP_EAGAIN);
    result.code=esp_http_client_get_status_code(client);
    auto ack=cJSON_Parse(response.body.c_str()); auto request=cJSON_Parse(body);
    const int sent=cJSON_GetArraySize(cJSON_GetObjectItem(request,"p_records"));
    const auto accepted=cJSON_GetObjectItem(ack,"accepted");
    result.ok=result.error==ESP_OK && !response.overflow && result.code==200 && cJSON_IsNumber(accepted) && accepted->valuedouble==sent;
    result.transient=result.code==0 || result.code==408 || result.code==429 || result.code>=500 || (result.code==200 && !result.ok);
    auto message=cJSON_GetObjectItem(ack,"message");
    if(cJSON_IsString(message)) for(const char *known:{"Dispositivo não autorizado","Lote inválido","Horário inválido","Contagem inválida","Endereço local inválido"})
        if(!strcmp(message->valuestring,known)) result.message=known;
    cJSON_Delete(request); cJSON_Delete(ack); esp_http_client_cleanup(client);
    return result;
}
using Digest=std::array<unsigned char,32>;
using StatusVersion=std::map<std::string,Digest>;
Digest digest(cJSON *item) {
    Digest hash{}; char *text=cJSON_PrintUnformatted(item);
    if(text) { mbedtls_sha256(reinterpret_cast<unsigned char *>(text),strlen(text),hash.data(),0); memset(text,0,strlen(text)); cJSON_free(text); }
    return hash;
}
cJSON *status_delta(const StatusVersion &acked,StatusVersion &sent) {
    auto all=cJSON_CreateObject();
    cJSON_AddBoolToObject(all,"storage_ok",park_history_healthy());
    cJSON_AddBoolToObject(all,"clock_ok",!park_today().empty());
    cJSON_AddItemToObject(all,"operation",park_power_cloud_status());
    if(auto camera=wifi_preview_cloud_access()) cJSON_AddItemToObject(all,"camera",camera);
    auto delta=cJSON_CreateObject();
    for(auto *item=all->child;item;item=item->next) {
        const auto hash=digest(item); const auto previous=acked.find(item->string);
        if(previous==acked.end() || previous->second!=hash) { cJSON_AddItemToObject(delta,item->string,cJSON_Duplicate(item,true)); sent[item->string]=hash; }
    }
    cJSON_Delete(all); return delta;
}
void task(void *) {
    park::CloudSchedule schedule; schedule.collect_at=1; // One backlog scan per boot.
    StatusVersion acked;
    uint32_t seen_revision=park_history_revision();
    bool scan=true, failed_while_closing=false, was_closing=false;
    uint64_t scan_at=0, wifi_wait=0;
    while(true) {
        if(paused.load()) { ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100)); continue; }
        if(reset_schedule.exchange(false)) { schedule.success(); schedule.collect_at=1; acked.clear(); scan=true; }
        const auto now=ms(); const bool shutdown=closing.load();
        if(shutdown && !was_closing) scan=true;
        was_closing=shutdown;
        if(!shutdown) { wifi_wait=0; failed_while_closing=false; }
        if(shutdown && (close_ready.load() || now>=close_deadline.load())) {
            if(!close_ready.load()) finish("budget_exhausted");
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        if(park_history_revision()!=seen_revision) { schedule.pending(now); scan=true; pending.store(true); seen_revision=park_history_revision(); }
        if(!park_history_initialized() && now<park::settings::request_ms) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        StatusVersion sent; auto status=status_delta(acked,sent);
        const bool changed=status->child!=nullptr;
        if(schedule.exhausted(shutdown) || (shutdown && schedule.failures && !failed_while_closing)) {
            if(shutdown) finish(schedule.permanent?"permanent_error":"send_failed");
            cJSON_Delete(status); vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        const bool due=schedule.due(now,shutdown);
        if((!due && !(changed && !schedule.failures)) || now<scan_at) {
            cJSON_Delete(status); next_attempt.store(schedule.failures?schedule.retry_at:schedule.collect_at);
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        xSemaphoreTake(cycle_mutex,portMAX_DELAY);
        if(paused.load()) { cJSON_Delete(status); xSemaphoreGive(cycle_mutex); continue; }
        char device[40]={},token[65]={},key[192]={}; nvs_handle_t nvs;
        bool ready=nvs_open("park_cloud",NVS_READONLY,&nvs)==ESP_OK;
        if(ready) { ready=read(nvs,"device",device,sizeof(device)) && read(nvs,"token",token,sizeof(token)) && read(nvs,"apikey",key,sizeof(key)); nvs_close(nvs); }
        configured.store(ready); wifi_ap_record_t ap;
        const bool wifi=esp_wifi_sta_get_ap_info(&ap)==ESP_OK;
        if(!ready || !wifi) {
            phase.store(!ready?"not_configured":"waiting_wifi");
            if(shutdown && !ready) finish("not_configured");
            else if(shutdown) { if(!wifi_wait) wifi_wait=now; if(now-wifi_wait>=park::settings::request_ms) finish("wifi_unavailable"); }
            cJSON_Delete(status); memset(token,0,sizeof(token)); xSemaphoreGive(cycle_mutex); vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        wifi_wait=0;
        ParkCloudBatch batch;
        if(scan && due) batch=park_history_cloud_batch();
        if(!batch.ok) { cJSON_Delete(batch.records); batch.records=nullptr; }
        const int count=cJSON_GetArraySize(batch.records);
        if(!strcmp(batch.error,"sd_busy")) {
            cJSON_Delete(batch.records); cJSON_Delete(status); xSemaphoreGive(cycle_mutex);
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        if(!count && !changed) {
            phase.store(batch.ok?"idle":"storage_unavailable");
            if(shutdown) finish(batch.ok?"drained":"storage_unavailable");
            scan=false; pending.store(!batch.ok); schedule.success();
            cJSON_Delete(batch.records); cJSON_Delete(status); memset(token,0,sizeof(token)); xSemaphoreGive(cycle_mutex);
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        auto request=cJSON_CreateObject();
        cJSON_AddStringToObject(request,"p_device",device); cJSON_AddStringToObject(request,"p_token",token);
        cJSON_AddItemToObject(request,"p_records",batch.records?batch.records:cJSON_CreateArray()); batch.records=nullptr;
        cJSON_AddItemToObject(request,"p_status",status);
        char *body=cJSON_PrintUnformatted(request); cJSON_Delete(request);
        phase.store("sending_https"); syncing.store(true); power_monitor_event("cloud_attempt");
        const auto deadline=std::min(ms()+park::settings::request_ms,shutdown?close_deadline.load():UINT64_MAX);
        auto result=body?send(body,key,deadline):Attempt{};
        if(body) { memset(body,0,strlen(body)); cJSON_free(body); } memset(token,0,sizeof(token));
        last_http.store(result.code); last_error.store(result.error); server_error.store(result.message);
        if(result.ok && count && !paused.load()) {
            result.ok=park_history_cloud_ack(batch);
            if(!result.ok) { result.transient=false; phase.store("sd_ack_failed"); }
        }
        if(result.ok) {
            for(const auto &field:sent) acked[field.first]=field.second;
            last_success.store(time(nullptr)); ++batches; failures.store(0); permanent_error.store(false);
            // A status-only ACK must not cancel a pending 10 s collection window.
            if(count) { schedule.success(); schedule.collect_at=ms()+park::settings::batch_gap_ms; scan=true; pending.store(true); }
            else { schedule.failures=0; schedule.retry_at=0; schedule.permanent=false; }
            phase.store("acknowledged"); power_monitor_event("cloud_ack");
            scan_at=count?ms()+park::settings::batch_gap_ms:0;
        } else {
            schedule.failure(ms(),shutdown,result.transient); failed_while_closing=shutdown;
            failures.store(schedule.failures); permanent_error.store(schedule.permanent);
            phase.store(result.transient?"retry_wait":"permanent_error"); pending.store(true);
            power_monitor_event("cloud_failed",result.transient?"temporary":"permanent");
        }
        syncing.store(false); next_attempt.store(schedule.failures?schedule.retry_at:schedule.collect_at);
        xSemaphoreGive(cycle_mutex); vTaskDelay(pdMS_TO_TICKS(50));
    }
}
}
bool park_cloud_start() {
    if(!park::cloud_policy_test()) { ESP_LOGE("CLOUD","Scheduling self-check failed"); return false; }
    ESP_LOGI("CLOUD","Scheduling self-check PASS; collection=10 s, batch=8, request=10 s, close=120 s");
    park_history_cloud_protect(); cycle_mutex=xSemaphoreCreateMutex();
    return cycle_mutex && xTaskCreate(task,"ParkCloud",12288,nullptr,1,&worker)==pdPASS;
}
void park_cloud_request_sync() { if(worker) xTaskNotifyGive(worker); }
void park_cloud_prepare_sleep(uint64_t deadline) {
    close_deadline.store(deadline); close_result.store(""); close_ready.store(false); closing.store(true);
    if(!worker) finish("not_configured"); else park_cloud_request_sync();
}
void park_cloud_cancel_sleep() { closing.store(false); close_ready.store(false); park_cloud_request_sync(); }
bool park_cloud_sleep_ready() { return close_ready.load(); }
const char *park_cloud_sleep_result() { return close_result.load(); }
bool park_cloud_pause(unsigned timeout_ms) {
    paused.store(true);
    if(cycle_mutex && xSemaphoreTake(cycle_mutex,pdMS_TO_TICKS(timeout_ms))==pdTRUE) { xSemaphoreGive(cycle_mutex); return true; }
    return false;
}
void park_cloud_resume() { paused.store(false); park_cloud_request_sync(); }
bool park_cloud_configure(const cJSON *json) {
    auto device=cJSON_GetObjectItemCaseSensitive(json,"device_id"), token=cJSON_GetObjectItemCaseSensitive(json,"token"), key=cJSON_GetObjectItemCaseSensitive(json,"publishable_key");
    if(!cJSON_IsString(device)||strlen(device->valuestring)!=36 || !cJSON_IsString(token)||strlen(token->valuestring)!=64 || !cJSON_IsString(key)||strlen(key->valuestring)>=192 || strncmp(key->valuestring,"sb_publishable_",15)) return false;
    for(const char *p=token->valuestring;*p;++p) if(!strchr("0123456789abcdef",*p)) return false;
    for(const char *p=device->valuestring;*p;++p) if(!strchr("0123456789abcdef-",*p)) return false;
    nvs_handle_t nvs;
    if(nvs_open("park_cloud",NVS_READWRITE,&nvs)!=ESP_OK) return false;
    const bool ok=nvs_set_str(nvs,"device",device->valuestring)==ESP_OK && nvs_set_str(nvs,"token",token->valuestring)==ESP_OK && nvs_set_str(nvs,"apikey",key->valuestring)==ESP_OK && nvs_commit(nvs)==ESP_OK;
    nvs_close(nvs); if(ok) { reset_schedule.store(true); park_cloud_request_sync(); } return ok;
}
cJSON *park_cloud_status() {
    auto j=cJSON_CreateObject();
    cJSON_AddBoolToObject(j,"configured",configured.load()); cJSON_AddBoolToObject(j,"syncing",syncing.load());
    cJSON_AddBoolToObject(j,"pending",pending.load()); cJSON_AddBoolToObject(j,"permanent_error",permanent_error.load());
    cJSON_AddNumberToObject(j,"last_http",last_http.load()); cJSON_AddStringToObject(j,"last_success",park_timestamp(last_success.load()).c_str());
    cJSON_AddStringToObject(j,"phase",phase.load()); cJSON_AddStringToObject(j,"error",esp_err_to_name(last_error.load()));
    cJSON_AddStringToObject(j,"server_error",server_error.load()); cJSON_AddStringToObject(j,"sleep_result",close_result.load());
    cJSON_AddNumberToObject(j,"batches",batches.load()); cJSON_AddNumberToObject(j,"failures",failures.load());
    cJSON_AddNumberToObject(j,"next_attempt_ms",next_attempt.load()>ms()?next_attempt.load()-ms():0);
    cJSON_AddNumberToObject(j,"free_heap",esp_get_free_heap_size()); cJSON_AddNumberToObject(j,"internal_largest",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    return j;
}
