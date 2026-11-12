#include <math.h>
#include <string.h>
#include "imu_math.h"

#define ACC_TAU_S       0.8f    // ivmeölçere güven: yavaş düzeltme, kısa ivmelenmeleri jiroskop taşır
#define ACC_DYN_G       0.25f   // telafiden sonra |a| 1 g'den bu kadar uzaksa (çukur, darbe) ivmeölçer yok sayılır
#define STILL_ACC_G     0.08f   // ivmeölçerin ölçek hatası (~%4) dahil durağan sayılır
#define STILL_GYRO      0.15f   // rad/s (~8.6°/s): durağan sayılır, jiroskop kayması öğrenilir. Kartta ölçülen sıfır
                                // kayması 3.3°/s; virajda sapma hızı öğrenilse de eğimi bozmaz (yukarı etrafında dönme)
#define BIAS_RATE       0.01f
#define G_MS2           9.80665f
#define DEG             57.29578f

static float dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool norm(float v[3])
{
    const float n = sqrtf(dot(v, v));
    if (n < 1e-6f) {
        return false;
    }
    v[0] /= n;
    v[1] /= n;
    v[2] /= n;
    return true;
}

/** v'nin u'ya dik bileşeni, birim; çok kısaysa false. */
static bool horiz(float out[3], const float v[3], const float u[3])
{
    const float vu = dot(v, u);
    for (int i = 0; i < 3; i++) {
        out[i] = v[i] - vu * u[i];
    }
    return dot(out, out) > 0.09f && norm(out);     // en az ~17° yatay bileşen
}

bool incl_axes(incl_axes_t *ax, const float up_cal[3], const float scr_in[3], const float def_up[3])
{
    float *u = ax->u, *x = ax->x, *f = ax->f;
    if (up_cal && dot(up_cal, up_cal) > 0.25f) {
        memcpy(u, up_cal, sizeof(ax->u));
    } else {
        memcpy(u, def_up, sizeof(ax->u));
    }
    if (!norm(u)) {
        return false;
    }
    // ileri: ekranın içi (sürücüye bakan ekranın arkası aracın önü); ekran yatay ise ekranın yukarısı
    if (!horiz(f, scr_in, u) && !horiz(f, def_up, u)) {
        return false;
    }
    // sağ = ileri x yukarı (sağ el kuralı: sağ x yukarı = geri)
    x[0] = f[1] * u[2] - f[2] * u[1];
    x[1] = f[2] * u[0] - f[0] * u[2];
    x[2] = f[0] * u[1] - f[1] * u[0];
    return true;
}

void incl_compensate(float acc[3], const float gyro[3], const incl_axes_t *ax, float speed_ms, float accel_ms2)
{
    // merkezcil ivme w x v = w_yukarı * v * (yukarı x ileri) = -w_yukarı * v * sağ; ivmeölçer a - g ölçer
    const float yaw = dot(gyro, ax->u);
    const float lat = -yaw * speed_ms / G_MS2, lon = accel_ms2 / G_MS2;
    for (int i = 0; i < 3; i++) {
        acc[i] -= lat * ax->x[i] + lon * ax->f[i];
    }
}

void incl_filter_step(incl_filter_t *f, const float acc[3], const float gyro[3], float dt)
{
    const float an = sqrtf(dot(acc, acc));
    if (an < 0.2f || an > 4.0f) {
        return;         // okuma bozuk
    }
    const float a[3] = {acc[0] / an, acc[1] / an, acc[2] / an};
    if (!f->init) {
        memcpy(f->g, a, sizeof(a));
        memset(f->bias, 0, sizeof(f->bias));
        f->init = true;
        return;
    }
    const float w[3] = {gyro[0] - f->bias[0], gyro[1] - f->bias[1], gyro[2] - f->bias[2]};
    if (fabsf(an - 1) < STILL_ACC_G && sqrtf(dot(w, w)) < STILL_GYRO) {
        for (int i = 0; i < 3; i++) {
            f->bias[i] += BIAS_RATE * (gyro[i] - f->bias[i]);
        }
    }
    // tahmin: dünyada sabit vektör gövde ekseninde dv/dt = -w x v
    float *g = f->g;
    const float p[3] = {
        g[0] - dt * (w[1] * g[2] - w[2] * g[1]),
        g[1] - dt * (w[2] * g[0] - w[0] * g[2]),
        g[2] - dt * (w[0] * g[1] - w[1] * g[0]),
    };
    // düzeltme: ivmeölçer, 1 g'ye yakınken
    const float dyn = fabsf(an - 1) / ACC_DYN_G;
    const float k = dyn >= 1 ? 0 : dt / ACC_TAU_S * (1 - dyn);
    for (int i = 0; i < 3; i++) {
        g[i] = p[i] + k * (a[i] - p[i]);
    }
    if (!norm(g)) {
        memcpy(g, a, sizeof(a));
    }
}

void incl_angles(const float g[3], const incl_axes_t *ax, float *roll, float *pitch)
{
    const float gx = dot(g, ax->x), gu = dot(g, ax->u), gf = dot(g, ax->f);
    *roll = atan2f(-gx, gu) * DEG;
    *pitch = atan2f(gf, sqrtf(gx * gx + gu * gu)) * DEG;
}
