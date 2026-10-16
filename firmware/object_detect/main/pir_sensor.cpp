#include "pir_sensor.hpp"
#include "driver/gpio.h"
#include "esp_timer.h"

cJSON *pir_sensor_status()
{
    constexpr gpio_num_t pin = GPIO_NUM_1;
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_INPUT;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t result = gpio_config(&config);
    auto json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", result == ESP_OK);
    cJSON_AddNumberToObject(json, "gpio", pin);
    if (result == ESP_OK) cJSON_AddNumberToObject(json, "level", gpio_get_level(pin));
    else cJSON_AddStringToObject(json, "error", esp_err_to_name(result));
    cJSON_AddNumberToObject(json, "uptime_ms", esp_timer_get_time() / 1000);
    return json;
}
