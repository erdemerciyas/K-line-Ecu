#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "board_lcd.h"
#include "ui_priv.h"
#include "src/draw/sw/lv_draw_sw.h"

// ---------------------------------------------------------------------------
// AFR standards (gasoline reference, expressed in lambda so they hold for any fuel)
//   < 0.75  very rich      : fouling plugs, washing cylinder walls
//   0.75-0.82 turbo WOT    : boosted full load (AFR 11.0-12.0)
//   0.82-0.90 power        : NA full load best power (AFR 12.0-13.2)
//   0.90-0.97 slightly rich: accel / warm-up (AFR 13.2-14.3)
//   0.97-1.03 stoich       : idle / closed loop cruise (AFR 14.3-15.1)
//   1.03-1.10 economy      : light load lean cruise (AFR 15.1-16.2)
//   > 1.10  very lean      : misfire / high EGT / knock risk
// ---------------------------------------------------------------------------
const ui_afr_zone_t UI_AFR_ZONES[] = {
    {0.75f, "ÇOK ZENGİN",    0xFF3B30},
    {0.82f, "TURBO TAM GAZ", 0xFF7A1A},
    {0.90f, "TAM GAZ / GÜÇ", 0xFFB020},
    {0.97f, "HAFİF ZENGİN",  0xC8E03C},
    {1.03f, "STOKİYOMETRİK", 0x2ED47A},
    {1.10f, "EKONOMİ",       0x00C8FF},
    {9.99f, "ÇOK FAKİR",     0xFF3B30},
};
const int UI_AFR_ZONE_COUNT = sizeof(UI_AFR_ZONES) / sizeof(UI_AFR_ZONES[0]);

float ui_stoich_afr(void)
{
    return SETTINGS_FUELS[g_settings.fuel].stoich;
}

const char *ui_fuel_name(void)
{
    static const char *const UPPER[] = {"BENZİN", "E85", "LPG", "METANOL", "DİZEL"};   // same order as SETTINGS_FUELS
    return g_settings.fuel < sizeof(UPPER) / sizeof(UPPER[0]) ? UPPER[g_settings.fuel] : "";
}

const ui_afr_zone_t *ui_afr_zone(float lambda)
{
    for (int i = 0; i < UI_AFR_ZONE_COUNT - 1; i++) {
        if (lambda < UI_AFR_ZONES[i].lambda_max) {
            return &UI_AFR_ZONES[i];
        }
    }
    return &UI_AFR_ZONES[UI_AFR_ZONE_COUNT - 1];
}

bool ui_lambda_valid(const vems_data_t *d)
{
    // raw 250..255 = WBO2 not ready / heating / not connected
    return d->ego_raw < 250 && d->lambda > 0.60f && d->lambda < 1.60f;
}


// ---------------------------------------------------------------------------
// Tema
// ---------------------------------------------------------------------------
lv_color_t ui_col(uint32_t rgb)
{
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    const int l = (r * 77 + g * 150 + b * 29) >> 8;     // ışıklılık
    switch (g_settings.theme) {
    case SET_THEME_NIGHT:
        // gece: %76 parlaklık, %20 soluk; karanlıkta göz kamaştırmasın, renkler yine ayırt edilsin
        r = (r * 80 + l * 20) * 76 / 10000;
        g = (g * 80 + l * 20) * 76 / 10000;
        b = (b * 80 + l * 20) * 76 / 10000;
        break;
    case SET_THEME_RED: {
        // kırmızı gece: tek renk, gece görüşünü bozmaz. Parlaklık = en parlak kanal ile ışıklılığın ortası
        const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const int v = (mx + l) / 2;
        r = v;
        g = v * 40 / 100;
        b = v * 26 / 100;
        break;
    }
    default:
        break;
    }
    return lv_color_make(r, g, b);
}

lv_color_t ui_accent(void)
{
    return ui_col(SETTINGS_ACCENTS[g_settings.accent < SETTINGS_ACCENT_COUNT ? g_settings.accent : 0].rgb);
}

bool ui_perf_update(bool *state, int64_t *t, float rpm, int shift, float map, int64_t now)
{
    // vites noktasına 2200 rpm kala ya da tam gazda girer, 1.5 sn sakin sürüşten sonra çıkar (eşikte titremez)
    const bool on = g_settings.smart && ((!isnan(rpm) && rpm >= shift - 2200) || (!isnan(map) && map > 85));
    if (on) {
        *t = now;
        *state = true;
    } else if (*state && (!g_settings.smart || now - *t > 1500 * 1000)) {
        *state = false;
    }
    return *state;
}

// ---------------------------------------------------------------------------
// Halka geometrisi
// ---------------------------------------------------------------------------
#define CX      (BOARD_LCD_H_RES / 2)
#define CY      (BOARD_LCD_V_RES / 2)
#define DEG     0.017453293f

void ui_sector_area(float a0, float a1, int rin, int rout, lv_area_t *out)
{
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    float angs[8];
    int n = 0;
    angs[n++] = a0;
    angs[n++] = a1;
    for (int k = (int)ceilf(a0 / 90); k * 90 < a1 && n < 8; k++) {
        angs[n++] = k * 90.0f;
    }
    for (int i = 0; i < n; i++) {
        const float c = cosf(angs[i] * DEG), s = sinf(angs[i] * DEG);
        const int rs[2] = {rin, rout};
        for (int j = 0; j < 2; j++) {
            const float x = CX + c * rs[j], y = CY + s * rs[j];
            x0 = fminf(x0, x);
            x1 = fmaxf(x1, x);
            y0 = fminf(y0, y);
            y1 = fmaxf(y1, y);
        }
    }
    out->x1 = LV_MAX(0, (lv_coord_t)x0 - 3);
    out->y1 = LV_MAX(0, (lv_coord_t)y0 - 3);
    out->x2 = LV_MIN(BOARD_LCD_H_RES - 1, (lv_coord_t)x1 + 3);
    out->y2 = LV_MIN(BOARD_LCD_V_RES - 1, (lv_coord_t)y1 + 3);
}

void ui_ring_invalidate(lv_obj_t *obj, float a0, float a1, int rin, int rout)
{
    for (float a = a0; a < a1; a += 20) {
        lv_area_t ar;
        ui_sector_area(a, fminf(a + 20, a1), rin, rout, &ar);
        lv_obj_invalidate_area(obj, &ar);
    }
}

// ---------------------------------------------------------------------------
// Kavisli yazı: her harf çemberin teğetine döndürülür, ikili doğrusal örneklemeyle maske olarak karıştırılır.
// Sabit yazılar için (başlık, ipucu): sadece sayfa kurulurken ve yazı değişince çizilir, karelerde maliyeti yok.
// ---------------------------------------------------------------------------
typedef struct {
    const lv_font_t *font;
    lv_color_t color;
    int16_t r, deg, ls;
    char txt[40];
} arc_text_t;

static float arc_text_width(const arc_text_t *t)
{
    float w = 0;
    uint32_t i = 0;
    while (t->txt[i]) {
        const uint32_t c = _lv_txt_encoded_next(t->txt, &i);
        uint32_t j = i;
        const uint32_t next = _lv_txt_encoded_next(t->txt, &j);
        w += lv_font_get_glyph_width(t->font, c, next) + t->ls;
    }
    return w > 0 ? w - t->ls : 0;
}

/** Yazının kapladığı halka parçası nesnenin alanı olur: geçersiz kılma sadece orası. */
static void arc_text_layout(lv_obj_t *o, const arc_text_t *t)
{
    const float span = arc_text_width(t) / t->r / DEG / 2 + 2;
    const int h = lv_font_get_line_height(t->font);
    lv_area_t a;
    ui_sector_area(t->deg - span, t->deg + span, t->r - h, t->r + h, &a);
    lv_obj_set_pos(o, a.x1, a.y1);
    lv_obj_set_size(o, lv_area_get_width(&a), lv_area_get_height(&a));
}

static inline int a4_at(const uint8_t *bmp, int w, int h, int x, int y)
{
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return 0;
    }
    const int i = y * w + x;
    const uint8_t b = bmp[i >> 1];
    return (i & 1) ? (b & 0x0F) : (b >> 4);
}

static void arc_text_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    const arc_text_t *t = lv_obj_get_user_data(o);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    const lv_font_t *f = t->font;
    const bool bottom = t->deg > 0 && t->deg < 180;     // altta saat yönünün tersine okunur: harfler dik kalır
    // yazının dikey ortası r üstünde: taban çizgisi büyük harf yüksekliğinin yarısı kadar içeride / dışarıda
    const float cap = (f->line_height - f->base_line) * 0.72f;
    const float rb = bottom ? t->r + cap / 2 : t->r - cap / 2;
    const float dir = bottom ? -1 : 1;
    float a = t->deg * DEG - dir * arc_text_width(t) / 2 / rb;     // ilk harfin başı

    uint32_t i = 0;
    while (t->txt[i]) {
        const uint32_t c = _lv_txt_encoded_next(t->txt, &i);
        uint32_t j = i;
        const uint32_t next = _lv_txt_encoded_next(t->txt, &j);
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(f, &g, c, next)) {
            continue;
        }
        const float adv = g.adv_w;
        const float am = a + dir * adv / 2 / rb;        // harfin ortası
        a += dir * (adv + t->ls) / rb;
        if (g.box_w == 0 || g.box_h == 0 || g.bpp != 4) {
            continue;
        }
        const uint8_t *bmp = lv_font_get_glyph_bitmap(g.resolved_font ? g.resolved_font : f, c);
        if (bmp == NULL) {
            continue;
        }
        // harf ekseni: ex okuma yönü, ey yukarı (üstte dışa, altta içe)
        const float ca = cosf(am), sa = sinf(am);
        const float ex_x = bottom ? sa : -sa, ex_y = bottom ? -ca : ca;
        const float ey_x = bottom ? -ca : ca, ey_y = bottom ? -sa : sa;
        const float px = CX + ca * rb, py = CY + sa * rb;  // taban çizgisi üstünde harf ortası
        const float gx0 = adv / 2 - g.ofs_x, gy0 = g.box_h + g.ofs_y;
        const float reach = adv / 2 + g.box_w + g.box_h + LV_ABS(g.ofs_y) + 2;
        lv_area_t box = {(lv_coord_t)(px - reach), (lv_coord_t)(py - reach), (lv_coord_t)(px + reach), (lv_coord_t)(py + reach)};
        lv_area_t clip;
        if (!_lv_area_intersect(&clip, &box, ctx->clip_area)) {
            continue;
        }
        const int cw = lv_area_get_width(&clip);
        lv_opa_t *mask = lv_mem_buf_get(cw);
        for (lv_coord_t y = clip.y1; y <= clip.y2; y++) {
            bool any = false;
            for (lv_coord_t x = clip.x1; x <= clip.x2; x++) {
                const float vx = x + 0.5f - px, vy = y + 0.5f - py;
                // harf bitmap koordinatı (piksel merkezi), ikili doğrusal örnek
                const float bx = vx * ex_x + vy * ex_y + gx0 - 0.5f;
                const float by = gy0 - (vx * ey_x + vy * ey_y) - 0.5f;
                int m = 0;
                if (bx > -1 && by > -1 && bx < g.box_w && by < g.box_h) {
                    const int ix = (int)floorf(bx), iy = (int)floorf(by);
                    const float fx = bx - ix, fy = by - iy;
                    const float v = (a4_at(bmp, g.box_w, g.box_h, ix, iy) * (1 - fx) +
                                     a4_at(bmp, g.box_w, g.box_h, ix + 1, iy) * fx) * (1 - fy) +
                                    (a4_at(bmp, g.box_w, g.box_h, ix, iy + 1) * (1 - fx) +
                                     a4_at(bmp, g.box_w, g.box_h, ix + 1, iy + 1) * fx) * fy;
                    m = (int)(v * 17 + 0.5f);
                }
                mask[x - clip.x1] = m > 255 ? 255 : m;
                any |= m > 0;
            }
            if (!any) {
                continue;
            }
            const lv_area_t row = {clip.x1, y, clip.x2, y};
            lv_draw_sw_blend_dsc_t bd;
            lv_memset_00(&bd, sizeof(bd));
            bd.blend_area = &row;
            bd.mask_area = &row;
            bd.mask_buf = mask;
            bd.mask_res = LV_DRAW_MASK_RES_CHANGED;
            bd.color = t->color;
            bd.opa = LV_OPA_COVER;
            lv_draw_sw_blend(ctx, &bd);
        }
        lv_mem_buf_release(mask);
    }
}

static void arc_text_del_cb(lv_event_t *e)
{
    lv_mem_free(lv_obj_get_user_data(lv_event_get_target(e)));
}

lv_obj_t *ui_arc_text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt, int r, int deg)
{
    arc_text_t *t = lv_mem_alloc(sizeof(arc_text_t));
    lv_memset_00(t, sizeof(*t));
    t->font = font;
    t->color = color;
    t->r = r;
    t->deg = deg % 360;
    t->ls = font == &ui_font_label ? 2 : 1;
    strlcpy(t->txt, txt, sizeof(t->txt));
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(o, t);
    lv_obj_add_event_cb(o, arc_text_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, arc_text_del_cb, LV_EVENT_DELETE, NULL);
    arc_text_layout(o, t);
    return o;
}

void ui_arc_text_set(lv_obj_t *o, const char *txt, lv_color_t color)
{
    arc_text_t *t = lv_obj_get_user_data(o);
    if (strcmp(t->txt, txt) == 0 && t->color.full == color.full) {
        return;
    }
    lv_obj_invalidate(o);
    strlcpy(t->txt, txt, sizeof(t->txt));
    t->color = color;
    arc_text_layout(o, t);
    lv_obj_invalidate(o);
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------
lv_obj_t *ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

void ui_label_set(lv_obj_t *label, const char *txt)
{
    if (strcmp(lv_label_get_text(label), txt) != 0) {
        lv_label_set_text(label, txt);
    }
}

void ui_text_color(lv_obj_t *o, lv_color_t c)
{
    // LVGL her stil atamasında nesneyi yeniden çizdirir: renk aynıysa dokunma
    if (lv_obj_get_style_text_color(o, 0).full != c.full) {
        lv_obj_set_style_text_color(o, c, 0);
    }
}

void ui_readout_create(ui_readout_t *r, lv_obj_t *parent, const lv_font_t *font, int cx, int y, const char *caption)
{
    r->cap = ui_label(parent, &ui_font_label, C_MUTED, caption);
    lv_obj_set_style_text_letter_space(r->cap, 1, 0);
    lv_obj_align(r->cap, LV_ALIGN_TOP_MID, cx, y);
    r->val = ui_label(parent, font, C_MUTED, "--");
    lv_obj_align(r->val, LV_ALIGN_TOP_MID, cx, y + 17);
    strcpy(r->last, "--");
}

void ui_readout_set(ui_readout_t *r, const char *txt, lv_color_t color)
{
    if (strcmp(r->last, txt) != 0) {
        strlcpy(r->last, txt, sizeof(r->last));
        lv_label_set_text(r->val, txt);
    }
    ui_text_color(r->val, color);
}

void ui_readout_set_num(ui_readout_t *r, const char *fmt, float v, lv_color_t color)
{
    char buf[16];
    if (isnan(v)) {
        ui_readout_set(r, "--", C_MUTED);
        return;
    }
    snprintf(buf, sizeof(buf), fmt, v);
    ui_readout_set(r, buf, color);
}

lv_obj_t *ui_vline(lv_obj_t *parent, int cx, int y, int h)
{
    lv_obj_t *l = lv_obj_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, 1, h);
    lv_obj_set_style_bg_color(l, C_TILE_BRD, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(l, LV_ALIGN_TOP_MID, cx, y);
    return l;
}

// sayfa göstergesi: alt kenarda yay üstünde noktalar, etkin sayfa vurgu renginde kısa yay
#define DOT_STEP    4.2f        // derece

static void dots_draw_cb(lv_event_t *e)
{
    const uintptr_t v = (uintptr_t)lv_event_get_user_data(e);
    const int active = v >> 8, count = v & 0xFF;
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    const lv_point_t c = {CX, CY};
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.rounded = 1;
    d.width = 6;
    for (int i = 0; i < count; i++) {
        // soldan sağa: altta açı azaldıkça sağa gidilir
        const float a = 90 + ((count - 1) / 2.0f - i) * DOT_STEP;
        const float half = i == active ? 1.6f : 0;
        d.color = i == active ? C_PRIMARY : C_TICK;
        lv_draw_arc(ctx, &d, &c, UI_DOTS_R + 3, (uint16_t)lroundf(a - half), (uint16_t)lroundf(a + half) + 1);
    }
}

void ui_page_dots(lv_obj_t *scr, int active, int count)
{
    const float span = (count - 1) / 2.0f * DOT_STEP + 3;
    lv_area_t a;
    ui_sector_area(90 - span, 90 + span, UI_DOTS_R - 6, UI_DOTS_R + 8, &a);
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, a.x1, a.y1);
    lv_obj_set_size(o, lv_area_get_width(&a), lv_area_get_height(&a));
    lv_obj_add_event_cb(o, dots_draw_cb, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)((active << 8) | count));
}


void ui_meter_label_div10_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->type == LV_METER_DRAW_PART_TICK && dsc->text != NULL) {
        lv_snprintf(dsc->text, 16, "%d", (int)(dsc->value / 10));
    }
}

lv_obj_t *ui_ring_meter(lv_obj_t *scr)
{
    lv_obj_t *m = lv_meter_create(scr);
    lv_obj_set_size(m, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_center(m);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 26, 0);                 // ticks start inside the outer ring
    lv_obj_set_style_text_color(m, C_MUTED, 0);
    lv_obj_set_style_text_font(m, &ui_font_label, 0);
    lv_obj_remove_style(m, NULL, LV_PART_INDICATOR);    // no needle hub
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m, ui_meter_label_div10_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    return m;
}

lv_obj_t *ui_header(lv_obj_t *scr, const char *txt, int y)
{
    return ui_arc_text(scr, &ui_font_label, C_MUTED, txt, UI_CROWN_R, 270);
}

// liste: üst ve alt kenarda içerik söner (yuvarlak camda satırlar kenara çarpmaz, kayarak kaybolur)
#define FADE_PX     34

static void list_fade_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_COVER_CHECK) {
        lv_event_set_cover_res(e, LV_COVER_RES_MASKED);
    } else if (code == LV_EVENT_DRAW_MAIN_BEGIN) {
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        lv_draw_mask_fade_param_t *top = lv_mem_buf_get(sizeof(lv_draw_mask_fade_param_t));
        lv_draw_mask_fade_param_t *bot = lv_mem_buf_get(sizeof(lv_draw_mask_fade_param_t));
        lv_draw_mask_fade_init(top, &a, LV_OPA_TRANSP, a.y1, LV_OPA_COVER, a.y1 + FADE_PX);
        lv_draw_mask_fade_init(bot, &a, LV_OPA_COVER, a.y2 - FADE_PX, LV_OPA_TRANSP, a.y2);
        lv_draw_mask_add(top, o);
        lv_draw_mask_add(bot, o);
    } else if (code == LV_EVENT_DRAW_POST_END) {
        for (int i = 0; i < 2; i++) {
            lv_draw_mask_fade_param_t *p = lv_draw_mask_remove_custom(o);
            if (p) {
                lv_draw_mask_free_param(p);
                lv_mem_buf_release(p);
            }
        }
    }
}

lv_obj_t *ui_scroll_list(lv_obj_t *scr)
{
    // inside the round glass: 332 px wide from y 70 to 434, üst ve alt kenarda sönerek
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 332, 364);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 74, 70);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_top(list, FADE_PX - 10, 0);
    lv_obj_set_style_pad_bottom(list, 56, 0);   // last row can scroll up out of the narrow bottom
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(list, list_fade_cb, LV_EVENT_COVER_CHECK, NULL);
    lv_obj_add_event_cb(list, list_fade_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(list, list_fade_cb, LV_EVENT_DRAW_POST_END, NULL);
    return list;
}

lv_obj_t *ui_list_row(lv_obj_t *list, const char *name, lv_obj_t **value)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, C_LINE, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(row, C_TILE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *n = ui_label(row, &ui_font_label_lg, C_SOFT, name);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 8, 0);
    *value = ui_label(row, &ui_font_label_lg, C_MUTED, "--");
    lv_obj_align(*value, LV_ALIGN_RIGHT_MID, -8, 0);
    return row;
}

lv_obj_t *ui_pill_button(lv_obj_t *parent, const char *txt, int w)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 48);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_color(b, C_TILE, 0);
    lv_obj_set_style_bg_color(b, lv_color_mix(C_PRIMARY, C_BG, LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, C_PRIMARY, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = ui_label(b, &ui_font_label_lg, C_TEXT, txt);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_center(l);
    return b;
}

// yay göstergesinin dolgu rengi stil yerine çizim anında verilir: renk değişince tüm nesne değil, sayfa
// ui_ring_invalidate ile sadece halka dilimlerini yeniden çizdirir (stil ataması 432 px'lik kutuyu çizdirirdi)
static void arc_color_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (d->part == LV_PART_INDICATOR && d->arc_dsc) {
        d->arc_dsc->color = *(const lv_color_t *)lv_event_get_user_data(e);
    }
}

void ui_arc_color_bind(lv_obj_t *arc, const lv_color_t *color)
{
    lv_obj_add_event_cb(arc, arc_color_cb, LV_EVENT_DRAW_PART_BEGIN, (void *)color);
}
