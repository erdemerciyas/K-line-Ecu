/*
 * Single gauge: one channel at a time, large, with a 270 degree ring over its range. Kanal adı üst taçta kavisli,
 * oturumun min / maks'ı halkada beyaz çentik ve altta tek satır. Tap the left / right half for the previous / next
 * channel, long press resets min / max. The channel list is whatever the source delivers.
 */
#include <math.h>
#include <stdio.h>
#include "ui_priv.h"

#define ARC_R       212         // halka: r 200..212
#define ARC_W       12
#define ARC_PAD     8           // nesne halkadan geniş: çentikler kırpılmasın
#define NOTCH_IN    (ARC_R - ARC_W - 6)
#define NOTCH_OUT   (ARC_R + 6)

static struct {
    lv_obj_t *arc;
    lv_obj_t *name;
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *minmax;
    lv_obj_t *index;
    vems_ch_t list[CH_COUNT];
    int count;
    int cur;
    lv_color_t arc_color;
    float notch[2];             // min / maks çentiğinin açısı, NAN = yok
} f;

uint32_t ui_focus_channels(void)
{
    return f.count ? CH_BIT(f.list[f.cur]) : 0;
}

/** Halkanın üstüne min / maks çentikleri. */
static void notch_draw_cb(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = C_TEXT;
    d.width = 3;
    d.round_start = d.round_end = 1;
    for (int i = 0; i < 2; i++) {
        if (isnan(f.notch[i])) {
            continue;
        }
        const float c = cosf(f.notch[i] * 0.017453293f), s = sinf(f.notch[i] * 0.017453293f);
        const lv_point_t a = {(lv_coord_t)lroundf(240 + c * NOTCH_IN), (lv_coord_t)lroundf(240 + s * NOTCH_IN)};
        const lv_point_t b = {(lv_coord_t)lroundf(240 + c * NOTCH_OUT), (lv_coord_t)lroundf(240 + s * NOTCH_OUT)};
        lv_draw_line(ctx, &d, &a, &b);
    }
}

static void notch_set(int i, float ang)
{
    const float old = f.notch[i];
    if ((isnan(old) && isnan(ang)) || fabsf(old - ang) < 0.5f) {
        return;
    }
    f.notch[i] = ang;
    if (!isnan(old)) {
        ui_ring_invalidate(f.arc, old - 2, old + 2, NOTCH_IN - 3, NOTCH_OUT + 3);
    }
    if (!isnan(ang)) {
        ui_ring_invalidate(f.arc, ang - 2, ang + 2, NOTCH_IN - 3, NOTCH_OUT + 3);
    }
}

lv_obj_t *ui_focus_create(int page, int pages, uint32_t mask)
{
    lv_obj_t *scr = ui_screen_create();
    vems_ch_t keep = f.count ? f.list[f.cur] : CH_RPM;
    f.count = ui_ch_list(mask, f.list, CH_COUNT);
    f.cur = 0;
    for (int i = 0; i < f.count; i++) {
        if (f.list[i] == keep) {
            f.cur = i;
        }
    }
    f.notch[0] = f.notch[1] = NAN;

    f.arc = lv_arc_create(scr);
    lv_obj_set_size(f.arc, 2 * (ARC_R + ARC_PAD), 2 * (ARC_R + ARC_PAD));
    lv_obj_set_style_pad_all(f.arc, ARC_PAD, 0);
    lv_obj_center(f.arc);
    lv_arc_set_bg_angles(f.arc, 135, 45);
    lv_arc_set_range(f.arc, 0, 1000);
    lv_arc_set_value(f.arc, 0);
    lv_obj_remove_style(f.arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(f.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(f.arc, ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(f.arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(f.arc, ARC_W, LV_PART_INDICATOR);
    f.arc_color = C_PRIMARY;
    ui_arc_color_bind(f.arc, &f.arc_color);
    lv_obj_add_event_cb(f.arc, notch_draw_cb, LV_EVENT_DRAW_POST, NULL);

    // kanal adı halkanın içinde, üstte kavisli
    f.name = ui_arc_text(scr, &ui_font_title, C_TEXT, "", 166, 270);

    f.value = ui_label(scr, &ui_font_big, C_MUTED, "--");
    lv_obj_align(f.value, LV_ALIGN_TOP_MID, 0, 150);

    f.unit = ui_label(scr, &ui_font_label_lg, C_MUTED, "");
    lv_obj_set_style_text_letter_space(f.unit, 1, 0);
    lv_obj_align(f.unit, LV_ALIGN_TOP_MID, 0, 250);

    f.minmax = ui_label(scr, &ui_font_label, C_MUTED, "");
    lv_label_set_recolor(f.minmax, true);
    lv_obj_set_style_text_letter_space(f.minmax, 1, 0);
    lv_obj_align(f.minmax, LV_ALIGN_TOP_MID, 0, 298);

    f.index = ui_label(scr, &ui_font_label, C_TICK, "");
    lv_obj_set_style_text_letter_space(f.index, 2, 0);
    lv_obj_align(f.index, LV_ALIGN_TOP_MID, 0, 352);

    ui_page_dots(scr, page, pages);
    return scr;
}

void ui_focus_step(int dir)
{
    if (f.count) {
        f.cur = (f.cur + dir + f.count) % f.count;
    }
}

void ui_focus_reset(void)
{
    if (f.count) {
        ui_ch_reset(f.list[f.cur]);
    }
}

/** Kanal değerinin halka oranı; AFR lambda ölçeğinde (bölgeler her yakıtta aynı anlamda). */
static float ring_frac(vems_ch_t c, float x)
{
    const ui_ch_info_t *ci = &UI_CH[c];
    float frac;
    if (c == CH_AFR || c == CH_TARGET) {
        frac = (x / ui_stoich_afr() - 0.7f) / 0.6f;
    } else if (c == CH_RPM) {
        frac = x / g_settings.rpm_max;
    } else {
        frac = (x - ci->min) / (ci->max - ci->min);
    }
    return frac < 0 ? 0 : frac > 1 ? 1 : frac;
}

void ui_focus_update(const vems_data_t *d, bool fresh)
{
    char buf[96], a[16], b[16];
    if (!f.count) {
        return;
    }
    const vems_ch_t c = f.list[f.cur];
    const ui_ch_info_t *ci = &UI_CH[c];
    const float x = fresh ? ui_ch_value(d, c) : NAN;

    ui_arc_text_set(f.name, ci->name, C_TEXT);
    ui_ch_format(c, x, buf, sizeof(buf));
    ui_label_set(f.value, buf);
    const lv_color_t col = ui_ch_color(c, x);
    ui_text_color(f.value, col);

    if (c == CH_AFR || c == CH_TARGET) {
        ui_label_set(f.unit, ui_fuel_name());
    } else if (c == CH_RUNTIME) {
        ui_label_set(f.unit, "dk:sn");
    } else {
        ui_label_set(f.unit, ci->unit);
    }

    // değerler ikincil yazı renginde (recolor, temaya göre)
    const unsigned long sc = lv_color_to32(C_SOFT) & 0xFFFFFF;
    float lo, hi;
    if (ui_ch_minmax(c, &lo, &hi)) {
        ui_ch_format(c, lo, a, sizeof(a));
        ui_ch_format(c, hi, b, sizeof(b));
        snprintf(buf, sizeof(buf), "MİN  #%06lX %s#     MAKS  #%06lX %s#", sc, a, sc, b);
        notch_set(0, 135 + 270 * ring_frac(c, lo));
        notch_set(1, 135 + 270 * ring_frac(c, hi));
    } else {
        snprintf(buf, sizeof(buf), "MİN  --     MAKS  --");
        notch_set(0, NAN);
        notch_set(1, NAN);
    }
    ui_label_set(f.minmax, buf);

    snprintf(buf, sizeof(buf), "%d / %d", f.cur + 1, f.count);
    ui_label_set(f.index, buf);

    lv_arc_set_value(f.arc, isnan(x) ? 0 : (int16_t)(ring_frac(c, x) * 1000));
    const lv_color_t ac = (col.full == C_TEXT.full || col.full == C_MUTED.full) ? C_PRIMARY : col;
    if (ac.full != f.arc_color.full) {
        f.arc_color = ac;
        ui_ring_invalidate(f.arc, 135, 405, ARC_R - ARC_W - 2, ARC_R + 2);
    }
}
