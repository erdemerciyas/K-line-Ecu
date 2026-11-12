/*
 * Eğim sayfası (inklinometre): solda önden görünüş (yan yatma), sağda yandan görünüş (burun eğimi).
 * Araç siluetleri gerçek açıyla döner, üstte ±40° ölçek ve gösterge üçgeni. Yan yatma ayarlardaki eşiğe
 * yaklaşınca sarı, aşınca kırmızı; burun eğimi eşik + 10°de. KALİBRE ET (iki dokunuş) ya da uzun bas: düz zeminde
 * şu anki duruş sıfır olur, en fazlalar silinir. Ayarlar sayfasında da aynı kalibrasyon satırı var.
 */
#include <math.h>
#include <stdio.h>
#include "esp_timer.h"
#include "imu.h"
#include "ui_priv.h"

#define DIAL_W          180
#define DIAL_R          80          // ölçek yarıçapı
#define DIAL_CY         210         // kadran merkezlerinin ekran y'si
#define DIAL_DX         92          // merkezden yatay uzaklık
#define SCALE_MAX       40
#define FRAME_MS        33
#define ZERO_MSG_US     (2500 * 1000)
#define PITCH_EXTRA     10          // burun eğimi uyarısı yan yatma eşiğinden bu kadar sonra
#define ARM_US          (3000 * 1000)   // kalibrasyon onayı için ikinci dokunuşun süresi

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *val;
    lv_obj_t *cap;
    float ang;                      // çizilen açı (ekranda saat yönü +)
    lv_color_t col;
    bool side;                      // yandan görünüş
} dial_t;

static struct {
    lv_obj_t *scr;
    dial_t d[2];                    // 0 yan yatma, 1 burun
    lv_obj_t *max[2];                // kadranların altında oturumun en fazlaları
    lv_obj_t *hint;
    lv_obj_t *none;
    float max_r, max_l, max_up, max_dn;
    int64_t zero_t;
    lv_obj_t *cal_btn, *cal_lbl;     // kalibrasyon düğmesi
    int64_t arm_t;                  // ilk dokunuş: onay bekleniyor
} in;

static void rot(lv_point_t *p, float x, float y, float c, float s, lv_coord_t cx, lv_coord_t cy)
{
    p->x = cx + (lv_coord_t)lroundf(x * c - y * s);
    p->y = cy + (lv_coord_t)lroundf(x * s + y * c);
}

static void poly(lv_draw_ctx_t *ctx, const lv_draw_rect_dsc_t *dsc, const float (*pts)[2], int n, float c, float s,
                 lv_coord_t cx, lv_coord_t cy)
{
    lv_point_t p[10];
    for (int i = 0; i < n; i++) {
        rot(&p[i], pts[i][0], pts[i][1], c, s, cx, cy);
    }
    lv_draw_polygon(ctx, dsc, p, n);
}

static void wheel(lv_draw_ctx_t *ctx, const lv_draw_rect_dsc_t *dsc, float x, float y, int r, float c, float s,
                  lv_coord_t cx, lv_coord_t cy)
{
    lv_point_t p;
    rot(&p, x, y, c, s, cx, cy);
    const lv_area_t a = {p.x - r, p.y - r, p.x + r, p.y + r};
    lv_draw_rect(ctx, dsc, &a);
}

static void dial_draw_cb(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    const dial_t *d = lv_event_get_user_data(e);
    lv_area_t box;
    lv_obj_get_coords(d->obj, &box);
    const lv_coord_t cx = (box.x1 + box.x2) / 2, cy = (box.y1 + box.y2) / 2 + 10;

    // ölçek: üst yayda ±40°, 10°'de bir çentik; yatayda düz referans çizgileri
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.width = 2;
    ln.round_start = ln.round_end = 1;
    for (int t = -SCALE_MAX; t <= SCALE_MAX; t += 10) {
        const float a = t / 57.29578f;
        const int r0 = t == 0 ? DIAL_R - 12 : DIAL_R - 7;
        lv_point_t p0 = {cx + (lv_coord_t)lroundf(sinf(a) * r0), cy - (lv_coord_t)lroundf(cosf(a) * r0)};
        lv_point_t p1 = {cx + (lv_coord_t)lroundf(sinf(a) * DIAL_R), cy - (lv_coord_t)lroundf(cosf(a) * DIAL_R)};
        ln.color = t == 0 ? C_SOFT : C_TICK;
        lv_draw_line(ctx, &ln, &p0, &p1);
    }
    ln.color = C_TICK;
    for (int side = -1; side <= 1; side += 2) {
        lv_point_t p0 = {cx + side * (DIAL_R - 4), cy}, p1 = {cx + side * (DIAL_R + 6), cy};
        lv_draw_line(ctx, &ln, &p0, &p1);
    }

    const float ang = d->ang / 57.29578f, c = cosf(ang), s = sinf(ang);
    // gösterge üçgeni ölçekte
    const float ta = fmaxf(-SCALE_MAX - 4, fminf(SCALE_MAX + 4, d->ang)) / 57.29578f;
    lv_point_t tri[3];
    const float tc = cosf(ta), ts = sinf(ta);
    rot(&tri[0], 0, -(DIAL_R - 10), tc, ts, cx, cy);
    rot(&tri[1], -7, -(DIAL_R + 6), tc, ts, cx, cy);
    rot(&tri[2], 7, -(DIAL_R + 6), tc, ts, cx, cy);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = d->col;
    rd.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(ctx, &rd, tri, 3);

    // araç
    lv_draw_rect_dsc_t wd;
    lv_draw_rect_dsc_init(&wd);
    wd.bg_color = C_TICK;
    wd.bg_opa = LV_OPA_COVER;
    wd.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect_dsc_t glass = rd;
    glass.bg_color = C_BG;
    if (!d->side) {
        static const float body[][2] = {{-50, -4}, {50, -4}, {52, 22}, {-52, 22}};
        static const float cabin[][2] = {{-38, -4}, {-28, -36}, {28, -36}, {38, -4}};
        static const float win[][2] = {{-30, -8}, {-23, -31}, {23, -31}, {30, -8}};
        wheel(ctx, &wd, -36, 26, 11, c, s, cx, cy);
        wheel(ctx, &wd, 36, 26, 11, c, s, cx, cy);
        poly(ctx, &rd, body, 4, c, s, cx, cy);
        poly(ctx, &rd, cabin, 4, c, s, cx, cy);
        poly(ctx, &glass, win, 4, c, s, cx, cy);
    } else {
        static const float body[][2] = {{-62, 18}, {-62, -6}, {-38, -10}, {40, -10}, {62, -5}, {62, 18}};
        static const float cabin[][2] = {{-38, -10}, {-22, -36}, {24, -36}, {40, -10}};
        static const float win[][2] = {{-31, -13}, {-19, -32}, {21, -32}, {33, -13}};
        poly(ctx, &rd, body, 6, c, s, cx, cy);
        poly(ctx, &rd, cabin, 4, c, s, cx, cy);
        poly(ctx, &glass, win, 4, c, s, cx, cy);
        wheel(ctx, &wd, -36, 20, 13, c, s, cx, cy);
        wheel(ctx, &wd, 38, 20, 13, c, s, cx, cy);
    }
}

static void dial_create(dial_t *d, lv_obj_t *scr, int dx, bool side)
{
    d->side = side;
    d->ang = 0;
    d->col = C_TEXT;
    d->obj = lv_obj_create(scr);
    lv_obj_remove_style_all(d->obj);
    lv_obj_set_size(d->obj, DIAL_W, DIAL_W);
    lv_obj_align(d->obj, LV_ALIGN_TOP_MID, dx, DIAL_CY - DIAL_W / 2);
    lv_obj_clear_flag(d->obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(d->obj, dial_draw_cb, LV_EVENT_DRAW_MAIN, d);

    d->val = ui_label(scr, &ui_font_label_lg, C_TEXT, "--");
    lv_obj_set_style_text_font(d->val, &ui_font_value, 0);
    lv_obj_align(d->val, LV_ALIGN_TOP_MID, dx, DIAL_CY + DIAL_W / 2 - 12);
    d->cap = ui_label(scr, &ui_font_label, C_MUTED, side ? "BURUN" : "YAN");
    lv_obj_set_style_text_letter_space(d->cap, 2, 0);
    lv_obj_align(d->cap, LV_ALIGN_TOP_MID, dx, DIAL_CY + DIAL_W / 2 + 26);
}

static lv_color_t level_color(float a, int warn)
{
    a = fabsf(a);
    return a >= warn ? C_ALERT : a >= warn - 5 ? C_WARN : C_TEXT;
}

static void dial_set(dial_t *d, float screen_ang, float shown, int warn, const char *pos, const char *neg)
{
    const lv_color_t col = level_color(shown, warn);
    if (fabsf(screen_ang - d->ang) >= 0.5f || col.full != d->col.full) {
        d->ang = screen_ang;
        d->col = col;
        lv_obj_invalidate(d->obj);
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f", fabsf(shown));
    ui_label_set(d->val, buf);
    ui_text_color(d->val, col);
    const int a = (int)lroundf(shown);
    snprintf(buf, sizeof(buf), "%s%s%s", d->side ? "BURUN" : "YAN", a ? " · " : "", a > 0 ? pos : a < 0 ? neg : "");
    ui_label_set(d->cap, buf);
}

static void frame(void)
{
    imu_incl_t m;
    imu_get(&m);
    if (!m.ok) {
        lv_obj_clear_flag(in.none, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(in.none, LV_OBJ_FLAG_HIDDEN);
    const int warn = g_settings.incl_warn;
    dial_set(&in.d[0], m.roll, m.roll, warn, "SAĞ", "SOL");
    dial_set(&in.d[1], -m.pitch, m.pitch, warn + PITCH_EXTRA, "YUKARI", "AŞAĞI");

    in.max_r = fmaxf(in.max_r, m.roll);
    in.max_l = fmaxf(in.max_l, -m.roll);
    in.max_up = fmaxf(in.max_up, m.pitch);
    in.max_dn = fmaxf(in.max_dn, -m.pitch);
    char buf[48];
    snprintf(buf, sizeof(buf), "MAKS  SAĞ %.0f° · SOL %.0f°", in.max_r, in.max_l);
    ui_label_set(in.max[0], buf);
    snprintf(buf, sizeof(buf), "MAKS  YUKARI %.0f° · AŞAĞI %.0f°", in.max_up, in.max_dn);
    ui_label_set(in.max[1], buf);

    const int64_t now = esp_timer_get_time();
    const bool armed = in.arm_t && now - in.arm_t < ARM_US;
    ui_label_set(in.cal_lbl, armed ? "ONAYLA" : "KALİBRE ET");
    ui_text_color(in.cal_lbl, armed ? C_WARN : C_TEXT);
    if (armed) {
        ui_label_set(in.hint, "ARAÇ DÜZ ZEMİNDE Mİ? TEKRAR DOKUN");
        ui_text_color(in.hint, C_WARN);
    } else if (in.zero_t && now - in.zero_t < ZERO_MSG_US) {
        ui_label_set(in.hint, "KALİBRE EDİLDİ");
        ui_text_color(in.hint, C_OK);
    } else if (!m.zeroed) {
        ui_label_set(in.hint, "KALİBRE EDİLMEDİ · DÜZ ZEMİNDE KALİBRE ET");
        ui_text_color(in.hint, C_WARN);
    } else {
        ui_label_set(in.hint, "");
    }
}

/** Kalibrasyon iki dokunuşla: ilki onay ister (ARM_US içinde), ikincisi kaydeder. */
static void cal_cb(lv_event_t *e)
{
    const int64_t now = esp_timer_get_time();
    if (in.arm_t && now - in.arm_t < ARM_US) {
        in.arm_t = 0;
        ui_incline_zero();
    } else {
        in.arm_t = now;
    }
    frame();
}

static void timer_cb(lv_timer_t *t)
{
    if (lv_scr_act() == in.scr) {
        frame();
    }
}

static void scr_delete_cb(lv_event_t *e)
{
    lv_timer_del(lv_event_get_user_data(e));
    in.scr = NULL;
}

lv_obj_t *ui_incline_create(int page, int pages)
{
    lv_obj_t *scr = ui_screen_create();
    in.scr = scr;
    ui_header(scr, "EĞİM", 66);
    dial_create(&in.d[0], scr, -DIAL_DX, false);
    dial_create(&in.d[1], scr, DIAL_DX, true);

    for (int i = 0; i < 2; i++) {
        in.max[i] = ui_label(scr, &ui_font_label, C_MUTED, "");
        lv_obj_align(in.max[i], LV_ALIGN_TOP_MID, i ? DIAL_DX : -DIAL_DX, 350);
    }
    in.hint = ui_label(scr, &ui_font_label, C_TICK, "");
    lv_obj_set_style_text_letter_space(in.hint, 1, 0);
    lv_obj_align(in.hint, LV_ALIGN_TOP_MID, 0, 94);
    in.cal_btn = ui_pill_button(scr, "KALİBRE ET", 170);
    lv_obj_align(in.cal_btn, LV_ALIGN_TOP_MID, 0, 376);
    lv_obj_clear_flag(in.cal_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(in.cal_btn, cal_cb, LV_EVENT_CLICKED, NULL);
    in.cal_lbl = lv_obj_get_child(in.cal_btn, 0);
    in.arm_t = 0;
    in.none = ui_label(scr, &ui_font_label_lg, C_MUTED, "EĞİM SENSÖRÜ YOK");
    lv_obj_center(in.none);
    lv_obj_add_flag(in.none, LV_OBJ_FLAG_HIDDEN);
    in.max_r = in.max_l = in.max_up = in.max_dn = 0;

    ui_page_dots(scr, page, pages);
    lv_timer_t *t = lv_timer_create(timer_cb, FRAME_MS, NULL);
    lv_obj_add_event_cb(scr, scr_delete_cb, LV_EVENT_DELETE, t);
    frame();
    return scr;
}

void ui_incline_zero(void)
{
    imu_incl_t m;
    imu_get(&m);
    if (!m.ok) {
        return;
    }
    imu_zero();
    in.max_r = in.max_l = in.max_up = in.max_dn = 0;
    in.zero_t = esp_timer_get_time();
}
