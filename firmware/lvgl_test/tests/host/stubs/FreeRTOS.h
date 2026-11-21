/*
 * Minimal FreeRTOS stubs for host unit tests of pure logic that includes
 * board_power.h (which needs TickType_t/SemaphoreHandle_t).
 * NOT for firmware builds.
 */
#ifndef HOST_STUB_FREERTOS_H
#define HOST_STUB_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   1
#define pdFAIL   0
#define portMAX_DELAY 0xFFFFFFFFu
#define configTICK_RATE_HZ 1000u
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif
