#pragma once
/*
 * Eğim hesabı (donanımdan bağımsız, PC testleri test/host/test_obd.c'de):
 * IMU ekseninde "yukarı" yönünü (durağanken ivmeölçerin gösterdiği 1 g) ivmeölçer + jiroskopla izler,
 * araç eksenine (sağ, yukarı, ileri) çevirip yan yatma ve burun eğimini derece olarak verir.
 * Kart araçta hangi açıyla durursa dursun: düz zeminde alınan sıfır "yukarı"yı, ekranın sağı (ekran açısı
 * ayarıyla döndürülmüş) aracın sağını tanımlar. Virajdaki merkezcil ivme (hız x sapma hızı) ve hız
 * değişimi ivmeölçerden çıkarılır: araç eğimi viraj ve frende sapmaz.
 */
#include <stdbool.h>

typedef struct {
    float x[3];         // aracın sağı, IMU ekseninde birim vektör
    float u[3];         // aracın yukarısı
    float f[3];         // aracın ilerisi
} incl_axes_t;

typedef struct {
    float g[3];         // izlenen yukarı yönü, birim vektör (IMU ekseni)
    float bias[3];      // jiroskop sıfır kayması, rad/s (durağanken öğrenilir)
    bool init;
} incl_filter_t;

/**
 * up_cal: düz zeminde kaydedilen yukarı (NULL ya da sıfır vektör = def_up varsayımı).
 * scr_in: IMU ekseninde ekranın içine doğru (ekran sürücüye baktığı için aracın ilerisi); kart ekran
 * düzleminde nasıl döndürülürse döndürülsün değişmez. Ekran yatay monte edildiyse (ileri yukarıya paralel)
 * ileri olarak def_up'ın yatay bileşeni alınır.
 */
bool incl_axes(incl_axes_t *ax, const float up_cal[3], const float scr_in[3], const float def_up[3]);

/** Aracın kendi ivmesini (viraj: hız x sapma hızı, ileri: hız değişimi) ivmeölçerden çıkarır. acc g, gyro rad/s. */
void incl_compensate(float acc[3], const float gyro[3], const incl_axes_t *ax, float speed_ms, float accel_ms2);

/** acc: g cinsinden, gyro: rad/s, IMU ekseninde. dt saniye. */
void incl_filter_step(incl_filter_t *f, const float acc[3], const float gyro[3], float dt);

/** roll > 0: sağ taraf aşağıda, pitch > 0: burun yukarıda (derece). */
void incl_angles(const float g[3], const incl_axes_t *ax, float *roll, float *pitch);
