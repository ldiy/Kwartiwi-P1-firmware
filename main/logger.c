/**
 * @file logger.c
 * @brief This file contains the implementation of the logger task and logger related functions.
 *
 * The logger task is responsible for logging the P1 data to the short term and long term logs.
 * The short term log is a ring buffer of the last 15 minutes of P1 data.
 * The following data is logged to the short term log:
 *   - Timestamp
 *   - Current average demand
 *   - Current power usage
 *
 * @todo Implement long term logging
 */

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "emucs_p1.h"
#include "logger.h"

/**
 * @brief The short term log
 * @details Ring buffer of the last 15 minutes of P1 data.
 *          The index of the next entry to be written is stored in short_term_log_head_index.
 *          The number of items in the log is stored in short_term_log_item_count.
 *          Items are sorted by timestamp, with the oldest entry at the tail and the newest entry at the head.
 */
log_entry_short_term_p1_data_t short_term_log[LOGGER_SHORT_TERM_LOG_SIZE];
static size_t short_term_log_head_index = 0;  // The index of the next entry to be written
static size_t short_term_log_item_count = 0;  // The number of items in the log
SemaphoreHandle_t short_term_log_mutex;
static const char *TAG = "logger";

// Function prototypes
static void add_short_term_log_entry(log_entry_short_term_p1_data_t *entry);
static void log_short_term_p1_data(emucs_p1_data_t *p1_data);


/**
 * @brief Logger task
 *
 * This task waits for a new telegram from the P1 task, then logs the data.
 *
 * @todo Add long term logging
 *
 * @param pvParameters
 */
_Noreturn void logger_task(void *pvParameters) {
    ESP_LOGD(TAG, "Starting logger task");
    EventGroupHandle_t telegram_event_group = emucs_p1_get_event_group_handle();
    SemaphoreHandle_t telegram_mutex = emucs_p1_get_telegram_mutex_handle();
    emucs_p1_data_t *p1_data = emucs_p1_get_telegram();

    if (telegram_event_group == NULL || telegram_mutex == NULL || p1_data == NULL) {
        ESP_LOGE(TAG, "Failed to get handles from emucs_p1");
        vTaskDelete(NULL);
        assert(0); // Should never get here
    }

    short_term_log_mutex = xSemaphoreCreateMutex();

    for(;;) {
        // Wait for a new telegram
        xEventGroupWaitBits(telegram_event_group, EMUCS_P1_EVENT_TELEGRAM_AVAILABLE_BIT, pdTRUE, pdTRUE, portMAX_DELAY);

        // Get the telegram semaphore
        xSemaphoreTake(telegram_mutex, portMAX_DELAY);

        // Log the short term data
        log_short_term_p1_data(p1_data);

        // Log the long term data
        // TODO: Log the long term data

        // Return the semaphore
        xSemaphoreGive(telegram_mutex);
    }
}

/**
 * @brief Add an entry to the short term log
 *
 * @note The short term log mutex must be taken before calling this function
 *
 * @param entry The data entry to add
 */
static void add_short_term_log_entry(log_entry_short_term_p1_data_t *entry) {
    // Add the entry to the log
    short_term_log[short_term_log_head_index] = *entry;

    // Move the head index to the next entry
    short_term_log_head_index = (short_term_log_head_index + 1) % LOGGER_SHORT_TERM_LOG_SIZE;

    // Increment the item count
    if (short_term_log_item_count < LOGGER_SHORT_TERM_LOG_SIZE) {
        short_term_log_item_count++;
    }
}

/**
 * @brief Log a P1 telegram to the short term log
 *
 * @note The short term log mutex must be taken before calling this function
 *
 * @param p1_data The P1 telegram to log
 */
static void log_short_term_p1_data(emucs_p1_data_t *p1_data) {
    ESP_LOGD(TAG, "Logging short term P1 data telegram");
    log_entry_short_term_p1_data_t entry = {
        .timestamp = p1_data->msg_timestamp,
        .current_avg_demand = p1_data->current_avg_demand,
        .current_power_usage = p1_data->current_power_usage
    };

    add_short_term_log_entry(&entry);
}

/**
 * @brief Get the short term log items in chronological order
 *
 * @note The short term log mutex must be taken before calling this function
 *
 * @param log The log to copy the items to, must be at least max_items in size
 * @param max_items The maximum number of items to copy to the log
 * @return The number of items copied to the log
 */
size_t logger_get_short_term_log_items(log_entry_short_term_p1_data_t *log, size_t max_items) {
    // Limit the number of items to the number of items in the log
    if (max_items > short_term_log_item_count) {
        max_items = short_term_log_item_count;
    }

    // Copy the items to the log
    size_t tail_index = (LOGGER_SHORT_TERM_LOG_SIZE + short_term_log_head_index - max_items) % LOGGER_SHORT_TERM_LOG_SIZE;
    for (size_t i = 0; i < max_items; i++) {
        log[i] = short_term_log[(tail_index + i) % LOGGER_SHORT_TERM_LOG_SIZE];
    }


    return max_items;
}

/**
 * @brief Get the short term log mutex handle
 *
 * @return The short term log mutex handle
 */
SemaphoreHandle_t logger_get_short_term_log_mutex_handle(void) {
    return short_term_log_mutex;
}
