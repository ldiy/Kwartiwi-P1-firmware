#include <stdio.h>
#include <string.h>
#include "emucs_p1.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "emucs_p1";

static uint8_t uart_buffer[UART_BUFFER_SIZE];

static void process_p1_data(size_t size);

void emucs_p1_task(void *pvParameters) {
    // UART configuration (8N1)
    uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
    };

    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    // Set UART pins (Only RX is used)
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_PIN_NO_CHANGE, P1_DATA_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // Invert RX signal TODO: uncomment inverted RX signal line
   // ESP_ERROR_CHECK(uart_set_line_inverse(UART_NUM, UART_SIGNAL_RXD_INV));

    // Install UART driver using an event queue
    QueueHandle_t uart_queue;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_BUFFER_SIZE, 0, UART_QUEUE_SIZE, &uart_queue, 0));

    //uint8_t *data = (uint8_t *) malloc(UART_BUFFER_SIZE);
    uart_event_t event;
    for(;;) {
        if(xQueueReceive(uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
            ESP_LOGI(TAG, "uart event:");
            switch (event.type) {

                case UART_DATA:
                    ESP_LOGI(TAG, "[UART DATA]: %d", event.size);
                    //uart_read_bytes(UART_NUM, data, event.size, portMAX_DELAY);
                    process_p1_data(event.size);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "uart buffer full");
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "uart fifo overflow");
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGW(TAG, "uart frame error");
                    break;

                default:
                    ESP_LOGW(TAG, "uart event unknown. Event: %d", event.type);
                    break;
            }
        }
    }

    vTaskDelete( NULL );
}


static void process_p1_data(size_t size) {
    static enum {
        P1_STATE_IDLE,
        P1_STATE_DATA,
        P1_STATE_END,
    } state = P1_STATE_IDLE;

    static uint8_t * uart_buffer_index = uart_buffer;
    static uint8_t * telegram_start = NULL;
    static uint8_t * telegram_end = NULL;
    static size_t telegram_size = 0;

    // Check if there is enough space in the buffer
    if (uart_buffer_index + size > uart_buffer + UART_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Not enough space in the uart buffer. Resetting state");
        // TODO: In some cases it might be possible to recover from this error, but for now we just reset the state
        //  and hope that the next telegram will be received correctly
        state = P1_STATE_IDLE;
        uart_buffer_index = uart_buffer;
        telegram_start = NULL;
        telegram_end = NULL;
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
                    ESP_LOGI(TAG, "Telegram start found");
                    state = P1_STATE_DATA;
                    telegram_start = data;
                }
                break;
            case P1_STATE_DATA:
                if (*data == '!') {
                    ESP_LOGI(TAG, "Telegram end found");
                    state = P1_STATE_END;
                    telegram_end = data;
                }
                break;
            case P1_STATE_END:
                if (*data == '\n') {
                    state = P1_STATE_IDLE;
                    // Calculate the size of the telegram (without the last CR + LF)
                    telegram_size = telegram_end - telegram_start + 3;
                    ESP_LOGI(TAG, "Complete telegram found with size: %d", telegram_size);
                    // TODO: Process the telegram

                    // Reset the telegram start, end and size
                    telegram_start = NULL;
                    telegram_end = NULL;
                    telegram_size = 0;
                }
                break;
        }
    }

    // Move the data to the start of the buffer if we are in the P1_STATE_DATA state and the telegram start is not at
    // the start of the buffer. Otherwise, the buffer size may be not sufficient to receive the complete telegram.
    if (state == P1_STATE_DATA && telegram_start != uart_buffer) {
        ESP_LOGI(TAG, "Moving received data to the start of the buffer");
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