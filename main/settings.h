#pragma once

#include <stdint.h>

/*
 * User settings changed on the device (settings page) and kept in NVS.
 * Defaults come from menuconfig. Read from any task, written only by the UI task.
 */

typedef enum { SET_PROTO_AUTO = 0, SET_PROTO_CAN, SET_PROTO_KLINE } set_proto_t;
typedef enum { SET_PRESS_MAP = 0, SET_PRESS_BOOST } set_press_t;
typedef enum { SET_VIEW_RPM = 0, SET_VIEW_SPEED } set_view_t;

typedef struct {
    uint8_t brightness;     // 5..100 %
    uint16_t shift_rpm;     // 3000..9500
    uint16_t rpm_max;       // 6000..10000
    uint8_t fuel;           // index into SETTINGS_FUELS
    uint8_t pressure;       // set_press_t: drive page shows MAP kPa or boost bar
    uint8_t proto;          // set_proto_t, OBD source only
    uint8_t clt_warn;       // 95..115 C
    uint8_t batt_low;       // 105..125 = 10.5..12.5 V
    uint8_t beep;           // boot melody 0 / 1
    uint8_t main_view;      // set_view_t: sürüş sayfasında büyük gösterilen değer
    uint16_t rotation;      // 0..359 derece, ekran saat yönünde döndürülür (kutu içinde yamuk duran ekran için)
    uint8_t ble;            // 0 / 1: Bluetooth telefon paneli yayını (yeni alanlar hep sona: eski kayıt okunabilsin)
    uint8_t gauge;          // set_gauge_t: sürüş sayfası analog (LED halka + ibre) ya da dijital
    uint8_t accent;         // dijital göstergenin vurgu rengi, SETTINGS_ACCENTS indeksi
    uint8_t dig_ch[4];      // dijital göstergenin 4 kutucuğu, SETTINGS_DIG_CH indeksi
    uint8_t incl_page;      // 0 / 1: eğim sayfası
    uint8_t incl_warn;      // 10..45°: yan yatma uyarısı
    int16_t incl_up[3];     // eğim sıfırı: düz zeminde ölçülen "yukarı" (IMU ekseninde, mili-g); 0,0,0 = sıfırlanmadı
    uint8_t shift_col;      // vites ışığı renk takımı, SETTINGS_SHIFT_COLS indeksi
    uint8_t theme;          // set_theme_t: gündüz, gece (kısık), kırmızı gece
    uint8_t smart;          // 0 / 1: akıllı ekran, sürüş sayfası tam gazda sadeleşir
} settings_t;

typedef enum { SET_GAUGE_ANALOG = 0, SET_GAUGE_DIGITAL } set_gauge_t;
typedef enum { SET_THEME_DAY = 0, SET_THEME_NIGHT, SET_THEME_RED } set_theme_t;

typedef struct {
    const char *name;
    uint32_t rgb;
} settings_accent_t;

extern const settings_accent_t SETTINGS_ACCENTS[];
extern const int SETTINGS_ACCENT_COUNT;
extern const uint8_t SETTINGS_DIG_CH[];     // vems_ch_t: dijital kutucuklarda seçilebilen veriler
extern const int SETTINGS_DIG_CH_COUNT;

// vites ışığı: dolarken 3 renk (ilk 2 / orta 3 / son 2 nokta) ve vites noktasındaki flaş; 0 = vurgu rengi
typedef struct {
    const char *name;
    uint32_t lo, mid, hi, flash;
} settings_shift_col_t;

extern const settings_shift_col_t SETTINGS_SHIFT_COLS[];
extern const int SETTINGS_SHIFT_COL_COUNT;

typedef struct {
    const char *name;
    float stoich;
} settings_fuel_t;

extern const settings_fuel_t SETTINGS_FUELS[];
extern const int SETTINGS_FUEL_COUNT;

extern settings_t g_settings;

/** Initialises NVS and loads the settings (menuconfig defaults if nothing valid is stored). */
void settings_init(void);
void settings_save(void);
void settings_reset(void);
