#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_event.h"
#include "emucs_p1.h"
#include "networking.h"
#include "logger.h"
#include "web_server.h"
#include "predict_peak.h"

static void * CJSON_CDECL cjson_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void app_main(void) {
    //esp_log_level_set("emucs_p1", ESP_LOG_DEBUG);
    xTaskCreate(emucs_p1_task, "emucs_p1_task", 4096, NULL, 5, NULL);

    //Initialize NVS
    esp_err_t err = nvs_flash_init();
    // Check if NVS partition was truncated
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Erase NVS partition and try again
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Initialize the event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize networking
    esp_log_level_set("networking", ESP_LOG_DEBUG);
    setup_networking();

    // Initialize web server
    esp_log_level_set("web_server", ESP_LOG_DEBUG);
    setup_web_server();

    // Run the logger task
    esp_log_level_set("logger", ESP_LOG_DEBUG);
    xTaskCreate(logger_task, "logger_task", 4096, NULL, 6, NULL);

    // Run the predict peak task
    esp_log_level_set("predict_peak", ESP_LOG_DEBUG);
    xTaskCreate(predict_peak_task, "predict_peak_task", 4096, NULL, 5, NULL);
}
