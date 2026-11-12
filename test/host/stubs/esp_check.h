#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(x, tag, msg) do { esp_err_t err_rc_ = (x); if (err_rc_ != ESP_OK) { (void)(tag); return err_rc_; } } while (0)
