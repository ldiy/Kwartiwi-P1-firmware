#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "emucs_p1.h"
#include "networking.h"


void app_main(void) {
    esp_log_level_set("emucs_p1", ESP_LOG_DEBUG);
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

    // Initialize networking
    esp_log_level_set("networking", ESP_LOG_DEBUG);
    xTaskCreate(setup_networking_task, "setup_networking_task", 4096, NULL, 5, NULL);
}
