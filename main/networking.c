/**
 * @file networking.c
 * @brief Networking functions
 * @todo  - Add support for static IP configuration in STA mode
 *        - Add support for custom DHCP settings in AP mode
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "networking.h"

// Bit definitions for the event group used in STA mode
#define WIFI_STA_CONNECTED_BIT BIT0
#define WIFI_STA_FAIL_BIT      BIT1

static const char *TAG = "networking";          // Tag used for logging
static EventGroupHandle_t s_wifi_event_group;   // Event group used in STA mode

// Function prototypes
static esp_err_t wifi_init_softap(wifi_config_t *config);
static esp_err_t wifi_init_sta(wifi_config_t *config);


/**
 * @brief Initialize Wi-Fi in AP or Station mode
 *
 * Setup a network interface, based on the configuration in NVS.
 *
 * @param[in] pvParameters Unused
 */
void setup_networking_task(void *pvParameters) {
    nvs_handle_t nvs_handle;
    enum networking_wifi_mode_e wifi_mode = NETWORKING_WIFI_MODE_AP;
    wifi_config_t wifi_config = { 0 };
    size_t max_len;
    esp_err_t err;

    // Open Non-volatile storage
    ESP_ERROR_CHECK(nvs_open(NETWORKING_NVS_NAMESPACE, NVS_READONLY, &nvs_handle));

    // Check if we need to set up Wi-Fi in AP mode or Station mode
    uint8_t wifi_mode_u8;
    ESP_ERROR_CHECK(nvs_get_u8(nvs_handle, NETWORKING_NVS_KEY_WIFI_MODE, &wifi_mode_u8));
    ESP_LOGD(TAG, "Wi-Fi mode u8: %d", wifi_mode_u8);
    wifi_mode = (enum networking_wifi_mode_e) wifi_mode_u8;
    ESP_LOGD(TAG, "Wi-Fi mode: %d", wifi_mode);

    // Read Wi-Fi configuration from NVS into wifi_config
    // Always read AP configuration, as it's needed when station mode fails
    ESP_LOGD(TAG, "Reading Wi-Fi configuration from NVS");
    max_len = sizeof(wifi_config.ap.ssid);
    ESP_ERROR_CHECK(nvs_get_str(nvs_handle, NETWORKING_NVS_KEY_AP_SSID, (char *) wifi_config.ap.ssid, &max_len));
    max_len = sizeof(wifi_config.ap.password);
    ESP_ERROR_CHECK(nvs_get_str(nvs_handle, NETWORKING_NVS_KEY_AP_PASS, (char *) wifi_config.ap.password, &max_len));
    ESP_ERROR_CHECK(nvs_get_u8(nvs_handle, NETWORKING_NVS_KEY_AP_CHANNEL, &wifi_config.ap.channel));
    if (wifi_mode == NETWORKING_WIFI_MODE_STATION) {
        max_len = sizeof(wifi_config.sta.ssid);
        ESP_ERROR_CHECK(nvs_get_str(nvs_handle, NETWORKING_NVS_KEY_STA_SSID, (char *) wifi_config.sta.ssid, &max_len));
        max_len = sizeof(wifi_config.sta.password);
        ESP_ERROR_CHECK(nvs_get_str(nvs_handle, NETWORKING_NVS_KEY_STA_PASS, (char *) wifi_config.sta.password, &max_len));
    }

    // Close the nvs handle now that all config data has been read
    nvs_close(nvs_handle);

    // Set up Wi-Fi in AP mode
    if (wifi_mode == NETWORKING_WIFI_MODE_AP) {
        err = wifi_init_softap(&wifi_config);
        ESP_ERROR_CHECK(err);
    }
    // Set up Wi-Fi in Station mode
    else if (wifi_mode == NETWORKING_WIFI_MODE_STATION) {
        err = wifi_init_sta(&wifi_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize Wi-Fi in Station mode, falling back to AP mode");
            err = wifi_init_softap(&wifi_config);
            ESP_ERROR_CHECK(err);
        }
    }

    // Delete this task
    vTaskDelete(NULL);
}

/**
 * @brief Wi-Fi AP event handler
 *
 * @param arg
 * @param event_base
 * @param event_id
 * @param event_data
 */
static void wifi_ap_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d", MAC2STR(event->mac), event->aid);
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d", MAC2STR(event->mac), event->aid);
    }
}

/**
 * @brief Initialize Wi-Fi in Access Point mode
 *
 * If the password is empty, the AP will be open. Otherwise, WPA2-PSK will be used.
 *
 * @param[in, out] config Pointer to wifi_config_t struct containing AP configuration.
 *                        ap.ssid, ap.password and ap.channel needs to be set.
 * @return ESP_OK on success
 */
static esp_err_t wifi_init_softap(wifi_config_t *config) {
    ESP_LOGI(TAG, "Setting up Wi-Fi in AP mode");

    // Set up Wi-Fi in AP mode
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    // Initialize Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_ap_event_handler, NULL, NULL));

    // Set up AP configuration
    config->ap.ssid_len = strlen((char *) config->ap.ssid);
    config->ap.max_connection = NETWORKING_WIFI_AP_MAX_CONN;
    config->ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    if (strlen((char *) config->ap.password) == 0) {
        config->ap.authmode = WIFI_AUTH_OPEN;
    }
    // Channel 0 means "use the least crowded channel",
    // if no valid channel is set, also use this option
    if (config->ap.channel == 0 || config->ap.channel > 13) {
        // TODO: Scan for available channels and pick the least crowded one, but for now just use channel 1
        config->ap.channel = 1;
    }

    // Start Wi-Fi in AP mode with the given configuration
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started. SSID: %s, password: %s, channel: %d", config->ap.ssid, config->ap.password, config->ap.channel);

    return ESP_OK;
}

/**
 * @brief Wifi station event handler
 *
 * @param[in] arg
 * @param[in] event_base
 * @param[in] event_id
 * @param[in] event_data
 */
static void wifi_sta_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    static uint8_t s_retry_num = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started");
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < NETWORKING_WIFI_STA_CONN_RETRIES) {
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying to connect to the AP. Attempt %d/%d", s_retry_num, NETWORKING_WIFI_STA_CONN_RETRIES);
            ESP_ERROR_CHECK(esp_wifi_connect());
        }
        else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_STA_FAIL_BIT);
        }
        ESP_LOGI(TAG,"Failed to connect to the AP");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Connected to AP. IP address: "IPSTR, IP2STR(&((ip_event_got_ip_t*)event_data)->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT);
    }
}

/**
 * @brief Initialize Wi-Fi in Station mode
 *
 * @param[in, out] config The Wi-Fi configuration. sta.ssid and sta.password must be set.
 * @return
 *   - ESP_OK: when successfully connected to AP,
 *   - ESP_FAIL: otherwise
 */
static esp_err_t wifi_init_sta(wifi_config_t *config) {
    esp_err_t ret;
    s_wifi_event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "Setting up Wi-Fi in STA mode");

    // Set up Wi-Fi in Station mode
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Initialize Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_sta_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_sta_event_handler, NULL, &instance_got_ip));

    // Set the Wi-Fi configuration
    config->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config->sta.pmf_cfg.capable = true;
    config->sta.pmf_cfg.required = false;
#ifdef NETWORKING_WIFI_STA_WPA2_ONLY
    config->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
#endif

    // Start Wi-Fi in Station mode with the given configuration
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for the Wi-Fi connection to be established (or fail)
    EventBits_t event_bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT | WIFI_STA_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    // Check if the connection was successful
    if (event_bits & WIFI_STA_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to SSID:%s", config->sta.ssid);
        ret = ESP_OK;
    }
    else if (event_bits & WIFI_STA_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", config->sta.ssid);
        ret = ESP_FAIL;
    }
    else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        ret = ESP_FAIL;
    }

    // Unregister event handlers and free resources
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);

    return ret;
}