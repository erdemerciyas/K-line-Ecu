/*
 * Açılış ekranı, drag yarışı başlangıcı gibi: sürüş kadranı karanlıktan belirir, başlık harf aralığı daralarak
 * açılır. Üç ışık buzzer'ın üç bip'iyle sırayla sarı yanar, uzun "GO" bip'inde hepsi yeşile döner, LED halka
 * bir tur atar, başlık yeşil yanıp açılıp toplanır (melodi: main.c BOOT_MELODY). Sonra ışıkların yerine
 * veri kaynağının gerçek açılış kaydı (srclog) gelir: ekran, arayüz, K-Line init, ECU, PID taraması. Her satır
 * sonucuyla biter (OK / HATA ...), süren satır yanıp söner. Halkanın yanık kısmı bağlantı ilerlemesidir.
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "board_lcd.h"
#include "srclog.h"
#include "ui_priv.h"

#define LINES           7
#define LINE_H          24
#define CON_Y           158
#define READY_HOLD_US   (700 * 1000)
#define TITLE_Y         92
#define LAMPS           3
#define LAMP_DX         84
#define LAMP_Y          236

// zaman çizelgesi (ms, ekran kurulduktan sonra); melodi ~120 ms sonra başlar: bip 0 / 300 / 600, GO 1000, pış 1470
#define T_BEEP0         120
#define T_LAMP_STEP     300
#define T_GO            (T_BEEP0 + 1000)
#define T_LOG           (T_BEEP0 + 1470)
#define T_TITLE         700
#define T_SWEEP_UP      520
#define T_SWEEP_DOWN    450
#define FRAME_MS        16

#define VERSION_TEXT    "ISO 9141-2  ·  ISO 14230-4  ·  v2.1"

#define C_LAMP_OFF      lv_color_hex(0x15181E)
#define C_LAMP_RIM      lv_color_hex(0x2A2F38)

typedef struct {
    lv_obj_t *halo2;
    lv_obj_t *halo1;
    lv_obj_t *core;
} lamp_t;

static struct {
    lv_obj_t *dial;
    lv_obj_t *ring;         // kadran pişirilemezse eski ince halka
    lv_obj_t *title;
    lv_obj_t *name;
    lv_obj_t *lamps_box;
    lamp_t lamps[LAMPS];
    int lamp_state;         // yanan sarı sayısı, LAMPS + 1 = yeşil
    lv_obj_t *con;
    lv_obj_t *text[LINES];
    lv_obj_t *res[LINES];
    lv_obj_t *msg;
    lv_timer_t *timer;
    bool sweep_done;
    uint32_t seq;
    int wait_line;          // index of the blinking line, -1 = none
    bool blink;
    int progress;           // shown, x10
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

static void anim_dial_cb(void *obj, int32_t v)
{
    ui_dial_set(v / 1000.0f);
}

static void run_anim(void *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t time, uint32_t delay,
                     lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

static lv_obj_t *circle(lv_obj_t *parent, int d)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, d, d);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(c);
    return c;
}

/** Işık: sönükken çerçeveli koyu yuva, yanınca renkli çekirdek ve iki kat hale. */
static void lamp_set(lamp_t *l, bool on, lv_color_t col)
{
    lv_obj_set_style_bg_color(l->core, on ? col : C_LAMP_OFF, 0);
    lv_obj_set_style_border_color(l->core, on ? lv_color_white() : C_LAMP_RIM, 0);
    lv_obj_set_style_border_opa(l->core, on ? LV_OPA_50 : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(l->halo1, col, 0);
    lv_obj_set_style_bg_opa(l->halo1, on ? LV_OPA_40 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(l->halo2, col, 0);
    lv_obj_set_style_bg_opa(l->halo2, on ? LV_OPA_10 : LV_OPA_TRANSP, 0);
}

static void sweep_down_ready_cb(lv_anim_t *a)
{
    sp.sweep_done = true;
}

static void sweep_up_ready_cb(lv_anim_t *a)
{
    // halka tepeden bağlantı ilerlemesine iner
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, sp.dial);
    lv_anim_set_exec_cb(&b, anim_dial_cb);
    lv_anim_set_values(&b, 1000, sp.progress);
    lv_anim_set_time(&b, T_SWEEP_DOWN);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&b, sweep_down_ready_cb);
    lv_anim_start(&b);
}

/** Başlangıç ışıkları zaman çizelgesi; GO'dan sonra kendini siler. */
static void timeline_cb(lv_timer_t *t)
{
    const int ms = (int)((esp_timer_get_time() - sp.t0) / 1000);
    int state = 0;
    for (int i = 0; i < LAMPS; i++) {
        state += ms >= T_BEEP0 + i * T_LAMP_STEP;
    }
    state = ms >= T_GO ? LAMPS + 1 : state;
    if (state == sp.lamp_state) {
        return;
    }
    sp.lamp_state = state;
    for (int i = 0; i < LAMPS; i++) {
        lamp_set(&sp.lamps[i], state > LAMPS || i < state, state > LAMPS ? C_OK : C_WARN);
    }
    if (state <= LAMPS) {
        return;
    }

    // GO: halka tur atar, başlık bir an yeşil yanıp harfleri açılıp toplanır
    if (sp.dial) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, sp.dial);
        lv_anim_set_exec_cb(&a, anim_dial_cb);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_time(&a, T_SWEEP_UP);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_ready_cb(&a, sweep_up_ready_cb);
        lv_anim_start(&a);
    } else {
        sp.sweep_done = true;
    }
    lv_obj_set_style_text_color(sp.name, C_OK, 0);
    run_anim(sp.name, anim_letter_cb, 14, 6, 380, 0, lv_anim_path_overshoot);
    run_anim(sp.name, anim_name_color_cb, 0, 1, 1, 300, lv_anim_path_step);
    // ışıklar söner, yerine açılış kaydı gelir
    run_anim(sp.lamps_box, anim_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP, 260, T_LOG - T_GO, lv_anim_path_ease_in);
    run_anim(sp.con, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 380, T_LOG - T_GO + 200, lv_anim_path_ease_out);
    lv_timer_del(t);
    sp.timer = NULL;
}

static void scr_delete_cb(lv_event_t *e)
{
    if (sp.timer) {
        lv_timer_del(sp.timer);
        sp.timer = NULL;
    }
}

lv_obj_t *ui_splash_create(void)
{
    lv_obj_t *scr = ui_screen_create();

    sp.dial = ui_dial_create(scr);
    sp.ring = NULL;
    if (sp.dial) {
        lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);    // kadran görüntüsü her pikseli boyar
    } else {
        sp.ring = lv_arc_create(scr);
        lv_obj_set_size(sp.ring, 460, 460);
        lv_obj_center(sp.ring);
        lv_arc_set_bg_angles(sp.ring, 0, 360);
        lv_arc_set_rotation(sp.ring, 270);
        lv_arc_set_range(sp.ring, 0, 1000);
        lv_arc_set_value(sp.ring, 0);
        lv_obj_remove_style(sp.ring, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(sp.ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(sp.ring, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_color(sp.ring, C_TRACK, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp.ring, 8, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp.ring, C_PRIMARY, LV_PART_INDICATOR);
    }

    // başlık: harf aralığı daralarak belirir; GO'da üstünden parıltı geçer
    sp.title = lv_obj_create(scr);
    lv_obj_remove_style_all(sp.title);
    lv_obj_set_size(sp.title, LV_PCT(100), 50);
    lv_obj_align(sp.title, LV_ALIGN_TOP_MID, 0, TITLE_Y);
    lv_obj_clear_flag(sp.title, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    sp.name = ui_label(sp.title, &ui_font_label_lg, C_TEXT, "K-LINE OBD");
    lv_obj_align(sp.name, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *what = ui_label(sp.title, &ui_font_label, C_PRIMARY, "CANLI VERİ  ·  ARAÇ PROFİLİ  ·  ARIZA KODU");
    lv_obj_set_style_text_letter_space(what, 1, 0);
    lv_obj_align(what, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_opa(sp.title, LV_OPA_TRANSP, 0);
    run_anim(sp.title, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, T_TITLE, 0, lv_anim_path_ease_out);
    run_anim(sp.name, anim_letter_cb, 26, 6, T_TITLE + 200, 0, lv_anim_path_ease_out);

    // başlangıç ışıkları
    sp.lamps_box = lv_obj_create(scr);
    lv_obj_remove_style_all(sp.lamps_box);
    lv_obj_set_size(sp.lamps_box, LAMP_DX * (LAMPS - 1) + 90, 90);
    lv_obj_align(sp.lamps_box, LV_ALIGN_TOP_MID, 0, LAMP_Y - 45);
    lv_obj_clear_flag(sp.lamps_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < LAMPS; i++) {
        lv_obj_t *slot = lv_obj_create(sp.lamps_box);
        lv_obj_remove_style_all(slot);
        lv_obj_set_size(slot, 86, 86);
        lv_obj_align(slot, LV_ALIGN_CENTER, (i - (LAMPS - 1) / 2) * LAMP_DX, 0);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lamp_t *l = &sp.lamps[i];
        l->halo2 = circle(slot, 84);
        l->halo1 = circle(slot, 58);
        l->core = circle(slot, 38);
        lv_obj_set_style_bg_opa(l->core, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(l->core, 2, 0);
        lamp_set(l, false, C_WARN);
    }
    sp.lamp_state = 0;

    // açılış kaydı, ışıklar sönünce belirir
    sp.con = lv_obj_create(scr);
    lv_obj_remove_style_all(sp.con);
    lv_obj_set_size(sp.con, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(sp.con, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < LINES; i++) {
        const int y = CON_Y + i * LINE_H;
        sp.text[i] = ui_label(sp.con, &ui_font_label, C_SOFT, "");
        lv_label_set_long_mode(sp.text[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(sp.text[i], 210);
        lv_obj_align(sp.text[i], LV_ALIGN_TOP_LEFT, 92, y);
        sp.res[i] = ui_label(sp.con, &ui_font_label, C_TICK, "");
        lv_obj_align(sp.res[i], LV_ALIGN_TOP_RIGHT, -92, y);
    }
    sp.msg = ui_label(sp.con, &ui_font_label_lg, C_MUTED, "");
    lv_obj_set_style_text_letter_space(sp.msg, 2, 0);
    lv_obj_align(sp.msg, LV_ALIGN_TOP_MID, 0, CON_Y + LINES * LINE_H + 6);
    lv_obj_t *ver = ui_label(sp.con, &ui_font_label, C_TICK, VERSION_TEXT);
    lv_obj_set_style_text_letter_space(ver, 1, 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, CON_Y + LINES * LINE_H + 38);
    lv_obj_set_style_opa(sp.con, LV_OPA_TRANSP, 0);

    sp.timer = lv_timer_create(timeline_cb, FRAME_MS, NULL);
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

bool ui_splash_update(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed_ms = (now - sp.t0) / 1000;
    const vems_state_t st = vems_get_state();

    show_log();
    if (sp.wait_line >= 0) {
        sp.blink = !sp.blink;
        lv_obj_set_style_text_opa(sp.res[sp.wait_line], sp.blink ? LV_OPA_COVER : LV_OPA_30, 0);
    }

    int target;
    const char *msg;
    switch (st) {
    case VEMS_STATE_ECU_OK:    target = 1000; msg = "HAZIR"; break;
    case VEMS_STATE_LINK_UP:   target = 850;  msg = "VERİLER TARANIYOR"; break;
    case VEMS_STATE_WAIT_LINK: target = 300 + (int)(elapsed_ms / 40 > 400 ? 400 : elapsed_ms / 40); msg = "PROTOKOL ARANIYOR"; break;
    default:                   target = 120;  msg = "BAŞLATILIYOR"; break;
    }
    sp.progress += (target - sp.progress) / 4 + (target > sp.progress ? 1 : 0);
    if (sp.ring) {
        lv_arc_set_value(sp.ring, sp.progress);
    } else if (sp.sweep_done) {
        ui_dial_set(sp.progress / 1000.0f);
    }
    ui_label_set(sp.msg, msg);
    lv_obj_set_style_text_color(sp.msg, st == VEMS_STATE_ECU_OK ? C_OK : C_MUTED, 0);

    if (st == VEMS_STATE_ECU_OK) {
        if (sp.ready_since == 0) {
            sp.ready_since = now;
        }
        return elapsed_ms >= CONFIG_VEMS_UI_SPLASH_MIN_MS && (now - sp.ready_since) >= READY_HOLD_US;
    }
    sp.ready_since = 0;
    return elapsed_ms >= CONFIG_VEMS_UI_SPLASH_TIMEOUT_MS;     // the pages show "ARANIYOR" themselves
}
