#include "frame_cap_pipeline.hpp"
#include "esp_video_init.h"
#include "video_capture.hpp"
#include "bsp/esp-bsp.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace who::frame_cap;
namespace { VideoCapture *application_capture = nullptr; }
bool restart_dvp_capture() {
    return application_capture && application_capture->stop()==ESP_OK && application_capture->start()==ESP_OK;
}

#if defined(CONFIG_HUMAN_FACE_DETECT_MODEL_LOCATION)
// num of frames the model take to get result
#define MODEL_TIME 2
#define MODEL_INPUT_W 160
#define MODEL_INPUT_H 120
#elif defined(CONFIG_PEDESTRIAN_DETECT_MODEL_LOCATION)
// num of frames the model take to get result
#define MODEL_TIME 3
#define MODEL_INPUT_W 224
#define MODEL_INPUT_H 224
#elif defined(CONFIG_CAT_DETECT_MODEL_LOCATION)
// num of frames the model take to get result
#if defined(CONFIG_ESPDET_PICO_224_224_CAT)
#define MODEL_TIME 3
#define MODEL_INPUT_W 224
#define MODEL_INPUT_H 224
#endif
#if defined(CONFIG_ESPDET_PICO_416_416_CAT)
#define MODEL_TIME 8
#define MODEL_INPUT_W 416
#define MODEL_INPUT_H 416
#endif
#elif defined(CONFIG_DOG_DETECT_MODEL_LOCATION)
// num of frames the model take to get result
#if defined(CONFIG_ESPDET_PICO_224_224_DOG)
#define MODEL_TIME 3
#define MODEL_INPUT_W 224
#define MODEL_INPUT_H 224
#endif
#if defined(CONFIG_ESPDET_PICO_416_416_DOG)
#define MODEL_TIME 8
#define MODEL_INPUT_W 416
#define MODEL_INPUT_H 416
#endif
#endif

// The size of the fb_count and ringbuf_len must be big enough. If you have no idea how to set them, try with 5 and
// larger.
#if CONFIG_IDF_TARGET_ESP32S3
#if CONFIG_APP_BOARD_FREENOVE_S3
// The GC0308 fitted to this board requires 20 MHz, unlike the EYE's 16 MHz.
static esp_cam_sensor_xclk_handle_t application_clock = nullptr;
static void release_failed_camera() {
    esp_video_deinit();
    if(application_clock) {
        esp_cam_sensor_xclk_stop(application_clock);
        esp_cam_sensor_xclk_free(application_clock); application_clock=nullptr;
    }
}
static esp_err_t freenove_camera_start()
{
    esp_cam_sensor_xclk_handle_t clock = nullptr;
    esp_cam_sensor_xclk_config_t clock_cfg = {};
    clock_cfg.ledc_cfg.timer = LEDC_TIMER_1;
    clock_cfg.ledc_cfg.clk_cfg = LEDC_AUTO_CLK;
    clock_cfg.ledc_cfg.channel = static_cast<ledc_channel_t>(CONFIG_BSP_CAMERA_XCLK_LEDC_CH);
    clock_cfg.ledc_cfg.xclk_freq_hz = 20000000;
    clock_cfg.ledc_cfg.xclk_pin = BSP_CAMERA_GPIO_XCLK;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_LEDC, &clock),
                        "FREENOVE", "XCLK allocation failed");
    esp_err_t status = esp_cam_sensor_xclk_start(clock, &clock_cfg);
    if (status != ESP_OK) {
        esp_cam_sensor_xclk_free(clock);
        return status;
    }
    application_clock=clock;
    // Let the external sensor settle after its clock starts. A CPU reset does
    // not power-cycle this board's camera.
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_video_init_dvp_config_t dvp = {};
    // Let esp_video own SCCB: its bus setup includes a glitch filter. Do not
    // share the EYE audio BSP bus or probe unrelated addresses after detection.
    dvp.sccb_config.init_sccb = true;
    dvp.sccb_config.i2c_config.port = BSP_I2C_NUM;
    dvp.sccb_config.i2c_config.sda_pin = BSP_I2C_SDA;
    dvp.sccb_config.i2c_config.scl_pin = BSP_I2C_SCL;
    dvp.sccb_config.freq = 100000;
    dvp.reset_pin = GPIO_NUM_NC;
    dvp.pwdn_pin = GPIO_NUM_NC;
    dvp.dvp_pin.data_width = CAM_CTLR_DATA_WIDTH_8;
    const gpio_num_t pins[] = {GPIO_NUM_11, GPIO_NUM_9, GPIO_NUM_8, GPIO_NUM_10,
                               GPIO_NUM_12, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_16};
    for (int i = 0; i < 8; ++i) dvp.dvp_pin.data_io[i] = pins[i];
    dvp.dvp_pin.vsync_io = GPIO_NUM_6;
    dvp.dvp_pin.de_io = GPIO_NUM_7;
    dvp.dvp_pin.pclk_io = GPIO_NUM_13;
    dvp.dvp_pin.xclk_io = GPIO_NUM_15;
    dvp.xclk_freq = 20000000;
    esp_video_init_config_t video = {};
    video.dvp = &dvp;
    status=esp_video_init(&video);
    if(status!=ESP_OK) release_failed_camera();
    return status;
}
#endif
WhoFrameCap *get_dvp_frame_cap_pipeline(bool lcd)
{
#if CONFIG_APP_BOARD_FREENOVE_S3
    const auto camera_status = freenove_camera_start();
    if (camera_status != ESP_OK) {
        ESP_LOGE("FREENOVE", "Camera initialization failed: %s; preserving network recovery.",
                 esp_err_to_name(camera_status));
        return nullptr;
    }
#else
    bsp_camera_cfg_t cam_cfg{};
    ESP_ERROR_CHECK(bsp_camera_start(&cam_cfg));
#endif
    // The ringbuf_len of FetchNode equals cam_fb_count - 2. The WhoFetchNode fb will display on lcd, if you want to
    // make sure the displayed detection result is synced with the frame, the ringbuf size must be big enough to
    // cover the process time from now to the the detection result is ready. If the ring_buf_len is 3, the frame
    // which the disp task will display is 2 frames before than the frame which feeds into detection task. So the
    // detection task must finish within 2 frames, or the detect result will have a delay compared to the displayed
    // frame.

    auto cap = new VideoCapture();
    auto buffer_cnt = lcd ? (MODEL_TIME + 3) : (MODEL_TIME + 2);
    auto cfg = VideoCapture::Config(ESP_VIDEO_DVP_DEVICE_NAME, V4L2_PIX_FMT_RGB565X, buffer_cnt);

#if CONFIG_APP_BOARD_FREENOVE_S3
    // Orientation can be adjusted after checking the installed camera view.
    cfg.set_hflip(true).set_vflip(true);
#elif defined(BSP_BOARD_ESP32_S3_EYE)
    cfg.set_vflip(true);
#elifdef BSP_BOARD_ESP32_S3_KORVO_2
    cfg.set_hflip(true).set_vflip(true);
#endif
    esp_err_t status = cap->init(cfg);
    if (status == ESP_OK) {
        status = cap->start();
    }
    if (status != ESP_OK) {
        ESP_LOGE("FREENOVE", "Camera could not start: %s. Check sensor model and ribbon connection.",
                 esp_err_to_name(status));
        delete cap;
#if CONFIG_APP_BOARD_FREENOVE_S3
        release_failed_camera();
#endif
        return nullptr;
    }

    auto frame_cap = new WhoFrameCap();
    application_capture=cap;
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cap);
    return frame_cap;
}

#elif CONFIG_IDF_TARGET_ESP32P4
WhoFrameCap *get_mipi_csi_frame_cap_pipeline(bool lcd)
{
    esp_log_level_set("ISP_AWB", ESP_LOG_ERROR);
    bsp_camera_cfg_t cam_cfg{};
    ESP_ERROR_CHECK(bsp_camera_start(&cam_cfg));

    auto cap = new VideoCapture();
    auto buffer_cnt = lcd ? (MODEL_TIME + 3) : (MODEL_TIME + 2);
    auto cfg = VideoCapture::Config(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, V4L2_PIX_FMT_RGB565, buffer_cnt).set_hflip(true);
    cap->init(cfg);
    cap->start();

    auto frame_cap = new WhoFrameCap();
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cap);
    return frame_cap;
}

#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
WhoFrameCap *get_uvc_frame_cap_pipeline(bool lcd)
{
    esp_video_init_usb_uvc_config_t usb_uvc_cfg = {
        .uvc =
            {
                .uvc_dev_num = 1,
                .task_stack = 4096,
                .task_priority = 10,
                .task_affinity = -1,
            },
        .usb =
            {
                .init_usb_host_lib = true,
                .peripheral_map = 0x00,
                .task_stack = 4096,
                .task_priority = 11,
                .task_affinity = -1,
            },
    };
    esp_video_init_config_t video_cfg = {};
    video_cfg.usb_uvc = &usb_uvc_cfg;
    esp_video_init(&video_cfg);

    auto cap = new VideoCapture();
    auto cfg = VideoCapture::Config(ESP_VIDEO_USB_UVC_NAME(0), V4L2_PIX_FMT_JPEG, 4).set_uvc_config({640, 480, 30});
    cap->init(cfg);
    cap->start();

    auto frame_cap = new WhoFrameCap();
    // The ringbuf_len of FetchNode equals cam_fb_count - 2, the ringbuf_len of FetchNode should take care of the
    // process time of the following Node. For example, if the DecodeNode takes 2 frame to decode, then the
    // FetchNode ringbuf_len is at least 2, and the fb_count of the cam is at least 4.
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cap, false);

    if (lcd) {
        // The DecodeNode ringbuf_len relies on the following PPAResizeNode process time, the time of data transfer.
        frame_cap->add_node<WhoDecodeNode>("FrameCapDecode", dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, 2, false);
        // The ppa resized fb will display on lcd, if you want to make sure the displayed detection result is synced
        // with the frame, the ringbuf size must be big enough to cover the process time from now to the the detection
        // result is ready.
        frame_cap->add_node<WhoPPAResizeNode>(
            "FrameCapPPAResize", 800, 600, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, MODEL_TIME + 1);
        return frame_cap;
    } else {
        frame_cap->add_node<WhoDecodeNode>("FrameCapDecode", dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, MODEL_TIME);
        return frame_cap;
    }
}
#endif
#endif
