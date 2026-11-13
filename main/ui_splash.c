/*
 * Açılış ekranı, teşhis cihazı HUD'u: dış kenarda çentikli ölçüm halkası, içinde zıt yönlere dönen iki tarama
 * yayı ve bağlantı ilerlemesini gösteren halka. Başlık harf aralığı daralarak belirir. "SİSTEM KONTROLÜ"nün üç
 * segmenti buzzer'ın üç bip'iyle camgöbeği yanar, uzun "GO" bip'inde yeşile döner, ilerleme halkası bir tur atar
 * (melodi: main.c BOOT_MELODY). Sonra yerine veri kaynağının gerçek açılış kaydı (srclog) gelir: ekran, arayüz,
 * K-Line init, ECU, PID taraması; her satır sonucuyla biter, süren satır yanıp söner. Altta gerçek durum çipleri:
 * eğim sensörü, Bluetooth telsizi ve K-Line bağlantısı.
 */
#include <math.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "board_lcd.h"
#include "srclog.h"
#include "imu.h"
#include "ui_priv.h"
#if CONFIG_VEMS_BLE_PANEL
#include "ble_panel.h"
#endif

#define LINES           6
#define LINE_H          24
#define CON_Y           148
#define READY_HOLD_US   (700 * 1000)
#define TITLE_Y         88
#define SEGS            3
#define CHIPS           3
#define CHIP_Y          332         // iç halkanın içinde kalır (3 çip 268 px)

// halkalar (yarıçap, merkezden)
#define R_TICK_OUT      234
#define R_TICK_IN       226
#define R_SCAN_A        214
#define R_SCAN_B        203
#define SCAN_A_LEN      40          // derece; kısa yay = küçük yeniden çizim kutusu
#define SCAN_B_LEN      24
#define R_PROG          190

// zaman çizelgesi (ms, ekran kurulduktan sonra); melodi ~120 ms sonra başlar: bip 0 / 300 / 600, GO 1000, pış 1470
#define T_BEEP0         120
#define T_SEG_STEP      300
#define T_GO            (T_BEEP0 + 1000)
#define T_LOG           (T_BEEP0 + 1470)
#define T_TITLE         700
#define T_SWEEP_UP      520
#define T_SWEEP_DOWN    450
#define SCAN_MS         33          // tarama yaylarının dönüş adımı (~30 fps)

#define C_HUD           ui_col(0x00C8FF)  // camgöbeği: tarama ve sistem kontrolü
#define C_SEG_OFF       ui_col(0x15181E)

static struct {
    lv_obj_t *ticks, *prog;      // ticks: çentikler + tarama yayları (elle çizilir)
    lv_obj_t *title, *name;
    lv_obj_t *check;        // sistem kontrolü (GO'dan sonra kaybolur)
    lv_obj_t *seg[SEGS];
    int seg_state;          // yanan segment, SEGS + 1 = yeşil
    lv_obj_t *con;
    lv_obj_t *text[LINES];
    lv_obj_t *res[LINES];
    lv_obj_t *msg;
    lv_obj_t *chip_dot[CHIPS];
    lv_obj_t *chip_txt[CHIPS];
    lv_timer_t *timeline, *scanner;
    int rot_a, rot_b;
    bool sweep_done;
    uint32_t seq;
    int wait_line;          // yanıp sönen satır, -1 = yok
    bool blink;
    int progress;           // gösterilen, x10
    int64_t t0;
    int64_t ready_since;
} sp;

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, v, 0);
}

static void anim_letter_cb(void *obj, int32_t v)
{
    lv_obj_set_style_text_letter_space(obj, v, 0);
}

static void anim_name_color_cb(void *obj, int32_t v)
{
    lv_obj_set_style_text_color(obj, v ? C_TEXT : C_OK, 0);
}

static void anim_prog_cb(void *obj, int32_t v)
{
    lv_arc_set_value(obj, v);
}

static void run_anim(void *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t time, uint32_t delay,
                     lv_anim_path_cb_t path, lv_anim_ready_cb_t ready)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_ready_cb(&a, ready);
    lv_anim_start(&a);
}

/** Sadece gösterge kısmı görünen yay (r: merkezden yarıçap, açılar derece). */
static lv_obj_t *hud_arc(lv_obj_t *scr, int r, int w, int a0, int a1, lv_color_t c, lv_opa_t opa)
{
    lv_obj_t *a = lv_arc_create(scr);
    lv_obj_set_size(a, 2 * r, 2 * r);
    lv_obj_center(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_angles(a, a0, a1);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, c, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, opa, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    return a;
}

/** Sabit çentik halkası: 72 çentik, her 6'da biri uzun ve parlak; dört köşede HUD parantezleri. */
static void ticks_draw_cb(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_obj_t *o = lv_event_get_target(e);
    lv_area_t box;
    lv_obj_get_coords(o, &box);
    const lv_coord_t cx = (box.x1 + box.x2) / 2, cy = (box.y1 + box.y2) / 2;
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.width = 2;
    for (int i = 0; i < 72; i++) {
        const bool major = i % 6 == 0;
        const float a = i * 5 / 57.29578f;
        const int r0 = major ? R_TICK_IN - 4 : R_TICK_IN;
        ln.color = major ? C_MUTED : C_TICK;
        ln.opa = major ? LV_OPA_COVER : LV_OPA_60;
        lv_point_t p0 = {cx + (lv_coord_t)lroundf(cosf(a) * r0), cy + (lv_coord_t)lroundf(sinf(a) * r0)};
        lv_point_t p1 = {cx + (lv_coord_t)lroundf(cosf(a) * R_TICK_OUT), cy + (lv_coord_t)lroundf(sinf(a) * R_TICK_OUT)};
        lv_draw_line(ctx, &ln, &p0, &p1);
    }

    // tarama yayları burada çizilir: lv_arc nesnesi dönerken tüm halkayı (~430 px kare) yeniden çizdirirdi
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.rounded = 1;
    const lv_point_t c = {cx, cy};
    ad.color = C_HUD;
    ad.width = 3;
    ad.opa = LV_OPA_80;
    lv_draw_arc(ctx, &ad, &c, R_SCAN_A, sp.rot_a, (sp.rot_a + SCAN_A_LEN) % 360);
    ad.color = C_PRIMARY;
    ad.width = 2;
    ad.opa = LV_OPA_50;
    lv_draw_arc(ctx, &ad, &c, R_SCAN_B, sp.rot_b, (sp.rot_b + SCAN_B_LEN) % 360);
}

/** Bir yay parçasının ekran kutusu (yalnız bu bölge yeniden çizilir). */
static void arc_area(int r, int w, int a0, int len, lv_area_t *out)
{
    const int pad = w / 2 + 3;
    out->x1 = out->y1 = LV_COORD_MAX;
    out->x2 = out->y2 = LV_COORD_MIN;
    for (int a = a0; a <= a0 + len; a += 4) {
        const float rad = a / 57.29578f;
        const lv_coord_t x = BOARD_LCD_H_RES / 2 + (lv_coord_t)lroundf(cosf(rad) * r);
        const lv_coord_t y = BOARD_LCD_V_RES / 2 + (lv_coord_t)lroundf(sinf(rad) * r);
        out->x1 = LV_MIN(out->x1, x - pad);
        out->y1 = LV_MIN(out->y1, y - pad);
        out->x2 = LV_MAX(out->x2, x + pad);
        out->y2 = LV_MAX(out->y2, y + pad);
    }
}

static void scan_step(int *rot, int step, int r, int w, int len)
{
    lv_area_t a;
    arc_area(r, w, *rot, len, &a);
    lv_obj_invalidate_area(sp.ticks, &a);               // eski konum
    *rot = (*rot + step + 360) % 360;
    arc_area(r, w, *rot, len, &a);
    lv_obj_invalidate_area(sp.ticks, &a);               // yeni konum
}

/** Tarama yayları zıt yönlere döner; açılış ekranı yaşadıkça. */
static void scanner_cb(lv_timer_t *t)
{
    scan_step(&sp.rot_a, 6, R_SCAN_A, 3, SCAN_A_LEN);
    scan_step(&sp.rot_b, -3, R_SCAN_B, 2, SCAN_B_LEN);
}

static void sweep_down_ready_cb(lv_anim_t *a)
{
    sp.sweep_done = true;
}

static void sweep_up_ready_cb(lv_anim_t *a)
{
    run_anim(sp.prog, anim_prog_cb, 1000, sp.progress, T_SWEEP_DOWN, 0, lv_anim_path_ease_in_out, sweep_down_ready_cb);
}

static void seg_set(int i, lv_color_t c)
{
    lv_obj_set_style_bg_color(sp.seg[i], c, 0);
    lv_obj_set_style_shadow_color(sp.seg[i], c, 0);
    lv_obj_set_style_shadow_opa(sp.seg[i], c.full == C_SEG_OFF.full ? LV_OPA_TRANSP : LV_OPA_60, 0);
}

/** Sistem kontrolü zaman çizelgesi (bip'lerle); GO'dan sonra kendini siler. */
static void timeline_cb(lv_timer_t *t)
{
    const int ms = (int)((esp_timer_get_time() - sp.t0) / 1000);
    int state = 0;
    for (int i = 0; i < SEGS; i++) {
        state += ms >= T_BEEP0 + i * T_SEG_STEP;
    }
    state = ms >= T_GO ? SEGS + 1 : state;
    if (state == sp.seg_state) {
        return;
    }
    sp.seg_state = state;
    for (int i = 0; i < SEGS; i++) {
        seg_set(i, state > SEGS ? C_OK : i < state ? C_HUD : C_SEG_OFF);
    }
    if (state <= SEGS) {
        return;
    }

    // GO: ilerleme halkası tur atar, başlık bir an yeşil yanıp harfleri açılıp toplanır
    run_anim(sp.prog, anim_prog_cb, 0, 1000, T_SWEEP_UP, 0, lv_anim_path_ease_out, sweep_up_ready_cb);
    lv_obj_set_style_text_color(sp.name, C_OK, 0);
    run_anim(sp.name, anim_letter_cb, 14, 6, 380, 0, lv_anim_path_overshoot, NULL);
    run_anim(sp.name, anim_name_color_cb, 0, 1, 1, 300, lv_anim_path_step, NULL);
    // sistem kontrolü söner, yerine açılış kaydı gelir
    run_anim(sp.check, anim_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP, 260, T_LOG - T_GO, lv_anim_path_ease_in, NULL);
    run_anim(sp.con, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 380, T_LOG - T_GO + 200, lv_anim_path_ease_out, NULL);
    lv_timer_del(t);
    sp.timeline = NULL;
}

static void scr_delete_cb(lv_event_t *e)
{
    if (sp.timeline) {
        lv_timer_del(sp.timeline);
        sp.timeline = NULL;
    }
    if (sp.scanner) {
        lv_timer_del(sp.scanner);
        sp.scanner = NULL;
    }
}

static void chip_create(lv_obj_t *row, int i, const char *txt)
{
    lv_obj_t *c = lv_obj_create(row);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 84, 26);
    lv_obj_set_style_radius(c, 13, 0);
    lv_obj_set_style_bg_color(c, C_TILE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, C_TILE_BRD, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    sp.chip_dot[i] = lv_obj_create(c);
    lv_obj_remove_style_all(sp.chip_dot[i]);
    lv_obj_set_size(sp.chip_dot[i], 8, 8);
    lv_obj_set_style_radius(sp.chip_dot[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(sp.chip_dot[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sp.chip_dot[i], C_TICK, 0);
    lv_obj_align(sp.chip_dot[i], LV_ALIGN_LEFT_MID, 10, 0);
    sp.chip_txt[i] = ui_label(c, &ui_font_label, C_MUTED, txt);
    lv_obj_set_style_text_letter_space(sp.chip_txt[i], 1, 0);
    lv_obj_align(sp.chip_txt[i], LV_ALIGN_LEFT_MID, 24, 0);
}

lv_obj_t *ui_splash_create(void)
{
    lv_obj_t *scr = ui_screen_create();

    // halkalar: sabit çentikler, iki tarama yayı, ilerleme
    lv_obj_t *ticks = lv_obj_create(scr);
    lv_obj_remove_style_all(ticks);
    lv_obj_set_size(ticks, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_clear_flag(ticks, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ticks, ticks_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    sp.ticks = ticks;
    lv_obj_t *track = hud_arc(scr, R_PROG, 6, 0, 360, C_TRACK, LV_OPA_COVER);
    lv_obj_set_style_arc_rounded(track, false, LV_PART_INDICATOR);
    sp.prog = hud_arc(scr, R_PROG, 6, 0, 0, C_PRIMARY, LV_OPA_COVER);
    lv_arc_set_bg_angles(sp.prog, 0, 360);
    lv_arc_set_rotation(sp.prog, 270);
    lv_arc_set_range(sp.prog, 0, 1000);
    lv_arc_set_value(sp.prog, 0);
    sp.rot_a = 0;
    sp.rot_b = 180;

    // başlık: harf aralığı daralarak belirir
    sp.title = lv_obj_create(scr);
    lv_obj_remove_style_all(sp.title);
    lv_obj_set_size(sp.title, LV_PCT(100), 50);
    lv_obj_align(sp.title, LV_ALIGN_TOP_MID, 0, TITLE_Y);
    lv_obj_clear_flag(sp.title, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    sp.name = ui_label(sp.title, &ui_font_label_lg, C_TEXT, "K-LINE OBD");
    lv_obj_align(sp.name, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *what = ui_label(sp.title, &ui_font_label, C_HUD, "TEŞHİS ARAYÜZÜ  ·  v2.2");
    lv_obj_set_style_text_letter_space(what, 1, 0);
    lv_obj_align(what, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_opa(sp.title, LV_OPA_TRANSP, 0);
    run_anim(sp.title, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, T_TITLE, 0, lv_anim_path_ease_out, NULL);
    run_anim(sp.name, anim_letter_cb, 26, 6, T_TITLE + 200, 0, lv_anim_path_ease_out, NULL);

    // sistem kontrolü: üç segment bip'lerle yanar
    sp.check = lv_obj_create(scr);
    lv_obj_remove_style_all(sp.check);
    lv_obj_set_size(sp.check, 300, 80);
    lv_obj_align(sp.check, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_clear_flag(sp.check, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ct = ui_label(sp.check, &ui_font_label_lg, C_SOFT, "SİSTEM KONTROLÜ");
    lv_obj_set_style_text_letter_space(ct, 3, 0);
    lv_obj_align(ct, LV_ALIGN_TOP_MID, 0, 0);
    for (int i = 0; i < SEGS; i++) {
        lv_obj_t *s = lv_obj_create(sp.check);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 78, 8);
        lv_obj_set_style_radius(s, 4, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(s, 14, 0);
        lv_obj_align(s, LV_ALIGN_TOP_MID, (i - (SEGS - 1) / 2) * 90, 44);
        sp.seg[i] = s;
        seg_set(i, C_SEG_OFF);
    }
    sp.seg_state = 0;

    // açılış kaydı, sistem kontrolü bitince belirir
    sp.con = lv_obj_create(scr);
    lv_obj_remove_style_all(sp.con);
    lv_obj_set_size(sp.con, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(sp.con, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < LINES; i++) {
        const int y = CON_Y + i * LINE_H;
        sp.text[i] = ui_label(sp.con, &ui_font_label, C_SOFT, "");
        lv_label_set_long_mode(sp.text[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(sp.text[i], 210);
        lv_obj_align(sp.text[i], LV_ALIGN_TOP_LEFT, 94, y);
        sp.res[i] = ui_label(sp.con, &ui_font_label, C_TICK, "");
        lv_obj_align(sp.res[i], LV_ALIGN_TOP_RIGHT, -94, y);
    }
    sp.msg = ui_label(sp.con, &ui_font_label_lg, C_MUTED, "");
    lv_obj_set_style_text_letter_space(sp.msg, 2, 0);
    lv_obj_align(sp.msg, LV_ALIGN_TOP_MID, 0, CON_Y + LINES * LINE_H + 4);
    lv_obj_set_style_opa(sp.con, LV_OPA_TRANSP, 0);

    // durum çipleri: sensör, telsiz, K-Line
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, CHIP_Y);
    chip_create(row, 0, "SENSÖR");
    chip_create(row, 1, "TELSİZ");
    chip_create(row, 2, "K-LINE");

    sp.timeline = lv_timer_create(timeline_cb, 16, NULL);
    sp.scanner = lv_timer_create(scanner_cb, SCAN_MS, NULL);
    lv_obj_add_event_cb(scr, scr_delete_cb, LV_EVENT_DELETE, NULL);
    sp.sweep_done = false;
    sp.seq = UINT32_MAX;
    sp.wait_line = -1;
    sp.progress = 0;
    sp.t0 = esp_timer_get_time();
    sp.ready_since = 0;
    return scr;
}

static lv_color_t state_color(uint8_t st)
{
    switch (st) {
    case SRCLOG_OK:   return C_OK;
    case SRCLOG_SKIP: return C_TICK;
    case SRCLOG_FAIL: return C_ALERT;
    default:          return C_WARN;
    }
}

static void show_log(void)
{
    srclog_line_t lines[LINES];
    uint32_t seq;
    const int n = srclog_read(lines, LINES, &seq);
    if (seq == sp.seq) {
        return;
    }
    sp.seq = seq;
    sp.wait_line = -1;
    char buf[48];
    for (int i = 0; i < LINES; i++) {
        if (i < n) {
            snprintf(buf, sizeof(buf), "› %.31s", lines[i].text);
            ui_label_set(sp.text[i], buf);
            ui_label_set(sp.res[i], lines[i].result);
            lv_obj_set_style_text_color(sp.res[i], state_color(lines[i].state), 0);
            lv_obj_set_style_text_opa(sp.res[i], LV_OPA_COVER, 0);     // yanıp sönerken biten satır soluk kalmasın
            lv_obj_set_style_text_color(sp.text[i], i == n - 1 ? C_TEXT : C_SOFT, 0);
            if (lines[i].state == SRCLOG_WAIT) {
                sp.wait_line = i;
            }
        } else {
            ui_label_set(sp.text[i], "");
            ui_label_set(sp.res[i], "");
        }
    }
}

static void chip_set(int i, lv_color_t dot, bool lit)
{
    if (lv_obj_get_style_bg_color(sp.chip_dot[i], 0).full != dot.full) {
        lv_obj_set_style_bg_color(sp.chip_dot[i], dot, 0);
    }
    ui_text_color(sp.chip_txt[i], lit ? C_SOFT : C_TICK);
}

static void chips_update(vems_state_t st)
{
    chip_set(0, imu_present() ? C_OK : C_TICK, imu_present());
#if CONFIG_VEMS_BLE_PANEL
    chip_set(1, !g_settings.ble ? C_TICK : ble_panel_connected() ? C_HUD : C_OK, g_settings.ble);
#else
    chip_set(1, C_TICK, false);
#endif
    const lv_color_t k = st == VEMS_STATE_ECU_OK ? C_OK : st == VEMS_STATE_LINK_UP ? C_HUD
                         : st == VEMS_STATE_WAIT_LINK ? (sp.blink ? C_WARN : C_TICK) : C_TICK;
    chip_set(2, k, st >= VEMS_STATE_WAIT_LINK);
}

bool ui_splash_update(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed_ms = (now - sp.t0) / 1000;
    const vems_state_t st = vems_get_state();

    show_log();
    sp.blink = !sp.blink;
    if (sp.wait_line >= 0) {
        lv_obj_set_style_text_opa(sp.res[sp.wait_line], sp.blink ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    chips_update(st);

    int target;
    const char *msg;
    switch (st) {
    case VEMS_STATE_ECU_OK:    target = 1000; msg = "HAZIR"; break;
    case VEMS_STATE_LINK_UP:   target = 850;  msg = "VERİLER TARANIYOR"; break;
    case VEMS_STATE_WAIT_LINK: target = 300 + (int)(elapsed_ms / 40 > 400 ? 400 : elapsed_ms / 40); msg = "PROTOKOL ARANIYOR"; break;
    default:                   target = 120;  msg = "BAŞLATILIYOR"; break;
    }
    sp.progress += (target - sp.progress) / 4 + (target > sp.progress ? 1 : 0);
    if (sp.sweep_done) {
        lv_arc_set_value(sp.prog, sp.progress);
    }
    const lv_color_t pc = st == VEMS_STATE_ECU_OK ? C_OK : C_PRIMARY;
    if (lv_obj_get_style_arc_color(sp.prog, LV_PART_INDICATOR).full != pc.full) {
        lv_obj_set_style_arc_color(sp.prog, pc, LV_PART_INDICATOR);
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "%s  %d%%", msg, sp.progress / 10);
    ui_label_set(sp.msg, buf);
    ui_text_color(sp.msg, st == VEMS_STATE_ECU_OK ? C_OK : C_MUTED);

    if (st == VEMS_STATE_ECU_OK) {
        if (sp.ready_since == 0) {
            sp.ready_since = now;
        }
        return elapsed_ms >= CONFIG_VEMS_UI_SPLASH_MIN_MS && (now - sp.ready_since) >= READY_HOLD_US;
    }
    sp.ready_since = 0;
    return elapsed_ms >= CONFIG_VEMS_UI_SPLASH_TIMEOUT_MS;     // the pages show "ARANIYOR" themselves
}
