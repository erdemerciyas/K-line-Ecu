/*
 * Sürüş sayfasının hareket katmanı. K-Line devri saniyede ~4, hızı ~2 kez getirir, ekran 60 fps çizer.
 * Her örnek cevabın geldiği anla damgalanıp bir alfa-beta filtresine girer (değer + değişim hızı); ekrana
 * o anki tahmin verilir, iki örnek arası boş kalmaz.
 */
#include <math.h>
#include "ui_priv.h"

// simülasyonda (±30 rpm gürültü, ~125 ms istek aralığı) ham örneğe göre devir hatası ~%20, hız hatası ~%55 az
#define AB_ALPHA        1.0f        // ölçüme güven: OBD değeri kesin, sadece eski
#define AB_BETA         0.85f       // değişim hızı neredeyse son iki örnekten
#define PREDICT_MAX_S   0.35f       // son örnekten en fazla bu kadar ileri tahmin (örnek yaşı + aralık)
#define SAMPLE_GAP_S    1.5f        // daha uzun ara: filtre baştan
#define RPM_RATE_MAX    15000.0f    // rpm/s
#define SPD_RATE_MAX    60.0f       // km/h/s
#define SPEED_SMOOTH_S  0.08f

typedef struct {
    int64_t t;
    float x;
    float v;
    bool ok;
} track_t;

static struct {
    track_t rpm;
    track_t spd;
    float speed_out;
    int64_t out_t;
} mo;

void ui_motion_reset(void)
{
    mo = (typeof(mo)){.speed_out = NAN};
}

static void track(track_t *k, int64_t t, float z, float rate_max)
{
    const float dt = (t - k->t) / 1e6f;
    if (!k->ok || dt > SAMPLE_GAP_S || dt < 0) {
        *k = (track_t){.t = t, .x = z, .ok = true};
        return;
    }
    if (dt == 0) {
        return;
    }
    const float r = z - (k->x + k->v * dt);
    k->x += k->v * dt + AB_ALPHA * r;
    k->v += AB_BETA * r / dt;
    k->v = fmaxf(-rate_max, fminf(rate_max, k->v));
    k->t = t;
}

static float track_at(const track_t *k, int64_t t)
{
    const float h = fmaxf(0, fminf(PREDICT_MAX_S, (t - k->t) / 1e6f));
    return fmaxf(0, k->x + k->v * h);
}

void ui_motion_feed(const vems_data_t *d)
{
    if (d->rpm_us && d->rpm_us != mo.rpm.t) {
        track(&mo.rpm, d->rpm_us, d->rpm, RPM_RATE_MAX);
    }
    if (d->speed_us && d->speed_us != mo.spd.t && !isnan(d->speed_kmh)) {
        track(&mo.spd, d->speed_us, d->speed_kmh, SPD_RATE_MAX);
    }
}

void ui_motion_get(int64_t now, ui_motion_t *out)
{
    out->rpm = mo.rpm.ok ? track_at(&mo.rpm, now) : NAN;
    const float target = mo.spd.ok ? track_at(&mo.spd, now) : NAN;
    const float dt = (now - mo.out_t) / 1e6f;
    mo.out_t = now;
    if (isnan(target) || isnan(mo.speed_out) || dt > 0.5f) {
        mo.speed_out = target;
    } else if (dt > 0) {
        mo.speed_out += (target - mo.speed_out) * (1 - expf(-dt / SPEED_SMOOTH_S));
    }
    out->speed = mo.speed_out;
}

// ---------------------------------------------------------------------------
// Vites ışığı
// ---------------------------------------------------------------------------
#define SHIFT_CLT_COLD  60          // °C: altında izin verilen devir düşer
#define SHIFT_COLD_MIN  3500        // 20 °C ve altı
#define SHIFT_COLD_STEP 250         // sınır bu adıma yuvarlanır (halka segmenti)

int ui_shift_limit(int shift_rpm, float clt)
{
    if (isnan(clt) || clt >= SHIFT_CLT_COLD) {
        return shift_rpm;
    }
    // 20 °C'de 3500, 60 °C'de vites noktası (doğrusal)
    const int lim = (int)(SHIFT_COLD_MIN + (clt - 20) * 62.5f) / SHIFT_COLD_STEP * SHIFT_COLD_STEP;
    return LV_MAX(SHIFT_COLD_MIN, LV_MIN(lim, shift_rpm));
}

bool ui_shift_update(ui_shift_t *s, float rpm, int limit, int64_t now)
{
    const bool on = !isnan(rpm) && rpm >= (s->on ? limit - UI_SHIFT_HYST : limit);
    if (on && !s->on) {
        s->t0 = now;
    }
    s->on = on;
    return on;
}

bool ui_shift_blink(const ui_shift_t *s, int64_t now, int64_t period_us)
{
    return s->on && ((now - s->t0) / period_us) % 2 == 0;
}

lv_color_t ui_shift_color(int stage)
{
    const settings_shift_col_t *p = &SETTINGS_SHIFT_COLS[g_settings.shift_col < SETTINGS_SHIFT_COL_COUNT ? g_settings.shift_col : 0];
    const uint32_t c = stage <= 0 ? p->lo : stage == 1 ? p->mid : stage == 2 ? p->hi : p->flash;
    return ui_col(c ? c : SETTINGS_ACCENTS[g_settings.accent < SETTINGS_ACCENT_COUNT ? g_settings.accent : 0].rgb);
}
