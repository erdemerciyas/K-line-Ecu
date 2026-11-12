#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** BLE telefon panelini başlatır: GATT sunucusu, reklam ve veri gönderen görev (NVS hazır olmalı). */
esp_err_t ble_panel_start(void);

/** Yayını açar / kapatır (ayarlar sayfası): kapalıyken reklam yok, bağlı telefon ayrılır. */
void ble_panel_set_enabled(bool on);

/** Telefon bağlı ve canlı veriye abone. */
bool ble_panel_connected(void);
