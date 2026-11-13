#include <stddef.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "settings.h"
#include "vems_proto.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE   "vems"
#define NVS_KEY         "settings"
#define SETTINGS_VER    3       // 3: gear_big eklendi (2'de o bayt yapı dolgusuydu)

const settings_fuel_t SETTINGS_FUELS[] = {
    {"Benzin", 14.7f},
    {"E85", 9.8f},
    {"LPG", 15.5f},
    {"Metanol", 6.4f},
    {"Dizel", 14.5f},
};
const int SETTINGS_FUEL_COUNT = sizeof(SETTINGS_FUELS) / sizeof(SETTINGS_FUELS[0]);

const settings_accent_t SETTINGS_ACCENTS[] = {
    {"Mavi", 0x2D9CFF},
    {"Turkuaz", 0x00C8FF},
    {"Yeşil", 0x2ED47A},
    {"Turuncu", 0xFF8A00},
    {"Kırmızı", 0xFF3B30},
    {"Beyaz", 0xF4F5F7},
};
const int SETTINGS_ACCENT_COUNT = sizeof(SETTINGS_ACCENTS) / sizeof(SETTINGS_ACCENTS[0]);

// yeni takım sona eklenir (kayıtlı indeks bozulmasın)
const settings_shift_col_t SETTINGS_SHIFT_COLS[] = {
    {"Klasik", 0x2ED47A, 0xFFC21A, 0xFF3B30, 0x4FB4FF},     // yeşil, sarı, kırmızı, mavi flaş
    {"F1", 0x2ED47A, 0xFF3B30, 0x3D5BFF, 0x3D5BFF},         // yeşil, kırmızı, mavi
    {"Vurgu rengi", 0, 0, 0, 0xFFFFFF},
    {"Kırmızı", 0xFF3B30, 0xFF3B30, 0xFF3B30, 0xFFFFFF},
    {"Beyaz", 0xF4F5F7, 0xF4F5F7, 0xF4F5F7, 0xFF3B30},
};
const int SETTINGS_SHIFT_COL_COUNT = sizeof(SETTINGS_SHIFT_COLS) / sizeof(SETTINGS_SHIFT_COLS[0]);

// sıra ayarlar sayfasındaki - / + sırası; yeni veri sona eklenir (kayıtlı indeksler bozulmasın)
const uint8_t SETTINGS_DIG_CH[] = {
    CH_CLT, CH_FUEL_L100, CH_BATT, CH_MAP, CH_FUEL_LH, CH_BOOST, CH_IAT, CH_TPS, CH_LOAD, CH_ADV, CH_AFR, CH_O2V,
    CH_OIL, CH_FUEL, CH_STFT, CH_LTFT, CH_MAF, CH_O2V2,
};
const int SETTINGS_DIG_CH_COUNT = sizeof(SETTINGS_DIG_CH);

settings_t g_settings;

typedef struct {
    uint8_t ver;
    settings_t s;
} stored_t;

static settings_t defaults(void)
{
    settings_t s = {
#if CONFIG_VEMS_UI_ENABLE
        .brightness = CONFIG_VEMS_UI_BACKLIGHT,
        .shift_rpm = CONFIG_VEMS_UI_RPM_SHIFT,
        .rpm_max = CONFIG_VEMS_UI_RPM_MAX,
#else
        .brightness = 80,
        .shift_rpm = 6500,
        .rpm_max = 8000,
#endif
#if CONFIG_VEMS_UI_FUEL_E85
        .fuel = 1,
#elif CONFIG_VEMS_UI_FUEL_LPG
        .fuel = 2,
#elif CONFIG_VEMS_UI_FUEL_METHANOL
        .fuel = 3,
#else
        .fuel = 0,
#endif
        .pressure = SET_PRESS_MAP,
        .proto = SET_PROTO_AUTO,
        .clt_warn = 105,
        .batt_low = 115,
#if CONFIG_VEMS_UI_BOOT_BEEP
        .beep = 1,
#else
        .beep = 0,
#endif
        .main_view = SET_VIEW_RPM,
        .ble = 1,
        .gear_big = 0,
        .gauge = SET_GAUGE_ANALOG,
        .accent = 0,
        .dig_ch = {0, 1, 2, 3},     // hararet, tüketim, akü, MAP
        .incl_page = 1,
        .incl_warn = 30,
    };
    return s;
}

static bool valid(const settings_t *s)
{
    return s->brightness >= 5 && s->brightness <= 100 &&
           s->shift_rpm >= 3000 && s->shift_rpm <= 9500 &&
           s->rpm_max >= 6000 && s->rpm_max <= 10000 &&
           s->fuel < SETTINGS_FUEL_COUNT && s->pressure <= SET_PRESS_BOOST && s->proto <= SET_PROTO_KLINE &&
           s->clt_warn >= 95 && s->clt_warn <= 115 && s->batt_low >= 105 && s->batt_low <= 125 && s->beep <= 1 &&
           s->main_view <= SET_VIEW_SPEED && s->rotation < 360 && s->ble <= 1 && s->gear_big <= 1 &&
           s->gauge <= SET_GAUGE_DIGITAL && s->accent < SETTINGS_ACCENT_COUNT && s->dig_ch[0] < SETTINGS_DIG_CH_COUNT &&
           s->dig_ch[1] < SETTINGS_DIG_CH_COUNT && s->dig_ch[2] < SETTINGS_DIG_CH_COUNT &&
           s->dig_ch[3] < SETTINGS_DIG_CH_COUNT && s->incl_page <= 1 && s->incl_warn >= 10 && s->incl_warn <= 45 &&
           s->shift_col < SETTINGS_SHIFT_COL_COUNT;
}

static TaskHandle_t s_saver;            // arka plan kayıt görevi (aşağıda)
static void saver_task(void *arg);

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    g_settings = defaults();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (%s), using defaults", esp_err_to_name(err));
        return;
    }

    nvs_handle_t h;
    // eski (ekran açısı alanı olmayan) kayıt da okunur: eksik kalan alanlar varsayılanla dolu kalır
    stored_t st = {.s = g_settings};
    size_t len = sizeof(st);
    const size_t min_len = offsetof(stored_t, s) + offsetof(settings_t, rotation);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_KEY, &st, &len) == ESP_OK && len >= min_len && (st.ver == SETTINGS_VER || st.ver == 2)) {
            if (st.ver == 2) {
                st.s.gear_big = defaults().gear_big;    // sürüm 2 kaydında bu bayt dolguydu: değeri anlamsız
            }
            if (valid(&st.s)) {
                g_settings = st.s;
            }
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "brightness %u%%, shift %u/%u rpm, fuel %s, proto %u, gauge %u",
             g_settings.brightness, g_settings.shift_rpm, g_settings.rpm_max,
             SETTINGS_FUELS[g_settings.fuel].name, g_settings.proto, g_settings.gauge);
    xTaskCreatePinnedToCore(saver_task, "settings", 3072, NULL, 1, &s_saver, 0);
}

/*
 * Kayıt arka planda: flash yazma / silme onlarca ms sürebilir, ekran görevini (LVGL) hiç bekletmesin.
 * settings_save() sadece haber verir; ardışık değişiklikler SAVE_DELAY_MS sonra tek yazmada toplanır.
 */
#define SAVE_DELAY_MS   500

static void save_now(void)
{
    stored_t st = {.ver = SETTINGS_VER, .s = g_settings};
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY, &st, sizeof(st));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void saver_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(SAVE_DELAY_MS));
        ulTaskNotifyTake(pdTRUE, 0);    // bu arada gelen istekler de bu yazmaya dahil
        save_now();
    }
}

void settings_save(void)
{
    if (s_saver) {
        xTaskNotifyGive(s_saver);
    } else {
        save_now();
    }
}

void settings_reset(void)
{
    // ekran açısı ve eğim sıfırı kutunun montajına bağlı: sıfırlamada korunur
    const uint16_t rot = g_settings.rotation;
    int16_t up[3];
    memcpy(up, g_settings.incl_up, sizeof(up));
    g_settings = defaults();
    g_settings.rotation = rot;
    memcpy(g_settings.incl_up, up, sizeof(up));
    settings_save();
}
