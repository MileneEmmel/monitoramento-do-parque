#include "camera_preview.hpp"
#include "wifi_preview.hpp"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "mbedtls/base64.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct Box { int x1, y1, x2, y2; float score; unsigned id; uint8_t state; };
struct Frame {
    uint8_t *pixels;
    int bytes, width, height;
    jpeg_pixel_format_t format;
    unsigned count, box_count;
    Box boxes[32];
    CountSnapshot counts;
};
QueueHandle_t queue = nullptr;
int64_t last_submit = 0;

void preview_task(void *)
{
    Frame frame;
    while (xQueueReceive(queue, &frame, portMAX_DELAY) == pdTRUE) {
        if (!wifi_preview_requested()) {
            heap_caps_free(frame.pixels);
            continue;
        }
        jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
        config.width = frame.width;
        config.height = frame.height;
        config.src_type = frame.format;
        config.quality = 50;
        jpeg_enc_handle_t encoder = nullptr;
        const int capacity = frame.width * frame.height * 2;
        auto jpeg = static_cast<uint8_t *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM));
        int size = 0;
        if (jpeg && jpeg_enc_open(&config, &encoder) == JPEG_ERR_OK &&
            jpeg_enc_process(encoder, frame.pixels, frame.bytes, jpeg, capacity, &size) == JPEG_ERR_OK) {
            const size_t encoded_capacity = 4 * ((size + 2) / 3) + 1;
            auto encoded = static_cast<unsigned char *>(heap_caps_malloc(encoded_capacity, MALLOC_CAP_SPIRAM));
            size_t encoded_size = 0;
            if (encoded && mbedtls_base64_encode(encoded, encoded_capacity, &encoded_size, jpeg, size) == 0) {
                encoded[encoded_size] = 0;
                std::string boxes = "[";
                for (unsigned i = 0; i < frame.box_count; ++i) {
                    char entry[160];
                    const Box &b = frame.boxes[i];
                    snprintf(entry, sizeof(entry), "%s[%d,%d,%d,%d,%.3f,%u,%u]", i ? "," : "",
                             b.x1, b.y1, b.x2, b.y2, static_cast<double>(b.score), b.id, b.state);
                    boxes += entry;
                }
                boxes += "]";
                const auto counts = visitor_counter_json(frame.counts);
                const size_t capacity = encoded_size + boxes.size() + counts.size() + 200;
                auto packet = static_cast<char *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM));
                if (packet) {
                    const int length = snprintf(packet, capacity,
                        "{\"width\":%d,\"height\":%d,\"people\":%u,\"counter\":%s,\"boxes\":%s,\"jpeg\":\"%s\"}",
                        frame.width, frame.height, frame.count, counts.c_str(), boxes.c_str(), encoded);
                    if (length > 0 && static_cast<size_t>(length) < capacity) wifi_preview_publish(packet, length);
                    else heap_caps_free(packet);
                }
            }
            heap_caps_free(encoded);
        } else {
            ESP_LOGW("PREVIEW", "JPEG encoding failed; frame skipped");
        }
        if (encoder) jpeg_enc_close(encoder);
        heap_caps_free(jpeg);
        heap_caps_free(frame.pixels);
    }
    vTaskDelete(nullptr);
}
}

bool camera_preview_start()
{
    if (!wifi_preview_start()) return false;
    queue = xQueueCreate(1, sizeof(Frame));
    if (!queue) return false;
    if (xTaskCreate(preview_task, "CameraPreview", 6144, nullptr, 1, nullptr) != pdPASS) {
        vQueueDelete(queue);
        queue = nullptr;
        return false;
    }
    ESP_LOGI("PREVIEW", "Wi-Fi camera preview enabled; images are no longer sent over USB");
    return true;
}

void camera_preview_submit(const who::detect::WhoDetect::result_t &result, const CountSnapshot &counts)
{
    const int64_t now = esp_timer_get_time();
    if (!queue || !wifi_preview_requested() || now - last_submit < 120000 || uxQueueMessagesWaiting(queue)) return;
    const auto &img = result.img;
    if (img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565BE &&
        img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565LE) return;
    Frame frame = {};
    frame.counts = counts;
    frame.width = img.width;
    frame.height = img.height;
    frame.bytes = img.bytes();
    frame.format = img.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB565BE ?
                   JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_RGB565_LE;
    frame.pixels = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, frame.bytes, MALLOC_CAP_SPIRAM));
    if (!frame.pixels) return;
    // The capture buffer is reused after this callback; own a copy for the preview worker.
    memcpy(frame.pixels, img.data, frame.bytes);
    frame.count = result.det_res.size();
    unsigned index = 0;
    for (const auto &det : result.det_res) {
        if (frame.box_count == 32) break;
        if (det.box.size() >= 4) frame.boxes[frame.box_count++] = {det.box[0], det.box[1], det.box[2], det.box[3], det.score, counts.ids[index], counts.states[index]};
        ++index;
        if (index == 32) break;
    }
    if (xQueueSend(queue, &frame, 0) != pdTRUE) heap_caps_free(frame.pixels);
    else last_submit = now;
}
