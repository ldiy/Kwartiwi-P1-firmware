#ifndef NETWORKING_H
#define NETWORKING_H

#include <stdbool.h>

// NVS keys
#define NETWORKING_NVS_NAMESPACE "networking"
#define NETWORKING_NVS_KEY_WIFI_MODE "wifi_mode"
#define NETWORKING_NVS_KEY_STA_SSID "station_ssid"
#define NETWORKING_NVS_KEY_STA_PASS "station_pass"
#define NETWORKING_NVS_KEY_AP_SSID "ap_ssid"
#define NETWORKING_NVS_KEY_AP_PASS "ap_pass"
#define NETWORKING_NVS_KEY_AP_CHANNEL "ap_channel"
#define NETWORKING_NVS_KEY_HOSTNAME "hostname"
#define NETWORKING_NVS_KEY_MDNS_INSTANCE_NAME "mdns_instance"

#define NETWORKING_WIFI_AP_MAX_CONN 20      // Max number of connections to AP
#define NETWORKING_WIFI_STA_CONN_RETRIES 5  // Number of times to retry connecting to AP before giving up (max 255)
#define NETWORKING_WIFI_STA_WPA2_ONLY true  // Only connect to APs that support WPA2

#define NETWORKING_HOSTNAME_MAX_LEN 32              // Max length of hostname
#define NETWORKING_MDNS_INSTANCE_NAME_MAX_LEN 32    // Max length of mDNS instance name

/**
 * Wi-Fi mode.
 * Determines whether the device will act as an access point or connect to an existing access point.
 */
enum networking_wifi_mode_e {
    NETWORKING_WIFI_MODE_AP = 0,
    NETWORKING_WIFI_MODE_STATION = 1
};

// Function prototypes
void setup_networking(void);

#endif //NETWORKING_H
