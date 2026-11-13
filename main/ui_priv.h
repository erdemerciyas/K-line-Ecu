#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "vems_proto.h"
#include "settings.h"

/*
 * HALKA renk rolleri. Her renk ui_col() süzgecinden geçer: tema (Ayarlar > Tema) gündüzde aynen, gecede kısık ve
 * hafif soluk, kırmızı gecede tek renk kırmızı tonlarda. Kural: gri her şeydir, vurgu (C_PRIMARY, Ayarlar > Vurgu
 * rengi) aracın şu anki halidir, iyi / dikkat / alarm renkleri sadece bir eşik aşılınca görünür.
 */
lv_color_t ui_col(uint32_t rgb);        // temaya göre renk
lv_color_t ui_accent(void);             // ayardaki vurgu rengi, temaya göre

#define C_BG        ui_col(0x000000)
#define C_TRACK     ui_col(0x16181D)    // yay zemini, sönük segment
#define C_TILE      ui_col(0x0D1015)    // yüzey: basılı satır, düğme
#define C_TILE_BRD  ui_col(0x232832)    // yüzey kenarı
#define C_LINE      ui_col(0x171A20)    // ayırıcı çizgi
#define C_TEXT      ui_col(0xF3F5F8)    // birincil yazı: kahraman değer
#define C_SOFT      ui_col(0xC4CAD4)    // ikincil değer
#define C_MUTED     ui_col(0x8A919D)    // başlık, birim, veri yok
#define C_TICK      ui_col(0x4E5562)    // üçüncül: ölçek rakamı, ipucu
#define C_ACCENT    ui_col(0x00C8FF)    // soğuk (motor ısınıyor, ekonomi bölgesi)
#define C_OK        ui_col(0x2ED47A)
#define C_WARN      ui_col(0xFFB020)
#define C_ALERT     ui_col(0xFF3B30)
#define C_PRIMARY   ui_accent()         // vurgu: ibre, seçili öğe, vites, sayfa göstergesi


// Barlow Semi Condensed (OFL), tools/gen_fonts.sh ile; rakamlar sabit genişlik (değer değişince sayı kaymaz)
LV_FONT_DECLARE(ui_font_big);       // 104 px semibold, rakam . + - : % ° only
LV_FONT_DECLARE(ui_font_display);   // 56 px bold, rakam . + - : % ° N R only (vites, büyük okuma)
LV_FONT_DECLARE(ui_font_value);     // 34 px semibold, rakam . + - : % ° only
LV_FONT_DECLARE(ui_font_title);     // 30 px semibold, falls back to Montserrat 24 for symbols
LV_FONT_DECLARE(ui_font_label);     // 16 px semibold, falls back to Montserrat 14 for symbols
LV_FONT_DECLARE(ui_font_label_lg);  // 22 px semibold, falls back to Montserrat 16 for symbols
LV_FONT_DECLARE(ui_font_gear);      // 160 px bold, vites: 1-6 N R - only

// HALKA geometrisi: taç (yazı / durum) dış kenarda, sayfa göstergesi altta kenarda
#define UI_CROWN_R  206     // kavisli sayfa başlığının yarıçapı (dış halkası olmayan sayfalar)
#define UI_DOTS_R   228     // kavisli sayfa göstergesi

// okuma: kutusuz, ortalı başlık + değer (eski kutucukların yerine); yan yana olanların arasına ui_vline
typedef struct {
    lv_obj_t *cap;
    lv_obj_t *val;
    char last[16];
} ui_readout_t;

// ---- common helpers (ui_common.c) ----
lv_obj_t *ui_screen_create(void);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt);
void ui_label_set(lv_obj_t *label, const char *txt);   // only invalidates on change
void ui_text_color(lv_obj_t *o, lv_color_t c);         // yazı rengi, sadece değişince (yeniden çizim yok)
void ui_readout_create(ui_readout_t *r, lv_obj_t *parent, const lv_font_t *font, int cx, int y, const char *caption);
void ui_readout_set(ui_readout_t *r, const char *txt, lv_color_t color);
void ui_readout_set_num(ui_readout_t *r, const char *fmt, float v, lv_color_t color);  // NAN -> "--"
lv_obj_t *ui_vline(lv_obj_t *parent, int cx, int y, int h);             // ince dikey ayırıcı (cx: merkeze göre)
void ui_page_dots(lv_obj_t *scr, int active, int count);                // alt kenarda kavisli sayfa göstergesi
lv_obj_t *ui_ring_meter(lv_obj_t *scr);
void ui_meter_label_div10_cb(lv_event_t *e);
/** Kavisli yazı: merkezi deg açısında (270 = üst, 90 = alt; altta harfler ters dönmez), r yarıçaplı çember üstünde. */
lv_obj_t *ui_arc_text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt, int r, int deg);
void ui_arc_text_set(lv_obj_t *o, const char *txt, lv_color_t color);  // sadece değişince yeniden çizer
lv_obj_t *ui_header(lv_obj_t *scr, const char *txt, int y);            // sayfa başlığı: üst taçta kavisli (y kullanılmaz)
lv_obj_t *ui_scroll_list(lv_obj_t *scr);                                // round-safe vertical list area, kenarları söner
lv_obj_t *ui_list_row(lv_obj_t *list, const char *name, lv_obj_t **value);
lv_obj_t *ui_pill_button(lv_obj_t *parent, const char *txt, int w);
/** Çember diliminin [a0, a1]° (saat yönü, 0 = sağ) rin..rout kısmının sınır kutusu, ekrana kırpılmış. */
void ui_sector_area(float a0, float a1, int rin, int rout, lv_area_t *out);
/** Halka parçasını 20°'lik dilimlerle geçersiz kılar: tek sınır kutusu neredeyse tüm ekran olurdu. */
void ui_ring_invalidate(lv_obj_t *obj, float a0, float a1, int rin, int rout);
/** Akıllı ekran (Ayarlar): tam gazda / vites noktasına yakın sürüş sayfası sadeleşir. Histerezisli. */
bool ui_perf_update(bool *state, int64_t *t, float rpm, int shift, float map, int64_t now);
/** Yayın dolgu rengini *color'dan çizer; renk değişince sayfa ui_ring_invalidate çağırır. */
void ui_arc_color_bind(lv_obj_t *arc, const lv_color_t *color);

// ---- AFR standards ----
typedef struct {
    float lambda_max;       // zone upper bound (lambda)
    const char *name;
    uint32_t color;
} ui_afr_zone_t;

extern const ui_afr_zone_t UI_AFR_ZONES[];
extern const int UI_AFR_ZONE_COUNT;

float ui_stoich_afr(void);
const char *ui_fuel_name(void);         // upper case, Turkish
const ui_afr_zone_t *ui_afr_zone(float lambda);
bool ui_lambda_valid(const vems_data_t *d);

// ---- channels (ui_channels.c): one table for every source ----
typedef struct {
    const char *name;       // list / single gauge, e.g. "Hararet"
    const char *caption;    // tiles, upper case, e.g. "HARARET"
    const char *unit;
    uint8_t dec;
    float min;              // single gauge ring range
    float max;
} ui_ch_info_t;

extern const ui_ch_info_t UI_CH[CH_COUNT];

/** Value of a channel from the latest frame, NAN when the frame has none (yet). */
float ui_ch_value(const vems_data_t *d, vems_ch_t c);
void ui_ch_format(vems_ch_t c, float v, char *buf, size_t len);
lv_color_t ui_ch_color(vems_ch_t c, float v);
/** Channels of a mask in display order; mask 0 (nothing connected yet) gives a typical set. */
int ui_ch_list(uint32_t mask, vems_ch_t *out, int max);
uint32_t ui_ch_layout_mask(uint32_t mask);
/** Session min / max per channel (single gauge page). */
void ui_ch_track(const vems_data_t *d, uint32_t mask);
bool ui_ch_minmax(vems_ch_t c, float *lo, float *hi);
void ui_ch_reset(vems_ch_t c);

// ---- hareket katmanı (ui_motion.c): seyrek K-Line örneklerinden 60 fps devir / hız / vites ----
#define UI_GEARS    5

typedef struct {
    float rpm;              // o anki tahmin, NAN = örnek yok
    float rpm_rate;         // rpm/s
    float speed;            // km/h, vites oturmuşsa devirden; NAN = yok
    int gear;               // 1..UI_GEARS, 0 = boş / bilinmiyor
} ui_motion_t;

void ui_motion_reset(void);
void ui_motion_feed(const vems_data_t *d);              // yeni çerçeve: yeni devir / hız örneği varsa filtreye
void ui_motion_get(int64_t now, ui_motion_t *out);

// ---- vites ışığı (ui_motion.c): iki sürüş sayfasında aynı kural ----
#define UI_SHIFT_HYST   150         // rpm: flaş noktanın bu kadar altına inmeden sönmez (eşikte titreme yok)

typedef struct {
    bool on;
    int64_t t0;             // flaşın başladığı an: ilk faz hep yanık
} ui_shift_t;

int ui_shift_limit(int shift_rpm, float clt);           // soğuk motorda düşürülmüş vites noktası
bool ui_shift_update(ui_shift_t *s, float rpm, int limit, int64_t now);
bool ui_shift_blink(const ui_shift_t *s, int64_t now, int64_t period_us);
lv_color_t ui_shift_color(int stage);                   // 0..2 dolma sırası, 3 flaş (Ayarlar > Vites ışığı rengi)

// ---- services of the screen manager (dash_ui.c), safe to call from widget events ----
void ui_request_rebuild(void);          // a setting changed the page layout
void ui_request_drive_rebuild(void);    // sadece sürüş + odak sayfası (gösterge ayarı): diğer sayfalar kalır
void ui_request_rescan(void);           // drop the connection, show the boot screen again
void dash_ui_set_rotation(uint16_t deg); // ekranı 0..359° döndürür (LVGL görevinden çağrılır)
void ui_request_dtc_page(void);         // arıza kodları sayfasına geç (uyarı şeridi)

// ---- uyarılar (ui_alert.c): her sayfanın üstünde şerit / kart ve buzzer ----
void ui_alert_init(void);
void ui_alert_update(const vems_data_t *d, bool fresh, uint32_t mask);
void ui_alert_theme(void);                  // tema değişince (sayfalar yeniden kurulurken)

// ---- screens ----
lv_obj_t *ui_splash_create(void);
bool ui_splash_update(void);                        // true when the splash may close


lv_obj_t *ui_main_create(int page, int pages, uint32_t mask);
void ui_main_update(const vems_data_t *d, bool fresh);
uint32_t ui_main_channels(void);                    // sayfadaki kanallar: OBD bunları hızlı sorgular
bool ui_main_toggle_view(void);                     // büyük değer devir <-> hız, hız yoksa false

lv_obj_t *ui_digital_create(int page, int pages, uint32_t mask);     // dijital sürüş sayfası
void ui_digital_update(const vems_data_t *d, bool fresh);
uint32_t ui_digital_channels(void);
bool ui_digital_toggle_view(void);

lv_obj_t *ui_incline_create(int page, int pages);                   // eğim sayfası (QMI8658)
void ui_incline_zero(void);

lv_obj_t *ui_focus_create(int page, int pages, uint32_t mask);
void ui_focus_update(const vems_data_t *d, bool fresh);
void ui_focus_step(int dir);
void ui_focus_reset(void);
uint32_t ui_focus_channels(void);

lv_obj_t *ui_list_create(int page, int pages, uint32_t mask);
uint32_t ui_list_channels(void);
void ui_list_update(const vems_data_t *d, bool fresh);

lv_obj_t *ui_afr_create(int page, int pages);
void ui_afr_update(const vems_data_t *d, bool fresh);
void ui_afr_reset_minmax(void);

lv_obj_t *ui_dtc_create(int page, int pages);
void ui_dtc_update(const vems_data_t *d, bool fresh);

lv_obj_t *ui_settings_create(int page, int pages);
void ui_settings_update(void);

