#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define LOGGER_SHORT_TERM_LOG_FREQUENCY_MS EMUCS_P1_TELEGRAM_INTERVAL_MS
#define LOGGER_SHORT_TERM_LOG_DURATION_S (60 * 15)
#define LOGGER_SHORT_TERM_LOG_SIZE ((LOGGER_SHORT_TERM_LOG_DURATION_S * 1000) / LOGGER_SHORT_TERM_LOG_FREQUENCY_MS)
#define LOGGER_LONG_TERM_LOG_FREQUENCY_S (60 * 60 * 24)

typedef struct {
    time_t timestamp;
    float current_avg_demand;
    float current_power_usage;
} log_entry_short_term_p1_data_t;

typedef struct {
    time_t timestamp;
    float electricity_delivered_tariff1;
    float electricity_delivered_tariff2;
    float electricity_returned_tariff1;
    float electricity_returned_tariff2;
} log_entry_long_term_p1_data_t;

// Function prototypes
_Noreturn void logger_task(void *pvParameters);
size_t logger_get_short_term_log_items(log_entry_short_term_p1_data_t *log, size_t max_items);
SemaphoreHandle_t logger_get_short_term_log_mutex_handle(void);

#endif //LOGGER_H
