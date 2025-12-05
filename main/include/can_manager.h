#ifndef CAN_MANAGER_H
#define CAN_MANAGER_H

#include <esp_err.h>

// Starts the FreeRTOS task that handles the CAN update
// Returns ESP_OK if started successfully
esp_err_t start_can_update_task(void);

// Helper to stop driver manually if needed (usually handled internally)
void stop_can_driver(void);

#endif // CAN_MANAGER_H