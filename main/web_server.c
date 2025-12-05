#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "test_data.h"

// Assuming these exist in your project structure
#include "app_shared.h" 
#include "web_page.h"
#include "can_manager.h" // Assuming start_can_update_task() is here

static const char *TAG = "WEB";

// --- SHARED VARIABLES ---
volatile bool SYSTEM_IS_BUSY = false;
uint8_t *firmware_buffer = NULL;
size_t firmware_len = 0;
volatile uint32_t ota_total_size = 0;
volatile uint32_t ota_sent_bytes = 0;
char ota_status_msg[32] = "Idle";

// Helper to convert hex char to int
uint8_t hex2int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10; 
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

// --- HANDLERS ---

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 1. UPLOAD HANDLER
static esp_err_t upload_post_handler(httpd_req_t *req) {
    if (SYSTEM_IS_BUSY) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "System Busy Flashing");
        return ESP_FAIL;
    }

    int total_len = req->content_len;
    int cur_len = 0;
    int received = 0;
    
    // JS cleans the string, so we assume 2 hex chars = 1 byte
    size_t binary_size = total_len / 2;

    if (firmware_buffer) free(firmware_buffer);
    firmware_buffer = malloc(binary_size);
    if (!firmware_buffer) {
        ESP_LOGE(TAG, "OOM");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *chunk = malloc(1024);
    size_t binary_idx = 0;
    char high_nibble = 0;
    bool have_high = false;

    while (cur_len < total_len) {
        received = httpd_req_recv(req, chunk, 1024);
        if (received <= 0) {
            free(chunk);
            return ESP_FAIL;
        }

        // Parse Hex Stream
        for (int i = 0; i < received; i++) {
            if (!have_high) {
                high_nibble = hex2int(chunk[i]);
                have_high = true;
            } else {
                uint8_t low_nibble = hex2int(chunk[i]);
                firmware_buffer[binary_idx++] = (high_nibble << 4) | low_nibble;
                have_high = false;
            }
        }
        cur_len += received;
    }
    free(chunk);

    firmware_len = binary_idx;
    ota_total_size = firmware_len;
    
    // ... (This is inside upload_post_handler, after firmware_len is set) ...

    ESP_LOGI(TAG, "Stored %d bytes. Starting Verification...", firmware_len);

    // --- START VERIFICATION LOGIC ---
    
    // 1. Check Size
    size_t expected_size = sizeof(EXPECTED_DATA);
    
    if (firmware_len != expected_size) {
        ESP_LOGE(TAG, "SIZE MISMATCH! Expected: %d, Got: %d", expected_size, firmware_len);
    } 
    else {
        // 2. Check Content (Fast Compare)
        int result = memcmp(firmware_buffer, EXPECTED_DATA, firmware_len);
        
        if (result == 0) {
            // SUCCESS
            ESP_LOGW(TAG, "------------------------------------------------");
            ESP_LOGW(TAG, "✅ SUCCESS: DATA IS 100%% ACCURATE");
            ESP_LOGW(TAG, "------------------------------------------------");
        } 
        else {
            // FAILURE - FIND THE EXACT ERROR
            ESP_LOGE(TAG, "❌ DATA CONTENT MISMATCH! Finding first error...");
            
            for (size_t i = 0; i < firmware_len; i++) {
                if (firmware_buffer[i] != EXPECTED_DATA[i]) {
                    ESP_LOGE(TAG, "Error at Index [%d] (Address 0x%X)", i, i);
                    ESP_LOGE(TAG, " -> Expected: 0x%02X", EXPECTED_DATA[i]);
                    ESP_LOGE(TAG, " -> Received: 0x%02X", firmware_buffer[i]);
                    break; // Stop at first error to avoid spamming
                }
            }
        }
    }
    // --- END VERIFICATION LOGIC ---

    char resp[64];
    
    snprintf(resp, 64, "{\"size\": %d}", firmware_len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// 2. FLASH TRIGGER HANDLER
static esp_err_t flash_post_handler(httpd_req_t *req) {
    if (!firmware_buffer || firmware_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No Data");
        return ESP_FAIL;
    }

    if (SYSTEM_IS_BUSY) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Already running");
        return ESP_FAIL;
    }

    // LOCK THE SYSTEM
    SYSTEM_IS_BUSY = true;
    ota_sent_bytes = 0;
    strcpy(ota_status_msg, "Starting...");

    // Start CAN Task (Ensure this function is defined in can_manager.c)
    if (start_can_update_task() != ESP_OK) {
        SYSTEM_IS_BUSY = false;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\": \"started\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 3. STATUS HANDLER
static esp_err_t status_get_handler(httpd_req_t *req) {
    char resp[128];
    snprintf(resp, 128, "{\"busy\": %s, \"status\": \"%s\", \"sent\": %ld, \"total\": %ld}",
             SYSTEM_IS_BUSY ? "true" : "false",
             ota_status_msg,
             ota_sent_bytes,
             ota_total_size);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_root);

        httpd_uri_t uri_upload = { .uri = "/api/upload", .method = HTTP_POST, .handler = upload_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_upload);

        httpd_uri_t uri_flash = { .uri = "/api/flash", .method = HTTP_POST, .handler = flash_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_flash);

        httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_status);
    }
    return server;
}