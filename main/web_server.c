/**
 * @file web_server.c
 */

#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_vfs_semihost.h"
#include "mdns.h"
#include "cJSON.h"
#include "emucs_p1.h"
#include "predict_peak.h"
#include "logger.h"
#include "web_server.h"

#define USE_SEMIHOST_FS 0
#define MDNS_SERVICE_NAME "_kwartiwi-p1"

static const char *TAG = "web_server";  // Tag used for logging

// Function prototypes
static esp_err_t init_fs(void);
static esp_err_t start_web_server(void);
static esp_err_t set_content_type_from_file_ext(httpd_req_t *req, const char *ext);
static esp_err_t http_404_handler(httpd_req_t *req, const char *msg);
static esp_err_t http_500_handler(httpd_req_t *req, const char *msg);
static esp_err_t setup_fronted_routes(httpd_handle_t server);
static esp_err_t setup_api_routes(httpd_handle_t server);
static esp_err_t send_json_response(httpd_req_t *req, cJSON *root, uint16_t status_code);
static esp_err_t frontend_get_handler(httpd_req_t *req);
static esp_err_t system_info_get_handler(httpd_req_t *req);
static esp_err_t p1_data_basic_get_handler(httpd_req_t *req);
static esp_err_t p1_data_complete_get_handler(httpd_req_t *req);
static esp_err_t api_version_get_handler(httpd_req_t *req);
static esp_err_t predicted_peak_data_get_handler(httpd_req_t *req);
static esp_err_t send_p1_data(httpd_req_t *req, bool complete);
static esp_err_t get_p1_data_in_json(cJSON *json_obj, bool complete);
static esp_err_t meter_data_get_handler(httpd_req_t *req);
static esp_err_t meter_data_history_get_handler(httpd_req_t *req);

/**
 * @brief Configure and start the web server
 *
 */
void setup_web_server(void) {
    ESP_LOGI(TAG, "Setting up web server");

    // Initialize the file system
    ESP_ERROR_CHECK(init_fs());

    // Start the web server
    ESP_ERROR_CHECK(start_web_server());

    // Advertise the web server on the network using mDNS TODO: check if mdns is initialized
    ESP_LOGI(TAG, "Advertising web server using mDNS");
    ESP_ERROR_CHECK(mdns_service_add(NULL, MDNS_SERVICE_NAME, "_tcp", 80, NULL, 0));
}

/**
 * @brief Initialize file system
 *
 * Initializes the SPIFFS file system and registers it with the VFS.
 * The file system is mounted at WEB_SERVER_FS_MOUNT_POINT (/www).
 *
 * @note If USE_SEMIHOST_FS is defined, the semihost file system will be initialized instead
 *
 * @return ESP_OK on success
 */
static esp_err_t init_fs(void) {
#if !USE_SEMIHOST_FS
    ESP_LOGD(TAG, "Initializing SPIFFS file system");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = WEB_SERVER_FS_MOUNT_POINT,
        .partition_label = WEB_SERVER_SPIFFS_PARTITION_LABEL,
        .max_files = WEB_SERVER_SPIFFS_MAX_OPEN_FILES,
        .format_if_mount_failed = false
    };

    // Register SPIFFS file system
    esp_err_t err = esp_vfs_spiffs_register(&conf);

    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        }
        else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(err));
        }
        return err;
    }

    return ESP_OK;
#else // USE_SEMIHOST_FS
    ESP_LOGD(TAG, "Initializing semihost file system");
    esp_err_t ret = esp_vfs_semihost_register(WEB_SERVER_FS_MOUNT_POINT);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register semihost driver (%s)!", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    return ESP_OK;
#endif // USE_SEMIHOST_FS
}

/**
 * @brief Determine the content type based on the file extension and set it in the HTTP response
 * @details The following file extensions are supported: html, css, js, png, jpg, ico, svg, json and csv.
 *          If the file extension is not supported, the content type will be set to text/plain
 *
 * @param[in] req The HTTP request
 * @param[in] ext The file extension
 * @return ESP_OK on success
 */
static esp_err_t set_content_type_from_file_ext(httpd_req_t *req, const char *ext) {
    char *type = NULL;
    if (strcmp(ext, "html") == 0) {
        type = "text/html";
    }
    else if (strcmp(ext, "css") == 0) {
        type = "text/css";
    }
    else if (strcmp(ext, "js") == 0) {
        type = "application/javascript";
    }
    else if (strcmp(ext, "png") == 0) {
        type = "image/png";
    }
    else if (strcmp(ext, "jpg") == 0) {
        type = "image/jpeg";
    }
    else if (strcmp(ext, "ico") == 0) {
        type = "image/x-icon";
    }
    else if (strcmp(ext, "svg") == 0) {
        type = "image/svg+xml";
    }
    else if (strcmp(ext, "json") == 0) {
        type = "application/json";
    }
    else if (strcmp(ext, "csv") == 0) {
        type = "text/csv";
    }
    else {
        type = "text/plain";
    }

    return httpd_resp_set_type(req, type);
}

/**
 * @brief Start the web server
 *
 * The httpd server will be started and the URI handlers will be registered.
 *
 * @note The web server will be started on port WEB_SERVER_PORT (80)
 *
 * @return ESP_OK on success
 */
static esp_err_t start_web_server(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.server_port = WEB_SERVER_PORT;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server!");
        return ESP_FAIL;
    }

    // Register URI handlers
    setup_api_routes(server);
    setup_fronted_routes(server);

    ESP_LOGI(TAG, "Web server started");

    return ESP_OK;
}

/**
 * @brief Setup the URI handlers for the frontend
 *
 * @note This function contains a wildcard URI handler on the root url, so it must be registered last
 *
 * @param[in] server The httpd server handle
 * @return
 */
static esp_err_t setup_fronted_routes(httpd_handle_t server) {
    httpd_uri_t index_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = frontend_get_handler,
            .user_ctx = NULL
    };
    return httpd_register_uri_handler(server, &index_uri);
}

/**
 * @brief Setup the URI handlers for the API
 *
 * @param[in] server The httpd server handle
 * @return ESP_OK on success
 */
static esp_err_t setup_api_routes(httpd_handle_t server) {
    // Version
    httpd_uri_t version_get_uri = {
            .uri =  WEB_SERVER_API_ROUTES_PREFIX "/version",
            .method = HTTP_GET,
            .handler = api_version_get_handler,
            .user_ctx = NULL
    };
    if (httpd_register_uri_handler(server, &version_get_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler for version");
        return ESP_FAIL;
    }

    // System info
    httpd_uri_t system_info_get_uri = {
            .uri =  WEB_SERVER_API_ROUTES_PREFIX "/system/info",
            .method = HTTP_GET,
            .handler = system_info_get_handler,
            .user_ctx = NULL
    };
    if (httpd_register_uri_handler(server, &system_info_get_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler for system info");
        return ESP_FAIL;
    }

    // Meter data
    httpd_uri_t meter_data_get_uri = {
            .uri =  WEB_SERVER_API_ROUTES_PREFIX "/meter-data",
            .method = HTTP_GET,
            .handler = meter_data_get_handler,
            .user_ctx = NULL
    };
    if (httpd_register_uri_handler(server, &meter_data_get_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler for the meter data");
        return ESP_FAIL;
    }

    // Meter data
    httpd_uri_t meter_data_history_get_uri = {
            .uri =  WEB_SERVER_API_ROUTES_PREFIX "/meter-data-history",
            .method = HTTP_GET,
            .handler = meter_data_history_get_handler,
            .user_ctx = NULL
    };
    if (httpd_register_uri_handler(server, &meter_data_history_get_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler for the meter data history");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Handler for sending a error page with http status code 404
 *
 * @param[in] req The request handle
 * @param[in] msg An optional message to send to the client. If NULL, "Not found" is sent.
 * @return ESP_OK on success
 */
static esp_err_t http_404_handler(httpd_req_t *req, const char *msg) {
    if (msg == NULL) {
        msg = "Not found";
    }

    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, msg);
}

/**
 * @brief Handler for sending a error page with http status code 500
 *
 * @param[in] req The request handle
 * @param[in] msg An optional message to send to the client. If NULL, "Internal server error" is sent.
 * @return ESP_OK on success
 */
static esp_err_t http_500_handler(httpd_req_t *req, const char *msg) {
    if (msg == NULL) {
        msg = "Internal server error";
    }

    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
}

/**
 * @brief Handler for getting the frontend
 *
 * This handler is called when the user requests a file from the web server that is not already a registered URI.
 * Based on the URI, the corresponding file is opened and sent to the client. If the file is not found, a 404 error is sent.
 * If the URI is '/', the file 'index.html' is sent.
 * Files are served from the file system at WEB_SERVER_FS_MOUNT_POINT (/www).
 *
 * @note This function will not work with binary files
 * @todo Add support for binary files
 *
 * @param[in] req The request handle
 * @return ESP_OK on success
 */
static esp_err_t frontend_get_handler(httpd_req_t *req) {
    char filepath[WEB_SERVER_MAX_FILE_PATH_LEN];    // Buffer to store the file path including the mount point
    char buf[WEB_SERVER_FILE_BUFFER_SIZE];          // Buffer to use when reading the file
    FILE *fp;                                       // File pointer
    char *ext;                                      // File extension
    esp_err_t err;

    // Set the prefix to the mount point
    strcpy(filepath, WEB_SERVER_FS_MOUNT_POINT);

    // Get the file path
    if (req->uri[strlen(req->uri) - 1] == '/') {
        // If the URI ends with a slash, use index.html
        strlcat(filepath, "/index.html", sizeof(filepath));
    }
    else {
        // Use the URI as the file path
        strlcat(filepath, req->uri, sizeof(filepath));
    }

    // Open index.html file
    fp = fopen(filepath, "r");
    if (fp == NULL) {
        ESP_LOGW(TAG, "Failed to open file: %s", filepath);
        return http_404_handler(req, "File not found");
    }

    // Set the content type based on the file extension
    ext = strrchr(filepath, '.');
    if (ext != NULL) {
        err = set_content_type_from_file_ext(req, ext + 1);
    }
    else {
        err = httpd_resp_set_type(req, "text/plain");
    }
    // Check if the content type was set successfully
    if (err != ESP_OK) {
        fclose(fp);
        return ESP_FAIL;
    }

    // Read the file and send it to the client in chunks
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        err = httpd_resp_sendstr_chunk(req, buf);
        if (err != ESP_OK) {
            fclose(fp);
            return err;
        }
    }

    // Close the file
    fclose(fp);

    // End the response by sending an empty chunk
    err = httpd_resp_sendstr_chunk(req, NULL);

    return err;
}

/**
 * @brief Send a JSON response
 *
 * This function sets the content type to application/json and sends the JSON string to the client.
 *
 * @param[in] req The request handle
 * @param[in] root The JSON object to send
 * @param[in] status_code The HTTP status code to send. If 0 or > 999, 200 is sent.
 * @return ESP_OK on success
 */
static esp_err_t send_json_response(httpd_req_t *req, cJSON *root, uint16_t status_code) {
    esp_err_t err = ESP_OK;
    char status_code_str[6];
    const char *json = cJSON_Print(root);

    // Convert the status code to a string
    if (status_code == 0 || status_code > 999) {
        status_code = 200;
    }
    sprintf(status_code_str, "%d", status_code);

    // Set the status code
    if (httpd_resp_set_status(req,status_code_str) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set status code");
        err = ESP_FAIL;
    }
    // Set the content type
    else if (httpd_resp_set_type(req, "application/json") != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set json response type");
        err = ESP_FAIL;
    }
    // Send the response
    else if (httpd_resp_sendstr(req, json) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send json response");
        err = ESP_FAIL;
    }

    // Free resources
    free((void *)json);

    return err;
}

/**
 * @brief Handler for the /api/system_info endpoint
 *
 * This handler returns the system information as a JSON object.
 *
 * @param[in] req The request handle
 * @return ESP_OK on success
 */
static esp_err_t system_info_get_handler(httpd_req_t *req) {
    esp_err_t err;
    esp_chip_info_t chip_info;
    cJSON *root = cJSON_CreateObject();

    // Get the system info
    esp_chip_info(&chip_info);

    // Create the JSON object containing the system info
    cJSON_AddStringToObject(root, "version", IDF_VER);
    cJSON_AddNumberToObject(root, "cores", chip_info.cores);

    // Send the JSON object
    err = send_json_response(req, root, 200);

    // Free resources
    cJSON_Delete(root);

    return err;
}

/**
 * @brief Handler for the /api/version endpoint
 *
 * This handler returns the API version as a JSON object.
 *
 * @param[in] req The request handle
 * @return ESP_OK on success
 */
static esp_err_t api_version_get_handler(httpd_req_t *req) {
    esp_err_t err;

    // Create the JSON object containing the api version
    cJSON *json_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(json_obj, "version", WEB_SERVER_API_VERSION);

    // Send the JSON object
    err = send_json_response(req, json_obj, 200);

    // Free resources
    cJSON_Delete(json_obj);

    return err;
}

/**
 * @brief Handler for the meter-data
 *
 * @param[in] req The request handle
 * @return ESP_OK on success
 */
static esp_err_t meter_data_get_handler(httpd_req_t *req) {
    struct predicted_peak_s predicted_peak;
    esp_err_t err;
    cJSON *json_obj;
    SemaphoreHandle_t predicted_peak_mutex = predict_peak_get_predicted_peak_mutex_handle();
    emucs_p1_data_t *p1_data;
    cJSON *tmp_obj;
    SemaphoreHandle_t mutex = emucs_p1_get_telegram_mutex_handle();

    json_obj = cJSON_CreateObject();

    // Get the semaphore
    if (xSemaphoreTake(mutex, WEB_SERVER_MAX_TIMEOUT_MS / portTICK_PERIOD_MS) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to get P1 data semaphore within %d ms", WEB_SERVER_MAX_TIMEOUT_MS);
        return ESP_FAIL;
    }

    // Get the P1 data pointer
    p1_data = emucs_p1_get_telegram();

    // Add the basic data to the root JSON object (json_obj)
    cJSON_AddNumberToObject(json_obj, "timestamp", (double)p1_data->msg_timestamp);
    cJSON_AddNumberToObject(json_obj, "electricityDeliveredTariff1", p1_data->electricity_delivered_tariff1);
    cJSON_AddNumberToObject(json_obj, "electricityDeliveredTariff2", p1_data->electricity_delivered_tariff2);
    cJSON_AddNumberToObject(json_obj, "electricityReturnedTariff1", p1_data->electricity_returned_tariff1);
    cJSON_AddNumberToObject(json_obj, "electricityReturnedTariff2", p1_data->electricity_returned_tariff2);
    cJSON_AddNumberToObject(json_obj, "currentAvgDemand", p1_data->current_avg_demand);
    cJSON_AddNumberToObject(json_obj, "currentPowerUsage", p1_data->current_power_usage);
    cJSON_AddNumberToObject(json_obj, "currentPowerReturn", p1_data->current_power_return);
    tmp_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(tmp_obj, "timestamp", (double) p1_data->max_demand_month.timestamp);
    cJSON_AddNumberToObject(tmp_obj, "demand", p1_data->max_demand_month.max_demand);
    cJSON_AddItemToObject(json_obj, "maxDemandMonth", tmp_obj);

    xSemaphoreGive(mutex);

    // Get the predicted peak data
    if (xSemaphoreTake(predicted_peak_mutex, pdMS_TO_TICKS(WEB_SERVER_MAX_TIMEOUT_MS)) == pdTRUE) {
        predicted_peak = predict_peak_get_predicted_peak();
        xSemaphoreGive(predicted_peak_mutex);
    }
    else {
        ESP_LOGE(TAG, "Failed to get predicted peak mutex within %d ms", WEB_SERVER_MAX_TIMEOUT_MS);
        return http_500_handler(req, "Failed to get predicted peak mutex");
    }

    //  predicted peak data
    cJSON_AddNumberToObject(json_obj, "predictedPeak", predicted_peak.value);
    cJSON_AddNumberToObject(json_obj, "predictedPeakTime", (double)predicted_peak.timestamp);

    // Send the JSON object
    err = send_json_response(req, json_obj, 200);

    // Free resources
    cJSON_Delete(json_obj);

    return err;
}

/**
 * @brief Handler for the meter-data
 *
 * @param[in] req The request handle
 * @return ESP_OK on success
 */
static esp_err_t meter_data_history_get_handler(httpd_req_t *req) {
    esp_err_t err;
    cJSON *json_obj;
    emucs_p1_data_t *p1_data;
    cJSON *tmp_obj;
    cJSON *tmp_array;
    SemaphoreHandle_t mutex = emucs_p1_get_telegram_mutex_handle();
    log_entry_short_term_p1_data_t *short_term_log_entry;
    log_entry_long_term_p1_data_t *long_term_log_entry;
    SemaphoreHandle_t short_term_log_mutex = logger_get_short_term_log_mutex_handle();
    SemaphoreHandle_t long_term_log_mutex = logger_get_long_term_log_mutex_handle();
    size_t item_count;
    struct tm * tm_ptr;

    ESP_LOGD(TAG, "meter_data_history_get_handler called");

    short_term_log_entry = malloc(LOGGER_SHORT_TERM_LOG_SIZE * sizeof(log_entry_short_term_p1_data_t));
    if (short_term_log_entry == NULL) {
        // TODO: return 500 error instead ?
        ESP_LOGE(TAG, "Failed to allocate memory for log_entry");
        vTaskDelete(NULL);
        assert(0); // Should never get here
    }

    json_obj = cJSON_CreateObject();

    // Get the semaphore
    if (xSemaphoreTake(mutex, WEB_SERVER_MAX_TIMEOUT_MS / portTICK_PERIOD_MS) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to get P1 data semaphore within %d ms", WEB_SERVER_MAX_TIMEOUT_MS);
        return ESP_FAIL;
    }

    // Get the P1 data pointer
    p1_data = emucs_p1_get_telegram();

    // Add the basic data to the root JSON object (json_obj)
    tmp_array = cJSON_CreateArray();
    for (int i = 0; i < 13; i++) {
        if (p1_data->max_demand_year[i].timestamp_appearance == 0) {
            break;
        }
        tmp_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(tmp_obj, "timestamp", (double) p1_data->max_demand_year[i].timestamp_appearance);
        cJSON_AddNumberToObject(tmp_obj, "demand", p1_data->max_demand_year[i].max_demand);
        cJSON_AddItemToArray(tmp_array, tmp_obj);
    }
    cJSON_AddItemToObject(json_obj, "maxDemandYear", tmp_array);

    xSemaphoreGive(mutex);

    // Copy the short term log to a local buffer, sorted on entry timestamp
    xSemaphoreTake(short_term_log_mutex, portMAX_DELAY);
    item_count = logger_get_short_term_log_items(short_term_log_entry, LOGGER_SHORT_TERM_LOG_SIZE);
    xSemaphoreGive(short_term_log_mutex);

    ESP_LOGD(TAG, "item_count = %d", item_count);

    // Find the first entry that starts at the beginning of a quarter-hour, i.e. 00, 15, 30 or 45 minutes
    // If no such entry is found, use the first entry available
    uint16_t first_entry_index = 0;
    for (size_t i = 0; i < item_count; i++) {
        tm_ptr = localtime(&short_term_log_entry[i].timestamp);
        if (tm_ptr->tm_min % 15 == 0 && tm_ptr->tm_sec == 0) {
            first_entry_index = i;
            break;
        }
    }
    ESP_LOGD(TAG, "first_entry_index = %d", first_entry_index);

    // Add the short term log data to the root JSON object (json_obj)
    tmp_array = cJSON_CreateArray();
    for (size_t i = first_entry_index; i < item_count; i++) {
        tmp_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(tmp_obj, "timestamp", (double) short_term_log_entry[i].timestamp);
        cJSON_AddNumberToObject(tmp_obj, "avgDemand", short_term_log_entry[i].current_avg_demand);
        cJSON_AddNumberToObject(tmp_obj, "powerUsage", short_term_log_entry[i].current_power_usage);
        cJSON_AddItemToArray(tmp_array, tmp_obj);
    }
    cJSON_AddItemToObject(json_obj, "shortTermHistory", tmp_array);

    free(short_term_log_entry);

    // Copy the long term log to a local buffer, sorted on entry timestamp
    long_term_log_entry = malloc(LOGGER_LONG_TERM_LOG_BUF_SIZE * sizeof(log_entry_short_term_p1_data_t));
    if (long_term_log_entry == NULL) {
        // TODO: return 500 error instead?
        ESP_LOGE(TAG, "Failed to allocate memory for log_entry");
        vTaskDelete(NULL);
        assert(0); // Should never get here
    }

    xSemaphoreTake(long_term_log_mutex, portMAX_DELAY);
    item_count = logger_get_long_term_log_items(long_term_log_entry, LOGGER_LONG_TERM_LOG_BUF_SIZE);
    xSemaphoreGive(long_term_log_mutex);

    // Add the long term log data to the root JSON object (json_obj)
    tmp_array = cJSON_CreateArray();
    for (size_t i = 0; i < item_count; i++) {
        tmp_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(tmp_obj, "timestamp", (double) long_term_log_entry[i].timestamp);
        cJSON_AddNumberToObject(tmp_obj, "electricityDeliveredTariff1", long_term_log_entry[i].electricity_delivered_tariff1);
        cJSON_AddNumberToObject(tmp_obj, "electricityDeliveredTariff2", long_term_log_entry[i].electricity_delivered_tariff2);
        cJSON_AddNumberToObject(tmp_obj, "electricityReturnedTariff1", long_term_log_entry[i].electricity_returned_tariff1);
        cJSON_AddNumberToObject(tmp_obj, "electricityReturnedTariff2", long_term_log_entry[i].electricity_returned_tariff2);
        cJSON_AddItemToArray(tmp_array, tmp_obj);
    }

    cJSON_AddItemToObject(json_obj, "longTermHistory", tmp_array);

    free(long_term_log_entry);

    // Send the JSON object
    err = send_json_response(req, json_obj, 200);

    // Free resources
    cJSON_Delete(json_obj);

    return err;
}