#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len) { return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) {}
