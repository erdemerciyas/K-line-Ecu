/*
 * Sürüş sayfası, yarış oyunu göstergesi havasında. Dış halka devir LED'leri: maviden camgöbeğine, vites
 * noktasına yaklaşırken sarı, sonra kırmızı. Yanan segmentin ucu beyaz parlar, devir düşünce sönen segmentler
 * kısa bir iz bırakır, tepe değeri beyaz segmentte bir süre kalır. Üstünde turuncu bıçak ibre yay fiziğiyle
 * oturur (hafif aşma), hızlı dönüşte arkasında iz kalır. Ortada büyük değer (hız ya da devir, dokununca
 * değişir), altında küçük olanı. Üstteki 7 ışık vites noktasına yaklaşırken
 * dolar (renk takımı Ayarlar > Vites ışığı rengi), noktada hepsi flaş yapar, halka ve kenar da yanıp söner.
 * Motor soğukken izin verilen devrin üstü buz mavisi, vites ışıkları o sınıra göre yanar.
 * Rozet satırı: bağlantı / arıza durumu, 0-100 süresi, YAKIT KESİK, MOTOR SOĞUK.
 * Ekran ilk açıldığında ve ECU bağlanınca ibre bir tur atar.
 *
 * Performans: kadran (segmentler, çentikler, çerçeve, parıltı) bir kez iki görüntüye pişirilir, sönük ve yanık
 * hali. Karelerde sadece değişen açı aralığı yeniden çizilir; yanık segmentler yanık görüntünün açı maskesiyle
 * kopyalanmasıdır. Devir / hız ui_motion'dan her karede (16 ms) gelir.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_lcd.h"
#include "ui_priv.h"

#define SCR_W           BOARD_LCD_H_RES
#define SCR_H           BOARD_LCD_V_RES
#define CX              (SCR_W / 2)
#define CY              (SCR_H / 2)
#define DEG             0.017453293f

// kadran geometrisi: 270° tarama, sol alttan (135°) saat yönünde
#define A0              135.0f
#define SPAN            270.0f
#define R_LED_IN        206
#define R_LED_OUT       230
#define R_TICK_MAJ      188
#define R_TICK_MIN      194
#define R_TICK_OUT      201
#define R_BEZEL         233.5f
#define R_NUM           172
#define R_NDL_IN        142
#define R_NDL_OUT       234
#define R_BAKE_MIN      138         // bunun içi düz siyah
#define SEG_RPM         250
#define SEG_GAP_PX      6.0f

#define FRAME_MS        16
#define GHOSTS          5           // ibre izi: son 5 karenin açısı
#define SPRING_W        26.0f       // ibre yayı: doğal frekans (rad/s) ve sönüm
#define SPRING_Z        0.62f
#define SPRING_STEP     0.002f      // s, integrasyon adımı
#define PEAK_HOLD_US    (1200 * 1000)
#define PEAK_DROP       3000.0f     // rpm/s
#define TRAIL_HOLD_US   (90 * 1000)
#define TRAIL_DROP      7000.0f     // rpm/s, sönen segmentlerin izi
#define SHIFT_LEAD      1200        // vites ışıkları bu kadar devir önce başlar
#define SHIFT_DOTS      7
#define BLINK_US        60000       // vites flaşı ~8 Hz
#define SWEEP_US        (1300 * 1000)
#define RESULT_US       (8000 * 1000)
#define RUN_MAX_US      (40 * 1000 * 1000)
#define CUT_KPA         32          // gaz kapalı, yüksek vakum: yakıt kesik
#define CLT_LO          40
#define CLT_HI          130
#define POD_MAX         2
#define POD_DX          60

#define C_SEG_FROZEN    ui_col(0x0F3C52)
#define C_NEEDLE        ui_col(0xFF5418)
#define C_NEEDLE_CORE   ui_col(0xFFD2B8)
#define C_DOT_OFF       ui_col(0x1A1D24)

// ---------------------------------------------------------------------------
// Pişirilmiş kadran (PSRAM), ayarlar değişmedikçe sayfa yeniden kurulsa da korunur
// ---------------------------------------------------------------------------
// pişirme ~0.5 sn sürer: LVGL görevini bekletmesin diye çekirdek 0'da düşük öncelikli görevde yapılır
static struct {
    lv_img_dsc_t off;
    lv_img_dsc_t lit;
    volatile int rpm_max;       // hazır görüntünün anahtarı, 0 = henüz yok
    volatile int shift;
    volatile int want_max;      // istenen anahtar
    volatile int want_shift;
    volatile int theme;         // kadran renkleri temaya göre (Ayarlar > Tema)
    volatile int want_theme;
    volatile uint32_t gen;      // her bitişte artar: sayfa bir kez yeniden çizilir
    TaskHandle_t task;
} bake;

typedef struct {
    float r, g, b;
} rgbf_t;

static inline float clamp01(float x)
{
    return x < 0 ? 0 : x > 1 ? 1 : x;
}

static rgbf_t hexf(uint32_t h)
{
    return (rgbf_t){((h >> 16) & 0xFF) / 255.0f, ((h >> 8) & 0xFF) / 255.0f, (h & 0xFF) / 255.0f};
}

static rgbf_t mixf(rgbf_t a, rgbf_t b, float t)
{
    return (rgbf_t){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

static rgbf_t scalef(rgbf_t a, float k)
{
    return (rgbf_t){a.r * k, a.g * k, a.b * k};
}

static lv_color_t to_color(rgbf_t c)
{
    // tema süzgeci (gece / kırmızı gece) pişirmede uygulanır: karelerde maliyeti yok
    return ui_col(((uint32_t)(clamp01(c.r) * 255) << 16) | ((uint32_t)(clamp01(c.g) * 255) << 8) |
                  (uint32_t)(clamp01(c.b) * 255));
}

/** Segment rengi, segmentin başladığı devre göre. */
static rgbf_t seg_rgb(int rpm, bool lit, int shift)
{
    const int warn = shift - SHIFT_LEAD;
    if (!lit) {
        return hexf(rpm >= shift ? 0x3A1216 : 0x171B23);
    }
    if (rpm >= shift) {
        return hexf(0xFF2A2A);
    }
    if (rpm >= warn) {
        return mixf(hexf(0xFFC21A), hexf(0xFF6A10), (rpm - warn) / (float)SHIFT_LEAD);
    }
    return mixf(hexf(0x1444FF), hexf(0x26D9FF), clamp01(rpm / (float)warn));
}

static bool bake_alloc(lv_img_dsc_t *d)
{
    if (d->data == NULL) {
        // siyahla başlar: pişme bitmeden görünürse çöp piksel olmasın
        void *px = heap_caps_calloc(1, SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (px == NULL) {
            return false;
        }
        d->header.cf = LV_IMG_CF_TRUE_COLOR;
        d->header.w = SCR_W;
        d->header.h = SCR_H;
        d->data_size = SCR_W * SCR_H * sizeof(lv_color_t);
        d->data = px;
    }
    return true;
}

/** Kadranı sönük ve yanık iki görüntüye çizer (piksel piksel). İstek değişirse yarıda bırakır: false. */
static bool bake_render(int rpm_max, int shift, int theme)
{
    lv_color_t *off = (lv_color_t *)bake.off.data;
    lv_color_t *lit = (lv_color_t *)bake.lit.data;
    const float deg_per_rpm = SPAN / rpm_max;
    const int nseg = rpm_max / SEG_RPM;
    const rgbf_t black = {0, 0, 0};
    const rgbf_t white = {1, 1, 1};
    const float r_mid = (R_LED_IN + R_LED_OUT) / 2.0f;
    const float half = (R_LED_OUT - R_LED_IN) / 2.0f;

    for (int y = 0; y < SCR_H; y++) {
        if (bake.want_max != rpm_max || bake.want_shift != shift || bake.want_theme != theme) {
            return false;
        }
        if ((y & 31) == 0) {
            vTaskDelay(1);      // boşta görevi de çalışsın (görev bekçisi)
        }
        for (int x = 0; x < SCR_W; x++) {
            const int i = y * SCR_W + x;
            const float dx = x + 0.5f - CX, dy = y + 0.5f - CY;
            const float r2 = dx * dx + dy * dy;
            if (r2 < R_BAKE_MIN * R_BAKE_MIN) {
                off[i] = lit[i] = lv_color_black();
                continue;
            }
            const float r = sqrtf(r2);
            float th = atan2f(dy, dx) / DEG;
            th = th < 0 ? th + 360 : th;
            float u = th - A0;                      // taramadaki açı: 0..270, ötesi alttaki boşluk
            u = u < 0 ? u + 360 : u;

            // zemin kenara doğru hafif lacivert-siyah, çerçeve sol üstten ışık alan ince halka
            rgbf_t base = mixf(black, hexf(0x090C14), clamp01((r - R_BAKE_MIN) / 100));
            const float bez = clamp01(r - R_BEZEL);
            if (bez > 0) {
                const float light = 0.35f + 0.65f * (0.5f + 0.5f * cosf((th - 235) * DEG));
                base = mixf(base, scalef(hexf(0x3A414E), light), bez);
            }
            rgbf_t o = base, l = base;

            if (u <= SPAN + 3) {
                const float rpm_u = u / deg_per_rpm;
                const float arc_px = deg_per_rpm * DEG * r;     // 1 rpm'in yay boyu

                // çentikler: 1000'lerde uzun, 500'lerde kısa; kırmızı bölgede kırmızı
                const float tick = roundf(rpm_u / 500) * 500;
                const bool major = ((int)tick % 1000) == 0;
                const float tw = major ? 1.7f : 1.0f;
                float tc = clamp01(tw + 0.5f - fabsf(rpm_u - tick) * arc_px) *
                           clamp01(r - (major ? R_TICK_MAJ : R_TICK_MIN) + 0.5f) * clamp01(R_TICK_OUT - r + 0.5f);
                if (tick > rpm_max) {
                    tc = 0;
                }
                if (tc > 0) {
                    const rgbf_t col = hexf(tick >= shift ? (major ? 0xD04040 : 0x6E2626)
                                                          : (major ? 0x9AA1AE : 0x434956));
                    o = mixf(o, col, tc);
                    l = mixf(l, col, tc);
                }

                // LED segmentleri: aralıklar her yarıçapta aynı piksel genişliğinde
                int seg = (int)(rpm_u / SEG_RPM);
                seg = seg >= nseg ? nseg - 1 : seg;
                const float in_seg = (rpm_u - seg * SEG_RPM) * arc_px;
                const float seg_px = SEG_RPM * arc_px;
                const float cov_a = clamp01(fminf(in_seg, seg_px - in_seg) - SEG_GAP_PX / 2 + 0.5f);
                const float cov_r = clamp01(r - R_LED_IN + 0.5f) * clamp01(R_LED_OUT - r + 0.5f);
                const float cov = u <= SPAN ? cov_a * cov_r : 0;
                const int seg_rpm = seg * SEG_RPM;
                if (cov > 0) {
                    const float bevel = 0.8f + 0.4f * (r - R_LED_IN) / (R_LED_OUT - R_LED_IN);
                    o = mixf(o, scalef(seg_rgb(seg_rpm, false, shift), bevel), cov);
                    const float mid = clamp01(1 - fabsf(r - r_mid) / half);
                    rgbf_t lc = mixf(seg_rgb(seg_rpm, true, shift), white, 0.45f * mid * mid * mid);    // sıcak çekirdek
                    l = mixf(l, scalef(lc, 0.72f + 0.28f * mid), cov);
                }

                // parıltı: yanık segmentten banda dışına ve aralıklara taşan ışık, tarama uçlarında söner
                const float dr = r < R_LED_IN ? R_LED_IN - r : r > R_LED_OUT ? r - R_LED_OUT : 0;
                const float ends = clamp01(u * DEG * r / 6) * clamp01((SPAN - u) * DEG * r / 6 + 0.3f);
                const float glow = 0.5f * expf(-dr / 6.0f) * (1 - cov) * ends;
                if (glow > 0.004f) {
                    const rgbf_t g = scalef(seg_rgb(seg_rpm, true, shift), glow);
                    l = (rgbf_t){l.r + g.r, l.g + g.g, l.b + g.b};
                }
            }
            off[i] = to_color(o);
            lit[i] = to_color(l);
        }
    }
    return true;
}

static void bake_task(void *arg)
{
    while (1) {
        const int mx = bake.want_max, sh = bake.want_shift, th = bake.want_theme;
        if (mx == bake.rpm_max && sh == bake.shift && th == bake.theme) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const int64_t t0 = esp_timer_get_time();
        if (bake_render(mx, sh, th)) {
            bake.rpm_max = mx;
            bake.shift = sh;
            bake.theme = th;
            bake.gen++;
            ESP_LOGI("ui_main", "kadran pişti: %d ms (arka planda)", (int)((esp_timer_get_time() - t0) / 1000));
        }
    }
}

/**
 * Kadran görüntüsünü ister. Görüntü nesnesi hemen kurulabilir; içerik arka planda pişer, bitince sayfa bir
 * kez yeniden çizilir (frame). Önceki pişmiş kadran o sırada yerinde kalır. Bellek yoksa false.
 */
static bool bake_dial(int rpm_max, int shift)
{
    if (!bake_alloc(&bake.off) || !bake_alloc(&bake.lit)) {
        return false;
    }
    if (bake.rpm_max == rpm_max && bake.shift == shift && bake.theme == g_settings.theme) {
        return true;
    }
    bake.want_max = rpm_max;
    bake.want_shift = shift;
    bake.want_theme = g_settings.theme;
    if (bake.task == NULL) {
        if (xTaskCreatePinnedToCore(bake_task, "bake", 4096, NULL, 1, &bake.task, 0) != pdPASS) {
            return false;
        }
    } else {
        xTaskNotifyGive(bake.task);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Sayfa durumu
// ---------------------------------------------------------------------------
enum { RUN_IDLE, RUN_ARMED, RUN_ON };

typedef struct {
    int lit;            // yanan segment sayısı
    int trail;          // iz dahil
    int peak;           // tepe segmenti, -1 = yok
    int frozen;         // soğuk motor sınırının ilk segmenti, nseg = yok
    lv_opa_t head;      // uç segment parlaklığı
    bool flash;
    bool vign;
    float ang[GHOSTS + 1];  // [0] = ibre, sonrakiler iz
} ring_state_t;

static struct {
    lv_obj_t *scr;
    lv_obj_t *ring;
    lv_obj_t *needle;
    lv_obj_t *dots[SHIFT_DOTS];
    lv_color_t sc[4];       // vites ışığı renkleri: 3 dolma + flaş (ayardan)
    ui_shift_t sh;
    lv_obj_t *status;
    lv_obj_t *val;
    lv_obj_t *unit;
    lv_obj_t *sec_val;
    lv_obj_t *sec_unit;
    lv_obj_t *pod_cap[POD_MAX];
    lv_obj_t *pod_val[POD_MAX];
    vems_ch_t pod_ch[POD_MAX];
    int npods;
    lv_obj_t *clt_arc;
    lv_obj_t *load_arc;
    lv_color_t clt_color;
    lv_color_t load_color;
    bool has_speed;
    bool has_clt;
    bool has_map;
    bool has_lh;
    bool speed_view;
    bool baked;
    uint32_t bake_gen;      // ekrana çizilmiş pişirme turu

    int rpm_max;
    int shift;
    int nseg;
    ring_state_t st;    // çizilen
    ring_state_t nx;    // bu karede hesaplanan

    // ibre ve halka dinamiği (devir biriminde)
    float pos;
    float vel;
    float trail_v;
    int64_t trail_t;
    float peak_v;
    int64_t peak_t;
    float hist[GHOSTS];
    int64_t last_us;
    int64_t sweep_t0;   // 0 = tur atılmadı, -1 = bitti

    // veri (ui_main_update)
    bool fresh;
    float clt;
    float map;
    bool mil_on;
    int mil_codes;
    bool cut;

    // rozetler
    char result[32];
    int64_t result_t;
    int run;
    int64_t run_t0;
    char last_val[16];
    char last_sec[16];

    // akıllı ekran: tam gazda kapsüller söner, büyük değer devir olur
    bool perf;
    int64_t perf_t;
    bool perf_shown;
} m;

static float rpm_ang(float rpm)
{
    // pişirilmiş kadranın ölçeği (sürüş sayfası ve açılış ekranı ortak), pişirilemediyse ayardaki
    return A0 + SPAN * rpm / (bake.rpm_max ? bake.rpm_max : m.rpm_max);
}

// ---------------------------------------------------------------------------
// Çizim
// ---------------------------------------------------------------------------
/** Yanık görüntünün [seg_a, seg_b) segment aralığı, açı maskesiyle. */
static void draw_lit(lv_draw_ctx_t *ctx, int seg_a, int seg_b, lv_opa_t opa)
{
    if (seg_b <= seg_a || bake.rpm_max == 0) {
        return;
    }
    const float a0 = rpm_ang(seg_a * SEG_RPM), a1 = rpm_ang(seg_b * SEG_RPM);
    lv_area_t sec, clip;
    ui_sector_area(a0, a1, R_TICK_MAJ - 10, CX, &sec);
    if (!_lv_area_intersect(&clip, ctx->clip_area, &sec)) {
        return;
    }
    // açı sınırları segment aralarının ortasında: tam sayı dereceye yuvarlama aralıkta kalır
    lv_draw_mask_angle_param_t ap;
    lv_draw_mask_angle_init(&ap, CX, CY, (lv_coord_t)lroundf(a0) % 360, (lv_coord_t)lroundf(a1) % 360);
    const int16_t id = lv_draw_mask_add(&ap, NULL);
    const lv_area_t *old = ctx->clip_area;
    ctx->clip_area = &clip;
    lv_draw_img_dsc_t dsc;
    lv_draw_img_dsc_init(&dsc);
    dsc.opa = opa;
    const lv_area_t full = {0, 0, SCR_W - 1, SCR_H - 1};
    lv_draw_img(ctx, &dsc, &full, &bake.lit);
    ctx->clip_area = old;
    lv_draw_mask_free_param(&ap);
    lv_draw_mask_remove_id(id);
}

#define SEG_FILL        (R_LED_OUT - R_LED_IN - 4)

/** Tek segmentin içine (kenarlardan biraz içeride, dış kenardan w kalınlıkta) renk: uç, tepe, flaş, soğuk bölge. */
static void draw_seg(lv_draw_ctx_t *ctx, int seg, lv_color_t col, lv_opa_t opa, int w)
{
    const float r = R_LED_OUT - 2;
    const float inset = (SEG_GAP_PX / 2 + 1.5f) / ((R_LED_IN + R_LED_OUT) / 2.0f) / DEG;
    const int a = (int)ceilf(rpm_ang(seg * SEG_RPM) + inset);
    const int b = (int)floorf(rpm_ang((seg + 1) * SEG_RPM) - inset);
    if (b <= a) {
        return;
    }
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = col;
    d.opa = opa;
    d.width = w;
    const lv_point_t c = {CX, CY};
    lv_draw_arc(ctx, &d, &c, (uint16_t)r, (uint16_t)(a % 360), (uint16_t)(b % 360));
}

static void ring_draw_cb(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    const ring_state_t *s = &m.st;
    for (int i = s->frozen; i < m.nseg; i++) {
        if (i >= s->trail) {
            draw_seg(ctx, i, C_SEG_FROZEN, LV_OPA_COVER, SEG_FILL);
        }
    }
    draw_lit(ctx, 0, s->lit, LV_OPA_COVER);
    draw_lit(ctx, s->lit, s->trail, LV_OPA_40);
    if (s->flash) {
        for (int i = 0; i < s->lit; i++) {
            draw_seg(ctx, i, m.sc[3], LV_OPA_90, SEG_FILL);
        }
    } else if (s->lit > 0 && s->head) {
        draw_seg(ctx, s->lit - 1, lv_color_white(), s->head, SEG_FILL);
    }
    if (s->peak >= s->lit) {
        draw_seg(ctx, s->peak, lv_color_white(), LV_OPA_COVER, 6);     // tepe: dış kenarda ince çizgi
    }
    if (s->vign) {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        const lv_point_t c = {CX, CY};
        d.color = C_ALERT;
        d.width = 7;
        lv_draw_arc(ctx, &d, &c, CX, 0, 360);
        d.width = 16;
        d.opa = LV_OPA_40;
        lv_draw_arc(ctx, &d, &c, CX - 6, 0, 360);
    }
}

static void needle_points(float ang, lv_point_t p[4])
{
    const float c = cosf(ang * DEG), s = sinf(ang * DEG);
    const float w0 = 5.0f, w1 = 1.6f;
    p[0] = (lv_point_t){(lv_coord_t)lroundf(CX + c * R_NDL_IN - s * w0), (lv_coord_t)lroundf(CY + s * R_NDL_IN + c * w0)};
    p[1] = (lv_point_t){(lv_coord_t)lroundf(CX + c * R_NDL_OUT - s * w1), (lv_coord_t)lroundf(CY + s * R_NDL_OUT + c * w1)};
    p[2] = (lv_point_t){(lv_coord_t)lroundf(CX + c * R_NDL_OUT + s * w1), (lv_coord_t)lroundf(CY + s * R_NDL_OUT - c * w1)};
    p[3] = (lv_point_t){(lv_coord_t)lroundf(CX + c * R_NDL_IN + s * w0), (lv_coord_t)lroundf(CY + s * R_NDL_IN - c * w0)};
}

static void needle_draw_cb(lv_event_t *e)
{
    static const lv_opa_t GHOST_OPA[GHOSTS] = {110, 70, 42, 24, 12};
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = C_NEEDLE;
    lv_point_t p[4];
    for (int i = GHOSTS; i >= 1; i--) {
        if (fabsf(m.st.ang[i] - m.st.ang[0]) > 0.4f) {
            d.bg_opa = GHOST_OPA[i - 1];
            needle_points(m.st.ang[i], p);
            lv_draw_polygon(ctx, &d, p, 4);
        }
    }
    d.bg_opa = LV_OPA_COVER;
    needle_points(m.st.ang[0], p);
    lv_draw_polygon(ctx, &d, p, 4);

    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = C_NEEDLE_CORE;
    l.width = 2;
    const float c = cosf(m.st.ang[0] * DEG), s = sinf(m.st.ang[0] * DEG);
    const lv_point_t a = {(lv_coord_t)lroundf(CX + c * (R_NDL_IN + 30)), (lv_coord_t)lroundf(CY + s * (R_NDL_IN + 30))};
    const lv_point_t b = {(lv_coord_t)lroundf(CX + c * (R_NDL_OUT - 3)), (lv_coord_t)lroundf(CY + s * (R_NDL_OUT - 3))};
    lv_draw_line(ctx, &l, &a, &b);
}

/** Değişen açı aralığını geçersiz kılar, sadece o kısım yeniden çizilir. */
/** Dış halkanın [a0, a1] aralığını 20°'lik dilimlerle geçersiz kılar: halkanın sınır kutusu tüm ekran olurdu. */
static void rim_invalidate(float a0, float a1)
{
    ui_ring_invalidate(m.ring, a0, a1, R_LED_IN - 4, CX);
}

static void ring_commit(void)
{
    const ring_state_t *o = &m.st, *n = &m.nx;
    float lo = 1e9f, hi = -1e9f;
#define TOUCH_SEG(a, b) do { if ((b) > (a)) { lo = fminf(lo, rpm_ang((a) * SEG_RPM)); \
                                              hi = fmaxf(hi, rpm_ang((b) * SEG_RPM)); } } while (0)
    if (o->lit != n->lit) {
        TOUCH_SEG(LV_MIN(o->lit, n->lit) - 1, LV_MAX(o->lit, n->lit));     // eski / yeni uç segment dahil
    }
    if (o->trail != n->trail) {
        TOUCH_SEG(LV_MIN(o->trail, n->trail), LV_MAX(o->trail, n->trail));
    }
    if (o->head != n->head) {
        TOUCH_SEG(n->lit - 1, n->lit);
    }
    if (o->peak != n->peak) {
        if (o->peak >= 0) {
            TOUCH_SEG(o->peak, o->peak + 1);
        }
        if (n->peak >= 0) {
            TOUCH_SEG(n->peak, n->peak + 1);
        }
    }
    if (o->frozen != n->frozen) {
        TOUCH_SEG(LV_MIN(o->frozen, n->frozen), m.nseg);
    }
#undef TOUCH_SEG
    bool moved = false;
    for (int i = 0; i <= GHOSTS; i++) {
        moved |= fabsf(o->ang[i] - n->ang[i]) > 0.05f;
    }
    if (moved) {
        for (int i = 0; i <= GHOSTS; i++) {
            lo = fminf(lo, fminf(o->ang[i], n->ang[i]) - 2);
            hi = fmaxf(hi, fmaxf(o->ang[i], n->ang[i]) + 2);
        }
    }
    // vites flaşı: tam ekran değil, sadece dış halka (segmentler + kenar) dilim dilim (~%30 piksel)
    if (o->vign != n->vign) {
        rim_invalidate(0, 360);
    } else if (o->flash != n->flash) {
        rim_invalidate(A0, rpm_ang(LV_MAX(o->lit, n->lit) * SEG_RPM));
    }
    if (hi > lo) {
        lv_area_t a;
        ui_sector_area(lo, hi, R_NDL_IN - 8, CX, &a);
        lv_obj_invalidate_area(m.ring, &a);
    }
    m.st = m.nx;
}

// ---------------------------------------------------------------------------
// Kurulum
// ---------------------------------------------------------------------------
static void pick_channels(uint32_t mask)
{
    mask = ui_ch_layout_mask(mask);
    m.has_speed = mask & CH_BIT(CH_SPEED);
    m.has_clt = mask & CH_BIT(CH_CLT);
    m.has_map = mask & CH_BIT(CH_MAP);
    m.has_lh = mask & CH_BIT(CH_FUEL_LH);
    // solda hararet, sağda sürücüyü en çok ilgilendiren
    const vems_ch_t prio[] = {CH_CLT, CH_FUEL_L100, CH_BATT, CH_FUEL_LH, CH_OIL, CH_IAT, CH_FUEL, CH_TPS, CH_LOAD,
                              CH_ADV};
    m.npods = 0;
    for (size_t i = 0; i < sizeof(prio) / sizeof(prio[0]) && m.npods < POD_MAX; i++) {
        const vems_ch_t c = prio[i];
        if (!(mask & CH_BIT(c)) || (c == CH_FUEL_LH && (mask & CH_BIT(CH_FUEL_L100)))) {
            continue;
        }
        m.pod_ch[m.npods++] = c;
    }
}

uint32_t ui_main_channels(void)
{
    uint32_t want = CH_BIT(CH_RPM) | (m.has_speed ? CH_BIT(CH_SPEED) : 0) | (m.has_clt ? CH_BIT(CH_CLT) : 0) |
                    (m.has_map ? CH_BIT(CH_MAP) : 0);
    for (int i = 0; i < m.npods; i++) {
        want |= CH_BIT(m.pod_ch[i]);
    }
    return want;
}

bool ui_main_toggle_view(void)
{
    if (!m.has_speed) {
        return false;
    }
    g_settings.main_view = m.speed_view ? SET_VIEW_RPM : SET_VIEW_SPEED;
    settings_save();
    ui_request_drive_rebuild();
    return true;
}

/** Ortalanmış "değer birim" satırı, yazı değişince kendiliğinden ortada kalır. */
static void value_row(lv_obj_t *scr, int y, const lv_font_t *font, const lv_font_t *ufont, lv_obj_t **val,
                      lv_obj_t **unit, const char *u)
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
    *unit = ui_label(row, ufont, C_MUTED, u);
    lv_obj_set_style_text_letter_space(*unit, 2, 0);
    // birim değerle aynı taban çizgisinde (satır alta hizalı: aradaki fark birimin alt boşluğu)
    lv_obj_set_style_pad_bottom(*unit, font->base_line - ufont->base_line, 0);
}

static const char *pod_caption(vems_ch_t c, char *buf, size_t len)
{
    const ui_ch_info_t *ci = &UI_CH[c];
    if (ci->unit[0] && c != CH_BOOST && c != CH_FUEL_L100) {
        snprintf(buf, len, "%s %s", ci->caption, ci->unit);
    } else {
        snprintf(buf, len, "%s", ci->caption);
    }
    return buf;
}

static lv_obj_t *gap_arc(lv_obj_t *scr, int a0, int a1, bool reverse)
{
    lv_obj_t *arc = lv_arc_create(scr);
    lv_obj_set_size(arc, 2 * 216, 2 * 216);
    lv_obj_center(arc);
    lv_obj_set_style_pad_all(arc, 0, 0);
    lv_arc_set_mode(arc, reverse ? LV_ARC_MODE_REVERSE : LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(arc, a0, a1);
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, C_OK, LV_PART_INDICATOR);
    return arc;
}

static void frame(bool force);

static void timer_cb(lv_timer_t *t)
{
    frame(false);
}

static void scr_delete_cb(lv_event_t *e)
{
    lv_timer_del(lv_event_get_user_data(e));
}

static lv_obj_t *layer_obj(lv_obj_t *scr, lv_event_cb_t cb)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, SCR_W, SCR_H);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, cb, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

lv_obj_t *ui_main_create(int page, int pages, uint32_t mask)
{
    lv_obj_t *scr = ui_screen_create();
    pick_channels(mask);
    m.scr = scr;
    m.speed_view = m.has_speed && g_settings.main_view == SET_VIEW_SPEED;
    m.rpm_max = g_settings.rpm_max / SEG_RPM * SEG_RPM;
    m.shift = g_settings.shift_rpm;
    m.nseg = m.rpm_max / SEG_RPM;
    m.baked = bake_dial(m.rpm_max, m.shift);
    m.bake_gen = bake.gen;

    if (m.baked) {
        lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);    // kadran görüntüsü her pikseli boyar
        lv_obj_t *bg = lv_img_create(scr);
        lv_img_set_src(bg, &bake.off);
        lv_obj_set_pos(bg, 0, 0);
    }
    m.ring = layer_obj(scr, ring_draw_cb);

    // 1000 rpm etiketleri, kırmızı bölgede kırmızı
    char buf[24];
    for (int k = 0; k * 1000 <= m.rpm_max; k++) {
        const float a = rpm_ang(k * 1000.0f) * DEG;
        snprintf(buf, sizeof(buf), "%d", k);
        lv_obj_t *l = ui_label(scr, &ui_font_label_lg, k * 1000 >= m.shift ? C_ALERT : C_MUTED, buf);
        lv_obj_align(l, LV_ALIGN_CENTER, (lv_coord_t)lroundf(cosf(a) * R_NUM), (lv_coord_t)lroundf(sinf(a) * R_NUM));
    }

    // dikey ritim, 8 px ızgara: görünür kenarlar arası (harf üstü / taban çizgisi) 16, gruplar arası 32,
    // başlık-değer 8. Blok, üstteki "4" etiketi ile alttaki yaylar arasında ortalı (~60 px üst ve alt).
    // Etiket kutusu üstünden harf üstüne: label 4, value 5, big 11 px (yazı tipi ölçülerinden)
    const int y_dots = 136;                 // ışıklar 136..148
    const int y_status = y_dots + 12 + 16 - 4;      // harf 164..175
    const int y_big = y_status + 4 + 11 + 16 - 11;  // rakam 191..265
    const int y_sec = y_big + 11 + 74 + 16 - 5;     // rakam 281..305
    const int y_cap = y_sec + 5 + 24 + 32 - 4;      // harf 337..348
    const int y_pod = y_cap + 4 + 11 + 8 - 5;       // rakam 356..380

    // vites ışıkları
    const int dot = 12, gap = 8, x0 = -(SHIFT_DOTS - 1) * (dot + gap) / 2;
    for (int i = 0; i < SHIFT_DOTS; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, dot, dot);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, C_DOT_OFF, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(d, LV_ALIGN_TOP_MID, x0 + i * (dot + gap), y_dots);
        m.dots[i] = d;
    }

    m.status = ui_label(scr, &ui_font_label, C_WARN, "");
    lv_obj_set_style_text_letter_space(m.status, 2, 0);
    lv_obj_align(m.status, LV_ALIGN_TOP_MID, 0, y_status);

    const bool big_speed = m.speed_view;
    value_row(scr, y_big, &ui_font_big, &ui_font_label_lg, &m.val, &m.unit, big_speed ? "KM/H" : "RPM");
    if (m.has_speed) {
        value_row(scr, y_sec, &ui_font_value, &ui_font_label, &m.sec_val, &m.sec_unit, big_speed ? "RPM" : "KM/H");
    } else {
        m.sec_val = NULL;
    }

    const int x = -(m.npods - 1) * POD_DX;
    for (int i = 0; i < m.npods; i++) {
        const int px = x + i * 2 * POD_DX;
        m.pod_cap[i] = ui_label(scr, &ui_font_label, C_MUTED, pod_caption(m.pod_ch[i], buf, sizeof(buf)));
        lv_obj_set_style_text_letter_space(m.pod_cap[i], 1, 0);
        lv_obj_align(m.pod_cap[i], LV_ALIGN_TOP_MID, px, y_cap);
        m.pod_val[i] = ui_label(scr, &ui_font_value, C_MUTED, "--");
        lv_obj_align(m.pod_val[i], LV_ALIGN_TOP_MID, px, y_pod);
    }

    // alt boşlukta iki yay: solda hararet, sağda motor yükü (MAP)
    m.clt_arc = m.has_clt ? gap_arc(scr, 100, 128, true) : NULL;
    m.load_arc = m.has_map ? gap_arc(scr, 52, 80, false) : NULL;
    m.clt_color = m.load_color = C_OK;
    if (m.clt_arc) {
        ui_arc_color_bind(m.clt_arc, &m.clt_color);
    }
    if (m.load_arc) {
        ui_arc_color_bind(m.load_arc, &m.load_color);
    }

    m.needle = layer_obj(scr, needle_draw_cb);
    ui_page_dots(scr, page, pages);

    // başlangıç durumu: ibre sıfırda, tur ekran ilk açılınca
    m.pos = m.vel = m.trail_v = m.peak_v = 0;
    for (int i = 0; i < GHOSTS; i++) {
        m.hist[i] = A0;
    }
    memset(&m.st, 0, sizeof(m.st));
    m.st.peak = -1;
    m.st.frozen = m.nseg;
    for (int i = 0; i <= GHOSTS; i++) {
        m.st.ang[i] = A0;
    }
    m.last_us = 0;
    m.sweep_t0 = 0;
    m.sh = (ui_shift_t){0};
    for (int i = 0; i < 4; i++) {
        m.sc[i] = ui_shift_color(i);
    }
    m.result_t = 0;
    m.run = RUN_IDLE;
    m.cut = false;
    m.clt = m.map = NAN;
    m.last_val[0] = m.last_sec[0] = '\0';
    m.perf = m.perf_shown = false;
    ui_motion_reset();

    lv_timer_t *t = lv_timer_create(timer_cb, FRAME_MS, NULL);
    lv_obj_add_event_cb(scr, scr_delete_cb, LV_EVENT_DELETE, t);
    return scr;
}

// ---------------------------------------------------------------------------
// Kare
// ---------------------------------------------------------------------------
static float ease_out(float t)
{
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

static void status_update(int64_t now, int cold_limit)
{
    char buf[40];
    const char *txt = "";
    lv_color_t col = C_MUTED;
    if (!m.fresh) {
        const vems_state_t st = vems_get_state();
        const bool lost = st == VEMS_STATE_ECU_OK;
        txt = lost ? LV_SYMBOL_WARNING "  VERİ YOK" : st == VEMS_STATE_LINK_UP ? "ECU SORGULANIYOR" : "OBD ARANIYOR";
        col = lost ? C_ALERT : C_WARN;
    } else if (m.mil_on) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  ARIZA · %d KOD", m.mil_codes > 0 ? m.mil_codes : 1);
        txt = buf;
        col = C_ALERT;
    } else if (m.result_t && now - m.result_t < RESULT_US) {
        txt = m.result;
        col = C_PRIMARY;
    } else if (m.cut) {
        txt = "YAKIT KESİK";
        col = C_OK;
    } else if (cold_limit < m.shift) {
        snprintf(buf, sizeof(buf), "MOTOR SOĞUK · %d", cold_limit);
        txt = buf;
        col = C_ACCENT;
    } else if (m.mil_codes > 0) {
        snprintf(buf, sizeof(buf), "%d KAYITLI KOD", m.mil_codes);
        txt = buf;
        col = C_WARN;
    }
    ui_label_set(m.status, txt);
    if (lv_obj_get_style_text_color(m.status, 0).full != col.full) {
        lv_obj_set_style_text_color(m.status, col, 0);
    }
}

static void set_color(lv_obj_t *o, lv_color_t c)
{
    if (lv_obj_get_style_text_color(o, 0).full != c.full) {
        lv_obj_set_style_text_color(o, c, 0);
    }
}

static void opa_anim_cb(void *o, int32_t v)
{
    lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

/** Akıllı ekran geçişi: kapsüller 220 ms'de söner / yanar (küçük alanlar), birimler devir / hıza döner. */
static void perf_apply(bool perf)
{
    m.perf_shown = perf;
    for (int i = 0; i < m.npods; i++) {
        lv_obj_t *objs[2] = {m.pod_cap[i], m.pod_val[i]};
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
    if (m.speed_view && m.has_speed) {
        ui_label_set(m.unit, perf ? "RPM" : "KM/H");
        if (m.sec_val) {
            ui_label_set(m.sec_unit, perf ? "KM/H" : "RPM");
        }
    }
}

/** 0-100 km/h: durunca kurulur, kalkışta başlar, 100'de biter. */
static void run_update(float spd, int64_t now)
{
    if (isnan(spd)) {
        m.run = RUN_IDLE;
        return;
    }
    if (spd < 1) {
        m.run = RUN_ARMED;
    } else if (m.run == RUN_ARMED) {
        m.run = RUN_ON;
        m.run_t0 = now;
    } else if (m.run == RUN_ON) {
        if (spd >= 100) {
            snprintf(m.result, sizeof(m.result), "0-100 · %.1f SN", (now - m.run_t0) / 1e6f);
            m.result_t = now;
            m.run = RUN_IDLE;
        } else if (now - m.run_t0 > RUN_MAX_US) {
            m.run = RUN_IDLE;
        }
    }
}

static void frame(bool force)
{
    if (!force && lv_scr_act() != m.scr) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    float dt = m.last_us ? (now - m.last_us) / 1e6f : 0;
    dt = dt > 0.05f ? 0.05f : dt;
    m.last_us = now;
    if (m.sweep_t0 == 0 && lv_scr_act() == m.scr) {
        m.sweep_t0 = now;
    }
    // kadran arka planda yeni pişti: bir kez tamamen yeniden çiz
    if (m.baked && m.bake_gen != bake.gen) {
        m.bake_gen = bake.gen;
        lv_obj_invalidate(m.scr);
    }

    ui_motion_t mo;
    ui_motion_get(now, &mo);
    const bool live = m.fresh && !isnan(mo.rpm);
    float target = live ? mo.rpm : 0;
    if (m.sweep_t0 > 0) {
        const float p = (now - m.sweep_t0) / (float)SWEEP_US;
        if (p >= 1) {
            m.sweep_t0 = -1;
            m.peak_v = m.trail_v = m.pos;   // turun tepesi kalmasın
        } else {
            const float s = p < 0.45f ? ease_out(p / 0.45f) : p < 0.55f ? 1 : 1 - ease_out((p - 0.55f) / 0.45f);
            target = fmaxf(target, s * m.rpm_max);
        }
    }

    // ibre: yay + sönüm, hedef değiştikçe hafif aşıp oturur. Sabit küçük adımlarla: kare uzasa da kararlı
    for (float left = dt; left > 0; left -= SPRING_STEP) {
        const float h = fminf(left, SPRING_STEP);
        const float acc = SPRING_W * SPRING_W * (target - m.pos) - 2 * SPRING_Z * SPRING_W * m.vel;
        m.vel += acc * h;
        m.pos += m.vel * h;
    }
    if (m.pos < 0 || m.pos > m.rpm_max * 1.01f) {
        m.pos = fmaxf(0, fminf(m.pos, m.rpm_max * 1.01f));
        m.vel = 0;
    }
    const float pos = fminf(m.pos, m.rpm_max);

    // iz ve tepe
    if (pos >= m.trail_v) {
        m.trail_v = pos;
        m.trail_t = now;
    } else if (now - m.trail_t > TRAIL_HOLD_US) {
        m.trail_v = fmaxf(pos, m.trail_v - TRAIL_DROP * dt);
    }
    if (pos >= m.peak_v) {
        m.peak_v = pos;
        m.peak_t = now;
    } else if (now - m.peak_t > PEAK_HOLD_US) {
        m.peak_v = fmaxf(pos, m.peak_v - PEAK_DROP * dt);
    }
    // soğuk motor: izin verilen devir 20 °C'de 3500, 60 °C'de vites noktası
    const int cold_limit = ui_shift_limit(m.shift, m.fresh && m.has_clt ? m.clt : NAN);
    const int eff_shift = cold_limit;
    // vites ışıkları devir tahmininden: ibrenin yay aşması sahte flaş yapmasın, gecikme olmasın.
    // açılış turunda ibreyi izler ama flaş yapmaz
    const bool sweeping = m.sweep_t0 >= 0;
    const float srpm = sweeping ? pos : live ? mo.rpm : 0;
    const bool shift = ui_shift_update(&m.sh, sweeping ? NAN : srpm, eff_shift, now);
    const bool blink = ui_shift_blink(&m.sh, now, BLINK_US);

    ring_state_t *n = &m.nx;
    n->lit = (int)ceilf(pos / SEG_RPM - 0.02f);
    n->lit = LV_MAX(0, LV_MIN(n->lit, m.nseg));
    n->trail = LV_MAX(n->lit, LV_MIN((int)ceilf(m.trail_v / SEG_RPM - 0.02f), m.nseg));
    const int pk = (int)ceilf(m.peak_v / SEG_RPM - 0.02f) - 1;
    n->peak = live && pk >= n->lit && pk >= 0 ? pk : -1;
    n->frozen = cold_limit < m.shift ? cold_limit / SEG_RPM : m.nseg;
    // uç segment devir hızla yükselirken daha parlak
    n->head = (lv_opa_t)(LV_MIN(200, 70 + (int)fmaxf(0, m.vel) / 40) / 26 * 26);
    n->flash = blink;
    n->vign = blink;
    n->ang[0] = rpm_ang(pos);
    for (int i = 0; i < GHOSTS; i++) {
        n->ang[i + 1] = m.hist[i];
    }
    memmove(&m.hist[1], &m.hist[0], (GHOSTS - 1) * sizeof(float));
    m.hist[0] = n->ang[0];
    ring_commit();

    // vites ışıkları: renkler Ayarlar > Vites ışığı rengi
    const float step = (float)SHIFT_LEAD / SHIFT_DOTS;
    for (int i = 0; i < SHIFT_DOTS; i++) {
        lv_color_t c = C_DOT_OFF;
        if (shift) {
            c = blink ? m.sc[3] : C_DOT_OFF;
        } else if (srpm >= eff_shift - SHIFT_LEAD + i * step) {
            c = m.sc[i < 2 ? 0 : i < 5 ? 1 : 2];
        }
        if (lv_obj_get_style_bg_color(m.dots[i], 0).full != c.full) {
            lv_obj_set_style_bg_color(m.dots[i], c, 0);
        }
    }

    // akıllı ekran: tam gazda büyük değer devir olur, kapsüller söner (sadece geçişte, karede stil yok)
    if (ui_perf_update(&m.perf, &m.perf_t, live ? srpm : NAN, eff_shift, m.map, now) != m.perf_shown) {
        perf_apply(m.perf);
    }

    // değerler: devir 50'lik adımla (okunur kalsın), hız tam sayı
    char buf[16];
    const float spd = live ? mo.speed : NAN;
    const float rpm_txt = live ? roundf(mo.rpm / 50) * 50 : NAN;
    const bool big_speed = m.speed_view && m.has_speed && !m.perf_shown;
    ui_ch_format(big_speed ? CH_SPEED : CH_RPM, big_speed ? spd : rpm_txt, buf, sizeof(buf));
    if (strcmp(buf, m.last_val) != 0) {
        strcpy(m.last_val, buf);
        lv_label_set_text(m.val, buf);
    }
    set_color(m.val, isnan(big_speed ? spd : rpm_txt) ? C_MUTED : (blink && !big_speed) ? C_ALERT : C_TEXT);
    if (m.sec_val) {
        ui_ch_format(big_speed ? CH_RPM : CH_SPEED, big_speed ? rpm_txt : spd, buf, sizeof(buf));
        if (strcmp(buf, m.last_sec) != 0) {
            strcpy(m.last_sec, buf);
            lv_label_set_text(m.sec_val, buf);
        }
        set_color(m.sec_val, isnan(spd) ? C_MUTED : (blink && big_speed) ? C_ALERT : C_SOFT);
    }

    if (live) {
        run_update(spd, now);
    }
    status_update(now, cold_limit);
}

void ui_main_update(const vems_data_t *d, bool fresh)
{
    char buf[24];
    m.fresh = fresh;
    if (fresh) {
        ui_motion_feed(d);
    }
    m.mil_on = fresh && d->mil_on;
    m.mil_codes = fresh ? d->mil_codes : 0;
    m.clt = fresh ? d->clt_c : NAN;
    m.map = fresh && m.has_map ? d->map_kpa : NAN;

    // gaz kapalı, yüksek vakum, devir yüksek: ECU yakıtı kesmiştir (motor freni)
    if (!fresh || isnan(m.map)) {
        m.cut = false;
    } else if (d->rpm > 1500 && m.map < CUT_KPA && (isnan(d->speed_kmh) || d->speed_kmh > 15)) {
        m.cut = true;
    } else if (d->rpm < 1200 || m.map > CUT_KPA + 4) {
        m.cut = false;
    }

    for (int i = 0; i < m.npods; i++) {
        vems_ch_t c = m.pod_ch[i];
        float x = fresh ? ui_ch_value(d, c) : NAN;
        if (c == CH_FUEL_L100) {
            // dururken L/100 km anlamsız: anlık L/saat göster
            const bool lh = fresh && isnan(x) && m.has_lh;
            ui_label_set(m.pod_cap[i], lh ? "L/SAAT" : UI_CH[CH_FUEL_L100].caption);
            if (lh) {
                c = CH_FUEL_LH;
                x = ui_ch_value(d, c);
            }
        }
        ui_ch_format(c, x, buf, sizeof(buf));
        ui_label_set(m.pod_val[i], buf);
        set_color(m.pod_val[i], ui_ch_color(c, x));
    }

    if (m.clt_arc) {
        const lv_color_t col = ui_ch_color(CH_CLT, m.clt);
        const lv_color_t ac = col.full == C_TEXT.full || col.full == C_MUTED.full ? C_OK : col;
        if (ac.full != m.clt_color.full) {
            m.clt_color = ac;
            ui_ring_invalidate(m.clt_arc, 100, 128, 204, 220);     // stil değil: sadece yay dilimi çizilir
        }
        const float f = isnan(m.clt) ? 0 : clamp01((m.clt - CLT_LO) / (CLT_HI - CLT_LO));
        const int16_t v = (int16_t)(f * 1000) / 10 * 10;
        if (lv_arc_get_value(m.clt_arc) != v) {
            lv_arc_set_value(m.clt_arc, v);
        }
    }
    if (m.load_arc) {
        // atmosferik motor: 25 kPa (rölanti / motor freni) .. 100 kPa (tam gaz)
        const float f = isnan(m.map) ? 0 : clamp01((m.map - 25) / 75);
        const lv_color_t ac = m.cut ? C_ACCENT : f > 0.8f ? C_ALERT : f > 0.55f ? C_WARN : C_PRIMARY;
        if (ac.full != m.load_color.full) {
            m.load_color = ac;
            ui_ring_invalidate(m.load_arc, 52, 80, 204, 220);
        }
        const int16_t v = (int16_t)(f * 1000) / 10 * 10;
        if (lv_arc_get_value(m.load_arc) != v) {
            lv_arc_set_value(m.load_arc, v);
        }
    }
    frame(true);
}
