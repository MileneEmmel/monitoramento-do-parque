// Initializes the PIR sensor, reads its signal, and configures GPIO wakeup

#include "pir_sensor.hpp"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include <atomic>

namespace {
    constexpr gpio_num_t PIR_GPIO = GPIO_NUM_1;
    std::atomic<bool> initialized{false};
}

// Returns GPIO1 from RTC wake mode to a normal polled input with a pull-down, safe to call repeatedly
bool pir_sensor_init() {
    // 1. Keep an already configured normal GPIO input available
    if(initialized)
        return true;

    // 2. Return the pin from RTC mode to the normal GPIO path
    if(rtc_gpio_deinit(PIR_GPIO) != ESP_OK)
        return false;

    // 3. Configure the polled input and publish the initialization result
    gpio_config_t gpio_input_config = {};
    gpio_input_config.pin_bit_mask  = 1ULL << PIR_GPIO;
    gpio_input_config.mode          = GPIO_MODE_INPUT;
    gpio_input_config.pull_down_en  = GPIO_PULLDOWN_ENABLE;
    gpio_input_config.pull_up_en    = GPIO_PULLUP_DISABLE;
    gpio_input_config.intr_type     = GPIO_INTR_DISABLE;
    initialized                     = gpio_config(&gpio_input_config) == ESP_OK;
    return initialized;
}

// Returns 0/1 for a configured input, or -1 when the normal GPIO path is unavailable
int pir_sensor_level() {
    return initialized ? gpio_get_level(PIR_GPIO):-1;
}

// Moves GPIO1 to the RTC input path and arms wake on a high level
// Low is checked both before and after configuration to detect motion arriving during the transition
bool pir_sensor_arm_wakeup() {
    // 1. Confirm the PIR signal is low before changing the pin mode
    if(pir_sensor_level() != 0)
        return false;

    // 2. Mark normal GPIO reads as unavailable during the RTC transition
    initialized = false;

    // 3. Configure the RTC input and its pull-down
    if(rtc_gpio_init(PIR_GPIO) != ESP_OK || rtc_gpio_set_direction(PIR_GPIO, RTC_GPIO_MODE_INPUT_ONLY) != ESP_OK ||
       rtc_gpio_pullup_dis(PIR_GPIO) != ESP_OK || rtc_gpio_pulldown_en(PIR_GPIO) != ESP_OK)
        return false;

    // 4. Arm high-level wakeup and confirm the signal is still low
    return esp_sleep_enable_ext0_wakeup(PIR_GPIO, 1) == ESP_OK && rtc_gpio_get_level(PIR_GPIO) == 0;
}

// Returns pin availability and a raw level for diagnostics, it does not count people or change activity timing
cJSON *pir_sensor_status() {
    const bool sensor_initialized = initialized.load();
    auto json                     = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", sensor_initialized);
    cJSON_AddNumberToObject(json, "gpio", PIR_GPIO);
    if(sensor_initialized)
        cJSON_AddNumberToObject(json, "level", pir_sensor_level());
    else
        cJSON_AddStringToObject(json, "error", "gpio_init_failed");
    cJSON_AddNumberToObject(json, "uptime_ms", esp_timer_get_time() / 1000);
    return json;
}
