#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

// Assuming these exist in your project
#include "app_shared.h" 
#include "web_page.h"
#include "can_manager.h" 

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

// 1. UPLOAD HANDLER (With SHA-256 Check)
static esp_err_t upload_post_handler(httpd_req_t *req) {
    if (SYSTEM_IS_BUSY) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "System Busy Flashing");
        return ESP_FAIL;
    }

    // 1. GET CHECKSUM HEADER
    char header_hash[65] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Payload-SHA256", header_hash, sizeof(header_hash)) != ESP_OK) {
        ESP_LOGE(TAG, "Security Error: Missing X-Payload-SHA256 header");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Integrity Header");
        return ESP_FAIL;
    }

    // 2. RECEIVE DATA
    int total_len = req->content_len;
    int cur_len = 0;
    int received = 0;
    
    // JS sends Hex String (2 chars = 1 byte)
    size_t binary_size = total_len / 2;

    // Reset Buffer if exists
    if (firmware_buffer) free(firmware_buffer);
    firmware_buffer = malloc(binary_size);
    if (!firmware_buffer) {
        ESP_LOGE(TAG, "OOM: Failed to allocate %d bytes", binary_size);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *chunk = malloc(1024);
    if (!chunk) {
        free(firmware_buffer);
        firmware_buffer = NULL;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t binary_idx = 0;
    char high_nibble = 0;
    bool have_high = false;

    // Stream Loop
    while (cur_len < total_len) {
        received = httpd_req_recv(req, chunk, 1024);
        if (received <= 0) {
            free(chunk);
            free(firmware_buffer);
            firmware_buffer = NULL;
            return ESP_FAIL;
        }

        // On-the-fly Hex -> Binary Conversion
        for (int i = 0; i < received; i++) {
            char c = chunk[i];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
                continue; 
            }

            if (!have_high) {
                high_nibble = hex2int(c);
                have_high = true;
            } else {
                uint8_t low_nibble = hex2int(c);
                if (binary_idx < binary_size) {
                    firmware_buffer[binary_idx++] = (high_nibble << 4) | low_nibble;
                }
                have_high = false;
            }
        }
        cur_len += received;
    }
    free(chunk);

    firmware_len = binary_idx;
    ota_total_size = firmware_len;
    
    ESP_LOGI(TAG, "Download Complete. Size: %d bytes. Verifying Integrity...", firmware_len);

    // --- DEBUG: PRINT FIRST 16 BYTES ---
    char debug_buf[64] = {0};
    for (int i = 0; i < 16 && i < firmware_len; i++) {
        sprintf(&debug_buf[i*3], "%02X ", firmware_buffer[i]);
    }
    ESP_LOGW(TAG, "--- DEBUG ---");
    ESP_LOGW(TAG, "First 16 Bytes in RAM: %s", debug_buf);
    ESP_LOGW(TAG, "Total Binary Len: %d", firmware_len);
    // ------------------------------------

    // 3. CALCULATE SHA-256
    unsigned char local_hash_bin[32];
    mbedtls_sha256_context ctx;
    
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); 
    mbedtls_sha256_update(&ctx, firmware_buffer, firmware_len);
    mbedtls_sha256_finish(&ctx, local_hash_bin);
    mbedtls_sha256_free(&ctx);

    // 4. CONVERT TO HEX STRING
    char local_hash_str[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&local_hash_str[i * 2], "%02x", local_hash_bin[i]);
    }
    local_hash_str[64] = '\0';

    // 5. COMPARE
    if (strcasecmp(header_hash, local_hash_str) == 0) {
        ESP_LOGI(TAG, "✅ INTEGRITY PASS: SHA-256 matches.");
        
        char resp[64];
        snprintf(resp, 64, "{\"status\": \"success\", \"size\": %d}", firmware_len);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "❌ INTEGRITY FAIL: Checksum mismatch!");
        ESP_LOGE(TAG, "Expected: %s", header_hash);
        ESP_LOGE(TAG, "Calculated: %s", local_hash_str);

        free(firmware_buffer);
        firmware_buffer = NULL;
        firmware_len = 0;

        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Data Integrity Failed - Checksum Mismatch");
        return ESP_FAIL;
    }
}

// 2. FLASH TRIGGER HANDLER
static esp_err_t flash_post_handler(httpd_req_t *req) {
    if (!firmware_buffer || firmware_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No Valid Firmware Loaded");
        return ESP_FAIL;
    }

    if (SYSTEM_IS_BUSY) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Already running");
        return ESP_FAIL;
    }

    SYSTEM_IS_BUSY = true;
    ota_sent_bytes = 0;
    strcpy(ota_status_msg, "Starting...");

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

// 4. RESET HANDLER (NEW)
static esp_err_t reset_post_handler(httpd_req_t *req) {
    // If flashing is active, we generally shouldn't allow reset unless we kill task
    // But per request, this is for "Upload State" or "Finished State".
    
    if (SYSTEM_IS_BUSY) {
        // If system is busy, force reset state (Warning: Task might still be running)
        // Ideally, we wait or kill task. For now, we assume this button is used when stalled or finished.
        SYSTEM_IS_BUSY = false;
    }

    // 1. Clear RAM
    if (firmware_buffer) {
        free(firmware_buffer);
        firmware_buffer = NULL;
    }
    firmware_len = 0;
    ota_total_size = 0;
    ota_sent_bytes = 0;
    
    // 2. Reset Status Text
    strcpy(ota_status_msg, "Idle");

    ESP_LOGI(TAG, "SYSTEM RESET via Web Interface");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\": \"reset\"}", HTTPD_RESP_USE_STRLEN);
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

        // Register Reset Handler
        httpd_uri_t uri_reset = { .uri = "/api/reset", .method = HTTP_POST, .handler = reset_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_reset);
    }
    return server;
}