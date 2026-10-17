#include "frame_cap_pipeline.hpp"
#include "who_detect_app_lcd.hpp"
#include "who_detect_app_term.hpp"
#include "bsp/esp-bsp.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "camera_preview.hpp"
#include "visitor_counter.hpp"
#include "park_history.hpp"
#include "park_cloud.hpp"
#include "park_power.hpp"
#include "app_settings.hpp"
#include "who_yield2idle.hpp"
#include <atomic>
#if defined(CONFIG_HUMAN_FACE_DETECT_MODEL_LOCATION)
#include "human_face_detect.hpp"
#elif defined(CONFIG_PEDESTRIAN_DETECT_MODEL_LOCATION)
#include "pedestrian_detect.hpp"
#elif defined(CONFIG_CAT_DETECT_MODEL_LOCATION)
#include "cat_detect.hpp"
#elif defined(CONFIG_DOG_DETECT_MODEL_LOCATION)
#include "dog_detect.hpp"
#endif

using namespace who::frame_cap;
using namespace who::app;

#if CONFIG_APP_BOARD_FREENOVE_S3
class FreenoveDetectApp : public WhoDetectAppTerm {
public:
    using WhoDetectAppTerm::WhoDetectAppTerm;
    void mark_initial_recovery() { m_initial_recovery.store(true); }
    bool run() override {
        const bool initial_retry=m_initial_recovery.load();
        bool ok=who::WhoYield2Idle::get_instance()->run();
        for (const auto &node:m_frame_cap->get_all_nodes()) ok &= node->run(4096,2,0);
        // The custom callback now carries count/diagnostic snapshots and a frame
        // descriptor. The original terminal-only callback had just 2560 bytes.
        ok &= m_detect->run(8192,2,1);
        if(!initial_retry) ok &= xTaskCreate(recovery_task,"CameraRecovery",4096,this,1,nullptr)==pdPASS;
        return ok;
    }

protected:
    void detect_result_cb(const who::detect::WhoDetect::result_t &result) override
    {
        park_power_detection(!result.det_res.empty());
        if(m_initial_recovery.exchange(false)) park_power_recovery_finished(true);
        const auto counts = visitor_counter_update(result);
        camera_preview_submit(result, counts);
        ++m_frames;
        const int64_t now = esp_timer_get_time();
        if (now - m_last_report >= 5000000) {
            ESP_LOGI("FREENOVE", "Camera + AI OK: frames=%lu, image=%ux%u, people_now=%u",
                     static_cast<unsigned long>(m_frames), static_cast<unsigned>(result.img.width),
                     static_cast<unsigned>(result.img.height), static_cast<unsigned>(result.det_res.size()));
            m_last_report = now;
        }
    }

private:
    static void recovery_task(void *arg) {
        auto *self=static_cast<FreenoveDetectApp *>(arg);
        for(;;) {
            if(park_power_recovery_requested()) {
                const auto deadline=esp_timer_get_time()/1000+park::settings::camera_recovery_ms;
                self->m_detect->pause_async();
                const auto nodes=self->m_frame_cap->get_all_nodes();
                for(auto *node:nodes) node->pause_async();
                bool all=false;
                while(esp_timer_get_time()/1000<deadline && park_power_recovery_requested()) {
                    all=(xEventGroupGetBits(self->m_detect->get_event_group()) & who::task::WhoTask::TASK_PAUSED)!=0;
                    for(auto *node:nodes) all &= (xEventGroupGetBits(node->get_event_group()) & who::task::WhoTask::TASK_PAUSED)!=0;
                    if(all) break;
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                if(all) {
                    self->m_detect->cleanup_for_paused();
                    for(auto *node:nodes) node->cleanup_for_paused();
                    if(restart_dvp_capture()) {
                        for(auto *node:nodes) node->resume();
                        self->m_detect->resume();
                        const auto before=self->m_frames.load();
                        while(self->m_frames.load()==before && esp_timer_get_time()/1000<deadline && park_power_recovery_requested()) vTaskDelay(pdMS_TO_TICKS(20));
                        park_power_recovery_finished(self->m_frames.load()!=before);
                    } else park_power_recovery_finished(false);
                } else park_power_recovery_finished(false);
                vTaskDelete(nullptr);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    std::atomic<uint32_t> m_frames{0};
    std::atomic<bool> m_initial_recovery{false};
    int64_t m_last_report = 0;
};
#endif

dl::detect::Detect *get_detect_model()
{
#if defined(CONFIG_HUMAN_FACE_DETECT_MODEL_LOCATION)
    return new HumanFaceDetect(static_cast<HumanFaceDetect::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL),
                               false);
#elif defined(CONFIG_PEDESTRIAN_DETECT_MODEL_LOCATION)
    return new PedestrianDetect(static_cast<PedestrianDetect::model_type_t>(CONFIG_DEFAULT_PEDESTRIAN_DETECT_MODEL),
                                false);
#elif defined(CONFIG_CAT_DETECT_MODEL_LOCATION)
    return new CatDetect(static_cast<CatDetect::model_type_t>(CONFIG_DEFAULT_CAT_DETECT_MODEL), false);
#elif defined(CONFIG_DOG_DETECT_MODEL_LOCATION)
    return new DogDetect(static_cast<DogDetect::model_type_t>(CONFIG_DEFAULT_DOG_DETECT_MODEL), false);
#else
    ESP_LOGE("MAIN", "No detect model component in idf_component.yml");
    return nullptr;
#endif
}

void run_detect_lcd()
{
    WhoFrameCapNode *lcd_disp_frame_cap_node = nullptr;
#if CONFIG_IDF_TARGET_ESP32S3
    auto frame_cap = get_dvp_frame_cap_pipeline(true);
#elif CONFIG_IDF_TARGET_ESP32P4
    auto frame_cap = get_mipi_csi_frame_cap_pipeline(true);
    // auto frame_cap = get_uvc_frame_cap_pipeline(true);
#endif
    auto detect_app = new WhoDetectAppLCD({{255, 0, 0}}, frame_cap, lcd_disp_frame_cap_node);
    // create model later to avoid memory fragmentation.
    detect_app->set_model(get_detect_model());
    detect_app->run();
}

void run_detect_term()
{
#if CONFIG_APP_BOARD_FREENOVE_S3
    const bool counter_ok = visitor_counter_start();
    if (!counter_ok) ESP_LOGE("COUNTER", "Counting unavailable; check boot diagnostics");
    const bool history_ok = park_history_start();
    const bool network_ok = camera_preview_start();
    const bool cloud_ok = park_cloud_start();
    // Baseline precedes mounting; the first visitor does not wait for the card.
    if (counter_ok) {
        if (!park_power_start()) ESP_LOGE("POWER", "Power manager unavailable; continuous mode retained");
    }
    if(!history_ok || !network_ok || !cloud_ok) ESP_LOGW("MAIN","Some services unavailable; see monitor");
    ESP_LOGI("POWER", "Waiting for PIR or authenticated live preview before camera startup");
    while (!park_power_should_start_camera()) vTaskDelay(pdMS_TO_TICKS(100));
    park_power_camera_started();
    bool retrying_camera=false;
#endif
#if CONFIG_IDF_TARGET_ESP32S3
    auto frame_cap = get_dvp_frame_cap_pipeline(false);
#elif CONFIG_IDF_TARGET_ESP32P4
    auto frame_cap = get_mipi_csi_frame_cap_pipeline(false);
    // auto frame_cap = get_uvc_frame_cap_pipeline(false);
#endif
#if CONFIG_APP_BOARD_FREENOVE_S3
    // Retain network/configuration and upload the existing SD backlog even if
    // the camera fails. Never manufacture new passage records in this mode.
    if (!frame_cap) {
        park_power_camera_failed();
        ESP_LOGE("MAIN", "Camera unavailable; counting stopped, network and SD synchronization remain available.");
        while(!park_power_recovery_requested()) vTaskDelay(pdMS_TO_TICKS(50));
        retrying_camera=true;
        frame_cap=get_dvp_frame_cap_pipeline(false);
        if(!frame_cap) { park_power_recovery_finished(false); return; }
    }
    auto detect_app = new FreenoveDetectApp(frame_cap);
    if(retrying_camera) detect_app->mark_initial_recovery();
#else
    if (!frame_cap) {
        ESP_LOGE("MAIN", "Camera unavailable; detection was not started.");
        return;
    }
    auto detect_app = new WhoDetectAppTerm(frame_cap);
#endif
    // create model later to avoid memory fragmentation.
    detect_app->set_model(get_detect_model());
    detect_app->run();
}

extern "C" void app_main(void)
{
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 5);
#if CONFIG_HUMAN_FACE_DETECT_MODEL_IN_SDCARD || CONFIG_PEDESTRIAN_DETECT_MODEL_IN_SDCARD || \
    CONFIG_CAT_DETECT_MODEL_IN_SDCARD || CONFIG_DOG_DETECT_MODEL_IN_SDCARD
    ESP_ERROR_CHECK(bsp_sdcard_mount());
#endif

// close led
#if defined(BSP_BOARD_ESP32_S3_EYE) && !CONFIG_APP_BOARD_FREENOVE_S3
    led_indicator_handle_t leds[BSP_LED_NUM];
    ESP_ERROR_CHECK(bsp_led_indicator_create(leds, NULL, BSP_LED_NUM));
    ESP_ERROR_CHECK(bsp_led_set(leds[0], false));
#endif

#if CONFIG_APP_BOARD_FREENOVE_S3
    ESP_LOGI("FREENOVE", "Board: ESP32-S3-WROOM N8R8, no LCD, console UART0 115200");
    ESP_LOGI("FREENOVE", "PSRAM initialized: %u bytes", static_cast<unsigned>(esp_psram_get_size()));
    run_detect_term();
#else
    run_detect_lcd();
#endif
}
