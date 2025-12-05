/*
 * Adapted from BMS CAN OTA Module V1.0 (Ashan Sandanayake)
 * Integrated for Web Server RAM upload
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "app_shared.h"

static const char *TAG = "CAN_OTA";

// --- CONFIGURATION ---
#define GPIO_RX 35
#define GPIO_TX 32

// --- STATUS DEFINITIONS ---
#define UPDATE_ONGOING 0x01
#define STOP_UPDATE 0x02
#define NO_UPDATE 0x00
#define START_UPDATE 0x03
#define HANDSHAKE_INIT 0x11
#define REQUEST_RECIEVE_MSG 0X04
#define COMPLETE_RECIEVE_MSG 0X05

#define INIT_DELAY 5000
#define START_DELAY 500
#define MS_DELAY 2 
#define CAN_SEND_DELAY (MS_DELAY/10)*5
#define CAN_RECIEVE_DELAY (MS_DELAY/10)*5

// --- GLOBAL FLAGS (Internal to CAN) ---
bool OTA_update_flag = false;
bool flash_write_status = false;
uint16_t flash_write_counter = 0;
uint16_t byte_count = 0; // Tracks position in firmware_buffer

// --- TWAI VARIABLES ---
twai_message_t tx_msg;
twai_message_t rx_msg;
esp_err_t twai_rx_state;
esp_err_t twai_tx_state;

typedef enum {
    BEGIN_UPDATE,
    RECIVE_REQUEST,
    SEND_HEX_DATA,
    RECIVE_COMPLETE,
    ABORT_UPDATE
} state;

// --- HELPER FUNCTIONS ---

uint16_t calcrc(uint8_t *ptr, int count) {
    uint16_t crc = 0;
    while (--count >= 0) {
        crc = crc ^ (int) * ptr++ << 8;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc = crc << 1;
        }
    }
    return (crc);
}

void initialize_twai(void) {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_TX, GPIO_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // Install TWAI driver
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        ESP_LOGI(TAG, "Driver installed");
    } else {
        ESP_LOGE(TAG, "Failed to install driver");
        return;
    }
    // Start TWAI driver
    if (twai_start() == ESP_OK) {   
        ESP_LOGI(TAG, "Driver started");
    } else {
        ESP_LOGE(TAG, "Failed to start driver");
        return;
    }
}

void release_twai(void) {
    twai_stop();
    twai_driver_uninstall();
    ESP_LOGI(TAG, "Driver Released");
}

uint16_t switch_ota_status(int sum_val) {
    uint16_t return_status = 0;
    if(sum_val == 8){
        OTA_update_flag = true;
        return_status = UPDATE_ONGOING;
    } 
    else if (sum_val == 16){
        OTA_update_flag = false;
        return_status = STOP_UPDATE;   
    }
    else if (sum_val == 24 || sum_val == 48){
        // CRITICAL: BMS is writing to flash, we must pause
        flash_write_status = true;
        return_status = UPDATE_ONGOING;
    }
    else if (sum_val == 32){
        // BMS finished writing
        flash_write_status = false;
        return_status = UPDATE_ONGOING;
        flash_write_counter++;
    }
    else if (sum_val == 0xFF){
        return_status = HANDSHAKE_INIT;
    }
    else if (sum_val == 290){
        return_status = START_UPDATE;
    }
    else if (sum_val == 0x88){
        return_status = REQUEST_RECIEVE_MSG;
    }
    else if (sum_val == 0x90){
        return_status = COMPLETE_RECIEVE_MSG;
    }
    else{
        return_status = NO_UPDATE;
    }
    return return_status;
}

uint16_t recieve_twai(void) {
    uint16_t OTA_status = 0;
    int sum = 0;
    
    twai_rx_state = twai_receive(&rx_msg, pdMS_TO_TICKS(CAN_RECIEVE_DELAY));
    
    if (twai_rx_state == ESP_OK) {
        if (rx_msg.identifier == 0x067B84) {
            // ESP_LOGI(TAG, "RX Data: %X %X ...", rx_msg.data[0], rx_msg.data[1]);
            sum = rx_msg.data[0] + rx_msg.data[1] + rx_msg.data[2] + rx_msg.data[3] + rx_msg.data[4] + rx_msg.data[5] + rx_msg.data[6] + rx_msg.data[7];
            OTA_status = switch_ota_status(sum);
        } 
    }
    else if(OTA_update_flag == true) {
        OTA_status = UPDATE_ONGOING;
    }
    else {
        OTA_status = NO_UPDATE;
    }  
    return OTA_status;
}

// --- SEND FUNCTIONS ---

void send_start_handshake() {
    tx_msg = (twai_message_t){ .extd = 1, .identifier = 0x017B84, .data_length_code = 8, .data = {0x01, 0, 0, 0, 0, 0, 0, 0} };
    twai_transmit(&tx_msg, pdMS_TO_TICKS(100));
}

void send_reset_BMS() {
    tx_msg = (twai_message_t){ .extd = 1, .identifier = 0x017B84, .data_length_code = 8, .data = {0x11, 0, 0, 0, 0, 0, 0, 0} };
    twai_transmit(&tx_msg, pdMS_TO_TICKS(100));
}

void send_start_cmd() {
    vTaskDelay(pdMS_TO_TICKS(START_DELAY));
    tx_msg = (twai_message_t){ .extd = 1, .identifier = 0x027B84, .data_length_code = 8, .data = {0x69, 0x32, 0, 0, 0, 0, 0, 0} };
    twai_transmit(&tx_msg, pdMS_TO_TICKS(100));
}

void send_size() {
    uint8_t size_bytes[2];
    size_bytes[0] = firmware_len & 0xFF;
    size_bytes[1] = firmware_len >> 8;
    tx_msg = (twai_message_t){ .extd = 1, .identifier = 0x037B84, .data_length_code = 8, .data = {size_bytes[0], size_bytes[1], 0, 0, 0, 0, 0, 0} };
    twai_transmit(&tx_msg, pdMS_TO_TICKS(100));
}

// --- STATE MACHINE FUNCTIONS ---

state runstate_begin_update() {
    if(ota_sent_bytes >= firmware_len) {
        return ABORT_UPDATE;
    } else {
        return RECIVE_REQUEST;
    }
}

state runstate_recieve_request() {
    // BLOCKING WAIT (Exactly like original code)
    // Waits forever until BMS requests data
    while(recieve_twai() != REQUEST_RECIEVE_MSG){
        // Optional: Add vTaskDelay(1) here if Watchdog triggers, 
        // but original code didn't have it.
    };
    return SEND_HEX_DATA;
}

state runstate_send_hex_data() {
    int i = 0;
    tx_msg = (twai_message_t){ .extd = 1, .identifier = 0x047B84, .data_length_code = 8, .data = {0,0,0,0,0,0,0,0} };
    
    for(i = 0; i < 2; i++) {
        if(ota_sent_bytes >= firmware_len) break;

        // READ FROM RAM BUFFER
        tx_msg.data[0] = firmware_buffer[byte_count];
        tx_msg.data[1] = firmware_buffer[byte_count + 1];
        tx_msg.data[2] = firmware_buffer[byte_count + 2];
        tx_msg.data[3] = firmware_buffer[byte_count + 3];
        tx_msg.data[4] = firmware_buffer[byte_count + 4];
        tx_msg.data[5] = firmware_buffer[byte_count + 5];

        uint16_t crc = calcrc(tx_msg.data, 6);
        tx_msg.data[6] = crc & 0xFF; 
        tx_msg.data[7] = crc >> 8;

        if(twai_transmit(&tx_msg, pdMS_TO_TICKS(100)) == ESP_OK) {
            ESP_LOGI(TAG, "Sent: %02X %02X ... (%ld/%d)", tx_msg.data[0], tx_msg.data[1], ota_sent_bytes, firmware_len);
            ota_sent_bytes += 6;
            byte_count += 6;
        } else {
            ESP_LOGE(TAG, "Failed to send message");
        }
        vTaskDelay(pdMS_TO_TICKS(CAN_SEND_DELAY));
    }
    return RECIVE_COMPLETE;
}

state runstate_recieve_complete() {
    // BLOCKING WAIT (Exactly like original code)
    while(recieve_twai() != COMPLETE_RECIEVE_MSG){};
    return BEGIN_UPDATE;
}

void ota_update_state_machine() {
    state OTA_update_state = BEGIN_UPDATE;
    bool enable_update = true;
    
    // Important: We don't reset byte_count here because 
    // we might re-enter this function. Code B resets it at start of app.
    
    while(enable_update) {
        switch(OTA_update_state) {
            case BEGIN_UPDATE:
                OTA_update_state = runstate_begin_update();
                break;
            case RECIVE_REQUEST:
                OTA_update_state = runstate_recieve_request();
                break;
            case SEND_HEX_DATA:
                OTA_update_state = runstate_send_hex_data();
                break;
            case RECIVE_COMPLETE:
                OTA_update_state = runstate_recieve_complete();
                break;
            case ABORT_UPDATE:
                enable_update = false;
                strcpy(ota_status_msg, "Done"); // UI Feedback
                break;
        }
    }
}

// --- MAIN TASK ---

void ota_task_entry(void *arg) {
    ESP_LOGI(TAG, "CAN Task Started. RAM Size: %d", firmware_len);
    
    initialize_twai();
    
    ota_sent_bytes = 0;
    byte_count = 0;
    OTA_update_flag = false;
    flash_write_status = false;
    
    strcpy(ota_status_msg, "Initializing...");

    // 1. Initial Sequence (Matched Code B)
    vTaskDelay(pdMS_TO_TICKS(INIT_DELAY));
    send_start_cmd();
    send_size();
    send_start_handshake();

    ESP_LOGI(TAG, "Entering Main Loop");

    while(1) {
        // Check if user requested stop (optional, but good for web)
        // if (!SYSTEM_IS_BUSY) break; 

        uint16_t status = recieve_twai();

        if(status == HANDSHAKE_INIT) {
            send_reset_BMS();
            ESP_LOGI(TAG, "HANDSHAKE OK");
            strcpy(ota_status_msg, "Handshake OK");
            
            // Wait for Start (Blocking)
            while(recieve_twai() != START_UPDATE){};
            
            vTaskDelay(pdMS_TO_TICKS(INIT_DELAY));
            send_start_cmd();
            send_size();
            ESP_LOGI(TAG, "STARTING OTA");
            strcpy(ota_status_msg, "Flashing...");
        }
        else if(status == UPDATE_ONGOING) {
            ota_update_state_machine();
            
            // If machine finishes, we assume success and break the task
            if(ota_sent_bytes >= firmware_len) {
                ESP_LOGI(TAG, "Update Finished Successfully");
                strcpy(ota_status_msg, "Success");
                break; 
            }
        }
        else {
            // ESP_LOGI(TAG, "Waiting...");
        }
        
        // Small yield to prevent Watchdog trigger in the outer loop
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    release_twai();
    SYSTEM_IS_BUSY = false;
    vTaskDelete(NULL);
}

esp_err_t start_can_update_task(void) {
    xTaskCreatePinnedToCore(ota_task_entry, "ota_can_task", 4096, NULL, 5, NULL, 1);
    return ESP_OK;
}