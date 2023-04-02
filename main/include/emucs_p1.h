#ifndef EMUCS_P1_H
#define EMUCS_P1_H

#include "driver/uart.h"

#define P1_DATA_PIN 5
#define UART_BUFFER_SIZE 1024
#define UART_NUM UART_NUM_1
#define UART_QUEUE_SIZE 10

#define TELEGRAM_BUFFER_SIZE 1500


void emucs_p1_task(void *pvParameters);


#endif // EMUCS_P1_H