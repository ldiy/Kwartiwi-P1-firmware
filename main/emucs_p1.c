/**
 * @file emucs_p1.c
 * @brief eMUCS P1 reader
 * @details This file contains functions for reading the P1 port of a DSMR 5.0 compatible smart meter.
 * Data is read from the P1 port using UART and is parsed into a struct.
 * @todo Add support for M-Bus data in the telegram
 */

#include <sys/cdefs.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "emucs_p1.h"

#define P1_DATA_PIN 5
#define UART_RING_BUFFER_SIZE 1024
#define UART_NUM UART_NUM_1
#define UART_QUEUE_SIZE 10
#define TELEGRAM_BUFFER_SIZE 1500
#define UART_BAUD_RATE 115200


static const char *TAG = "emucs_p1";                // Tag for logging
static uint8_t uart_buffer[TELEGRAM_BUFFER_SIZE];   // Buffer for storing uart data to find the telegram in
emucs_p1_data_t p1_telegram;                        // Struct for storing the parsed telegram
SemaphoreHandle_t p1_telegram_mutex;                // Mutex for accessing the telegram struct

// Function prototypes
static void process_p1_data(size_t size);
static void parse_telegram(uint8_t * telegram, size_t size);
static bool check_telegram_crc(uint8_t * telegram, size_t size);
static uint16_t crc16(uint8_t * data, size_t size);
static inline bool starts_with(const char * line, const char * needle);
static esp_err_t get_string_between_chars(const char * src, char start, char end, char * result, size_t max_size);
static time_t get_timestamp_between_chars(const char * src, char start, char end);
static float get_float_between_chars(const char * src, char start, char end);
static uint32_t get_uint32_between_chars(const char * src, char start, char end);


/**
 * @brief Task that reads data from the P1 port and processes it
 *
 * @param pvParameters
 */
_Noreturn void emucs_p1_task(void *pvParameters) {
    uart_event_t event;
    QueueHandle_t uart_queue;

    // UART configuration (8N1)
    uart_config_t uart_config = {
            .baud_rate = UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_LOGD(TAG, "Configuring UART");

    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    // Set UART pins (Only RX is used)
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_PIN_NO_CHANGE, P1_DATA_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // Invert RX signal
    ESP_ERROR_CHECK(uart_set_line_inverse(UART_NUM, UART_SIGNAL_RXD_INV));

    // Install UART driver using an event queue
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_RING_BUFFER_SIZE, 0, UART_QUEUE_SIZE, &uart_queue, 0));

    // Create a mutex for the telegram data
    p1_telegram_mutex = xSemaphoreCreateMutex();

    if (p1_telegram_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create telegram mutex");
        assert(false);
    }

    for(;;) {
        if(xQueueReceive(uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
            switch (event.type) {
                case UART_DATA:
                    ESP_LOGD(TAG, "[UART DATA]: %d", event.size);
                    process_p1_data(event.size);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART buffer full");
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART FIFO overflow");
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGW(TAG, "UART frame error");
                    break;

                default:
                    ESP_LOGW(TAG, "UART event unknown. Event type: %d", event.type);
                    break;
            }
        }
    }
}

/**
 * @brief Get the telegram data
 * @note Be sure to lock the mutex before reading the data
 *
 * @return A pointer to the telegram data
 */
emucs_p1_data_t * emucs_p1_get_telegram(void) {
        return &p1_telegram;
}

/**
 * @brief Get the handle to the mutex that protects the telegram data
 *
 * @return The handle to the mutex
 */
SemaphoreHandle_t emucs_p1_get_telegram_mutex_handle(void) {
    return p1_telegram_mutex;
}

/**
 * @brief Read size bytes from the UART and process them.
 *
 * @param[in] size The number of bytes to read from the UART
 */
static void process_p1_data(size_t size) {
    static enum {
        P1_STATE_IDLE,
        P1_STATE_DATA,
        P1_STATE_END,
    } state = P1_STATE_IDLE;

    static uint8_t * uart_buffer_index = uart_buffer;
    static uint8_t * telegram_start = NULL;
    static size_t telegram_size = 0;

    // Check if there is enough space in the buffer
    if (uart_buffer_index + size > uart_buffer + TELEGRAM_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Not enough space in the uart buffer. Resetting state");
        // TODO: In some cases it might be possible to recover from this error, but for now we just reset the state
        //  and hope that the next telegram will be received correctly
        state = P1_STATE_IDLE;
        uart_buffer_index = uart_buffer;
        telegram_start = NULL;
        telegram_size = 0;
        return;
    }

    // Read data from the UART
    int bytes_read = uart_read_bytes(UART_NUM, uart_buffer_index, size, 0);

    // Check if the read was successful
    if (bytes_read == -1) {
        ESP_LOGE(TAG, "Error reading from UART");
        return;
    }
    else if (bytes_read != size) {
        ESP_LOGE(TAG, "Not all bytes were read from UART, expected %d, got %d", size, bytes_read);
        return;
    }

    // Process the received data
    for (uint8_t * data = uart_buffer_index; data < uart_buffer_index + size; data++) {
        switch (state) {
            case P1_STATE_IDLE:
                if (*data == '/') {
                    ESP_LOGD(TAG, "Telegram start found");
                    state = P1_STATE_DATA;
                    telegram_start = data;
                }
                break;
            case P1_STATE_DATA:
                if (*data == '!') {
                    ESP_LOGD(TAG, "Telegram end found");
                    state = P1_STATE_END;
                }
                break;
            case P1_STATE_END:
                if (*(data - 1) == '\r' && *data == '\n') {
                    state = P1_STATE_IDLE;

                    // Calculate the size of the telegram
                    telegram_size = data - telegram_start + 1;
                    ESP_LOGI(TAG, "Complete telegram found with size: %d", telegram_size);

                    // Replace the last '\n' with a '\0' terminator
                    *data = '\0';

                    // Parse the telegram
                    parse_telegram(telegram_start, telegram_size);

                    // Reset the telegram start, end and size
                    telegram_start = NULL;
                    telegram_size = 0;
                }
                break;
        }
    }

    // Move the data to the start of the buffer if we are in the P1_STATE_DATA state and the telegram start is not at
    // the start of the buffer. Otherwise, the buffer size may be not sufficient to receive the complete telegram.
    if (state == P1_STATE_DATA && telegram_start != uart_buffer) {
        ESP_LOGD(TAG, "Moving received data to the start of the buffer");
        // Calculate the size of the data after the telegram start
        size = uart_buffer_index + size - telegram_start;
        // Move the data to the start of the buffer
        memmove(uart_buffer, telegram_start, size);
        // Update the buffer index and telegram start
        uart_buffer_index = uart_buffer + size;
        telegram_start = uart_buffer;
    }
    else {
        // Move the index to the next free space in the buffer
        uart_buffer_index += size;
    }
}

/**
 * @brief Parse the telegram.
 *
 * @note The content of the last two bytes doesn't matter (normally '\\r' and '\\n')
 * @todo Parse M-Bus data
 *
 * @param[in] telegram The c string containing the telegram
 * @param[in] size The size of the telegram
 */
static void parse_telegram(uint8_t * telegram, size_t size) {
    // Check if the telegram CRC16 is correct
    if (!check_telegram_crc(telegram, size)) {
        ESP_LOGW(TAG, "Telegram CRC16 is incorrect");
        return;
    }

    // Get the telegram semaphore
    xSemaphoreTake(p1_telegram_mutex, portMAX_DELAY);

    // Parse the telegram
    ESP_LOGD(TAG, "Parsing telegram...");
    //emucs_p1_data_t p1_telegram;
    memset(&p1_telegram, 0, sizeof(emucs_p1_data_t));
    // Read the telegram line by line
    char * line = strtok((char *) telegram, "\r\n");
    while (line != NULL) {
        // Version information
        if (starts_with(line, "0-0:96.1.4")) {
            get_string_between_chars(line, '(', ')', p1_telegram.version_info, sizeof(p1_telegram.version_info));
            ESP_LOGD(TAG, "Version info: %s", p1_telegram.version_info);
        }
        // Equipment identifier
        else if (starts_with(line, "0-0:96.1.1")) {
            get_string_between_chars(line, '(', ')', p1_telegram.equipment_id, sizeof(p1_telegram.equipment_id));
            ESP_LOGD(TAG, "Equipment ID: %s", p1_telegram.equipment_id);
        }
        // Timestamp
        else if (starts_with(line, "0-0:1.0.0")) {
           p1_telegram.msg_timestamp =  get_timestamp_between_chars(line, '(', ')');
           ESP_LOGD(TAG, "Timestamp: %lli", p1_telegram.msg_timestamp);
        }
        // Electricity delivered to client (Tariff 1)
        else if (starts_with(line, "1-0:1.8.1")) {
            p1_telegram.electricity_delivered_tariff1 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Electricity delivered to client (low tariff): %f kWh", p1_telegram.electricity_delivered_tariff1);
        }
        // Electricity delivered to client (Tariff 2)
        else if (starts_with(line, "1-0:1.8.2")) {
            p1_telegram.electricity_delivered_tariff2 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Electricity delivered to client (high tariff): %f kWh", p1_telegram.electricity_delivered_tariff2);
        }
        // Electricity delivered by client (Tariff 1)
        else if (starts_with(line, "1-0:2.8.1")) {
            p1_telegram.electricity_returned_tariff1 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Electricity delivered by client (low tariff): %f kWh", p1_telegram.electricity_returned_tariff1);
        }
        // Electricity delivered by client (Tariff 2)
        else if (starts_with(line, "1-0:2.8.2")) {
            p1_telegram.electricity_returned_tariff2 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Electricity delivered by client (high tariff): %f kWh", p1_telegram.electricity_returned_tariff2);
        }
        // Tariff indicator electricity
        else if (starts_with(line, "0-0:96.14.0")) {
            p1_telegram.tariff_indicator = (uint16_t)get_uint32_between_chars(line, '(', ')');
            ESP_LOGD(TAG, "Tariff indicator electricity: %d", p1_telegram.tariff_indicator);
        }
        // Current average demand - Active energy import
        else if (starts_with(line, "1-0:1.4.0")) {
            p1_telegram.current_avg_demand = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Current average demand: %f kW", p1_telegram.current_avg_demand);
        }
        // Maximum demand - Active energy import of the running month
        else if (starts_with(line, "1-0:1.6.0")) {
            char * next;
            p1_telegram.max_demand_month.timestamp = get_timestamp_between_chars(line, '(', ')');
            next = strchr(line, ')');
            p1_telegram.max_demand_month.max_demand = get_float_between_chars(next, '(', '*');
            ESP_LOGD(TAG, "Maximum demand of the running month: %f kW at %lli", p1_telegram.max_demand_month.max_demand, p1_telegram.max_demand_month.timestamp);
        }
        // Maximum demand - Active energy import of the last 13 months
        else if (starts_with(line, "0-0:98.1.0")) {
            uint8_t months_available = (uint8_t)get_uint32_between_chars(line, '(', ')');
            ESP_LOGD(TAG, "%d months available", months_available);
            // There are 2 (1-0:1.6.0) strings before the actual data
            char * next = strchr(line, ')') + 1;
            next = strchr(next, ')') + 1;

            for(uint8_t i = 0; i < months_available; i++) {
                next = strchr(next, ')') + 1;
                next = strchr(next, ')') + 1;
                p1_telegram.max_demand_year[i].timestamp_appearance = get_timestamp_between_chars(next, '(', ')');
                next = strchr(next, ')') + 1;
                p1_telegram.max_demand_year[i].max_demand = get_float_between_chars(next, '(', '*');
                ESP_LOGD(TAG, "Maximum demand of the last 13 months: %f kW at %lli", p1_telegram.max_demand_year[i].max_demand, p1_telegram.max_demand_year[i].timestamp_appearance);
            }
        }
        // Actual electricity power delivered to client from the grid (+P)
        else if (starts_with(line, "1-0:1.7.0")) {
            p1_telegram.current_power_usage = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Actual electricity power delivered to client from the grid (+P): %f kW", p1_telegram.current_power_usage);
        }
        // Actual electricity power delivered by client to the grid (-P)
        else if (starts_with(line, "1-0:2.7.0")) {
            p1_telegram.current_power_return = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Actual electricity power delivered by client to the grid (-P): %f kW", p1_telegram.current_power_return);
        }
        // Instantaneous active power L1 (+P)
        else if (starts_with(line, "1-0:21.7.0")) {
            p1_telegram.current_power_usage_l1 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Instantaneous active power L1 (+P): %f kW", p1_telegram.current_power_usage_l1);
        }
        // Instantaneous active power L2 (+P)
        else if (starts_with(line, "1-0:41.7.0")) {
            p1_telegram.current_power_usage_l2 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Instantaneous active power L2 (+P): %f kW", p1_telegram.current_power_usage_l2);
        }
        // Instantaneous active power L3 (+P)
        else if (starts_with(line, "1-0:61.7.0")) {
            p1_telegram.current_power_usage_l3 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Instantaneous active power L3 (+P): %f kW", p1_telegram.current_power_usage_l3);
        }
        // Instantaneous active power L1 (-P)
        else if (starts_with(line, "1-0:22.7.0")) {
            p1_telegram.current_power_return_l1 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Instantaneous active power L1 (-P): %f kW", p1_telegram.current_power_return_l1);
        }
        // Instantaneous active power L2 (-P)
        else if (starts_with(line, "1-0:42.7.0")) {
            p1_telegram.current_power_return_l2 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Instantaneous active power L2 (-P): %f kW", p1_telegram.current_power_return_l2);
        }
        // Instantaneous active power L3 (-P)
        else if (starts_with(line, "1-0:62.7.0")) {
            p1_telegram.current_power_return_l3 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Instantaneous active power L3 (-P): %f kW", p1_telegram.current_power_return_l3);
        }
        // Voltage L1
        else if (starts_with(line, "1-0:32.7.0")) {
            p1_telegram.voltage_l1 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Voltage L1: %f V", p1_telegram.voltage_l1);
        }
        // Voltage L2
        else if (starts_with(line, "1-0:52.7.0")) {
            p1_telegram.voltage_l2 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Voltage L2: %f V", p1_telegram.voltage_l2);
        }
        // Voltage L3
        else if (starts_with(line, "1-0:72.7.0")) {
            p1_telegram.voltage_l3 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Voltage L3: %f V", p1_telegram.voltage_l3);
        }
        // Current L1
        else if (starts_with(line, "1-0:31.7.0")) {
            p1_telegram.current_l1 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Current L1: %f A", p1_telegram.current_l1);
        }
        // Current L2
        else if (starts_with(line, "1-0:51.7.0")) {
            p1_telegram.current_l2 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Current L2: %f A", p1_telegram.current_l2);
        }
        // Current L3
        else if (starts_with(line, "1-0:71.7.0")) {
            p1_telegram.current_l3 = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Current L3: %f A", p1_telegram.current_l3);
        }
        // Breaker state
        else if (starts_with(line, "0-0:96.3.10")) {
            p1_telegram.breaker_state = (emucs_p1_breaker_state_t)get_uint32_between_chars(line, '(', ')');
            ESP_LOGD(TAG, "Breaker state: %d", p1_telegram.breaker_state);
        }
        // Limiter threshold
        else if (starts_with(line, "0-0:17.0.0")) {
            p1_telegram.limiter_threshold = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Limiter threshold: %f kW", p1_telegram.limiter_threshold);
        }
        // Fuse supervision threshold
        else if (starts_with(line, "1-0:31.4.0")) {
            p1_telegram.fuse_supervision_threshold = get_float_between_chars(line, '(', '*');
            ESP_LOGD(TAG, "Fuse supervision threshold: %f A", p1_telegram.fuse_supervision_threshold);
        }
        // Text message
        else if (starts_with(line, "0-0:96.13.1")) {
            // Not implemented
        }
        // TODO: M-Bus devices

        line = strtok(NULL, "\r\n");
    }

    // Release the semaphore
    xSemaphoreGive(p1_telegram_mutex);
}

/**
 * Check if a string starts with a specific string
 *
 * @param[in] line The string to check
 * @param[in] needle The string to check for
 * @return True if the string starts with the needle, false otherwise
 */
static inline bool starts_with(const char * line, const char * needle) {
    return strncmp(line, needle, strlen(needle)) == 0;
}

/**
 * @brief Get a string between two characters
 *
 * @param[in] src The source string
 * @param[in] start The start character (the character before the string)
 * @param[in] end The end character (the character after the string)
 * @param[out] result The resulting string
 * @param[in] max_size The maximum size of the resulting string
 * @return  - ESP_OK if successful
 *          - APP_ERROR_FAIL otherwise
 */
static esp_err_t get_string_between_chars(const char * src, const char start, const char end, char * result, size_t max_size) {
    // Find the start character
    const char * start_pos = strchr(src, start);
    if (start_pos == NULL) {
        ESP_LOGW(TAG, "Start character '%c' not found", start);
        return ESP_FAIL;
    }

    // Find the end character
    const char * end_pos = strchr(start_pos, end);
    if (end_pos == NULL) {
        ESP_LOGW(TAG, "End character '%c' not found", end);
        return ESP_FAIL;
    }

    // Copy the string between the start and end character to the result and add a null terminator
    size_t length = end_pos - start_pos - 1;
    if (length >= max_size) {
        ESP_LOGW(TAG, "String between '%c' and '%c' longer then max_size.", start, end);
        length = max_size - 1;
    }
    strncpy(result, start_pos + 1, length);
    result[length] = '\0';

    return ESP_OK;
}

/**
 * @brief Get the timestamp string between the start and end character and parse it to a time_t
 *
 * @note The timestamp string format is YYMMDDhhmmss
 *
 * @param[in] src The source string where the timestamp string is located in
 * @param[in] start The start character (character before the timestamp string)
 * @param[in] end The end character (character after the timestamp string)
 * @return The timestamp as a time_t, 0 if failed
 */
static time_t get_timestamp_between_chars(const char * src, const char start, const char end) {
    char timestamp_str[14];

    // Get the timestamp string between the start and end character
    if (get_string_between_chars(src, start, end, timestamp_str, sizeof(timestamp_str)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get timestamp string between '%c' and '%c'", start, end);
        return 0;
    }

    // Convert the timestamp string to a time_t
    // Timestamp string format: YYMMDDhhmmss
    struct tm timestamp;
    memset(&timestamp, 0, sizeof(struct tm));
    timestamp.tm_year = (timestamp_str[0] - '0') * 10 + (timestamp_str[1] - '0') + 2000 - 1900;
    timestamp.tm_mon = (timestamp_str[2] - '0') * 10 + (timestamp_str[3] - '0') - 1;
    timestamp.tm_mday = (timestamp_str[4] - '0') * 10 + (timestamp_str[5] - '0');
    timestamp.tm_hour = (timestamp_str[6] - '0') * 10 + (timestamp_str[7] - '0');
    timestamp.tm_min = (timestamp_str[8] - '0') * 10 + (timestamp_str[9] - '0');
    timestamp.tm_sec = (timestamp_str[10] - '0') * 10 + (timestamp_str[11] - '0');
    return mktime(&timestamp);
}

/**
 * @brief Get the float string between the start and end character and parse it to a float
 *
 * @param[in] src The source string where the float string is located in
 * @param[in] start The start character (character before the float string)
 * @param[in] end The end character (character after the float string)
 * @return The float. If the conversion was unsuccessful, 0 is returned
 */
static float get_float_between_chars(const char * src, const char start, const char end) {
    char float_str[20];
    float result;
    char * end_ptr = NULL;

    // Get the float string between the start and end character
    if (get_string_between_chars(src, start, end, float_str, sizeof(float_str)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get float string between '%c' and '%c'", start, end);
        return 0;
    }

    // Convert the float string to a float
    errno = 0;
    result = (float)strtod(float_str, &end_ptr);
    if((result == 0 && errno != 0) || end_ptr == float_str) {
        ESP_LOGE(TAG, "Failed to convert string '%s' to float", float_str);
    }

    return result;
}

/**
 * @brief Get the uint32 string between the start and end character and parse it to an uint16
 *
 * @param[in] src The source string where the uint16 string is located in
 * @param[in] start The start character (character before the uint32 string)
 * @param[in] end The end character (character after the uint32 string)
 * @return The result as an uint32. If the conversion was unsuccessful, 0 is returned
 */
static uint32_t get_uint32_between_chars(const char * src, const char start, const char end) {
    char uint16_str[11];
    uint16_t result;
    char * end_ptr = NULL;

    // Get the uint16 string between the start and end character
    if (get_string_between_chars(src, start, end, uint16_str, sizeof(uint16_str)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get uint32 string between '%c' and '%c'", start, end);
        return 0;
    }

    // Convert the uint16 string to an uint16
    errno = 0;
    result = (uint32_t)strtoul(uint16_str, &end_ptr, 10);
    if((result == 0 && errno != 0) || end_ptr == uint16_str) {
        ESP_LOGE(TAG, "Failed to convert string '%s' to uint16", uint16_str);
    }

    return result;
}

/**
 * @brief Check if the telegram CRC16 is correct.
 *
 * @param[in] telegram The telegram to check
 * @param[in] size The size of the telegram
 * @return True if the telegram CRC16 is correct, false otherwise
 */
static bool check_telegram_crc(uint8_t * telegram, size_t size) {
    // Calculate the CRC16 of the telegram (up to the '!' character)
    uint16_t crc = crc16(telegram, size - 6);

    // Convert the CRC16 to a string
    char crc_string[5];
    sprintf(crc_string, "%04X", crc);

    // Check if the calculated CRC matches the CRC in the telegram
    return strncmp((char *) telegram + size - 6, crc_string, 4) == 0;
}

/**
 * @brief Calculate the CRC16 of the data.
 *
 * @details
 * The CRC16 is calculated with the following parameters:
 *   - polynomial: x^16 + x^15 + x^2 + 1 (0xa001)
 *   - initial value: 0
 *   - final XOR value: 0
 *   - MSB first
 * @param data[in] The data to calculate the CRC16 for
 * @param size[in] The length of the data
 * @return The CRC16 of the data
 */
static uint16_t crc16(uint8_t * data, size_t size) {
    uint16_t crc = 0;

    while (size--) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; ++i) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xa001;
            }
            else {
                crc = (crc >> 1);
            }
        }
    }

    return crc;
}