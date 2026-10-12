#include "visitor_counter.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <cstdio>

namespace {
park::Counter counter;
SemaphoreHandle_t mutex = nullptr;
SemaphoreHandle_t save_mutex = nullptr;
bool ready = false, storage_ok = false;
uint32_t revision = 0, saved_revision = 0;
// Single versioned blob keeps totals and calibration in the same NVS transaction.
struct Saved {
    uint32_t version;
    float first, second;
    uint32_t horizontal, reverse;
    uint64_t entries, exits;
};
bool valid(const park::Config &c) {
    return std::isfinite(c.first) && std::isfinite(c.second) &&
           c.first >= .05f && c.second <= .95f && c.second-c.first >= .049999f;
}
esp_err_t write_state(const Saved &state) {
    nvs_handle_t handle;
    esp_err_t error=nvs_open("park_counter",NVS_READWRITE,&handle);
    if(error==ESP_OK) {
        error=nvs_set_blob(handle,"state",&state,sizeof(state));
        if(error==ESP_OK) error=nvs_commit(handle);
        nvs_close(handle);
    }
    return error;
}
CountSnapshot snapshot_locked() {
    CountSnapshot s;
    s.entries=counter.entries; s.exits=counter.exits; s.config=counter.config;
    s.ready=ready; s.storage_ok=storage_ok; s.pending=revision!=saved_revision;
    s.created=counter.created; s.expired=counter.expired; s.resumed=counter.resumed;
    s.discarded=counter.discarded; s.frames=counter.frames; s.empty_frames=counter.empty_frames;
    s.event_sequence=counter.event_sequence;
    for (int i=0;i<8;++i) s.events[i]=counter.events[i];
    return s;
}
void persistence_task(void *) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        // Always take save -> counter. Prevent an older background snapshot
        // from overwriting a reset that was already acknowledged to the browser.
        xSemaphoreTake(save_mutex,portMAX_DELAY);
        xSemaphoreTake(mutex,portMAX_DELAY);
        const uint32_t saving=revision;
        bool dirty=saving!=saved_revision;
        Saved state={1,counter.config.first,counter.config.second,
                     counter.config.horizontal,counter.config.reverse,counter.entries,counter.exits};
        xSemaphoreGive(mutex);
        if (!dirty) {xSemaphoreGive(save_mutex);continue;}
        esp_err_t error=write_state(state);
        xSemaphoreTake(mutex,portMAX_DELAY);
        storage_ok=error==ESP_OK;
        if (storage_ok) saved_revision=saving;
        xSemaphoreGive(mutex);
        xSemaphoreGive(save_mutex);
        if (error!=ESP_OK) ESP_LOGE("COUNTER","Cannot save counts: %s",esp_err_to_name(error));
    }
}
}
bool visitor_counter_start() {
    if (!park::self_test()) { ESP_LOGE("COUNTER","Core self-tests FAILED; counting disabled"); return false; }
    ESP_LOGI("COUNTER","Core self-tests PASS (crossing scenario suite)");
    mutex=xSemaphoreCreateMutex();
    save_mutex=xSemaphoreCreateMutex();
    if (!mutex || !save_mutex) return false;
    esp_err_t error=nvs_flash_init();
    if (error!=ESP_OK) { ESP_LOGE("COUNTER","NVS unavailable; no data erased"); return false; }
    nvs_handle_t handle;
    error=nvs_open("park_counter",NVS_READWRITE,&handle);
    if (error!=ESP_OK) return false;
    Saved state={}; size_t size=sizeof(state);
    error=nvs_get_blob(handle,"state",&state,&size);
    nvs_close(handle);
    if (error==ESP_OK) {
        park::Config c{state.first,state.second,state.horizontal!=0,state.reverse!=0};
        if (size!=sizeof(state) || state.version!=1 || state.horizontal>1 || state.reverse>1 || !valid(c)) {
            ESP_LOGE("COUNTER","Invalid saved state; preserved, counting disabled"); return false;
        }
        counter.config=c; counter.entries=state.entries; counter.exits=state.exits;
    } else if (error!=ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE("COUNTER","Cannot load counts: %s; preserved",esp_err_to_name(error)); return false;
    }
    storage_ok=true;
    if (xTaskCreate(persistence_task,"CountSave",3072,nullptr,1,nullptr)!=pdPASS) { storage_ok=false; return false; }
    ready=true;
    ESP_LOGI("COUNTER","Ready: entries=%llu exits=%llu; checkpoint every 10 s",counter.entries,counter.exits);
    return true;
}
CountSnapshot visitor_counter_snapshot() {
    if (!mutex) return CountSnapshot{};
    xSemaphoreTake(mutex,portMAX_DELAY); auto s=snapshot_locked(); xSemaphoreGive(mutex); return s;
}
CountSnapshot visitor_counter_update(const who::detect::WhoDetect::result_t &result) {
    if (!mutex) return CountSnapshot{};
    park::Observation observations[park::capacity]; int indices[park::capacity];
    int size=0,index=0;
    for (const auto &d:result.det_res) {
        if (index>=park::capacity) break;
        if (d.box.size()>=4 && d.score>=.5f && result.img.width>0 && result.img.height>0 &&
            d.box[2]>d.box[0] && d.box[3]>d.box[1]) {
            observations[size]={std::clamp((d.box[0]+d.box[2])/(2.f*result.img.width),0.f,1.f),
                                std::clamp((d.box[1]+d.box[3])/(2.f*result.img.height),0.f,1.f),
                                std::clamp((d.box[2]-d.box[0])/static_cast<float>(result.img.width),.001f,1.f),
                                std::clamp((d.box[3]-d.box[1])/static_cast<float>(result.img.height),.001f,1.f)};
            indices[size++]=index;
        }
        ++index;
    }
    xSemaphoreTake(mutex,portMAX_DELAY);
    unsigned ids[park::capacity]={};
    if (ready) {
        auto before=counter.entries+counter.exits;
        counter.update(observations,size,esp_timer_get_time(),ids);
        if (before!=counter.entries+counter.exits) {
            ++revision;
            ESP_LOGI("COUNTER","Crossing: entries=%llu exits=%llu",counter.entries,counter.exits);
        }
    }
    auto s=snapshot_locked();
    for (int i=0;i<size;++i) {
        s.ids[indices[i]]=ids[i];
        s.states[indices[i]]=counter.state(ids[i],esp_timer_get_time());
    }
    xSemaphoreGive(mutex); return s;
}
bool visitor_counter_configure(const park::Config &c) {
    if (!mutex || !valid(c)) return false;
    xSemaphoreTake(mutex,portMAX_DELAY);
    bool ok=ready;
    if (ok) { counter.config=c; counter.clear_tracks(); ++revision; }
    xSemaphoreGive(mutex); return ok;
}
std::string visitor_counter_json(const CountSnapshot &s) {
    char json[768];
    // Totals are strings to preserve uint64 precision in browser/exports.
    snprintf(json,sizeof(json),"{\"entries\":\"%llu\",\"exits\":\"%llu\",\"ready\":%s,\"storage_ok\":%s,\"pending\":%s,\"first\":%.4f,\"second\":%.4f,\"horizontal\":%s,\"reverse\":%s,\"created\":%u,\"expired\":%u,\"resumed\":%u,\"discarded\":%u,\"frames\":%u,\"empty_frames\":%u,\"events\":[",
        s.entries,s.exits,s.ready?"true":"false",s.storage_ok?"true":"false",s.pending?"true":"false",
        static_cast<double>(s.config.first),static_cast<double>(s.config.second),
        s.config.horizontal?"true":"false",s.config.reverse?"true":"false",
        s.created,s.expired,s.resumed,s.discarded,s.frames,s.empty_frames);
    std::string output=json;
    const unsigned count=std::min(s.event_sequence,8u);
    for(unsigned n=0;n<count;++n) {
        const auto &e=s.events[(s.event_sequence-count+n)%8];
        char event[128];
        snprintf(event,sizeof(event),"%s{\"seq\":%u,\"id\":%u,\"code\":%d,\"ms\":%lld}",
                 n?",":"",e.sequence,e.id,e.code,e.at/1000);
        output+=event;
    }
    output+="]}";
    return output;
}
