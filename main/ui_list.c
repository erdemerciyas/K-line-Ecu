/*
 * All data: every channel the source delivers in one scrolling list, like a scan tool.
 */
#include <math.h>
#include <stdio.h>
#include "ui_priv.h"

static struct {
    vems_ch_t ch[CH_COUNT];
    lv_obj_t *val[CH_COUNT];
    lv_obj_t *row[CH_COUNT];
    lv_obj_t *list;
    int count;
} l;

/** Kaydırılmış listede şu an görünen satırların kanalları: K-Line yalnız bunları hızlı turda sorar. */
uint32_t ui_list_channels(void)
{
    uint32_t want = 0;
    if (l.list == NULL) {
        return 0;
    }
    lv_area_t box, r;
    lv_obj_get_coords(l.list, &box);
    for (int i = 0; i < l.count; i++) {
        lv_obj_get_coords(l.row[i], &r);
        if (r.y2 >= box.y1 && r.y1 <= box.y2) {
            want |= CH_BIT(l.ch[i]);
        }
    }
    return want;
}

lv_obj_t *ui_list_create(int page, int pages, uint32_t mask)
{
    char buf[48];
    lv_obj_t *scr = ui_screen_create();
    l.count = ui_ch_list(mask, l.ch, CH_COUNT);

    snprintf(buf, sizeof(buf), "TÜM VERİLER · %d", l.count);
    ui_header(scr, buf, 66);

    l.list = ui_scroll_list(scr);
    for (int i = 0; i < l.count; i++) {
        l.row[i] = ui_list_row(l.list, UI_CH[l.ch[i]].name, &l.val[i]);
    }

    ui_page_dots(scr, page, pages);
    return scr;
}

void ui_list_update(const vems_data_t *d, bool fresh)
{
    char v[16], buf[40];
    for (int i = 0; i < l.count; i++) {
        const vems_ch_t c = l.ch[i];
        const float x = fresh ? ui_ch_value(d, c) : NAN;
        ui_ch_format(c, x, v, sizeof(v));
        const char *u = (c == CH_AFR || c == CH_TARGET || c == CH_RUNTIME || isnan(x)) ? "" : UI_CH[c].unit;
        snprintf(buf, sizeof(buf), u[0] ? "%s %s" : "%s%s", v, u);
        ui_label_set(l.val[i], buf);
        ui_text_color(l.val[i], ui_ch_color(c, x));
    }
}
