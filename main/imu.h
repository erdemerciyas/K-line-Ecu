#pragma once
/*
 * Kart üstündeki QMI8658 (ivmeölçer + jiroskop, I2C 0x6B/0x6A) ile eğim ölçer.
 * 50 Hz görev: OBD hızıyla viraj / fren ivmesini çıkarır, imu_math.c filtresiyle yan yatma ve burun eğimi verir.
 */
#include <stdbool.h>

typedef struct {
    bool ok;            // sensör var ve okuyor
    bool zeroed;        // düz zeminde sıfırlanmış (ayarlarda kayıtlı)
    float roll;         // derece, + sağ taraf aşağıda
    float pitch;        // derece, + burun yukarıda
    float acc[3];       // ham ivme (g, IMU ekseni), montaj kontrolü için
} imu_incl_t;

/** board_lcd_init'ten sonra: sensörü bulur, görevi başlatır. Sensör yoksa false (eğim sayfası gizlenir). */
bool imu_start(void);
bool imu_present(void);
void imu_get(imu_incl_t *out);

/** Şu anki duruşu "düz" kabul eder ve ayarlara kaydeder (araç düz zeminde dururken). Ekran görevinden. */
void imu_zero(void);
