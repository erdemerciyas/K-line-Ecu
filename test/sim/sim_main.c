/*
 * Ekran simülatörü: gerçek LVGL ve ui_*.c sayfaları PC'de çalışır, her sahnenin son karesi PNG olarak yazılır.
 * Zaman sahtedir (2 ms adım), K-Line verisi senaryodan gelir (~140 ms'de bir çerçeve). Çizim istatistiği
 * cihazdaki monitor_cb ile aynı ölçüdür: kare başına yeniden çizilen piksel (%). Gerçek süre ölçülmez,
 * PC hızlıdır; takılma riski "px %" ile görülür.
 *
 *   pwsh -File test\sim\run.ps1                 tüm sahneler -> test\sim\out\*.png
 *   pwsh -File test\sim\run.ps1 -Scene drive    adında "drive" geçen sahneler
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_lcd.h"
#include "ui_priv.h"
#include "imu.h"
#include "srclog.h"
#include "ble_panel.h"

// ---------------------------------------------------------------------------
// Windows (kernel32 / msvcrt), başlık dosyası olmadan
// ---------------------------------------------------------------------------
typedef void *HANDLE;
HANDLE CreateThread(void *sa, size_t stack, unsigned long (*fn)(void *), void *arg, unsigned long flags, unsigned long *id);
HANDLE CreateEventA(void *sa, int manual, int initial, const char *name);
int SetEvent(HANDLE h);
unsigned long WaitForSingleObject(HANDLE h, unsigned long ms);
void Sleep(unsigned long ms);
void ExitThread(unsigned long code);
unsigned long GetCurrentThreadId(void);
int _vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
char *GetCommandLineA(void);

// msvc hedefinde derleyici yerleşikleri bu kütüphane çağrılarına dönüşür (CRT yok)
int _fltused = 0;
#undef lroundf
#undef roundf
#undef ceilf
#undef floorf
#undef truncf
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float truncf(float x) { return x < 0 ? -floorf(-x) : floorf(x); }
float roundf(float x) { return x < 0 ? -floorf(-x + 0.5f) : floorf(x + 0.5f); }
long lroundf(float x) { return (long)roundf(x); }

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    // msvcrt _vsnprintf taşmada sonlandırmaz ve -1 döner
    int r = _vsnprintf(buf, n, fmt, ap);
    if (n) {
        buf[n - 1] = '\0';
    }
    return r < 0 ? (int)n : r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n) {
        size_t c = len >= n ? n - 1 : len;
        memcpy(d, s, c);
        d[c] = '\0';
    }
    return len;
}

static bool s_verbose;

void sim_log(char level, const char *tag, const char *fmt, ...)
{
    if (!s_verbose && level != 'E') {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("  %c %s: %s\n", level, tag, buf);
}

// ---------------------------------------------------------------------------
// Sahte saat ve FreeRTOS (görev = Windows iş parçacığı)
// ---------------------------------------------------------------------------
static volatile int64_t s_now_us = 1000000;

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

typedef struct {
    void (*fn)(void *);
    void *arg;
    HANDLE ev;
    unsigned long tid;
} sim_task_t;

static sim_task_t s_tasks[16];
static int s_ntasks;

static unsigned long task_tramp(void *p)
{
    sim_task_t *t = p;
    t->fn(t->arg);
    return 0;
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core)
{
    if (s_ntasks >= 16) {
        return pdFAIL;
    }
    sim_task_t *t = &s_tasks[s_ntasks++];
    t->fn = fn;
    t->arg = arg;
    t->ev = CreateEventA(NULL, 0, 0, NULL);
    if (out) {
        *out = t;
    }
    CreateThread(NULL, 2u << 20, task_tramp, t, 0, &t->tid);
    return pdPASS;
}

BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *out)
{
    return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, 0);
}

void vTaskDelete(TaskHandle_t t)
{
    if (t == NULL) {
        ExitThread(0);
    }
}

void vTaskDelay(TickType_t ticks)
{
    Sleep(0);
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    const unsigned long me = GetCurrentThreadId();
    for (int i = 0; i < s_ntasks; i++) {
        if (s_tasks[i].tid == me) {
            WaitForSingleObject(s_tasks[i].ev, 0xFFFFFFFFu);
            return 1;
        }
    }
    return 0;
}

BaseType_t xTaskNotifyGive(TaskHandle_t t)
{
    SetEvent(((sim_task_t *)t)->ev);
    return pdPASS;
}

// ---------------------------------------------------------------------------
// Donanım ve veri kaynağı taklitleri
// ---------------------------------------------------------------------------
void board_lcd_set_backlight(uint8_t percent) {}
void board_buzzer_set(bool on) {}
void *board_lcd_frame_buffer(int index) { return NULL; }
bool board_touch_read(uint16_t *x, uint16_t *y) { return false; }
void board_lcd_present(const void *fb) {}

bool ble_panel_connected(void) { return false; }
void ble_panel_set_enabled(bool on) {}

void ui_request_rebuild(void) {}
void ui_request_drive_rebuild(void) {}
void ui_request_rescan(void) {}
void ui_request_dtc_page(void) {}
void dash_ui_set_rotation(uint16_t deg) {}

static imu_incl_t s_imu = {.ok = true, .zeroed = true, .roll = 4.0f, .pitch = -2.5f};
bool imu_present(void) { return true; }
void imu_get(imu_incl_t *out) { *out = s_imu; }
void imu_zero(void) {}

static vems_data_t s_data;
static uint32_t s_mask;
static vems_state_t s_state = VEMS_STATE_ECU_OK;

vems_state_t vems_get_state(void) { return s_state; }
uint32_t vems_get_channels(void) { return s_mask; }
void vems_set_wanted_channels(uint32_t mask) {}
void vems_rescan(void) {}
const char *vems_mode_name(vems_mode_t mode) { return "OBD"; }
void vems_get_stats(vems_stats_t *out) { memset(out, 0, sizeof(*out)); }
int vems_get_profile(vems_pid_info_t *out, int max) { return 0; }
bool vems_dtc_clear(void) { return true; }

bool vems_get_latest(vems_data_t *out)
{
    *out = s_data;
    return s_data.seq != 0;
}

void vems_get_link_info(vems_link_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->protocol = "KWP FAST";
    out->standard = "ISO 14230-4";
    strcpy(out->ecu, "0x10");
    strcpy(out->line, "10400 baud");
    out->obd = true;
}

int vems_dtc_read(uint16_t *codes, int max)
{
    codes[0] = 0x0130;      // P0130
    codes[1] = 0x0301;      // P0301
    return 2;
}

int srclog_read(srclog_line_t *out, int max, uint32_t *seq)
{
    static const srclog_line_t L[] = {
        {"K-Line 5 baud init", "0x08 0x08", SRCLOG_OK},
        {"ECU", "0x10", SRCLOG_OK},
        {"PID taraması", "18 kanal", SRCLOG_OK},
    };
    const int n = (int)(sizeof(L) / sizeof(L[0])) < max ? (int)(sizeof(L) / sizeof(L[0])) : max;
    memcpy(out, L, n * sizeof(L[0]));
    *seq = 1;
    return n;
}

// ---------------------------------------------------------------------------
// Senaryo verisi
// ---------------------------------------------------------------------------
typedef struct {
    float rpm, speed, clt, map, tps, load;
    bool afr;
} scen_t;

#define MASK_OBD (CH_BIT(CH_RPM) | CH_BIT(CH_SPEED) | CH_BIT(CH_MAP) | CH_BIT(CH_CLT) | CH_BIT(CH_IAT) | \
                  CH_BIT(CH_BATT) | CH_BIT(CH_TPS) | CH_BIT(CH_LOAD) | CH_BIT(CH_ADV) | CH_BIT(CH_O2V) | \
                  CH_BIT(CH_O2V2) | CH_BIT(CH_STFT) | CH_BIT(CH_LTFT) | CH_BIT(CH_FUEL_LH) | CH_BIT(CH_FUEL_L100))

static void data_frame(const scen_t *s)
{
    vems_data_t *d = &s_data;
    d->seq++;
    d->timestamp_us = s_now_us;
    d->rpm = (uint16_t)s->rpm;
    d->rpm_us = s_now_us;
    d->speed_kmh = s->speed;
    d->speed_us = s_now_us;
    d->clt_c = s->clt;
    d->map_kpa = s->map;
    d->iat_c = 31;
    d->batt_v = 14.1f;
    d->tps_pct = s->tps;
    d->load_pct = s->load;
    d->ign_adv_deg = 18.5f;
    d->o2_v = 0.62f;
    d->o2b_v = 0.71f;
    d->ego_corr_pct = 102.1f;
    d->ltft_pct = -1.6f;
    d->fuel_lh = 2.4f;
    d->fuel_l100 = s->speed > 5 ? 6.3f : NAN;
    d->oil_c = d->fuel_pct = d->maf_gs = d->baro_kpa = d->runtime_s = NAN;
    d->mil_codes = 0;
    if (s->afr) {
        d->ego_raw = 120;
        d->lambda = 0.99f;
        d->lambda_target = 1.0f;
    } else {
        d->ego_raw = 255;
        d->lambda = 0;
        d->lambda_target = 0;
    }
}

// ---------------------------------------------------------------------------
// Ekran sürücüsü ve ölçüm
// ---------------------------------------------------------------------------
static lv_color_t *s_fb;
static struct {
    uint32_t frames, px, px_max;
} st;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    lv_disp_flush_ready(drv);
}

static void monitor_cb(lv_disp_drv_t *drv, uint32_t time_ms, uint32_t px)
{
    st.frames++;
    st.px += px;
    st.px_max = px > st.px_max ? px : st.px_max;
}

/** Sahte zamanı ilerletir; LVGL görevinin yaptığı gibi her adımda zamanlayıcıları çalıştırır. */
static void run_ms(int ms, const scen_t *s, void (*update)(const vems_data_t *, bool), uint32_t mask,
                   void (*vary)(scen_t *, int))
{
    scen_t cur = s ? *s : (scen_t){0};
    for (int t = 0; t < ms; t += 2) {
        s_now_us += 2000;
        lv_tick_inc(2);
        if (s && (t % 140) == 0) {
            if (vary) {
                vary(&cur, t);
            }
            data_frame(&cur);
        }
        if ((t % 20) == 0) {
            vems_data_t d;
            const bool have = vems_get_latest(&d);
            const bool fresh = have && s_now_us - d.timestamp_us < 1500000;
            if (fresh) {
                ui_ch_track(&d, mask);
            }
            ui_alert_update(&d, fresh, mask);
            if (update) {
                update(&d, fresh);
            }
        }
        lv_timer_handler();
    }
}

// PNG: sıkıştırmasız deflate blokları, cam dışındaki pikseller %30 parlaklıkta (taşan içerik görünsün)
static uint32_t crc_tab[256];

static uint32_t crc32(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_tab[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++) {
                k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            }
            crc_tab[i] = k;
        }
    }
    c = ~c;
    while (n--) {
        c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    }
    return ~c;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8];
    put32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    fwrite(data, 1, len, f);
    uint32_t c = crc32(0, (const uint8_t *)type, 4);
    c = crc32(c, data, len);
    put32(h, c);
    fwrite(h, 1, 4, f);
}

static void png_write(const char *path)
{
    const int W = BOARD_LCD_H_RES, H = BOARD_LCD_V_RES;
    const size_t raw_len = (size_t)H * (1 + W * 3);
    uint8_t *raw = malloc(raw_len);
    uint8_t *p = raw;
    for (int y = 0; y < H; y++) {
        *p++ = 0;
        for (int x = 0; x < W; x++) {
            const lv_color_t c = s_fb[y * W + x];
            uint8_t r = (c.ch.red * 255) / 31, g = (c.ch.green * 255) / 63, b = (c.ch.blue * 255) / 31;
            const float dx = x + 0.5f - W / 2.0f, dy = y + 0.5f - H / 2.0f;
            if (dx * dx + dy * dy > (W / 2.0f) * (W / 2.0f)) {
                r = r * 3 / 10 + 18;
                g = g * 3 / 10 + 18;
                b = b * 3 / 10 + 24;
            }
            *p++ = r;
            *p++ = g;
            *p++ = b;
        }
    }
    const size_t nblk = (raw_len + 65534) / 65535;
    const size_t z_len = 2 + raw_len + nblk * 5 + 4;
    uint8_t *z = malloc(z_len);
    uint8_t *q = z;
    *q++ = 0x78;
    *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; off += 65535) {
        const size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        *q++ = off + n == raw_len;
        *q++ = n & 0xFF;
        *q++ = n >> 8;
        *q++ = ~n & 0xFF;
        *q++ = (~n >> 8) & 0xFF;
        memcpy(q, raw + off, n);
        q += n;
        for (size_t i = 0; i < n; i++) {
            a = (a + raw[off + i]) % 65521;
            b = (b + a) % 65521;
        }
    }
    put32(q, (b << 16) | a);
    q += 4;

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("  ! yazılamadı: %s\n", path);
        free(raw);
        free(z);
        return;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, W);
    put32(ihdr + 4, H);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

// ---------------------------------------------------------------------------
// Sahneler
// ---------------------------------------------------------------------------
static const char *s_out = "out";
static const char *s_filter;
static int s_pages = 7;

static void stats_reset(void)
{
    memset(&st, 0, sizeof(st));
}

static void stats_print(const char *name)
{
    const uint32_t full = BOARD_LCD_H_RES * BOARD_LCD_V_RES;
    if (st.frames == 0) {
        printf("  %-22s  kare yok (boşta)\n", name);
        return;
    }
    printf("  %-22s  %4lu kare  ort %%%-5.1f  en çok %%%.1f\n", name, (unsigned long)st.frames,
           st.px * 100.0 / st.frames / full, st.px_max * 100.0 / full);
}

static void snap(const char *name)
{
    char path[260];
    snprintf(path, sizeof(path), "%s/%s.png", s_out, name);
    // kalan çizimler bitsin: sadece LVGL (veri yok)
    lv_refr_now(NULL);
    png_write(path);
}

/** Sahne: sayfayı kurar, yerleşsin diye bekler, sonra ölçümlü süre boyunca çalıştırır ve PNG yazar. */
static void scene(const char *name, lv_obj_t *(*create)(void), void (*update)(const vems_data_t *, bool),
                  const scen_t *s, uint32_t mask, int settle_ms, int run_ms_, void (*vary)(scen_t *, int))
{
    if (s_filter && strstr(name, s_filter) == NULL) {
        return;
    }
    s_mask = mask;
    lv_obj_t *old = lv_scr_act();
    lv_obj_t *scr = create();
    lv_scr_load(scr);
    if (old && old != scr) {
        lv_obj_del(old);
    }
    run_ms(settle_ms, s, update, mask, vary);
    Sleep(150);         // arka plan pişirmesi (gerçek iş parçacığı) bitsin
    run_ms(200, s, update, mask, vary);
    stats_reset();
    run_ms(run_ms_, s, update, mask, vary);
    stats_print(name);
    snap(name);
}

// sayfa kurucuları: sayfa indeksi ve sayısı sabit
static lv_obj_t *mk_drive(void) { return ui_main_create(0, s_pages, s_mask); }
static lv_obj_t *mk_digital(void) { return ui_digital_create(0, s_pages, s_mask); }
static lv_obj_t *mk_focus(void) { return ui_focus_create(1, s_pages, s_mask); }
static lv_obj_t *mk_list(void) { return ui_list_create(2, s_pages, s_mask); }
static lv_obj_t *mk_afr(void) { return ui_afr_create(3, s_pages); }
static lv_obj_t *mk_incline(void) { return ui_incline_create(4, s_pages); }
static lv_obj_t *mk_dtc(void) { return ui_dtc_create(5, s_pages); }
static lv_obj_t *mk_settings(void) { return ui_settings_create(6, s_pages); }
static lv_obj_t *mk_splash(void) { return ui_splash_create(); }

static void upd_settings(const vems_data_t *d, bool fresh) { ui_settings_update(); }
static void upd_splash(const vems_data_t *d, bool fresh) { ui_splash_update(); }

// devir taraması: 3000 -> 6800 rpm, 3. vites
static void vary_pull(scen_t *s, int t)
{
    const float k = (t % 2400) / 2400.0f;
    s->rpm = 3000 + 3800 * k;
    s->speed = s->rpm / 6800 * 118;
}

static void vary_cruise(scen_t *s, int t)
{
    s->rpm = 2380 + 60 * sinf(t / 400.0f);
}

void sim_entry(void)
{
    // komut satırı: [çıktı klasörü] [sahne süzgeci] [-v]
    char *cl = GetCommandLineA();
    static char args[4][260];
    int na = 0;
    for (char *p = cl; *p && na < 4;) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        const bool q = *p == '"';
        p += q;
        char *d = args[na];
        while (*p && (q ? *p != '"' : *p != ' ')) {
            *d++ = *p++;
        }
        *d = '\0';
        p += q && *p;
        na++;
    }
    for (int i = 1; i < na; i++) {
        if (strcmp(args[i], "-v") == 0) {
            s_verbose = true;
        } else if (i == 1) {
            s_out = args[i];
        } else {
            s_filter = strcmp(args[i], "*") ? args[i] : NULL;
        }
    }

    settings_init();
    lv_init();
    s_fb = calloc(BOARD_LCD_H_RES * BOARD_LCD_V_RES, sizeof(lv_color_t));
    static lv_disp_draw_buf_t db;
    lv_disp_draw_buf_init(&db, s_fb, NULL, BOARD_LCD_H_RES * BOARD_LCD_V_RES);
    static lv_disp_drv_t dd;
    lv_disp_drv_init(&dd);
    dd.hor_res = BOARD_LCD_H_RES;
    dd.ver_res = BOARD_LCD_V_RES;
    dd.flush_cb = flush_cb;
    dd.draw_buf = &db;
    dd.direct_mode = 1;
    dd.monitor_cb = monitor_cb;
    lv_disp_t *disp = lv_disp_drv_register(&dd);
    lv_disp_set_bg_opa(disp, LV_OPA_TRANSP);
    ui_alert_init();

    printf("sahne                    kare      yeniden çizim / kare\n");
    const scen_t cruise = {2380, 87, 88, 45, 18, 36, false};
    const scen_t idle = {850, 0, 84, 32, 0, 18, false};
    const scen_t pull = {3000, 52, 90, 98, 100, 92, false};
    const scen_t hot = {2380, 87, 112, 45, 18, 36, false};
    const scen_t cold = {1450, 24, 46, 40, 12, 28, false};
    const scen_t afr = {2380, 87, 88, 45, 18, 36, true};
    const uint32_t mask_afr = MASK_OBD | CH_BIT(CH_AFR) | CH_BIT(CH_TARGET);

    scene("splash", mk_splash, upd_splash, NULL, 0, 600, 1000, NULL);
    scene("drive_idle", mk_drive, ui_main_update, &idle, MASK_OBD, 2500, 1000, NULL);
    scene("drive_cruise", mk_drive, ui_main_update, &cruise, MASK_OBD, 2500, 2000, vary_cruise);
    scene("drive_pull", mk_drive, ui_main_update, &pull, MASK_OBD, 2500, 2300, vary_pull);
    scene("drive_cold", mk_drive, ui_main_update, &cold, MASK_OBD, 2500, 1000, NULL);
    scene("digital_cruise", mk_digital, ui_digital_update, &cruise, MASK_OBD, 2500, 2000, vary_cruise);
    scene("digital_pull", mk_digital, ui_digital_update, &pull, MASK_OBD, 2500, 2300, vary_pull);
    scene("focus", mk_focus, ui_focus_update, &cruise, MASK_OBD, 1000, 1000, vary_cruise);
    scene("list", mk_list, ui_list_update, &cruise, MASK_OBD, 1000, 1000, vary_cruise);
    scene("afr", mk_afr, ui_afr_update, &afr, mask_afr, 1000, 1000, NULL);
    scene("incline", mk_incline, NULL, &cruise, MASK_OBD, 1000, 1000, NULL);
    scene("dtc", mk_dtc, ui_dtc_update, &cruise, MASK_OBD, 1500, 500, NULL);
    scene("settings", mk_settings, upd_settings, &cruise, MASK_OBD, 500, 500, NULL);
    // temalar: gece (kısık) ve kırmızı gece; uyarı katmanı da yeni renklerle kurulur
    g_settings.theme = SET_THEME_NIGHT;
    ui_alert_theme();
    scene("gece_drive", mk_drive, ui_main_update, &cruise, MASK_OBD, 2500, 1000, vary_cruise);
    scene("gece_afr", mk_afr, ui_afr_update, &afr, mask_afr, 1000, 500, NULL);
    g_settings.theme = SET_THEME_RED;
    ui_alert_theme();
    scene("kirmizi_drive", mk_drive, ui_main_update, &cruise, MASK_OBD, 2500, 1000, vary_cruise);
    scene("kirmizi_digital", mk_digital, ui_digital_update, &cruise, MASK_OBD, 1000, 500, vary_cruise);
    scene("kirmizi_alert", mk_drive, ui_main_update, &hot, MASK_OBD, 2500, 1000, NULL);
    g_settings.theme = SET_THEME_DAY;
    ui_alert_theme();
    scene("alert_hot", mk_drive, ui_main_update, &hot, MASK_OBD, 2500, 1500, NULL);
    exit(0);
}
