#include <stdio.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "emucs_p1.h"



void app_main(void)
{
    esp_log_level_set("emucs_p1", ESP_LOG_INFO);
    xTaskCreate(emucs_p1_task, "emucs_p1_task", 4096, NULL, 5, NULL);
}
