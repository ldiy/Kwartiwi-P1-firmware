/**
 * @file predict_peak.c
 *
 * @brief Predict the peak of the current average demand at the end of the current quarter-hour
 *
 * There are two methods of predicting the peak:
 *   - Linear regression
 *   - Weighted average
 * The method used is determined by a variable in the NVS.
 */
#include <esp_types.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "logger.h"
#include "emucs_p1.h"
#include "predict_peak.h"

#define MAX_ITEM_COUNT (60 * 15 * 1000 / EMUCS_P1_TELEGRAM_INTERVAL_MS) // The maximum number of items in the short-term log

static const char *TAG = "predict_peak";
struct predicted_peak_s predicted_peak;
SemaphoreHandle_t predicted_peak_mutex;

// Function prototypes
static time_t get_timestamp_at_end_of_quarter_hour(time_t timestamp);
static struct predicted_peak_s predict_peak_linear_regression(log_entry_short_term_p1_data_t log_entry[], uint16_t item_count);
static struct predicted_peak_s predict_peak_weighted_average(log_entry_short_term_p1_data_t log_entry[], uint16_t item_count);

/**
 * @brief Predict the peak of the current average demand at the end of the current quarter-hour
 *
 * @param pvParameters Pointer to the interval at which the task will run (in ticks) (TickType_t *)
 */
_Noreturn void predict_peak_task(void *pvParameters) {
    ESP_LOGD(TAG, "predict_peak_task started");
    SemaphoreHandle_t short_term_log_mutex;
    size_t item_count;
    struct tm * tm_ptr;
    struct predicted_peak_s predicted_peak_temp;
    enum predict_peak_method_e predict_peak_method;
    uint8_t predict_peak_method_temp;
    log_entry_short_term_p1_data_t *log_entry;
    // Set the interval at which the task will run from pvParameters
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(PREDICT_PEAK_TASK_INTERVAL_MS);

    short_term_log_mutex = logger_get_short_term_log_mutex_handle();
    if (short_term_log_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to get short_term_log_mutex");
        vTaskDelete(NULL);
        assert(0); // Should never get here
    }

    log_entry = malloc(MAX_ITEM_COUNT * sizeof(log_entry_short_term_p1_data_t));
    if (log_entry == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for log_entry");
        vTaskDelete(NULL);
        assert(0); // Should never get here
    }

    predicted_peak_mutex = xSemaphoreCreateMutex();
    if (predicted_peak_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create predicted_peak_mutex");
        free(log_entry);
        vTaskDelete(NULL);
        assert(0); // Should never get here
    }

    // Read settings from NVS
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open(PREDICT_PEAK_NVS_NAMESPACE, NVS_READONLY, &nvs_handle));
    ESP_ERROR_CHECK(nvs_get_u8(nvs_handle, PREDICT_PEAK_NVS_KEY_METHOD, &predict_peak_method_temp));
    nvs_close(nvs_handle);
    predict_peak_method = (enum predict_peak_method_e)predict_peak_method_temp;


    for(;;) {
        // Copy the short term log to a local buffer, sorted on entry timestamp
        xSemaphoreTake(short_term_log_mutex, portMAX_DELAY);
        item_count = logger_get_short_term_log_items(log_entry, MAX_ITEM_COUNT);
        xSemaphoreGive(short_term_log_mutex);
        if (item_count > 1) {

            // Find the first entry that starts at the beginning of a quarter-hour, i.e. 00, 15, 30 or 45 minutes
            // If no such entry is found, use the first entry available
            uint16_t first_entry_index = 0;
            for (size_t i = 0; i < item_count; i++) {
                tm_ptr = localtime(&log_entry[i].timestamp);
                if (tm_ptr->tm_min % 15 == 0 && tm_ptr->tm_sec == 0) {
                    first_entry_index = i;
                    break;
                }
            }
            ESP_LOGD(TAG, "first_entry_index: %d", first_entry_index);

            // Predict the peak based on the selected method
            switch (predict_peak_method) {
                case PREDICT_PEAK_METHOD_LINEAR_REGRESSION:
                    predicted_peak_temp = predict_peak_linear_regression(&log_entry[first_entry_index], item_count - first_entry_index);
                    break;
                case PREDICT_PEAK_METHOD_WEIGHTED_AVERAGE:
                    predicted_peak_temp = predict_peak_weighted_average(log_entry, item_count);
                    break;
                default:
                    ESP_LOGE(TAG, "Unknown predict_peak_method: %d", predict_peak_method);
                    assert(0); // Should never get here
            }

            ESP_LOGD(TAG, "Predicted peak: %f kW at %s", predicted_peak_temp.value, ctime(&(predicted_peak_temp.timestamp)));

            // Update the global predicted peak, so that it can be read by other tasks
            xSemaphoreTake(predicted_peak_mutex, portMAX_DELAY);
            predicted_peak = predicted_peak_temp;
            xSemaphoreGive(predicted_peak_mutex);

        }

        // Wait for the next cycle
       xTaskDelayUntil(&xLastWakeTime, xFrequency);
    }

    free(log_entry);
    vTaskDelete(NULL);
}

/**
 * @brief Get the semaphore handle of the predicted_peak_mutex
 */
SemaphoreHandle_t predict_peak_get_predicted_peak_mutex_handle(void) {
    return predicted_peak_mutex;
}

/**
 * @brief Get the predicted peak
 *
 * @note This function should only be called after the predicted_peak_mutex has been taken
 *
 * @return The predicted peak
 */
struct predicted_peak_s predict_peak_get_predicted_peak(void) {
    return predicted_peak;
}

/**
 * @brief Calculate the timestamp at the end of the quarter-hour that contains the given timestamp
 *
 * @param timestamp The timestamp in the quarter-hour for which the end timestamp should be calculated
 * @return The calculated timestamp at the end of the quarter-hour
 */
static time_t get_timestamp_at_end_of_quarter_hour(time_t timestamp) {
    struct tm * tm_ptr = localtime(&timestamp);
    tm_ptr->tm_sec = 0;
    tm_ptr->tm_min = (tm_ptr->tm_min / 15 + 1) * 15;
    if (tm_ptr->tm_min == 60) {
        tm_ptr->tm_min = 0;
        tm_ptr->tm_hour++;
    }
    return mktime(tm_ptr);
}

/**
 * @brief Calculate the linear regression of the current_avg_demand values using the least squares method
 *
 * See: https://en.wikipedia.org/wiki/Least_squares and https://web.archive.org/web/20150715022401/http://faculty.cs.niu.edu/~hutchins/csci230/best-fit.htm
 *
 * @todo Give the most recent values a higher weight, otherwise the predicted peak may be lower than the curren avg demand.
 *
 * @param log_entry The log entries to calculate the linear regression for. The entries must be sorted on timestamp, and the first one must be the first entry of a quarter-hour.
 * @param item_count The number of items in the log_entry array
 * @return The predicted peak at the end of the quarter-hour
 */
static struct predicted_peak_s predict_peak_linear_regression(log_entry_short_term_p1_data_t log_entry[], uint16_t item_count) {
    uint32_t timestamp_val;
    uint32_t sum_timestamp = 0;
    uint32_t sum_timestamp_squared = 0;
    float sum_current_avg_demand = 0;
    float sum_timestamp_current_avg_demand = 0;
    float timestamp_mean;
    float current_avg_demand_mean;
    float slope;
    struct predicted_peak_s result;
    time_t last_timestamp = 0;
    float last_avg_demand = 0.0;
    time_t end_timestamp;

    for (uint16_t i = 0; i < item_count; i++) {
        timestamp_val = (log_entry[i].timestamp - log_entry[0].timestamp);
        sum_timestamp += timestamp_val;
        sum_current_avg_demand += log_entry[i].current_avg_demand;
        sum_timestamp_squared += timestamp_val * timestamp_val;
        sum_timestamp_current_avg_demand += (float) timestamp_val * log_entry[i].current_avg_demand;
    }

    // Calculate the mean values
    timestamp_mean = (float) sum_timestamp / (float) item_count;
    current_avg_demand_mean = sum_current_avg_demand / (float) item_count;

    // Calculate the slope and intercept
    slope = (sum_timestamp_current_avg_demand - (float) sum_timestamp * current_avg_demand_mean) /
            ((float) sum_timestamp_squared - (float) sum_timestamp * timestamp_mean);

    // Calculate the timestamp at which the quarter-hour will end
    end_timestamp = get_timestamp_at_end_of_quarter_hour(log_entry[0].timestamp);

    // Get the data of the last entry.
    if (item_count > 0) {
        last_timestamp = log_entry[item_count - 1].timestamp;
        last_avg_demand = log_entry[item_count - 1].current_avg_demand;
    }

    // Calculate the predicted peak at the end of the quarter-hour
    result.value = last_avg_demand + slope * (float) (end_timestamp - last_timestamp);
    result.timestamp = end_timestamp;

    return result;
}

/**
 * @brief Calculate the predicted peak using a weighted average.
 *
 * Calculate the predicted peak using a weighted average of the current_power_usage values
 * The most recent entry has the highest weight, and the weight decreases linearly with the age of the entry.
 * The calculated power usage is used as a constant load for the remaining time of the quarter-hour.
 *
 * @param log_entry The log entries to calculate the weighted average for. The entries must be sorted on timestamp
 * @param item_count The number of items in the log_entry array
 * @return The predicted peak at the end of the quarter-hour
 */
static struct predicted_peak_s predict_peak_weighted_average(log_entry_short_term_p1_data_t log_entry[], uint16_t item_count) {
    struct predicted_peak_s result;
    float sum_weighted_current_power_usage = 0;
    uint32_t sum_weight = 0;
    time_t end_timestamp;

    for (uint16_t i = 0; i < item_count; i++) {
        uint32_t weight = log_entry[i].timestamp - log_entry[0].timestamp + 1;
        sum_weighted_current_power_usage += (float)weight * log_entry[i].current_power_usage;
        sum_weight += weight;
    }

    // Calculate the timestamp at which the quarter-hour will end
    end_timestamp = get_timestamp_at_end_of_quarter_hour(log_entry[0].timestamp);

    // Calculate the predicted peak at the end of the quarter-hour
    result.value = (float) sum_weighted_current_power_usage / (float) sum_weight;
    result.timestamp = end_timestamp;

    return result;
}