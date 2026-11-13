/*
 * Dijital sürüş sayfası (Ayarlar > Gösterge tipi: Dijital): kenarda vurgu renginde devir yayı, ortada büyük
 * devir / hız ("Ana gösterge"), altında ikinci değer, en altta Ayarlar'dan seçilen 4 veri
 * kutusuz okumalar halinde. Devir ve hız hareket katmanından (ui_motion.c) 30 fps; vites noktasında yay ve devir
 * kırmızı yanıp söner. Akıllı ekran açıksa tam gazda okumalar söner, büyük değer devir olur.
 * Yayın rengi stil değil çizimde verilir: flaşta sadece halka dilimleri yeniden çizilir (tüm 452 px kutu değil).
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
#define RD_DX           66          // okuma sütunları (merkezden)

static struct {
    lv_obj_t *scr;
    lv_obj_t *arc;
    lv_obj_t *red;              // kırmızı bölge (yayın zemini)
    lv_obj_t *val, *unit, *sec, *sec_unit;
    ui_readout_t rd[4];
    vems_ch_t ch[4];
    lv_color_t arc_color;       // yayın o anki rengi (çizimde okunur)
    bool has_speed, speed_view, fresh;
    uint32_t mask;
    int rpm_max, shift;
    float clt;                  // soğuk motorda vites noktası düşer
    float map;
    ui_shift_t sh;
    lv_color_t sc[4];           // vites ışığı renkleri (ayardan)
    bool perf, perf_shown;
    int64_t perf_t;
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
    lv_obj_set_style_pad_bottom(unit, font == &ui_font_big ? 10 : 3, 0);
    return unit;
}

static const char *rd_caption(vems_ch_t c, char *buf, size_t len)
{
    const ui_ch_info_t *ci = &UI_CH[c];
    if (ci->unit[0] && c != CH_FUEL_L100) {
        snprintf(buf, len, "%s %s", ci->caption, ci->unit);
    } else {
        snprintf(buf, len, "%s", ci->caption);
    }
    return buf;
}

uint32_t ui_digital_channels(void)
{
    uint32_t want = CH_BIT(CH_RPM) | (g.has_speed ? CH_BIT(CH_SPEED) : 0);     // MAP sadece kutudaysa (K-Line yavaş)
    for (int i = 0; i < 4; i++) {
        want |= CH_BIT(g.ch[i]);
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

static void opa_anim_cb(void *o, int32_t v)
{
    lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

/** Akıllı ekran geçişi: okumalar 220 ms'de söner / yanar, birimler devir / hıza döner. */
static void perf_apply(bool perf)
{
    g.perf_shown = perf;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *objs[2] = {g.rd[i].cap, g.rd[i].val};
        for (int k = 0; k < 2; k++) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, objs[k]);
            lv_anim_set_exec_cb(&a, opa_anim_cb);
            lv_anim_set_values(&a, lv_obj_get_style_opa(objs[k], 0), perf ? LV_OPA_30 : LV_OPA_COVER);
            lv_anim_set_time(&a, 220);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }
    }
    if (g.speed_view && g.has_speed) {
        ui_label_set(g.unit, perf ? "RPM" : "KM/H");
        if (g.sec) {
            ui_label_set(g.sec_unit, perf ? "KM/H" : "RPM");
        }
    }
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
                          : lead < 0 ? C_PRIMARY : g.sc[lead < SHIFT_LEAD * 2 / 7 ? 0 : lead < SHIFT_LEAD * 5 / 7 ? 1 : 2];
    if (ac.full != g.arc_color.full) {
        g.arc_color = ac;
        // sadece yanık kısım: halka dilim dilim
        ui_ring_invalidate(g.arc, ARC_A0, ARC_A0 + ARC_SPAN * v / 1000.0f + 1, ARC_SIZE / 2 - ARC_W - 2, ARC_SIZE / 2 + 2);
    }

    if (ui_perf_update(&g.perf, &g.perf_t, live ? rpm : NAN, limit, g.map, now) != g.perf_shown) {
        perf_apply(g.perf);
    }

    char buf[16];
    const float spd = live ? mo.speed : NAN;
    const float rpm_txt = live ? roundf(mo.rpm / 50) * 50 : NAN;
    const bool big_speed = g.speed_view && g.has_speed && !g.perf_shown;
    ui_ch_format(big_speed ? CH_SPEED : CH_RPM, big_speed ? spd : rpm_txt, buf, sizeof(buf));
    ui_label_set(g.val, buf);
    set_color(g.val, isnan(big_speed ? spd : rpm_txt) ? C_MUTED : (blink && !big_speed) ? C_ALERT : C_TEXT);
    if (g.sec) {
        ui_ch_format(big_speed ? CH_RPM : CH_SPEED, big_speed ? rpm_txt : spd, buf, sizeof(buf));
        ui_label_set(g.sec, buf);
        set_color(g.sec, isnan(spd) ? C_MUTED : C_SOFT);
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
    g.has_speed = g.mask & CH_BIT(CH_SPEED);
    g.speed_view = g.has_speed && g_settings.main_view == SET_VIEW_SPEED;
    g.rpm_max = g_settings.rpm_max;
    g.shift = g_settings.shift_rpm;
    g.fresh = false;
    g.clt = g.map = NAN;
    g.sh = (ui_shift_t){0};
    g.perf = g.perf_shown = false;
    for (int i = 0; i < 4; i++) {
        g.sc[i] = ui_shift_color(i);
    }

    // kırmızı bölge yayın zemininde, devir göstergesi üstünde
    const int red_a0 = ARC_A0 + (int)lroundf((float)g.shift / g.rpm_max * ARC_SPAN);
    g.red = arc_create(scr, ui_col(0x3A1210), red_a0 % 360, (ARC_A0 + ARC_SPAN) % 360);
    lv_obj_set_style_arc_opa(g.red, LV_OPA_TRANSP, LV_PART_INDICATOR);
    g.arc = arc_create(scr, C_TRACK, ARC_A0, (ARC_A0 + ARC_SPAN) % 360);
    g.arc_color = C_PRIMARY;
    ui_arc_color_bind(g.arc, &g.arc_color);
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
    g.unit = value_row(scr, 92, &ui_font_big, &ui_font_label_lg, &g.val, big_speed ? "KM/H" : "RPM");
    if (g.has_speed) {
        g.sec_unit = value_row(scr, 188, &ui_font_value, &ui_font_label, &g.sec, big_speed ? "RPM" : "KM/H");
    } else {
        g.sec = NULL;
    }

    // 4 okuma, 2 x 2, ortada ince ayırıcı; yayın uç etiketleri (y~376) dışarıda kalır
    for (int i = 0; i < 4; i++) {
        g.ch[i] = SETTINGS_DIG_CH[g_settings.dig_ch[i]];
        ui_readout_create(&g.rd[i], scr, &ui_font_value, (i & 1) ? RD_DX : -RD_DX, i < 2 ? 284 : 340,
                          rd_caption(g.ch[i], buf, sizeof(buf)));
    }
    ui_vline(scr, 0, 290, 96);

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
    g.map = fresh && (g.mask & CH_BIT(CH_MAP)) ? d->map_kpa : NAN;
    if (fresh) {
        ui_motion_feed(d);
    }
    char buf[16];
    for (int i = 0; i < 4; i++) {
        const vems_ch_t c = g.ch[i];
        const float v = fresh && (g.mask & CH_BIT(c)) ? ui_ch_value(d, c) : NAN;
        ui_ch_format(c, v, buf, sizeof(buf));
        ui_readout_set(&g.rd[i], buf, isnan(v) ? C_MUTED : ui_ch_color(c, v));
    }
    frame();
}
