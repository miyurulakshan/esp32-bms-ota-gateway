#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_crc.h"

#include "app_shared.h" 
#include "can_manager.h"

static const char *TAG = "CAN_MGR";

#define GPIO_RX 35
#define GPIO_TX 32

// CAN IDs
#define ID_HANDSHAKE 0x017B84
#define ID_START     0x027B84
#define ID_SIZE      0x037B84
#define ID_DATA      0x047B84
#define ID_BMS_ACK   0x067B84

static void init_twai() {
    twai_stop();
    twai_driver_uninstall();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_TX, GPIO_RX, TWAI_MODE_NORMAL);
    g_config.tx_queue_len = 20; 
    
    // 250Kbit/s
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS(); 
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        ESP_LOGI(TAG, "CAN Driver Installed");
    }
    twai_start();
}

// Custom CRC-16 (XMODEM/CCITT-FALSE)
static uint16_t calc_crc_custom(uint8_t *ptr, int count) {
    uint16_t crc = 0;
    while (--count >= 0) {
        crc = crc ^ (int) * ptr++ << 8;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc = crc << 1;
        }
    }
    return crc;
}

static void send_frame(uint32_t id, uint8_t *data, uint8_t len) {
    twai_message_t tx_msg = { .extd = 1, .identifier = id, .data_length_code = 8 };
    memcpy(tx_msg.data, data, len);
    
    // --- VERBOSE LOGGING: SENDING ---
    ESP_LOGI(TAG, "TX ID: 0x%06lX Data: %02X %02X %02X %02X %02X %02X %02X %02X", 
             id, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
    // --------------------------------

    twai_transmit(&tx_msg, pdMS_TO_TICKS(100)); // 100ms TX timeout
}

// Returns true if ACK received, false if timeout
static bool wait_for_bms_ack(uint32_t timeout_ms) {
    twai_message_t rx_msg;
    TickType_t start = xTaskGetTickCount();
    TickType_t wait = pdMS_TO_TICKS(timeout_ms);
    if(wait == 0) wait = 1;

    while ((xTaskGetTickCount() - start) < wait) {
        if (twai_receive(&rx_msg, 10) == ESP_OK) { 
            
            // --- VERBOSE LOGGING: RECEIVING ---
            ESP_LOGI(TAG, "RX ID: 0x%06lX Data: %02X %02X %02X %02X %02X %02X %02X %02X", 
                     rx_msg.identifier, 
                     rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3], 
                     rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
            // ----------------------------------

            // Check if message is from BMS (ID_BMS_ACK)
            if (rx_msg.identifier == ID_BMS_ACK) {
                return true; 
            }
        }
    }
    return false; 
}

// --- PRODUCTION OTA TASK ---
void ota_task(void *arg) {
    ESP_LOGI(TAG, "--- STARTING OTA PROCESS ---");
    init_twai();

    strcpy(ota_status_msg, "Handshake...");
    uint8_t data[8] = {0};

    // 1. Handshake
    ESP_LOGI(TAG, "Sending Handshake...");
    data[0] = 0x01;
    send_frame(ID_HANDSHAKE, data, 8);
    
    // PRODUCTION: CRITICAL CHECK
    if (!wait_for_bms_ack(2000)) { 
        ESP_LOGE(TAG, "ERROR: No BMS Response (Handshake Timeout)");
        strcpy(ota_status_msg, "Error: No BMS Connection");
        goto cleanup_error;
    }

    // 2. Meta Data (Size)
    ESP_LOGI(TAG, "Sending Size...");
    data[0] = firmware_len & 0xFF; data[1] = firmware_len >> 8;
    send_frame(ID_SIZE, data, 8);
    // Note: Logging for RX happens inside wait_for_bms_ack
    
    vTaskDelay(pdMS_TO_TICKS(100)); // Small delay for BMS processing

    // 3. Start Command
    ESP_LOGI(TAG, "Sending Start Command...");
    memset(data, 0, 8);
    data[0] = 0x69; data[1] = 0x32;
    send_frame(ID_START, data, 8);
    
    if (!wait_for_bms_ack(1000)) {
        ESP_LOGE(TAG, "ERROR: BMS did not ACK start command");
        strcpy(ota_status_msg, "Error: Start Rejected");
        goto cleanup_error;
    }

    // 4. DATA TRANSMISSION LOOP
    strcpy(ota_status_msg, "Flashing...");
    ota_sent_bytes = 0;
    
    ESP_LOGI(TAG, "Starting Data Transfer (%d bytes)", firmware_len);

    while (ota_sent_bytes < firmware_len) {
        
        // Send Burst: 2 Packets (12 bytes data + CRCs)
        for (int i = 0; i < 2; i++) {
            if (ota_sent_bytes >= firmware_len) break;

            uint8_t chunk[8] = {0};
            int rem = firmware_len - ota_sent_bytes;
            int len = (rem > 6) ? 6 : rem;

            memcpy(chunk, &firmware_buffer[ota_sent_bytes], len);
            
            // Calculate CRC for this chunk
            uint16_t crc = calc_crc_custom(chunk, 6);
            chunk[6] = crc & 0xFF; chunk[7] = crc >> 8;

            send_frame(ID_DATA, chunk, 8);
            ota_sent_bytes += len;
        }
        
        // Wait for ACK
        if (!wait_for_bms_ack(500)) { // 500ms timeout
            ESP_LOGE(TAG, "ERROR: Write Timeout at offset %ld", ota_sent_bytes);
            strcpy(ota_status_msg, "Error: Write Timeout");
            goto cleanup_error;
        }
    }

    strcpy(ota_status_msg, "Success");
    ESP_LOGI(TAG, "UPDATE COMPLETE SUCCESSFULLY");

    // Fall through to cleanup
    goto cleanup_success;

cleanup_error:
    twai_stop();
    twai_driver_uninstall();
    SYSTEM_IS_BUSY = false;
    vTaskDelete(NULL);
    return;

cleanup_success:
    twai_stop();
    twai_driver_uninstall();
    if (firmware_buffer) { free(firmware_buffer); firmware_buffer = NULL; }
    SYSTEM_IS_BUSY = false; 
    vTaskDelete(NULL);
}

// --- START FUNCTION (Pinned to Core 1) ---
esp_err_t start_can_update_task(void) {
    TaskHandle_t task_handle;
    BaseType_t res = xTaskCreatePinnedToCore(
        ota_task,         
        "ota_can_task",   
        4096,             
        NULL,             
        20,               // High Priority
        &task_handle,     
        1                 // Core 1
    );
    return (res == pdPASS) ? ESP_OK : ESP_FAIL;
}