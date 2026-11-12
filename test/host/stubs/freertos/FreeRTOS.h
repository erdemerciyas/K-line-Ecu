#pragma once
// Tek iş parçacıklı FreeRTOS taklidi: zaman simülatörün saatidir (1 tick = 1 ms, CONFIG_FREERTOS_HZ=1000)
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))
#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFu)
#define pdTRUE              1
#define pdFALSE             0
#define pdPASS              1
