// Encodes camera frames as JPEG and publishes the local preview with counting data

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
    // Preview overlay: pixel coordinates, detector score, tracking ID, and crossing display state
    struct Box {
        int x1, y1, x2, y2;
        float score;
        unsigned id;
        uint8_t state;
    };

    // Queue item containing an owned image copy and the count snapshot from the same detection result
    struct Frame {
        uint8_t *pixels;
        int bytes, width, height;
        jpeg_pixel_format_t format;
        unsigned count, box_count;
        Box boxes[32];
        CountSnapshot counts;
    };

    QueueHandle_t preview_queue = nullptr;
    int64_t last_submit_us      = 0;

    // Consumes copied frames outside the detection task, encodes JPEG/Base64, and transfers a JSON packet to Wi-Fi
    // It owns each received pixel buffer and releases it on both success and failure
    void preview_task(void *) {
        // 1. Receive an owned frame and discard it if the viewer has disconnected
        Frame frame;
        while(xQueueReceive(preview_queue, &frame, portMAX_DELAY) == pdTRUE) {
            // A viewer may disconnect after submission, discard queued pixels before doing expensive encoding
            if(!wifi_preview_requested()) {
                heap_caps_free(frame.pixels);
                continue;
            }

            // 2. Configure and encode the image as JPEG
            // Default JPEG initializer supplied by espressif/esp_new_jpeg, invoked unchanged
            // Image dimensions, pixel format, and quality below are project-specific settings
            jpeg_enc_config_t jpeg_config  = DEFAULT_JPEG_ENC_CONFIG();
            jpeg_config.width              = frame.width;
            jpeg_config.height             = frame.height;
            jpeg_config.src_type           = frame.format;
            jpeg_config.quality            = 50;
            jpeg_enc_handle_t jpeg_encoder = nullptr;
            const int jpeg_capacity        = frame.width * frame.height * 2;
            auto jpeg_buffer               = static_cast<uint8_t *>(heap_caps_malloc(jpeg_capacity, MALLOC_CAP_SPIRAM));
            int jpeg_size                  = 0;

            if(jpeg_buffer && jpeg_enc_open(&jpeg_config, &jpeg_encoder) == JPEG_ERR_OK &&
               jpeg_enc_process(jpeg_encoder, frame.pixels, frame.bytes, jpeg_buffer, jpeg_capacity, &jpeg_size) == JPEG_ERR_OK) {
                // 3. Convert the JPEG to Base64 for transport inside JSON
                // Base64 allows the binary JPEG to travel inside JSON. Allocate room for expansion and a terminating zero
                const size_t base64_capacity = 4 * ((jpeg_size + 2) / 3) + 1;
                auto base64_buffer           = static_cast<unsigned char *>(heap_caps_malloc(base64_capacity, MALLOC_CAP_SPIRAM));
                size_t base64_size           = 0;

                if(base64_buffer && mbedtls_base64_encode(base64_buffer, base64_capacity, &base64_size, jpeg_buffer, jpeg_size) == 0) {
                    // 4. Build overlay boxes, matching counts, and the JSON packet
                    base64_buffer[base64_size] = 0;
                    std::string boxes_json     = "[";

                    for(unsigned box_index = 0; box_index < frame.box_count; ++box_index) {
                        char box_json[160];
                        const Box &box = frame.boxes[box_index];
                        snprintf(box_json, sizeof(box_json), "%s[%d,%d,%d,%d,%.3f,%u,%u]", box_index ? ",":"",
                                 box.x1, box.y1, box.x2, box.y2, static_cast<double>(box.score), box.id, box.state);
                        boxes_json += box_json;
                    }
                    boxes_json                  += "]";
                    const auto counts_json       = visitor_counter_json(frame.counts);
                    const size_t packet_capacity = base64_size + boxes_json.size() + counts_json.size() + 200;
                    auto packet                  = static_cast<char *>(heap_caps_malloc(packet_capacity, MALLOC_CAP_SPIRAM));
                    if(packet) {
                        // Package image dimensions, overlay boxes, and the matching count snapshot for the browser
                        const int packet_length = snprintf(
                            packet, packet_capacity,
                            "{\"width\":%d,\"height\":%d,\"people\":%u,\"counter\":%s,\"boxes\":%s,\"jpeg\":\"%s\"}",
                            frame.width, frame.height, frame.count, counts_json.c_str(), boxes_json.c_str(), base64_buffer);

                        // 5. Publish a complete packet, otherwise release its buffer
                        if(packet_length > 0 && static_cast<size_t>(packet_length) < packet_capacity)
                            wifi_preview_publish(packet, packet_length);
                        else
                            heap_caps_free(packet);
                    }
                }
                heap_caps_free(base64_buffer);
            } else {
                ESP_LOGW("PREVIEW", "JPEG encoding failed; frame skipped");
            }
            // Release encoder resources and the owned JPEG and pixel buffers
            if(jpeg_encoder)
                jpeg_enc_close(jpeg_encoder);
            heap_caps_free(jpeg_buffer);
            heap_caps_free(frame.pixels);
        }
        vTaskDelete(nullptr);
    }
}

// Starts networking, a one-frame queue, and the encoder worker, returns false if required resources cannot start
bool camera_preview_start() {
    if(!wifi_preview_start())
        return false;

    preview_queue = xQueueCreate(1, sizeof(Frame));
    if(!preview_queue)
        return false;

    if(xTaskCreate(preview_task, "CameraPreview", 6144, nullptr, 1, nullptr) != pdPASS) {
        vQueueDelete(preview_queue);
        preview_queue = nullptr;
        return false;
    }
    ESP_LOGI("PREVIEW", "Wi-Fi camera preview enabled; images are no longer sent over USB");
    return true;
}


// Offers a preview without blocking inference. Skips work unless a viewer is active, time allows, and the queue is free
// Copies pixels because the detector reuses its image buffer after the callback returns
void camera_preview_submit(const who::detect::WhoDetect::result_t &result, const CountSnapshot &counts) {
    // 1. Check viewer demand, submission interval, and queue availability
    const int64_t now_us = esp_timer_get_time();
    // Throttle preview submissions to at most one every 120 ms and never build a backlog behind the encoder
    if(!preview_queue || !wifi_preview_requested() || now_us - last_submit_us < 120000 || uxQueueMessagesWaiting(preview_queue))
        return;

    // 2. Accept only the supported RGB565 pixel formats
    const auto &image = result.img;
    if(image.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565BE && image.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565LE)
        return;

    // 3. Prepare frame metadata and copy pixels into an owned buffer
    Frame frame  = {};
    frame.counts = counts;
    frame.width  = image.width;
    frame.height = image.height;
    frame.bytes  = image.bytes();
    frame.format = image.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB565BE ? JPEG_PIXEL_FORMAT_RGB565_BE:JPEG_PIXEL_FORMAT_RGB565_LE;
    frame.pixels = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, frame.bytes, MALLOC_CAP_SPIRAM));
    if(!frame.pixels)
        return;

    // The capture buffer is reused after this callback, own a copy for the preview worker
    memcpy(frame.pixels, image.data, frame.bytes);

    // 4. Match overlay boxes to detection IDs and display states
    frame.count              = result.det_res.size();
    unsigned detection_index = 0;

    // Keep overlay entries aligned with detector order so IDs/states refer to the correct boxes
    for(const auto &detection : result.det_res) {
        if(frame.box_count == 32)
            break;
        if(detection.box.size() >= 4)
            frame.boxes[frame.box_count++] = {detection.box[0], detection.box[1], detection.box[2], detection.box[3],
                                              detection.score, counts.ids[detection_index], counts.states[detection_index]};
        ++detection_index;
        if(detection_index == 32)
            break;
    }

    // 5. Transfer the frame to the queue or release pixels if the queue is full
    // A successful send transfers pixel ownership to preview_task, a full queue leaves cleanup with this caller
    if(xQueueSend(preview_queue, &frame, 0) != pdTRUE)
        heap_caps_free(frame.pixels);
    else
        last_submit_us = now_us;
}
