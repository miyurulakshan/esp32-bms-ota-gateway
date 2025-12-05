#ifndef APP_SHARED_H
#define APP_SHARED_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Status Flags
extern volatile bool SYSTEM_IS_BUSY;

// Data Pointers
extern uint8_t *firmware_buffer;
extern size_t firmware_len;

// Progress Tracking
extern volatile uint32_t ota_sent_bytes;
extern char ota_status_msg[32];

#endif // APP_SHARED_H