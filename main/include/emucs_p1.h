#ifndef EMUCS_P1_H
#define EMUCS_P1_H

#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "driver/uart.h"

#define EMUCS_P1_TELEGRAM_INTERVAL_MS 1000  // Interval between P1 telegrams in ms
#define EMUCS_P1_EVENT_TELEGRAM_AVAILABLE_BIT BIT0

typedef enum emucs_p1_breaker_state_e {
    EMUCS_P1_BREAKER_STATE_DISCONNECTED = 0,
    EMUCS_P1_BREAKER_STATE_CONNECTED = 1,
    EMUCS_P1_BREAKER_STATE_READY_FOR_CONNECTION = 2
} emucs_p1_breaker_state_t;

typedef struct {
    /*                                          OBIS code   Unit    Value */
    char version_info[5+1];                 //  0-0:96.1.4  -       Version information
    char equipment_id[96+1];                //  0-0:96.1.1  -       Equipment identifier
    time_t msg_timestamp;                   //  0-0:1.0.0   -       Date-time stamp of P1 message
    float electricity_delivered_tariff1;    //  1-0:1.8.1   kWh     Meter reading electricity delivered to client (Tariff 1)
    float electricity_delivered_tariff2;    //  1-0:1.8.2   kWh     Meter reading electricity delivered to client (Tariff 2)
    float electricity_returned_tariff1;     //  1-0:2.8.1   kWh     Meter reading electricity delivered by client (Tariff 1)
    float electricity_returned_tariff2;     //  1-0:2.8.2   kWh     Meter reading electricity delivered by client (Tariff 2)
    uint16_t tariff_indicator;              //  0-0:96.14.0 -       Tariff indicator electricity (1=High, 2=Low)
    float current_avg_demand;               //  1-0:1.4.0   kW      Current average demand - Active energy import
    struct {                                //  1-0:1.6.0   -      Maximum demand - Active energy import of the running month
        time_t timestamp;
        float max_demand;   // kW
    } max_demand_month;
    struct {                                //  0-0:89.1.0   kW      Maximum demand - Active energy import of the last 13 months
        time_t timestamp_appearance;
        float max_demand;   // kW
    } max_demand_year[13];
    float current_power_usage;              //  1-0:1.7.0   kW      Actual electricity power delivered to client from the grid (+P)
    float current_power_return;             //  1-0:2.7.0   kW      Actual electricity power injected by client in the grid (-P)
    float current_power_usage_l1;           //  1-0:21.7.0  kW      Instantaneous active power L1 (+P)
    float current_power_usage_l2;           //  1-0:41.7.0  kW      Instantaneous active power L2 (+P)
    float current_power_usage_l3;           //  1-0:61.7.0  kW      Instantaneous active power L3 (+P)
    float current_power_return_l1;          //  1-0:22.7.0  kW      Instantaneous active power L1 (-P)
    float current_power_return_l2;          //  1-0:42.7.0  kW      Instantaneous active power L2 (-P)
    float current_power_return_l3;          //  1-0:62.7.0  kW      Instantaneous active power L3 (-P)
    float voltage_l1;                       //  1-0:32.7.0  V       Instantaneous voltage L1
    float voltage_l2;                       //  1-0:52.7.0  V       Instantaneous voltage L2
    float voltage_l3;                       //  1-0:72.7.0  V       Instantaneous voltage L3
    float current_l1;                       //  1-0:31.7.0  A       Instantaneous current L1
    float current_l2;                       //  1-0:51.7.0  A       Instantaneous current L2
    float current_l3;                       //  1-0:71.7.0  A       Instantaneous current L3
    emucs_p1_breaker_state_t breaker_state; //  0-0:96.3.10 -       Breaker state
    float limiter_threshold;                //  0-0:17.0.0  kW      Limiter threshold (0-999.8 = threshold, 999 = deactivated)
    float fuse_supervision_threshold;       //  1-0:31.4.0  A       Fuse supervision threshold (0-998 = threshold, 999 = deactivated)
    // char text_message[1024+1];           //  0-0:96.13.0 -       Text message (max 1024 characters) (not implemented)
} emucs_p1_data_t;


// Function prototypes
_Noreturn void emucs_p1_task(void *pvParameters);
emucs_p1_data_t * emucs_p1_get_telegram(void);
SemaphoreHandle_t emucs_p1_get_telegram_mutex_handle(void);
EventGroupHandle_t emucs_p1_get_event_group_handle(void);


#endif // EMUCS_P1_H