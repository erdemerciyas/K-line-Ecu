#pragma once
// Simülatör: görevler Windows iş parçacığı, bildirim Windows olayı (sim_rtos.c)
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))
#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFu)
#define pdTRUE              1
#define pdFALSE             0
#define pdPASS              1
#define pdFAIL              0
