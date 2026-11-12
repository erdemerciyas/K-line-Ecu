#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board_lcd.h"
#include "settings.h"
#include "vems_proto.h"
#include "imu_math.h"
#include "imu.h"

static const char *TAG = "imu";

#define QMI_WHO_AM_I    0x00    // 0x05
#define QMI_CTRL1       0x02
#define QMI_CTRL2       0x03    // ivmeölçer ölçek + hız
#define QMI_CTRL3       0x04    // jiroskop ölçek + hız
#define QMI_CTRL5       0x06    // alçak geçiren süzgeçler
#define QMI_CTRL7       0x08    // sensör aç
#define QMI_AX_L        0x35    // ax ay az gx gy gz, 16 bit küçük uçlu
#define QMI_RESET       0x60

#define ACC_LSB_G       8192.0f     // ±4 g
#define GYRO_LSB_DPS    64.0f       // ±512 °/s
#define PERIOD_MS       20
#define SPEED_STALE_US  (1500 * 1000)
#define LOG_FOR_US      (60 * 1000 * 1000)  // açılıştan sonra 1 dk ham değer logu: montaj ekseni kontrolü

// IMU ekseninde ekranın içi = aracın ilerisi. Ölçüldü: ekran yukarı bakarken z = -0.96 g, yani +Z ekranın içine.
// Kart ekran düzleminde nasıl döndürülürse döndürülsün Z değişmez; yukarı yönü düz zeminde sıfırlanarak bulunur.
static const float SCR_IN[3] = {0, 0, 1};
static const float DEF_UP[3] = {0, 1, 0};      // sıfırlanmadan önce varsayım (sayfa "sıfırla" uyarısı gösterir)

static i2c_master_dev_handle_t s_dev;
static volatile bool s_ok;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static imu_incl_t s_out;
static float s_up[3];           // son filtre çıktısı: sıfırlamada kaydedilir

static esp_err_t reg_write(uint8_t reg, uint8_t v)
{
    const uint8_t b[2] = {reg, v};
    return i2c_master_transmit(s_dev, b, sizeof(b), 50);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static bool qmi_init(void)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) {
        return false;
    }
    static const uint8_t ADDRS[] = {0x6B, 0x6A};
    uint8_t addr = 0;
    for (size_t i = 0; i < sizeof(ADDRS) && !addr; i++) {
        if (i2c_master_probe(bus, ADDRS[i], 50) == ESP_OK) {
            addr = ADDRS[i];
        }
    }
    if (!addr) {
        ESP_LOGW(TAG, "QMI8658 bulunamadı (0x6B / 0x6A)");
        return false;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        return false;
    }
    uint8_t id = 0;
    if (reg_read(QMI_WHO_AM_I, &id, 1) != ESP_OK || id != 0x05) {
        ESP_LOGW(TAG, "0x%02X adresinde QMI8658 kimliği yok (0x%02X)", addr, id);
        return false;
    }
    reg_write(QMI_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));
    const bool ok = reg_write(QMI_CTRL1, 0x40) == ESP_OK &&     // adres otomatik artar, küçük uçlu
                    reg_write(QMI_CTRL2, 0x16) == ESP_OK &&     // ±4 g, ~112 Hz
                    reg_write(QMI_CTRL3, 0x56) == ESP_OK &&     // ±512 °/s, ~112 Hz
                    reg_write(QMI_CTRL5, 0x11) == ESP_OK &&     // iki süzgeç de açık
                    reg_write(QMI_CTRL7, 0x03) == ESP_OK;       // ivmeölçer + jiroskop
    ESP_LOGI(TAG, "QMI8658 0x%02X %s", addr, ok ? "hazır" : "ayarlanamadı");
    return ok;
}

static void imu_task(void *arg)
{
    incl_filter_t flt = {0};
    TickType_t wake = xTaskGetTickCount();
    float spd_last = NAN, accel = 0;
    int64_t spd_t = 0, log_t = 0;
    const int64_t t0 = esp_timer_get_time();
    vems_data_t d;
    int fails = 0, skip = 10;      // 200 ms

    while (1) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
        uint8_t b[12];
        if (reg_read(QMI_AX_L, b, sizeof(b)) != ESP_OK) {
            if (++fails > 25) {
                s_ok = false;
            }
            continue;
        }
        fails = 0;
        if (skip > 0) {
            skip--;         // sensör sıfırlandıktan sonraki ilk okumalar oturmamış
            continue;
        }
        float acc[3], gyro[3];
        for (int i = 0; i < 3; i++) {
            acc[i] = (int16_t)(b[2 * i] | b[2 * i + 1] << 8) / ACC_LSB_G;
            gyro[i] = (int16_t)(b[6 + 2 * i] | b[7 + 2 * i] << 8) / GYRO_LSB_DPS * (float)(M_PI / 180);
        }
        const float raw[3] = {acc[0], acc[1], acc[2]};

        // araç ekseni: ayarlardaki sıfır ve ekran açısı
        float up[3];
        bool zeroed = false;
        for (int i = 0; i < 3; i++) {
            up[i] = g_settings.incl_up[i] / 1000.0f;
            zeroed |= g_settings.incl_up[i] != 0;
        }
        incl_axes_t ax;
        const bool ax_ok = incl_axes(&ax, zeroed ? up : NULL, SCR_IN, DEF_UP);

        // aracın kendi ivmesi: OBD hızı (yeni örnekte türev, süzülmüş) ve jiroskopun sapma hızı
        const int64_t now = esp_timer_get_time();
        float speed = 0;
        if (vems_get_latest(&d) && !isnan(d.speed_kmh) && now - d.timestamp_us < SPEED_STALE_US) {
            speed = d.speed_kmh / 3.6f;
            if (d.speed_us && d.speed_us != spd_t) {
                if (spd_t && !isnan(spd_last)) {
                    const float dt = (d.speed_us - spd_t) / 1e6f;
                    if (dt > 0.05f && dt < 2) {
                        accel += 0.5f * ((speed - spd_last) / dt - accel);
                    }
                }
                spd_last = speed;
                spd_t = d.speed_us;
            }
        } else {
            accel = 0;
            spd_last = NAN;
        }
        if (ax_ok) {
            incl_compensate(acc, gyro, &ax, speed, accel);
        }
        incl_filter_step(&flt, acc, gyro, PERIOD_MS / 1000.0f);

        imu_incl_t o = {.ok = true, .zeroed = zeroed};
        memcpy(o.acc, raw, sizeof(raw));
        if (ax_ok && flt.init) {
            incl_angles(flt.g, &ax, &o.roll, &o.pitch);
        }
        taskENTER_CRITICAL(&s_mux);
        s_out = o;
        memcpy(s_up, flt.g, sizeof(s_up));
        taskEXIT_CRITICAL(&s_mux);
        s_ok = true;

        if (now - t0 < LOG_FOR_US && now - log_t > 2000000) {
            log_t = now;
            ESP_LOGI(TAG, "ivme %+.3f %+.3f %+.3f g · jiro kayması %+.1f %+.1f %+.1f °/s · yan %+.1f° burun %+.1f°%s",
                     raw[0], raw[1], raw[2], flt.bias[0] * 57.3f, flt.bias[1] * 57.3f, flt.bias[2] * 57.3f, o.roll,
                     o.pitch, zeroed ? "" : " (sıfırlanmadı)");
        }
    }
}

bool imu_start(void)
{
    if (!qmi_init()) {
        return false;
    }
    s_ok = true;
    return xTaskCreatePinnedToCore(imu_task, "imu", 3072, NULL, 2, NULL, 0) == pdPASS;
}

bool imu_present(void)
{
    return s_dev != NULL && s_ok;
}

void imu_get(imu_incl_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_out;
    taskEXIT_CRITICAL(&s_mux);
    out->ok = imu_present();
}

void imu_zero(void)
{
    float up[3];
    taskENTER_CRITICAL(&s_mux);
    memcpy(up, s_up, sizeof(up));
    taskEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < 3; i++) {
        g_settings.incl_up[i] = (int16_t)lroundf(up[i] * 1000);
    }
    settings_save();
    ESP_LOGI(TAG, "eğim sıfırlandı: yukarı %+.3f %+.3f %+.3f", up[0], up[1], up[2]);
}
