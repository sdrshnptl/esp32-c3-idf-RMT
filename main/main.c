/*
 * ESP32-C3 IR Remote Controller — application entry point.
 *
 * This file stays thin on purpose: it wires components together in the correct
 * order and then returns. All logic lives in components/.
 */

#include "board.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_indicator.h"

static const char *TAG = "ir-remote";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C3 IR remote controller booting (IDF %s)", esp_get_idf_version());

    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(led_indicator_init());

    /* Visual bring-up: three quick flashes, then the advertising pulse. */
    ESP_ERROR_CHECK(led_indicator_set_state(LED_STATE_BOOT));
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_ERROR_CHECK(led_indicator_set_state(LED_STATE_ADVERTISING));

    ESP_LOGI(TAG, "boot complete");
}
