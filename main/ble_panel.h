#pragma once

#include "esp_err.h"

/** BLE telefon panelini başlatır: GATT sunucusu, reklam ve veri gönderen görev (NVS hazır olmalı). */
esp_err_t ble_panel_start(void);
