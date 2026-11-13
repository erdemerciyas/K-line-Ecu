/*
 * AFR screen: zone-colored ring (lambda 0.68 .. 1.36 x stoich) with a moving marker,
 * big AFR value + zone name, lambda / hedef / kısa yakıt düzeltmesi kutusuz okumalar halinde. Oturumun
 * min / maks AFR'si ölçek üstünde ince iz, altında tek satır (basılı tut: sıfırla).
 */
#include <stdio.h>
#include <math.h>
#include "sdkconfig.h"
#include "ui_priv.h"

#define LAMBDA_MIN      0.68f
#define LAMBDA_MAX      1.36f
#define MARKER_HALF     1           // marker half width in scale units (0.1 AFR)
#define READ_DX         104         // okumalar arası (merkezden)

enum { T_LAMBDA, T_TARGET, T_EGO, T_COUNT };

static struct {
    lv_obj_t *meter;
    lv_meter_indicator_t *marker;
    lv_meter_indicator_t *mark_min;     // oturum min / maks izi (ince yay)
    lv_meter_indicator_t *mark_max;
    int scale_min;
    int scale_max;
    lv_obj_t *afr;
    lv_obj_t *zone;
    lv_obj_t *minmax;
    ui_readout_t rd[T_COUNT];
    float afr_min;
    float afr_max;
    bool have_minmax;
} a;

static inline int afr_to_scale(float afr)
{
    return (int)lroundf(afr * 10.0f);
}

static lv_meter_indicator_t *trace_mark(lv_meter_scale_t *s)
{
    lv_meter_indicator_t *m = lv_meter_add_arc(a.meter, s, 4, C_SOFT, 30);
    lv_meter_set_indicator_start_value(a.meter, m, a.scale_min);
    lv_meter_set_indicator_end_value(a.meter, m, a.scale_min + 1);
    m->opa = LV_OPA_TRANSP;
    return m;
}

lv_obj_t *ui_afr_create(int page, int pages)
{
    const float st = ui_stoich_afr();
    char buf[48];
    lv_obj_t *scr = ui_screen_create();

    // ring scale in 0.1 AFR units, labels in whole AFR
    a.meter = ui_ring_meter(scr);
    a.scale_min = afr_to_scale(st * LAMBDA_MIN);
    a.scale_max = afr_to_scale(st * LAMBDA_MAX);
    lv_meter_scale_t *s = lv_meter_add_scale(a.meter);
    lv_meter_set_scale_range(a.meter, s, a.scale_min, a.scale_max, 270, 135);
    lv_meter_set_scale_ticks(a.meter, s, (a.scale_max - a.scale_min) / 2 + 1, 2, 8, C_TICK);
    lv_meter_set_scale_major_ticks(a.meter, s, 5, 3, 16, C_SOFT, 12);

    // colored zone bands
    float lo = LAMBDA_MIN;
    for (int i = 0; i < UI_AFR_ZONE_COUNT && lo < LAMBDA_MAX; i++) {
        float hi = UI_AFR_ZONES[i].lambda_max < LAMBDA_MAX ? UI_AFR_ZONES[i].lambda_max : LAMBDA_MAX;
        if (hi <= lo) {
            continue;
        }
        lv_meter_indicator_t *band = lv_meter_add_arc(a.meter, s, 14, ui_col(UI_AFR_ZONES[i].color), 24);
        lv_meter_set_indicator_start_value(a.meter, band, afr_to_scale(st * lo));
        lv_meter_set_indicator_end_value(a.meter, band, afr_to_scale(st * hi));
        band->opa = LV_OPA_60;
        lo = hi;
    }

    // stoich tick + live marker
    lv_meter_indicator_t *stoich = lv_meter_add_scale_lines(a.meter, s, C_TEXT, C_TEXT, false, 0);
    lv_meter_set_indicator_start_value(a.meter, stoich, afr_to_scale(st));
    lv_meter_set_indicator_end_value(a.meter, stoich, afr_to_scale(st));

    a.mark_min = trace_mark(s);
    a.mark_max = trace_mark(s);
    a.marker = lv_meter_add_arc(a.meter, s, 22, C_TEXT, 28);
    lv_meter_set_indicator_start_value(a.meter, a.marker, a.scale_min);
    lv_meter_set_indicator_end_value(a.meter, a.marker, a.scale_min + 2 * MARKER_HALF);
    a.marker->opa = LV_OPA_TRANSP;

    snprintf(buf, sizeof(buf), "AFR  ·  %s %.1f", ui_fuel_name(), st);
    lv_obj_t *hdr = ui_label(scr, &ui_font_label, C_MUTED, buf);
    lv_obj_set_style_text_letter_space(hdr, 2, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 86);

    a.afr = ui_label(scr, &ui_font_big, C_MUTED, "--.-");
    lv_obj_align(a.afr, LV_ALIGN_TOP_MID, 0, 108);

    a.zone = ui_label(scr, &ui_font_label_lg, C_MUTED, "SENSÖR BEKLENİYOR");
    lv_obj_set_style_text_letter_space(a.zone, 2, 0);
    lv_obj_align(a.zone, LV_ALIGN_TOP_MID, 0, 208);

    ui_readout_create(&a.rd[T_LAMBDA], scr, &ui_font_value, -READ_DX, 254, "LAMBDA");
    ui_readout_create(&a.rd[T_TARGET], scr, &ui_font_value, 0, 254, "HEDEF");
    ui_readout_create(&a.rd[T_EGO], scr, &ui_font_value, READ_DX, 254, "KISA DÜZ. %");
    ui_vline(scr, -READ_DX / 2, 258, 48);
    ui_vline(scr, READ_DX / 2, 258, 48);

    a.minmax = ui_label(scr, &ui_font_label, C_MUTED, "");
    lv_label_set_recolor(a.minmax, true);
    lv_obj_set_style_text_letter_space(a.minmax, 1, 0);
    lv_obj_align(a.minmax, LV_ALIGN_TOP_MID, 0, 334);

    ui_page_dots(scr, page, pages);
    ui_afr_reset_minmax();
    return scr;
}

static void set_arc(lv_meter_indicator_t *m, int v, bool visible)
{
    const lv_opa_t opa = visible ? LV_OPA_COVER : LV_OPA_TRANSP;
    v = v < a.scale_min + MARKER_HALF ? a.scale_min + MARKER_HALF : v > a.scale_max - MARKER_HALF ? a.scale_max - MARKER_HALF : v;
    const int half = m == a.marker ? MARKER_HALF : 0;
    if (m->opa != opa) {
        // görünürlük: sadece yayın eski ve yeni yeri (tüm ölçek değil)
        const float k = 270.0f / (a.scale_max - a.scale_min);
        const float a0 = 135 + (LV_MIN(m->start_value, v - half) - a.scale_min) * k;
        const float a1 = 135 + (LV_MAX(m->end_value, v + half + 1) - a.scale_min) * k;
        m->opa = opa;
        ui_ring_invalidate(a.meter, a0 - 2, a1 + 2, 180, 240);
    }
    if (m->start_value != v - half) {
        lv_meter_set_indicator_start_value(a.meter, m, v - half);
        lv_meter_set_indicator_end_value(a.meter, m, v + half + (half ? 0 : 1));
    }
}

static void minmax_text(void)
{
    char buf[96];
    if (!a.have_minmax) {
        snprintf(buf, sizeof(buf), "MİN  --    MAKS  --");
    } else {
        // değerler ikincil yazı renginde (temaya göre)
        const uint32_t c = lv_color_to32(C_SOFT) & 0xFFFFFF;
        snprintf(buf, sizeof(buf), "MİN  #%06lX %.1f#    MAKS  #%06lX %.1f#", (unsigned long)c, a.afr_min, (unsigned long)c,
                 a.afr_max);
    }
    ui_label_set(a.minmax, buf);
}

void ui_afr_reset_minmax(void)
{
    a.have_minmax = false;
    set_arc(a.mark_min, a.scale_min, false);
    set_arc(a.mark_max, a.scale_min, false);
    minmax_text();
}

void ui_afr_update(const vems_data_t *d, bool fresh)
{
    const float st = ui_stoich_afr();
    char buf[32];

    // target and EGO correction are valid whenever the ECU talks
    if (fresh) {
        ui_readout_set_num(&a.rd[T_TARGET], "%.1f", d->lambda_target > 0.3f ? d->lambda_target * st : NAN, C_TEXT);
        ui_readout_set_num(&a.rd[T_EGO], "%+.1f", d->ego_corr_pct - 100.0f,
                           fabsf(d->ego_corr_pct - 100.0f) > 15.0f ? C_WARN : C_TEXT);
    } else {
        ui_readout_set(&a.rd[T_TARGET], "--", C_MUTED);
        ui_readout_set(&a.rd[T_EGO], "--", C_MUTED);
    }

    if (!fresh || !ui_lambda_valid(d)) {
        ui_label_set(a.afr, "--.-");
        ui_text_color(a.afr, C_MUTED);
        ui_label_set(a.zone, fresh ? "SENSÖR HAZIR DEĞİL" : "VERİ YOK");
        ui_text_color(a.zone, fresh ? C_WARN : C_ALERT);
        ui_readout_set(&a.rd[T_LAMBDA], "--", C_MUTED);
        set_arc(a.marker, a.scale_min, false);
        return;
    }

    const float afr = d->lambda * st;
    const ui_afr_zone_t *z = ui_afr_zone(d->lambda);
    const lv_color_t zc = ui_col(z->color);

    snprintf(buf, sizeof(buf), "%.1f", afr);
    ui_label_set(a.afr, buf);
    ui_text_color(a.afr, zc);
    ui_label_set(a.zone, z->name);
    ui_text_color(a.zone, zc);
    set_arc(a.marker, afr_to_scale(afr), true);

    snprintf(buf, sizeof(buf), "%.2f", d->lambda);
    ui_readout_set(&a.rd[T_LAMBDA], buf, zc);

    const bool first = !a.have_minmax;
    const float lo0 = a.afr_min, hi0 = a.afr_max;
    if (first) {
        a.afr_min = a.afr_max = afr;
        a.have_minmax = true;
    } else {
        a.afr_min = fminf(a.afr_min, afr);
        a.afr_max = fmaxf(a.afr_max, afr);
    }
    if (first || lo0 != a.afr_min || hi0 != a.afr_max) {
        set_arc(a.mark_min, afr_to_scale(a.afr_min), true);
        set_arc(a.mark_max, afr_to_scale(a.afr_max), true);
        minmax_text();
    }
}
