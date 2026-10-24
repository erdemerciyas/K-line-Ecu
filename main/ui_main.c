/*
 * Drive page: RPM ring with shift light, big RPM, speed (when the ECU reports it), six tiles and an
 * info line. Tiles are picked from the channels the source really delivers, in priority order:
 * pressure (MAP or boost, settings), AFR or narrow band O2, coolant, intake air, battery, throttle ...
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "board_lcd.h"
#include "ui_priv.h"

#define TILE_COUNT  6
#define EXTRA_COUNT 3

static struct {
    lv_obj_t *meter;
    lv_meter_indicator_t *rpm_arc;
    lv_color_t rpm_color;
    lv_obj_t *shift;            // bezel ring flashing at the shift point
    bool blink;
    lv_obj_t *status;
    lv_obj_t *rpm;
    lv_obj_t *sub;
    lv_obj_t *info;
    lv_obj_t *flags;
    ui_tile_t tiles[TILE_COUNT];
    vems_ch_t tile_ch[TILE_COUNT];
    int ntiles;
    vems_ch_t extra[EXTRA_COUNT];
    int nextra;
    bool has_speed;
    int64_t rate_t0;
    uint32_t rate_seq0;
    int hz;
} m;

static bool chosen(vems_ch_t c)
{
    for (int i = 0; i < m.ntiles; i++) {
        if (m.tile_ch[i] == c) {
            return true;
        }
    }
    return false;
}

static void pick_channels(uint32_t mask)
{
    mask = ui_ch_layout_mask(mask);
    const vems_ch_t pressure = g_settings.pressure == SET_PRESS_BOOST ? CH_BOOST : CH_MAP;
    const vems_ch_t prio[] = {pressure, CH_AFR, CH_O2V, CH_CLT, CH_IAT, CH_BATT, CH_TPS, CH_FUEL_L100, CH_LOAD,
                              CH_ADV, CH_OIL, CH_FUEL, CH_PW, CH_VE, CH_MAF};
    m.ntiles = 0;
    for (size_t i = 0; i < sizeof(prio) / sizeof(prio[0]) && m.ntiles < TILE_COUNT; i++) {
        vems_ch_t c = prio[i];
        if (!(mask & CH_BIT(c)) || (c == CH_O2V && (mask & CH_BIT(CH_AFR)))) {
            continue;
        }
        m.tile_ch[m.ntiles++] = c;
    }
    const vems_ch_t extra[] = {CH_FUEL_LH, CH_O2V2, CH_ADV, CH_LOAD, CH_OIL, CH_FUEL, CH_PW, CH_VE, CH_MAF, CH_STFT, CH_LTFT,
                               CH_IAC};
    m.nextra = 0;
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]) && m.nextra < EXTRA_COUNT; i++) {
        if ((mask & CH_BIT(extra[i])) && !chosen(extra[i])) {
            m.extra[m.nextra++] = extra[i];
        }
    }
    m.has_speed = mask & CH_BIT(CH_SPEED);
}

uint32_t ui_main_channels(void)
{
    uint32_t want = CH_BIT(CH_RPM) | (m.has_speed ? CH_BIT(CH_SPEED) : 0);
    for (int i = 0; i < m.ntiles; i++) {
        want |= CH_BIT(m.tile_ch[i]);
    }
    for (int i = 0; i < m.nextra; i++) {
        want |= CH_BIT(m.extra[i]);
    }
    return want;
}

lv_obj_t *ui_main_create(int page, int pages, uint32_t mask)
{
    const int rpm_max = g_settings.rpm_max, rpm_shift = g_settings.shift_rpm;
    lv_obj_t *scr = ui_screen_create();
    pick_channels(mask);

    // RPM ring, scale in 100 rpm units, labels in 1000 rpm
    m.meter = ui_ring_meter(scr);
    const int max = rpm_max / 100;
    lv_meter_scale_t *s = lv_meter_add_scale(m.meter);
    lv_meter_set_scale_range(m.meter, s, 0, max, 270, 135);
    lv_meter_set_scale_ticks(m.meter, s, max / 2 + 1, 2, 8, C_TICK);
    lv_meter_set_scale_major_ticks(m.meter, s, 5, 3, 16, C_SOFT, 12);

    lv_meter_indicator_t *red_ticks = lv_meter_add_scale_lines(m.meter, s, C_ALERT, C_ALERT, false, 0);
    lv_meter_set_indicator_start_value(m.meter, red_ticks, rpm_shift / 100);
    lv_meter_set_indicator_end_value(m.meter, red_ticks, max);
    lv_meter_indicator_t *red_arc = lv_meter_add_arc(m.meter, s, 3, C_ALERT, 0);
    lv_meter_set_indicator_start_value(m.meter, red_arc, rpm_shift / 100);
    lv_meter_set_indicator_end_value(m.meter, red_arc, max);

    lv_meter_indicator_t *track = lv_meter_add_arc(m.meter, s, 16, C_TRACK, 24);
    lv_meter_set_indicator_start_value(m.meter, track, 0);
    lv_meter_set_indicator_end_value(m.meter, track, max);
    m.rpm_color = C_PRIMARY;
    m.rpm_arc = lv_meter_add_arc(m.meter, s, 16, m.rpm_color, 24);
    lv_meter_set_indicator_start_value(m.meter, m.rpm_arc, 0);
    lv_meter_set_indicator_end_value(m.meter, m.rpm_arc, 0);

    m.shift = lv_obj_create(scr);
    lv_obj_remove_style_all(m.shift);
    lv_obj_set_size(m.shift, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_center(m.shift);
    lv_obj_set_style_radius(m.shift, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(m.shift, C_ALERT, 0);
    lv_obj_set_style_border_width(m.shift, 8, 0);
    lv_obj_set_style_border_opa(m.shift, LV_OPA_COVER, 0);
    lv_obj_clear_flag(m.shift, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(m.shift, LV_OBJ_FLAG_HIDDEN);

    m.status = ui_label(scr, &ui_font_label, C_WARN, "");
    lv_obj_set_style_text_letter_space(m.status, 1, 0);
    lv_obj_align(m.status, LV_ALIGN_TOP_MID, 0, 80);

    m.rpm = ui_label(scr, &ui_font_big, C_MUTED, "----");
    lv_obj_align(m.rpm, LV_ALIGN_TOP_MID, 0, 98);
    m.sub = ui_label(scr, &ui_font_label_lg, C_MUTED, "RPM");
    lv_obj_set_style_text_letter_space(m.sub, 4, 0);
    lv_obj_align(m.sub, LV_ALIGN_TOP_MID, 0, 178);

    static const int X[3] = {UI_COL_X0, UI_COL_X1, UI_COL_X2};
    for (int i = 0; i < m.ntiles; i++) {
        char cap[24];
        const ui_ch_info_t *ci = &UI_CH[m.tile_ch[i]];
        if (ci->unit[0] && m.tile_ch[i] != CH_BOOST && m.tile_ch[i] != CH_FUEL_L100) {
            snprintf(cap, sizeof(cap), "%s %s", ci->caption, ci->unit);
        } else {
            snprintf(cap, sizeof(cap), "%s", ci->caption);
        }
        ui_tile_create(&m.tiles[i], scr, X[i % 3], 210 + (i / 3) * 66, cap);
    }

    m.info = ui_label(scr, &ui_font_label_lg, C_TEXT, "");
    lv_label_set_recolor(m.info, true);
    lv_obj_align(m.info, LV_ALIGN_TOP_MID, 0, 344);

    m.flags = ui_label(scr, &ui_font_label, C_MUTED, "");
    lv_label_set_recolor(m.flags, true);
    lv_obj_set_style_text_letter_space(m.flags, 1, 0);
    lv_obj_align(m.flags, LV_ALIGN_TOP_MID, 0, 372);

    ui_page_dots(scr, page, pages);
    m.rate_t0 = 0;
    m.hz = 0;
    return scr;
}

static void set_rpm_arc(uint16_t rpm)
{
    const int rpm_shift = g_settings.shift_rpm;
    lv_color_t col = rpm >= rpm_shift ? (m.blink ? C_TEXT : C_ALERT) : (rpm >= rpm_shift - 1000 ? C_WARN : C_PRIMARY);
    if (col.full != m.rpm_color.full) {
        m.rpm_color = col;
        m.rpm_arc->type_data.arc.color = col;
        lv_obj_invalidate(m.meter);
    }
    int v = rpm / 100;
    if (v > g_settings.rpm_max / 100) {
        v = g_settings.rpm_max / 100;
    }
    if (m.rpm_arc->end_value != v) {
        lv_meter_set_indicator_end_value(m.meter, m.rpm_arc, v);
    }
}

static void set_shift_light(bool on)
{
    if (on) {
        lv_obj_clear_flag(m.shift, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(m.shift, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_main_update(const vems_data_t *d, bool fresh)
{
    char buf[200], v[16];

    if (!fresh) {
        const vems_state_t st = vems_get_state();
        const char *wait = st == VEMS_STATE_ECU_OK || st == VEMS_STATE_LINK_UP ? "ECU SORGULANIYOR"
                           : "OBD ARANIYOR";
        ui_label_set(m.status, d->seq && st != VEMS_STATE_WAIT_LINK ? LV_SYMBOL_WARNING "  VERİ YOK" : wait);
        lv_obj_set_style_text_color(m.status, d->seq && st != VEMS_STATE_WAIT_LINK ? C_ALERT : C_WARN, 0);
        ui_label_set(m.rpm, "----");
        lv_obj_set_style_text_color(m.rpm, C_MUTED, 0);
        ui_label_set(m.sub, m.has_speed ? "-- KM/H" : "RPM");
        lv_obj_set_style_text_color(m.sub, C_MUTED, 0);
        m.blink = false;
        set_rpm_arc(0);
        set_shift_light(false);
        for (int i = 0; i < m.ntiles; i++) {
            ui_tile_set(&m.tiles[i], "--", C_MUTED);
        }
        ui_label_set(m.info, "");
        ui_label_set(m.flags, "");
        m.hz = 0;
        return;
    }

    int64_t now = esp_timer_get_time();
    if (m.rate_t0 == 0) {
        m.rate_t0 = now;
        m.rate_seq0 = d->seq;
    } else if (now - m.rate_t0 >= 1000000) {
        m.hz = (int)((d->seq - m.rate_seq0) * 1e6f / (float)(now - m.rate_t0) + 0.5f);
        m.rate_t0 = now;
        m.rate_seq0 = d->seq;
    }
    vems_link_info_t li;
    vems_get_link_info(&li);
    snprintf(buf, sizeof(buf), "%s  |  %d Hz", li.protocol, m.hz);
    ui_label_set(m.status, buf);
    lv_obj_set_style_text_color(m.status, C_OK, 0);

    // shift light: arc, number and bezel flash at 5 Hz (sayfa yeni veride de güncellenir, saatten türetilir)
    const bool shift = d->rpm >= g_settings.shift_rpm;
    m.blink = shift && ((now / 100000) & 1);
    snprintf(buf, sizeof(buf), "%u", d->rpm);
    ui_label_set(m.rpm, buf);
    lv_obj_set_style_text_color(m.rpm, (shift && m.blink) ? C_ALERT : C_TEXT, 0);
    set_rpm_arc(d->rpm);
    set_shift_light(m.blink);

    if (m.has_speed) {
        ui_ch_format(CH_SPEED, d->speed_kmh, v, sizeof(v));
        snprintf(buf, sizeof(buf), "%s KM/H", v);
        ui_label_set(m.sub, buf);
        lv_obj_set_style_text_color(m.sub, isnan(d->speed_kmh) ? C_MUTED : C_TEXT, 0);
    }

    for (int i = 0; i < m.ntiles; i++) {
        float x = ui_ch_value(d, m.tile_ch[i]);
        ui_ch_format(m.tile_ch[i], x, v, sizeof(v));
        ui_tile_set(&m.tiles[i], v, ui_ch_color(m.tile_ch[i], x));
    }

    buf[0] = '\0';
    for (int i = 0; i < m.nextra; i++) {
        char part[48];
        ui_ch_format(m.extra[i], ui_ch_value(d, m.extra[i]), v, sizeof(v));
        snprintf(part, sizeof(part), "%s#8A909C %s#  %s", i ? "   " : "", UI_CH[m.extra[i]].caption, v);
        strlcat(buf, part, sizeof(buf));
    }
    ui_label_set(m.info, buf);

    char mil[40] = "";
    if (d->mil_on) {
        snprintf(mil, sizeof(mil), "#FF3B30 MIL · %d KOD#   ", d->mil_codes);
    } else if (d->mil_codes > 0) {
        snprintf(mil, sizeof(mil), "#FFB020 %d KOD#   ", d->mil_codes);
    }
    snprintf(buf, sizeof(buf), "%s%s%s%s%s", mil,
             (d->engine_status & VEMS_ENG_RUNNING) ? "#2ED47A ÇALIŞIYOR#   " : "#4A505C DURUYOR#   ",
             (d->status1 & 0x04) ? "#2ED47A KAPALI ÇEVRİM#   " : "",
             (d->engine_status & VEMS_ENG_WARMUP) ? "#00C8FF ISINMA#   " : "",
             (d->status1 & 0x10) ? "#EE2323 KESME#" : "");
    ui_label_set(m.flags, buf);
}
