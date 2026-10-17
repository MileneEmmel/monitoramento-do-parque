#include "pir_sensor.hpp"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include <atomic>

namespace { constexpr gpio_num_t pin = GPIO_NUM_1; std::atomic<bool> initialized{false}; }
bool pir_sensor_init()
{
    if (initialized) return true;
    if (rtc_gpio_deinit(pin) != ESP_OK) return false;
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_INPUT;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    initialized = gpio_config(&config) == ESP_OK;
    return initialized;
}
int pir_sensor_level() { return initialized ? gpio_get_level(pin) : -1; }
bool pir_sensor_arm_wakeup()
{
    if (pir_sensor_level() != 0) return false;
    initialized = false;
    if (rtc_gpio_init(pin) != ESP_OK || rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY) != ESP_OK ||
        rtc_gpio_pullup_dis(pin) != ESP_OK || rtc_gpio_pulldown_en(pin) != ESP_OK) return false;
    return esp_sleep_enable_ext0_wakeup(pin, 1) == ESP_OK && rtc_gpio_get_level(pin) == 0;
}
cJSON *pir_sensor_status()
{
    const bool ok = initialized.load();
    auto json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", ok);
    cJSON_AddNumberToObject(json, "gpio", pin);
    if (ok) cJSON_AddNumberToObject(json, "level", pir_sensor_level());
    else cJSON_AddStringToObject(json, "error", "gpio_init_failed");
    cJSON_AddNumberToObject(json, "uptime_ms", esp_timer_get_time() / 1000);
    return json;
}
