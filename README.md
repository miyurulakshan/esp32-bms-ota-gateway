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

// IDs
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
    
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS(); 
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        ESP_LOGI(TAG, "Driver Installed (Simulation)");
    }
    twai_start();
}

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
    twai_transmit(&tx_msg, pdMS_TO_TICKS(10));
}

static bool wait_for_bms_ack(uint32_t timeout_ms) {
    twai_message_t rx_msg;
    TickType_t start = xTaskGetTickCount();
    TickType_t wait = pdMS_TO_TICKS(timeout_ms);
    if(wait == 0) wait = 1;

    while ((xTaskGetTickCount() - start) < wait) {
        if (twai_receive(&rx_msg, 0) == ESP_OK) { 
            if (rx_msg.identifier == ID_BMS_ACK) return true; 
        }
        taskYIELD(); 
    }
    return false; 
}

// --- SIMULATION TASK ---
void ota_task(void *arg) {
    ESP_LOGI(TAG, "--- STARTED SIMULATION (NO BMS REQUIRED) ---");
    init_twai();

    strcpy(ota_status_msg, "Handshake...");
    uint8_t data[8] = {0};

    // 1. Handshake (Simulated)
    data[0] = 0x01;
    send_frame(ID_HANDSHAKE, data, 8);
    
    // SIMULATION: Ignore failure
    if (!wait_for_bms_ack(500)) {
        ESP_LOGW(TAG, "[SIM] No BMS Handshake, continuing anyway...");
    }

    // 2. Meta Data
    data[0] = firmware_len & 0xFF; data[1] = firmware_len >> 8;
    send_frame(ID_SIZE, data, 8);
    vTaskDelay(pdMS_TO_TICKS(50)); 

    // 3. Start
    memset(data, 0, 8);
    data[0] = 0x69; data[1] = 0x32;
    send_frame(ID_START, data, 8);
    vTaskDelay(pdMS_TO_TICKS(100)); 

    // 4. SIMULATED DATA LOOP
    strcpy(ota_status_msg, "Flashing...");
    ota_sent_bytes = 0;
    
    while (ota_sent_bytes < firmware_len) {
        
        // BURST: 2 Packets
        for (int i = 0; i < 2; i++) {
            if (ota_sent_bytes >= firmware_len) break;

            uint8_t chunk[8] = {0};
            int rem = firmware_len - ota_sent_bytes;
            int len = (rem > 6) ? 6 : rem;

            memcpy(chunk, &firmware_buffer[ota_sent_bytes], len);
            uint16_t crc = calc_crc_custom(chunk, 6);
            chunk[6] = crc & 0xFF; chunk[7] = crc >> 8;

            send_frame(ID_DATA, chunk, 8);
            ota_sent_bytes += len;
        }
        
        // SIMULATION: Ignore Missing ACK
        if (!wait_for_bms_ack(10)) {
            // Do nothing, just keep going
        }

        // SIMULATION: Add artificial delay so you can SEE the bar move.
        // Without this, the ESP32 is so fast in simulation it finishes instantly.
        // 10ms delay roughly simulates a BMS writing to flash.
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }

    strcpy(ota_status_msg, "Success");
    ESP_LOGI(TAG, "SIMULATION COMPLETE");

    // Jump to cleanup to avoid compiler warning
    goto cleanup;

cleanup:
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