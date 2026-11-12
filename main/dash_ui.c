/*
 * LVGL port + screen manager.
 *
 *   boot screen  ->  [ drive | single gauge | all data | AFR* | fault codes | settings ]
 *
 * The page set is built from the channels the connected source delivers (AFR only with a wide band
 * lambda) and rebuilt when that set or a layout setting
 * changes. Swipe left / right switches pages; taps belong to the page (single gauge: left / right
 * half = previous / next value), long press resets min / max (single gauge, AFR).
 * Sürüş sayfasına dokunmak büyük değeri devir <-> hız değiştirir. Uyarılar (ui_alert.c) her sayfanın üstündedir.
 *
 * Rendering is tear-free: LVGL draws straight into the hidden one of the two panel frame buffers and
 * flush swaps them at the end of the scanned-out frame. Direct mode: sadece değişen alanlar çizilir, takastan
 * sonra aynı alanlar diğer buffer'a kopyalanır (bir sonraki kare güncel zeminden devam eder).
 * Screen transitions slide / fade two snapshots (the frame on the glass and the target screen
 * rendered once) instead of re-rendering both live screens every frame.
 *
 * All LVGL calls run in the LVGL task; data is pulled from vems_get_latest() by an lv_timer. Yeni çerçeve
 * gelir gelmez (UI_POLL_MS) sayfa güncellenir; ekrandaki kanallar OBD'ye bildirilir, onların PID'leri öne alınır.
 */
#include <math.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "board_lcd.h"
#include "ui_priv.h"
#include "dash_ui.h"
#include "imu.h"

static const char *TAG = "ui";

#define LVGL_TICK_MS        2
#define UI_POLL_MS          20      // yeni veri kontrolü: ECU cevabından ekrana en fazla ~20 ms + 1 kare
#define UI_UPDATE_MS        100     // yeni veri yokken de bu aralıkla güncelle (bekleme yazıları, yanıp sönme)
#define DATA_STALE_MS       1500
#define SWITCH_DEBOUNCE_US  (400 * 1000)
#define SLIDE_MS            320
#define FADE_MS             450
#define PERF_LOG_US         (10 * 1000 * 1000)

typedef enum { PG_DRIVE, PG_FOCUS, PG_LIST, PG_AFR, PG_INCLINE, PG_DTC, PG_SETTINGS, PAGE_MAX } page_type_t;

typedef enum { TR_LEFT, TR_RIGHT, TR_FADE } transition_t;

static struct {
    lv_disp_draw_buf_t draw_buf;
    lv_disp_drv_t disp_drv;
    lv_indev_drv_t indev_drv;
    lv_obj_t *pages[PAGE_MAX];
    page_type_t type[PAGE_MAX];
    int count;
    int page;
    bool in_splash;
    int64_t last_switch;
    uint32_t layout_mask;       // channels the pages were built for (0 = typical set before the first connection)
    bool rebuild_req;
    bool digital;               // sürüş sayfası dijital (sayfalar kurulurken ayardan)
    bool rescan_req;
    bool dtc_req;
    vems_data_t d;             // latest frame, shared by the update of every page
    bool fresh;
} ui;

static struct {
    bool active;
    transition_t type;
    lv_obj_t *scr;          // temporary screen holding the two snapshots
    lv_obj_t *img_from;
    lv_obj_t *img_to;
    lv_obj_t *from;
    lv_obj_t *to;
    bool del_from;
} tr;

// ---------------------------------------------------------------------------
// LVGL port
// ---------------------------------------------------------------------------
/** Camdaki karede bu turda çizilen alanları diğer buffer'a kopyalar, o buffer bir kare geride kalmasın. */
static void sync_back_buffer(const lv_color_t *front)
{
    const lv_disp_t *disp = _lv_refr_get_disp_refreshing();
    lv_color_t *back = (lv_color_t *)(front == ui.draw_buf.buf1 ? ui.draw_buf.buf2 : ui.draw_buf.buf1);
    for (int i = 0; i < disp->inv_p; i++) {
        if (disp->inv_area_joined[i]) {
            continue;
        }
        const lv_area_t *a = &disp->inv_areas[i];
        const size_t row = lv_area_get_width(a) * sizeof(lv_color_t);
        for (lv_coord_t y = a->y1; y <= a->y2; y++) {
            const size_t off = (size_t)y * BOARD_LCD_H_RES + a->x1;
            memcpy(back + off, front + off, row);
        }
    }
}

/*
 * Ekran açısı (0..359°). 0 iken LVGL doğrudan panel buffer'larına çizer (yukarıdaki yol).
 * Açı varken LVGL PSRAM'deki tek bir "mantıksal" kareye çizer; flush, kirli alanların döndürülmüş
 * sınır kutusunu panelin gizli buffer'ına en yakın komşu örneklemeyle yazar, takas eder ve aynı kutuyu
 * diğer panel buffer'ına kopyalar. Dokunma noktası ters yönde döndürülür.
 */
#define ROT_TILE    32          // kutu karolar halinde işlenir: kaynak okumaları PSRAM önbelleğinde kalır
#define ROT_CX      (((BOARD_LCD_H_RES - 1) << 16) / 2)    // Q16 merkez (piksel merkezleri)
#define ROT_CY      (((BOARD_LCD_V_RES - 1) << 16) / 2)

static struct {
    uint16_t deg;
    int32_t cos16, sin16;       // Q16
    lv_color_t *render;         // açı varken LVGL'in çizdiği mantıksal kare
    lv_color_t *glass;          // camdaki panel buffer'ı
} rot = {.cos16 = 1 << 16};

/** Mantıksal alanın camdaki (döndürülmüş) sınır kutusu, ekrana kırpılmış. */
static void rot_bbox(const lv_area_t *a, lv_area_t *out)
{
    const int32_t xs[2] = {a->x1, a->x2 + 1}, ys[2] = {a->y1, a->y2 + 1};
    int32_t x1 = INT32_MAX, y1 = INT32_MAX, x2 = INT32_MIN, y2 = INT32_MIN;
    for (int i = 0; i < 4; i++) {
        const int64_t dx = ((int64_t)xs[i & 1] << 16) - ROT_CX - (1 << 15);
        const int64_t dy = ((int64_t)ys[i >> 1] << 16) - ROT_CY - (1 << 15);
        const int32_t x = (int32_t)((dx * rot.cos16 - dy * rot.sin16) >> 32) + (ROT_CX >> 16);
        const int32_t y = (int32_t)((dx * rot.sin16 + dy * rot.cos16) >> 32) + (ROT_CY >> 16);
        x1 = x < x1 ? x : x1;
        x2 = x > x2 ? x : x2;
        y1 = y < y1 ? y : y1;
        y2 = y > y2 ? y : y2;
    }
    // 2 px pay: köşe hesabındaki kesme ve en yakın komşu yuvarlaması
    out->x1 = x1 - 2 < 0 ? 0 : x1 - 2;
    out->y1 = y1 - 2 < 0 ? 0 : y1 - 2;
    out->x2 = x2 + 2 >= BOARD_LCD_H_RES ? BOARD_LCD_H_RES - 1 : x2 + 2;
    out->y2 = y2 + 2 >= BOARD_LCD_V_RES ? BOARD_LCD_V_RES - 1 : y2 + 2;
}

/** Cam koordinatındaki kutuyu mantıksal kareden örnekleyerek dst'ye yazar; kare dışı siyah. */
static void rot_blit(lv_color_t *dst, const lv_color_t *src, const lv_area_t *b)
{
    for (int32_t ty = b->y1; ty <= b->y2; ty += ROT_TILE) {
        const int32_t ty2 = LV_MIN(ty + ROT_TILE - 1, b->y2);
        for (int32_t tx = b->x1; tx <= b->x2; tx += ROT_TILE) {
            const int32_t tx2 = LV_MIN(tx + ROT_TILE - 1, b->x2);
            for (int32_t y = ty; y <= ty2; y++) {
                // ters dönüş: u = dx cos + dy sin, v = -dx sin + dy cos (+ merkez, + yarım piksel yuvarlama), Q16
                const int64_t dx = ((int64_t)tx << 16) - ROT_CX, dy = ((int64_t)y << 16) - ROT_CY;
                int32_t u = (int32_t)((dx * rot.cos16 + dy * rot.sin16) >> 16) + ROT_CX + (1 << 15);
                int32_t v = (int32_t)((dy * rot.cos16 - dx * rot.sin16) >> 16) + ROT_CY + (1 << 15);
                lv_color_t *d = dst + (size_t)y * BOARD_LCD_H_RES + tx;
                for (int32_t x = tx; x <= tx2; x++) {
                    const int32_t sx = u >> 16, sy = v >> 16;
                    if ((uint32_t)sx < BOARD_LCD_H_RES && (uint32_t)sy < BOARD_LCD_V_RES) {
                        *d = src[(size_t)sy * BOARD_LCD_H_RES + sx];
                    } else {
                        d->full = 0;
                    }
                    d++;
                    u += rot.cos16;
                    v -= rot.sin16;
                }
            }
        }
    }
}

static void flush_rotated(void)
{
    const lv_disp_t *disp = _lv_refr_get_disp_refreshing();
    lv_color_t *fb0 = board_lcd_frame_buffer(0), *fb1 = board_lcd_frame_buffer(1);
    lv_color_t *back = rot.glass == fb0 ? fb1 : fb0;
    lv_area_t b;
    for (int i = 0; i < disp->inv_p; i++) {
        if (!disp->inv_area_joined[i]) {
            rot_bbox(&disp->inv_areas[i], &b);
            rot_blit(back, rot.render, &b);
        }
    }
    board_lcd_present(back);
    lv_color_t *other = rot.glass;
    rot.glass = back;
    // diğer panel buffer'ı bir kare geride kalmasın: aynı kutular kopyalanır
    for (int i = 0; i < disp->inv_p; i++) {
        if (!disp->inv_area_joined[i]) {
            rot_bbox(&disp->inv_areas[i], &b);
            const size_t row = lv_area_get_width(&b) * sizeof(lv_color_t);
            for (lv_coord_t y = b.y1; y <= b.y2; y++) {
                const size_t off = (size_t)y * BOARD_LCD_H_RES + b.x1;
                memcpy(other + off, back + off, row);
            }
        }
    }
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    // direct mode: her kirli alan için çağrılır; karenin son alanında takas
    if (lv_disp_flush_is_last(drv)) {
        if (rot.deg) {
            flush_rotated();
        } else {
            board_lcd_present(color_map);       // color_map bir panel frame buffer'ı
            rot.glass = color_map;
            sync_back_buffer(color_map);
        }
    }
    lv_disp_flush_ready(drv);
}

void dash_ui_set_rotation(uint16_t deg)
{
    deg %= 360;
    if (deg && rot.render == NULL) {
        rot.render = heap_caps_malloc(BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(lv_color_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (rot.render == NULL) {
            ESP_LOGE(TAG, "döndürme buffer'ı ayrılamadı");
            return;
        }
    }
    const float r = deg * (float)M_PI / 180.0f;
    rot.cos16 = (int32_t)lroundf(cosf(r) * 65536.0f);
    rot.sin16 = (int32_t)lroundf(sinf(r) * 65536.0f);
    const bool was = rot.deg != 0;
    rot.deg = deg;
    if (deg && !was) {
        lv_disp_draw_buf_init(&ui.draw_buf, rot.render, NULL, BOARD_LCD_H_RES * BOARD_LCD_V_RES);
    } else if (!deg && was) {
        // LVGL camda olmayan panel buffer'ından başlar
        lv_color_t *fb0 = board_lcd_frame_buffer(0), *fb1 = board_lcd_frame_buffer(1);
        lv_disp_draw_buf_init(&ui.draw_buf, rot.glass == fb0 ? fb1 : fb0, rot.glass, BOARD_LCD_H_RES * BOARD_LCD_V_RES);
    }
    if (lv_disp_get_default()) {
        lv_obj_invalidate(lv_scr_act());        // tüm kare yeni açıyla yeniden çizilir
        lv_obj_invalidate(lv_layer_top());
    }
}

/** 10 sn'de bir çizim istatistiği: kare sayısı, ortalama çizim süresi ve yeniden çizilen piksel. */
static void monitor_cb(lv_disp_drv_t *drv, uint32_t time_ms, uint32_t px)
{
    static int64_t t0;
    static uint32_t frames, ms, pixels;
    frames++;
    ms += time_ms;
    pixels += px;
    const int64_t now = esp_timer_get_time();
    if (now - t0 >= PERF_LOG_US) {
        if (t0) {
            ESP_LOGI(TAG, "ekran: %.1f kare/sn, kare %lu ms, %lu px (%%%lu)", frames * 1e6f / (now - t0),
                     (unsigned long)(ms / frames), (unsigned long)(pixels / frames),
                     (unsigned long)(pixels / frames * 100 / (BOARD_LCD_H_RES * BOARD_LCD_V_RES)));
        }
        t0 = now;
        frames = ms = pixels = 0;
    }
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (board_touch_read(&x, &y)) {
        if (rot.deg) {
            // camdaki nokta mantıksal kareye ters dönüşle taşınır
            const int64_t dx = ((int64_t)x << 16) - ROT_CX, dy = ((int64_t)y << 16) - ROT_CY;
            const int32_t u = ((int32_t)((dx * rot.cos16 + dy * rot.sin16) >> 16) + ROT_CX + (1 << 15)) >> 16;
            const int32_t v = ((int32_t)((dy * rot.cos16 - dx * rot.sin16) >> 16) + ROT_CY + (1 << 15)) >> 16;
            x = LV_CLAMP(0, u, BOARD_LCD_H_RES - 1);
            y = LV_CLAMP(0, v, BOARD_LCD_V_RES - 1);
        }
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
}

static void lvgl_task(void *arg)
{
    while (1) {
        // frame pacing comes from flush (it blocks until the frame swap), so only yield briefly
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms < 2) {
            wait_ms = 2;
        } else if (wait_ms > 10) {
            wait_ms = 10;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

// ---------------------------------------------------------------------------
// Snapshot transitions
// ---------------------------------------------------------------------------
#define FRAME_BYTES     (BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(lv_color_t))

static lv_img_dsc_t *snapshot_alloc(void)
{
    lv_img_dsc_t *dsc = lv_mem_alloc(sizeof(lv_img_dsc_t));
    void *data = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dsc == NULL || data == NULL) {
        lv_mem_free(dsc);
        heap_caps_free(data);
        return NULL;
    }
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->header.w = BOARD_LCD_H_RES;
    dsc->header.h = BOARD_LCD_V_RES;
    dsc->data_size = FRAME_BYTES;
    dsc->data = data;
    return dsc;
}

static void snapshot_free(lv_img_dsc_t *dsc)
{
    if (dsc) {
        heap_caps_free((void *)dsc->data);
        lv_mem_free(dsc);
    }
}

/** Copies the frame currently on the glass (the buffer LVGL is not drawing into). */
static lv_img_dsc_t *snapshot_front_buffer(void)
{
    const lv_disp_draw_buf_t *db = &ui.draw_buf;
    // döndürülmüş modda tek (mantıksal) buffer var, camdaki kare odur
    const void *front = db->buf2 == NULL ? db->buf1 : (db->buf_act == db->buf1) ? db->buf2 : db->buf1;
    lv_img_dsc_t *dsc = snapshot_alloc();
    if (dsc) {
        memcpy((void *)dsc->data, front, FRAME_BYTES);
    }
    return dsc;
}

/** Renders a (not loaded) screen once into an image. */
static lv_img_dsc_t *snapshot_screen(lv_obj_t *scr)
{
    lv_img_dsc_t *dsc = snapshot_alloc();
    if (dsc && lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, dsc, (void *)dsc->data, FRAME_BYTES) != LV_RES_OK) {
        snapshot_free(dsc);
        dsc = NULL;
    }
    return dsc;
}

static void snapshot_img_deleted_cb(lv_event_t *e)
{
    snapshot_free(lv_event_get_user_data(e));
}

static lv_obj_t *snapshot_img_create(lv_obj_t *parent, lv_img_dsc_t *dsc)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, dsc);
    lv_obj_add_event_cb(img, snapshot_img_deleted_cb, LV_EVENT_DELETE, dsc);   // frees with the screen
    return img;
}

static void transition_anim_cb(void *var, int32_t v)
{
    switch (tr.type) {
    case TR_LEFT:       // content moves to the left, target comes in from the right
        lv_obj_set_x(tr.img_from, -v);
        lv_obj_set_x(tr.img_to, BOARD_LCD_H_RES - v);
        break;
    case TR_RIGHT:
        lv_obj_set_x(tr.img_from, v);
        lv_obj_set_x(tr.img_to, v - BOARD_LCD_H_RES);
        break;
    case TR_FADE:
        lv_obj_set_style_img_opa(tr.img_to, v, 0);
        break;
    }
}

static void transition_ready_cb(lv_anim_t *a)
{
    if (tr.del_from) {
        lv_obj_del(tr.from);
    }
    // screen loads are applied on the next LVGL tick; auto_del removes the snapshot screen
    // (and frees the snapshots) only once the target is really on the glass
    lv_scr_load_anim(tr.to, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
    tr.active = false;
}

static void transition_start(lv_obj_t *to, transition_t type, uint32_t time_ms, bool del_from)
{
    lv_obj_t *from = lv_scr_act();
    lv_img_dsc_t *snap_from = snapshot_front_buffer();
    lv_img_dsc_t *snap_to = snap_from ? snapshot_screen(to) : NULL;
    if (snap_to == NULL) {
        // out of PSRAM: fall back to LVGL's live screen animation
        snapshot_free(snap_from);
        static const lv_scr_load_anim_t FALLBACK[] = {LV_SCR_LOAD_ANIM_MOVE_LEFT, LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                                                      LV_SCR_LOAD_ANIM_FADE_ON};
        lv_scr_load_anim(to, FALLBACK[type], time_ms, 0, del_from);
        return;
    }

    tr = (typeof(tr)){.active = true, .type = type, .from = from, .to = to, .del_from = del_from};
    tr.scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(tr.scr);    // the snapshots cover every pixel, no background fill
    tr.img_from = snapshot_img_create(tr.scr, snap_from);
    tr.img_to = snapshot_img_create(tr.scr, snap_to);
    transition_anim_cb(NULL, 0);
    lv_scr_load(tr.scr);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tr.scr);
    lv_anim_set_exec_cb(&a, transition_anim_cb);
    lv_anim_set_values(&a, 0, type == TR_FADE ? LV_OPA_COVER : BOARD_LCD_H_RES);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, transition_ready_cb);
    lv_anim_start(&a);
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
static lv_obj_t *page_create(page_type_t t, int idx, int count)
{
    switch (t) {
    case PG_DRIVE:
        // Ayarlar > Gösterge tipi: analog (LED halka + ibre) ya da dijital
        return ui.digital ? ui_digital_create(idx, count, ui.layout_mask) : ui_main_create(idx, count, ui.layout_mask);
    case PG_INCLINE: return ui_incline_create(idx, count);
    case PG_FOCUS:  return ui_focus_create(idx, count, ui.layout_mask);
    case PG_LIST:   return ui_list_create(idx, count, ui.layout_mask);
    case PG_AFR:    return ui_afr_create(idx, count);
    case PG_DTC:    return ui_dtc_create(idx, count);
    default:       return ui_settings_create(idx, count);
    }
}

static void page_update(int idx)
{
    switch (ui.type[idx]) {
    case PG_DRIVE:
        if (ui.digital) {
            ui_digital_update(&ui.d, ui.fresh);
        } else {
            ui_main_update(&ui.d, ui.fresh);
        }
        break;
    case PG_FOCUS:  ui_focus_update(&ui.d, ui.fresh); break;
    case PG_LIST:   ui_list_update(&ui.d, ui.fresh); break;
    case PG_AFR:    ui_afr_update(&ui.d, ui.fresh); break;
    case PG_DTC:    ui_dtc_update(&ui.d, ui.fresh); break;
    case PG_SETTINGS: ui_settings_update(); break;
    default:        break;
    }
}

static void page_event_cb(lv_event_t *e);

/** (Re)creates every page for ui.layout_mask, keeps the same kind of page on the glass. */
static void pages_build(void)
{
    lv_obj_t *old[PAGE_MAX];
    const int old_count = ui.count;
    const page_type_t cur = old_count ? ui.type[ui.page] : PG_DRIVE;
    memcpy(old, ui.pages, sizeof(old));

    ui.digital = g_settings.gauge == SET_GAUGE_DIGITAL;
    page_type_t list[PAGE_MAX];
    int n = 0;
    list[n++] = PG_DRIVE;
    list[n++] = PG_FOCUS;
    list[n++] = PG_LIST;
    if (ui.layout_mask & CH_BIT(CH_AFR)) {
        list[n++] = PG_AFR;
    }
    if (g_settings.incl_page && imu_present()) {
        list[n++] = PG_INCLINE;
    }
    list[n++] = PG_DTC;
    list[n++] = PG_SETTINGS;

    ui.count = n;
    ui.page = 0;
    for (int i = 0; i < n; i++) {
        ui.type[i] = list[i];
        ui.pages[i] = page_create(list[i], i, n);
        lv_obj_add_event_cb(ui.pages[i], page_event_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(ui.pages[i], page_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
        lv_obj_add_event_cb(ui.pages[i], page_event_cb, LV_EVENT_LONG_PRESSED, NULL);
        if (list[i] == cur) {
            ui.page = i;
        }
    }
    if (!ui.in_splash && old_count) {
        page_update(ui.page);
        lv_scr_load(ui.pages[ui.page]);
    }
    for (int i = 0; i < old_count; i++) {
        lv_obj_del(old[i]);
    }
    ESP_LOGI(TAG, "%d pages for channel mask 0x%08lx", n, (unsigned long)ui.layout_mask);
}

/** A screen change is still in flight (snapshot transition or LVGL's deferred load). */
static bool ui_busy(void)
{
    return tr.active || lv_disp_get_default()->scr_to_load != NULL;
}

static void show_page(int page, transition_t type)
{
    int64_t now = esp_timer_get_time();
    page = (page + ui.count) % ui.count;
    if (ui_busy() || now - ui.last_switch < SWITCH_DEBOUNCE_US || page == ui.page) {
        return;
    }
    ui.last_switch = now;
    ui.page = page;
    page_update(page);      // the target snapshot shows live values
    transition_start(ui.pages[page], type, SLIDE_MS, false);
}

static void page_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (ui.in_splash || ui_busy()) {
        return;
    }
    const page_type_t t = ui.type[ui.page];
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT) {
            show_page(ui.page + 1, TR_LEFT);
        } else if (dir == LV_DIR_RIGHT) {
            show_page(ui.page - 1, TR_RIGHT);
        }
        lv_indev_wait_release(lv_indev_get_act());
    } else if (code == LV_EVENT_SHORT_CLICKED && t == PG_DRIVE) {
        if (ui.digital) {
            ui_digital_toggle_view();
        } else {
            ui_main_toggle_view();
        }
    } else if (code == LV_EVENT_SHORT_CLICKED && t == PG_FOCUS) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_get_act(), &p);
        ui_focus_step(p.x < BOARD_LCD_H_RES / 2 ? -1 : 1);
        page_update(ui.page);
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (t == PG_FOCUS) {
            ui_focus_reset();
        } else if (t == PG_AFR) {
            ui_afr_reset_minmax();
        } else if (t == PG_INCLINE) {
            ui_incline_zero();      // araç düz zeminde: şu anki duruş sıfır olur
        }
        ui.last_switch = esp_timer_get_time();
    }
}

void ui_request_rebuild(void)
{
    ui.rebuild_req = true;
}

void ui_request_rescan(void)
{
    ui.rescan_req = true;
}

void ui_request_dtc_page(void)
{
    ui.dtc_req = true;
}

/** O an ekrandaki sayfanın gösterdiği kanallar, 0 = hepsi (varsayılan sorgu sırası). */
static uint32_t page_channels(void)
{
    if (ui.in_splash || ui_busy()) {
        return 0;
    }
    // hararet ekranda değilse obd.c onu kendi bekçi sırasında okur (~2 sn): uyarı gecikmez, istekler ekrandaki veriye
    switch (ui.type[ui.page]) {
    case PG_DRIVE:  return ui.digital ? ui_digital_channels() : ui_main_channels() | CH_BIT(CH_CLT);
    case PG_INCLINE: return CH_BIT(CH_SPEED);    // eğim: viraj / fren telafisi hızla yapılır
    case PG_FOCUS:  return ui_focus_channels();
    case PG_LIST:   return ui_list_channels();
    case PG_AFR:    return CH_BIT(CH_AFR) | CH_BIT(CH_TARGET) | CH_BIT(CH_STFT);
    default:        return 0;
    }
}

static void ui_update_cb(lv_timer_t *timer)
{
    static uint32_t last_seq;
    static int64_t last_us;
    const int64_t now = esp_timer_get_time();
    const bool have = vems_get_latest(&ui.d);
    // yeni çerçeve gelir gelmez çiz; yoksa UI_UPDATE_MS'de bir (açılış ekranı sadece bu hızda)
    const bool new_data = have && ui.d.seq != last_seq && !ui.in_splash;
    if (!new_data && now - last_us < UI_UPDATE_MS * 1000 && !ui.rescan_req && !ui.rebuild_req) {
        return;
    }
    last_seq = have ? ui.d.seq : 0;
    last_us = now;
    vems_set_wanted_channels(page_channels());
    if (!have) {
        memset(&ui.d, 0, sizeof(ui.d));
    }
    ui.fresh = have && (now - ui.d.timestamp_us) < (int64_t)DATA_STALE_MS * 1000;
    const uint32_t mask = vems_get_channels();
    if (ui.fresh) {
        ui_ch_track(&ui.d, mask);
    }
    if (!ui.in_splash) {
        ui_alert_update(&ui.d, ui.fresh, mask);
    }

    if (ui.rescan_req && !ui_busy()) {
        ui.rescan_req = false;
        vems_rescan();
        if (!ui.in_splash) {
            ui.in_splash = true;
            transition_start(ui_splash_create(), TR_FADE, FADE_MS, false);   // pages stay
        }
        return;
    }

    if (ui.in_splash) {
        if (ui_splash_update() && !ui_busy()) {
            if (mask && mask != ui.layout_mask) {
                ui.layout_mask = mask;
                pages_build();          // not on the glass yet
            }
            ui.in_splash = false;
            ui.page = 0;
            ui.last_switch = esp_timer_get_time();
            page_update(ui.page);
            transition_start(ui.pages[ui.page], TR_FADE, FADE_MS, true);  // frees the splash
        }
        return;
    }

    if (!ui_busy() && ((mask && mask != ui.layout_mask) || ui.rebuild_req)) {
        if (mask) {
            ui.layout_mask = mask;
        }
        ui.rebuild_req = false;
        pages_build();
        return;
    }
    if (ui.dtc_req && !ui_busy()) {
        ui.dtc_req = false;
        for (int i = 0; i < ui.count; i++) {
            if (ui.type[i] == PG_DTC) {
                show_page(i, TR_FADE);
            }
        }
    }
    page_update(ui.page);
}

// ---------------------------------------------------------------------------
esp_err_t dash_ui_start(esp_lcd_panel_handle_t panel)
{
    lv_init();

    // draw straight into the panel frame buffers; fb0 is on the glass after init, so start with fb1
    void *fb_front = board_lcd_frame_buffer(0);
    void *fb_back = board_lcd_frame_buffer(1);
    ESP_RETURN_ON_FALSE(fb_front && fb_back, ESP_ERR_INVALID_STATE, TAG, "frame buffers");
    lv_disp_draw_buf_init(&ui.draw_buf, fb_back, fb_front, BOARD_LCD_H_RES * BOARD_LCD_V_RES);

    lv_disp_drv_init(&ui.disp_drv);
    ui.disp_drv.hor_res = BOARD_LCD_H_RES;
    ui.disp_drv.ver_res = BOARD_LCD_V_RES;
    ui.disp_drv.flush_cb = flush_cb;
    ui.disp_drv.draw_buf = &ui.draw_buf;
    ui.disp_drv.direct_mode = 1;
    ui.disp_drv.monitor_cb = monitor_cb;
    ui.disp_drv.user_data = panel;
    lv_disp_t *disp = lv_disp_drv_register(&ui.disp_drv);
    lv_disp_set_bg_opa(disp, LV_OPA_TRANSP);    // every screen paints its own background
    rot.glass = fb_front;
    dash_ui_set_rotation(g_settings.rotation);

    lv_indev_drv_init(&ui.indev_drv);
    ui.indev_drv.type = LV_INDEV_TYPE_POINTER;
    ui.indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&ui.indev_drv);

    const esp_timer_create_args_t tick_args = {.callback = tick_cb, .name = "lv_tick"};
    esp_timer_handle_t tick;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_MS * 1000), TAG, "tick start");

    // pages are built up-front (typical layout) so switching is instant; rebuilt once the ECU is known
    ui.in_splash = true;
    ui.layout_mask = 0;
    pages_build();
    lv_scr_load(ui_splash_create());
    ui_alert_init();
    lv_timer_create(ui_update_cb, UI_POLL_MS, NULL);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);
    return ESP_OK;
}
