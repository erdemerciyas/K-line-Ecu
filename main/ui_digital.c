/*
 * Dijital sürüş sayfası (Ayarlar > Gösterge tipi: Dijital): kenarda vurgu renginde devir yayı, ortada büyük
 * devir / hız ("Ana gösterge"), altında ikinci değer ve vites kutucuğu, en altta Ayarlar'dan seçilen 4 veri.
 * Devir ve hız hareket katmanından (ui_motion.c) 30 fps; vites noktasında yay ve devir kırmızı yanıp söner.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "ui_priv.h"

#define FRAME_MS        33
#define ARC_SIZE        452
#define ARC_W           16
#define ARC_A0          135         // yay: sol alttan üstten sağ alta 270°
#define ARC_SPAN        270
#define SHIFT_LEAD      1000        // vites noktasından bu kadar önce sarı
#define BLINK_US        (120 * 1000)
#define TILE_W          150
#define TILE_H          44

typedef struct {
    lv_obj_t *box;
    lv_obj_t *cap;
    lv_obj_t *val;
    vems_ch_t ch;
} dtile_t;

static struct {
    lv_obj_t *scr;
    lv_obj_t *arc;
    lv_obj_t *red;              // kırmızı bölge (yayın zemini)
    lv_obj_t *val, *unit, *sec, *sec_unit;
    lv_obj_t *gear_box, *gear;
    dtile_t t[4];
    lv_color_t accent;
    bool has_speed, speed_view, fresh;
    uint32_t mask;
    int rpm_max, shift, gear_shown;
    int64_t gear_t;
    float clt;                  // soğuk motorda vites noktası düşer
    ui_shift_t sh;
    lv_color_t sc[4];           // vites ışığı renkleri (ayardan)
} g;

static void set_color(lv_obj_t *o, lv_color_t c)
{
    if (lv_obj_get_style_text_color(o, 0).full != c.full) {
        lv_obj_set_style_text_color(o, c, 0);
    }
}

static lv_obj_t *arc_create(lv_obj_t *scr, lv_color_t track, int a0, int a1)
{
    lv_obj_t *a = lv_arc_create(scr);
    lv_obj_set_size(a, ARC_SIZE, ARC_SIZE);
    lv_obj_center(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(a, a0, a1);
    lv_arc_set_range(a, 0, 1000);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_arc_width(a, ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, track, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
    return a;
}

static lv_obj_t *value_row(lv_obj_t *scr, int y, const lv_font_t *font, const lv_font_t *ufont, lv_obj_t **val,
                           const char *u)
{
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    *val = ui_label(row, font, C_MUTED, "--");
    lv_obj_t *unit = ui_label(row, ufont, C_MUTED, u);
    lv_obj_set_style_text_letter_space(unit, 2, 0);
    lv_obj_set_style_pad_bottom(unit, font == &ui_font_big ? 8 : 2, 0);
    return unit;
}

static void tile_create(dtile_t *t, lv_obj_t *scr, int x, int y, vems_ch_t ch)
{
    t->ch = ch;
    t->box = lv_obj_create(scr);
    lv_obj_remove_style_all(t->box);
    lv_obj_set_size(t->box, TILE_W, TILE_H);
    lv_obj_set_style_radius(t->box, 10, 0);
    lv_obj_set_style_bg_color(t->box, C_TILE, 0);
    lv_obj_set_style_bg_opa(t->box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t->box, C_TILE_BRD, 0);
    lv_obj_set_style_border_width(t->box, 1, 0);
    lv_obj_clear_flag(t->box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(t->box, LV_ALIGN_TOP_MID, x, y);

    lv_obj_t *stripe = lv_obj_create(t->box);
    lv_obj_remove_style_all(stripe);
    lv_obj_set_size(stripe, 3, TILE_H - 16);
    lv_obj_set_style_radius(stripe, 2, 0);
    lv_obj_set_style_bg_color(stripe, g.accent, 0);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);
    lv_obj_align(stripe, LV_ALIGN_LEFT_MID, 6, 0);

    char buf[24];
    const ui_ch_info_t *ci = &UI_CH[ch];
    snprintf(buf, sizeof(buf), "%s", ci->caption);
    t->cap = ui_label(t->box, &ui_font_label, C_MUTED, buf);
    lv_obj_align(t->cap, LV_ALIGN_TOP_LEFT, 15, 3);
    lv_obj_t *unit = ui_label(t->box, &ui_font_label, C_TICK, ci->unit);
    lv_obj_align(unit, LV_ALIGN_BOTTOM_LEFT, 15, -3);
    t->val = ui_label(t->box, &ui_font_value, C_MUTED, "--");
    lv_obj_align(t->val, LV_ALIGN_RIGHT_MID, -10, 1);
}

uint32_t ui_digital_channels(void)
{
    uint32_t want = CH_BIT(CH_RPM) | (g.has_speed ? CH_BIT(CH_SPEED) : 0);
    for (int i = 0; i < 4; i++) {
        want |= CH_BIT(g.t[i].ch);
    }
    return want;
}

bool ui_digital_toggle_view(void)
{
    if (!g.has_speed) {
        return false;
    }
    g_settings.main_view = g.speed_view ? SET_VIEW_RPM : SET_VIEW_SPEED;
    settings_save();
    ui_request_drive_rebuild();
    return true;
}

static void frame(void)
{
    const int64_t now = esp_timer_get_time();
    ui_motion_t mo;
    ui_motion_get(now, &mo);
    const bool live = g.fresh && !isnan(mo.rpm);
    const float rpm = live ? mo.rpm : 0;
    const int limit = ui_shift_limit(g.shift, g.clt);
    const bool shift = ui_shift_update(&g.sh, live ? rpm : NAN, limit, now);
    const bool blink = ui_shift_blink(&g.sh, now, BLINK_US);

    // devir yayı: vurgu rengi, vites noktasına yaklaşınca vites ışığı renkleriyle dolar, noktada flaş
    const int v = (int)lroundf(fminf(rpm / g.rpm_max, 1) * 1000);
    if (lv_arc_get_value(g.arc) != v) {
        lv_arc_set_value(g.arc, v);
    }
    const float lead = rpm - (limit - SHIFT_LEAD);
    const lv_color_t ac = shift ? (blink ? g.sc[3] : C_TICK)
                          : lead < 0 ? g.accent : g.sc[lead < SHIFT_LEAD * 2 / 7 ? 0 : lead < SHIFT_LEAD * 5 / 7 ? 1 : 2];
    if (lv_obj_get_style_arc_color(g.arc, LV_PART_INDICATOR).full != ac.full) {
        lv_obj_set_style_arc_color(g.arc, ac, LV_PART_INDICATOR);
    }

    char buf[16];
    const float spd = live ? mo.speed : NAN;
    const float rpm_txt = live ? roundf(mo.rpm / 50) * 50 : NAN;
    const bool big_speed = g.speed_view && g.has_speed;
    ui_ch_format(big_speed ? CH_SPEED : CH_RPM, big_speed ? spd : rpm_txt, buf, sizeof(buf));
    ui_label_set(g.val, buf);
    set_color(g.val, isnan(big_speed ? spd : rpm_txt) ? C_MUTED : (blink && !big_speed) ? C_ALERT : C_TEXT);
    if (g.sec) {
        ui_ch_format(big_speed ? CH_RPM : CH_SPEED, big_speed ? rpm_txt : spd, buf, sizeof(buf));
        ui_label_set(g.sec, buf);
        set_color(g.sec, isnan(spd) ? C_MUTED : C_SOFT);
    }

    if (g.has_speed) {
        const int gear = live ? mo.gear : 0;
        if (gear != g.gear_shown) {
            g.gear_shown = gear;
            g.gear_t = now;
            snprintf(buf, sizeof(buf), gear ? "%d" : "N", gear);
            lv_label_set_text(g.gear, buf);
        }
        lv_color_t gc = !live ? C_TICK : shift ? (blink ? C_ALERT : C_TEXT) : now - g.gear_t < 250 * 1000 ? g.accent
                        : rpm >= limit - SHIFT_LEAD ? C_WARN : C_TEXT;
        set_color(g.gear, gc);
    }
}

static void timer_cb(lv_timer_t *t)
{
    if (lv_scr_act() == g.scr) {
        frame();
    }
}

static void scr_delete_cb(lv_event_t *e)
{
    lv_timer_del(lv_event_get_user_data(e));
    // yeniden kurulumda eski ekran yenisi oluştuktan sonra silinir: yeni ekranı unutma
    if (lv_event_get_target(e) == g.scr) {
        g.scr = NULL;
    }
}

lv_obj_t *ui_digital_create(int page, int pages, uint32_t mask)
{
    lv_obj_t *scr = ui_screen_create();
    g.scr = scr;
    g.mask = ui_ch_layout_mask(mask);
    g.accent = lv_color_hex(SETTINGS_ACCENTS[g_settings.accent].rgb);
    g.has_speed = g.mask & CH_BIT(CH_SPEED);
    g.speed_view = g.has_speed && g_settings.main_view == SET_VIEW_SPEED;
    g.rpm_max = g_settings.rpm_max;
    g.shift = g_settings.shift_rpm;
    g.gear_shown = -1;
    g.fresh = false;
    g.clt = NAN;
    g.sh = (ui_shift_t){0};
    for (int i = 0; i < 4; i++) {
        g.sc[i] = ui_shift_color(i);
    }

    // kırmızı bölge yayın zemininde, devir göstergesi üstünde
    const int red_a0 = ARC_A0 + (int)lroundf((float)g.shift / g.rpm_max * ARC_SPAN);
    g.red = arc_create(scr, lv_color_hex(0x3A1210), red_a0 % 360, (ARC_A0 + ARC_SPAN) % 360);
    lv_obj_set_style_arc_opa(g.red, LV_OPA_TRANSP, LV_PART_INDICATOR);
    g.arc = arc_create(scr, C_TRACK, ARC_A0, (ARC_A0 + ARC_SPAN) % 360);
    lv_obj_set_style_arc_color(g.arc, g.accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(g.arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t *track = arc_create(scr, C_TRACK, ARC_A0, red_a0 % 360);     // kırmızı bölgeye kadar koyu zemin
    lv_obj_set_style_arc_opa(track, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_move_background(track);
    lv_obj_move_background(g.red);

    // 1000 rpm etiketleri yayın içinde
    char buf[24];
    for (int k = 0; k * 1000 <= g.rpm_max; k++) {
        const float a = (ARC_A0 + (float)k * 1000 / g.rpm_max * ARC_SPAN) / 57.29578f;
        snprintf(buf, sizeof(buf), "%d", k);
        lv_obj_t *l = ui_label(scr, &ui_font_label, k * 1000 >= g.shift ? C_ALERT : C_TICK, buf);
        lv_obj_align(l, LV_ALIGN_CENTER, (lv_coord_t)lroundf(cosf(a) * 192), (lv_coord_t)lroundf(sinf(a) * 192));
    }

    const bool big_speed = g.speed_view;
    g.unit = value_row(scr, 110, &ui_font_big, &ui_font_label_lg, &g.val, big_speed ? "KM/H" : "RPM");
    if (g.has_speed) {
        g.sec_unit = value_row(scr, 184, &ui_font_value, &ui_font_label, &g.sec, big_speed ? "RPM" : "KM/H");
        g.gear_box = lv_obj_create(scr);
        lv_obj_remove_style_all(g.gear_box);
        lv_obj_set_size(g.gear_box, 52, 40);
        lv_obj_set_style_radius(g.gear_box, 10, 0);
        lv_obj_set_style_bg_color(g.gear_box, C_TILE, 0);
        lv_obj_set_style_bg_opa(g.gear_box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(g.gear_box, g.accent, 0);
        lv_obj_set_style_border_width(g.gear_box, 1, 0);
        lv_obj_clear_flag(g.gear_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(g.gear_box, LV_ALIGN_TOP_MID, 0, 222);
        g.gear = ui_label(g.gear_box, &ui_font_label_lg, C_TICK, "N");
        lv_obj_center(g.gear);
    } else {
        g.sec = NULL;
        g.gear = NULL;
    }

    for (int i = 0; i < 4; i++) {
        const vems_ch_t ch = SETTINGS_DIG_CH[g_settings.dig_ch[i]];
        tile_create(&g.t[i], scr, (i & 1) ? (TILE_W + 10) / 2 : -(TILE_W + 10) / 2, i < 2 ? 272 : 320, ch);    // yayın uç etiketleri (y~376) altta kalır
    }

    ui_page_dots(scr, page, pages);
    ui_motion_reset();
    lv_timer_t *t = lv_timer_create(timer_cb, FRAME_MS, NULL);
    lv_obj_add_event_cb(scr, scr_delete_cb, LV_EVENT_DELETE, t);
    return scr;
}

void ui_digital_update(const vems_data_t *d, bool fresh)
{
    g.fresh = fresh;
    g.clt = fresh && (g.mask & CH_BIT(CH_CLT)) ? d->clt_c : NAN;
    if (fresh) {
        ui_motion_feed(d);
    }
    char buf[16];
    for (int i = 0; i < 4; i++) {
        dtile_t *t = &g.t[i];
        const float v = fresh && (g.mask & CH_BIT(t->ch)) ? ui_ch_value(d, t->ch) : NAN;
        ui_ch_format(t->ch, v, buf, sizeof(buf));
        ui_label_set(t->val, buf);
        set_color(t->val, isnan(v) ? C_MUTED : ui_ch_color(t->ch, v));
    }
    frame();
}
