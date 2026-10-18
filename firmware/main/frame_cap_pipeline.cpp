// Configures image capture and camera recovery for detection
// Based on ESP-WHO examples/object_detect/main/frame_cap_pipeline.cpp
// Freenove startup, recovery state, and DVP failure handling are project-specific adaptations

#include "frame_cap_pipeline.hpp"
#include "esp_video_init.h"
#include "video_capture.hpp"
#include "bsp/esp-bsp.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace who::frame_cap;

namespace {
    VideoCapture *application_capture = nullptr;
}

// Stops and starts the existing capture object. The caller must have paused all capture/detection consumers
bool restart_dvp_capture() {
    if(!application_capture)
        return false;

    if(application_capture->stop() != ESP_OK)
        return false;

    return application_capture->start() == ESP_OK;
}


// Model buffering intervals from the original ESP-WHO object_detect example - Original logic preserved
#if defined(CONFIG_HUMAN_FACE_DETECT_MODEL_LOCATION)
#define MODEL_TIME 2
#elif defined(CONFIG_PEDESTRIAN_DETECT_MODEL_LOCATION)
#define MODEL_TIME 3
#elif defined(CONFIG_CAT_DETECT_MODEL_LOCATION)
#if defined(CONFIG_ESPDET_PICO_224_224_CAT)
#define MODEL_TIME 3
#endif
#if defined(CONFIG_ESPDET_PICO_416_416_CAT)
#define MODEL_TIME 8
#endif
#elif defined(CONFIG_DOG_DETECT_MODEL_LOCATION)
#if defined(CONFIG_ESPDET_PICO_224_224_DOG)
#define MODEL_TIME 3
#endif
#if defined(CONFIG_ESPDET_PICO_416_416_DOG)
#define MODEL_TIME 8
#endif
#endif

#if CONFIG_APP_BOARD_FREENOVE_S3
// The GC0308 fitted to this board requires 20 MHz, unlike the EYE's 16 MHz
static esp_cam_sensor_xclk_handle_t application_clock = nullptr;

// Releases video initialization and the external sensor clock after a failed startup
static void release_failed_camera() {
    esp_video_deinit();

    if(application_clock) {
        esp_cam_sensor_xclk_stop(application_clock);
        esp_cam_sensor_xclk_free(application_clock);
        application_clock = nullptr;
    }
}


// Project-specific Freenove initialization adapted from the ESP-IDF camera configuration structures
// Clock settings, SCCB ownership, wiring, and cleanup differ from the original EYE BSP startup
// Returns an ESP-IDF error so the application can keep network and history services alive on failure
static esp_err_t freenove_camera_start() {
    // 1. Allocate and start the external sensor clock at the frequency required by this board
    esp_cam_sensor_xclk_handle_t clock        = nullptr;
    esp_cam_sensor_xclk_config_t clock_config = {};
    clock_config.ledc_cfg.timer               = LEDC_TIMER_1;
    clock_config.ledc_cfg.clk_cfg             = LEDC_AUTO_CLK;
    clock_config.ledc_cfg.channel             = static_cast<ledc_channel_t>(CONFIG_BSP_CAMERA_XCLK_LEDC_CH);
    clock_config.ledc_cfg.xclk_freq_hz        = 20000000;
    clock_config.ledc_cfg.xclk_pin            = BSP_CAMERA_GPIO_XCLK;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_LEDC, &clock), "FREENOVE", "XCLK allocation failed");

    esp_err_t status = esp_cam_sensor_xclk_start(clock, &clock_config);

    if(status != ESP_OK) {
        esp_cam_sensor_xclk_free(clock);

        return status;
    }

    application_clock = clock;

    // 2. Allow the sensor to stabilize after its clock starts, a CPU reset does not power-cycle the camera
    vTaskDelay(pdMS_TO_TICKS(20));

    // 3. Let esp_video initialize and manage the camera SCCB control bus with its glitch filter
    // Avoid additional bus initialization or address scans during camera startup
    esp_video_init_dvp_config_t dvp_config    = {};
    dvp_config.sccb_config.init_sccb          = true;
    dvp_config.sccb_config.i2c_config.port    = BSP_I2C_NUM;
    dvp_config.sccb_config.i2c_config.sda_pin = BSP_I2C_SDA;
    dvp_config.sccb_config.i2c_config.scl_pin = BSP_I2C_SCL;
    dvp_config.sccb_config.freq               = 100000;

    // 4. Map the parallel image bus, this board does not use separate reset or power-down pins
    dvp_config.reset_pin          = GPIO_NUM_NC;
    dvp_config.pwdn_pin           = GPIO_NUM_NC;
    dvp_config.dvp_pin.data_width = CAM_CTLR_DATA_WIDTH_8;

    // Map the eight sensor data bits in order, control pins below carry frame and pixel timing
    const gpio_num_t data_pins[] = {GPIO_NUM_11, GPIO_NUM_9, GPIO_NUM_8, GPIO_NUM_10, GPIO_NUM_12, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_16};

    for(int i = 0; i < 8; ++i)
        dvp_config.dvp_pin.data_io[i] = data_pins[i];

    dvp_config.dvp_pin.vsync_io = GPIO_NUM_6;
    dvp_config.dvp_pin.de_io    = GPIO_NUM_7;
    dvp_config.dvp_pin.pclk_io  = GPIO_NUM_13;
    dvp_config.dvp_pin.xclk_io  = GPIO_NUM_15;

    // 5. Initialize the video driver with the configured clock and parallel image bus
    dvp_config.xclk_freq                 = 20000000;
    esp_video_init_config_t video_config = {};
    video_config.dvp                     = &dvp_config;
    status                               = esp_video_init(&video_config);

    // 6. Release video and clock resources if initialization failed, then return the result
    if(status != ESP_OK)
        release_failed_camera();

    return status;
}
#endif
// ESP-WHO DVP pipeline adapted with Freenove startup, recovery access, and nonfatal error handling
// Creates RGB565 capture for ESP32-S3 and exposes its fetch node to the detection application
// The returned pipeline is long-lived, nullptr indicates initialization failed
WhoFrameCap *get_dvp_frame_cap_pipeline(bool lcd) {
#if CONFIG_APP_BOARD_FREENOVE_S3
    const auto camera_status = freenove_camera_start();

    if(camera_status != ESP_OK) {
        ESP_LOGE("FREENOVE", "Camera initialization failed: %s; preserving network recovery.", esp_err_to_name(camera_status));
        return nullptr;
    }
#else
    // Camera startup from the original ESP-WHO example for other ESP32-S3 boards - Original logic preserved
    bsp_camera_cfg_t camera_config{};
    ESP_ERROR_CHECK(bsp_camera_start(&camera_config));
#endif
    // Capture and buffer setup from the original ESP-WHO example - Original logic preserved
    // Reserve capture buffers according to the model processing interval
    // LCD output needs one additional buffer to keep displayed frames aligned with detection results

    auto capture        = new VideoCapture();
    auto buffer_count   = lcd ? (MODEL_TIME + 3):(MODEL_TIME + 2);
    auto capture_config = VideoCapture::Config(ESP_VIDEO_DVP_DEVICE_NAME, V4L2_PIX_FMT_RGB565X, buffer_count);

#if CONFIG_APP_BOARD_FREENOVE_S3
    // Rotate the previous orientation by 180 degrees for use with the board held upside down
    capture_config.set_hflip(false).set_vflip(false);
#elif defined(BSP_BOARD_ESP32_S3_EYE)
    // EYE orientation from the original ESP-WHO example - Original logic preserved
    capture_config.set_vflip(true);
#elifdef BSP_BOARD_ESP32_S3_KORVO_2
    // KORVO orientation from the original ESP-WHO example - Original logic preserved
    capture_config.set_hflip(true).set_vflip(true);
#endif
    // Initialization and streaming are separate operations. Clean up both video and clock state if either fails
    esp_err_t status = capture->init(capture_config);
    if(status == ESP_OK) {
        status = capture->start();
    }

    if(status != ESP_OK) {
        ESP_LOGE("FREENOVE", "Camera could not start: %s. Check sensor model and ribbon connection.", esp_err_to_name(status));
        delete capture;
#if CONFIG_APP_BOARD_FREENOVE_S3
        release_failed_camera();
#endif
        return nullptr;
    }

    auto frame_cap = new WhoFrameCap();

    // Keep the initialized capture object available for the recovery worker, the pipeline fetch node uses it too
    application_capture = capture;
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", capture);
    return frame_cap;
}
