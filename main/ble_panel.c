/*
 * Telefon paneli (BLE): ekransız kartlar (YD-ESP32-23 / DevKitC-1) için NimBLE GATT sunucusu.
 * Android Chrome, GitHub Pages'teki panel.html sayfasından Web Bluetooth ile bağlanır.
 *
 * Servis 4b4c0001-..., iki karakteristik:
 *   STREAM (notify): satır sonu ile biten JSON mesajları, MTU'ya göre parçalanır.
 *                    {"t":"live",...} canlı veri, {"t":"log",...} açılış adımları, {"t":"dtc",...} kodlar.
 *   CMD (write):     "dtc" kodları oku, "clear" kodları sil, "rescan" yeniden ara,
 *                    "want <hex>" öne alınan kanallar (CH_BIT maskesi, 0 = varsayılan liste).
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "vems_proto.h"
#include "settings.h"
#include "srclog.h"
#include "ble_panel.h"

static const char *TAG = "panel";

// UUID'ler NimBLE'da ters bayt sırasıyla: 4b4c000X-0b5e-4e1a-9a7d-6f62642d6b6c
#define KL_UUID(x) BLE_UUID128_INIT(0x6c, 0x6b, 0x2d, 0x64, 0x62, 0x6f, 0x7d, 0x9a, \
                                    0x1a, 0x4e, 0x5e, 0x0b, (x), 0x00, 0x4c, 0x4b)
static const ble_uuid128_t SVC_UUID = KL_UUID(0x01);
static const ble_uuid128_t STREAM_UUID = KL_UUID(0x02);
static const ble_uuid128_t CMD_UUID = KL_UUID(0x03);

static uint16_t s_stream_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_sub;             // telefon bildirimlere abone
static volatile bool s_fresh;           // yeni abone: her şeyi baştan gönder
static uint8_t s_own_addr_type;
static QueueHandle_t s_cmdq;

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
typedef struct {
    char *buf;
    size_t len, cap;
} jw_t;

static void jw(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jw(jw_t *w, const char *fmt, ...)
{
    if (w->len >= w->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        w->len = (size_t)n < w->cap - w->len ? w->len + n : w->cap;
    }
}

// ECU vermiyorsa (kanal maskesinde yok ya da NAN) null
static void jnum(jw_t *w, const char *key, uint32_t mask, vems_ch_t ch, float v, int dec)
{
    if (!(mask & CH_BIT(ch)) || isnan(v)) {
        jw(w, ",\"%s\":null", key);
    } else {
        jw(w, ",\"%s\":%.*f", key, dec, v);
    }
}

static void jstr(jw_t *w, const char *s)
{
    jw(w, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            jw(w, "\\%c", *s);
        } else if ((unsigned char)*s >= 0x20) {
            jw(w, "%c", *s);
        }
    }
    jw(w, "\"");
}

static size_t build_live(char *buf, size_t cap)
{
    jw_t w = {buf, 0, cap};
    vems_data_t d;
    const bool have = vems_get_latest(&d);
    const uint32_t mask = have ? vems_get_channels() : 0;
    vems_link_info_t li;
    vems_get_link_info(&li);
    vems_stats_t st;
    vems_get_stats(&st);

    static const char *const STATE[] = {"stopped", "wait", "link", "ok"};
    jw(&w, "{\"t\":\"live\",\"state\":\"%s\",\"proto\":", STATE[vems_get_state()]);
    jstr(&w, li.protocol ? li.protocol : "");
    jw(&w, ",\"ecu\":");
    jstr(&w, li.ecu);
    jw(&w, ",\"ok\":%lu,\"to\":%lu", (unsigned long)st.frames_ok, (unsigned long)st.timeouts);
    if (have) {
        const float st_afr = SETTINGS_FUELS[g_settings.fuel].stoich;
        const bool lam = d.ego_raw < 250 && d.lambda > 0.60f && d.lambda < 1.60f;
        jw(&w, ",\"age\":%lld", (esp_timer_get_time() - d.timestamp_us) / 1000);
        jnum(&w, "rpm", mask, CH_RPM, d.rpm, 0);
        jnum(&w, "spd", mask, CH_SPEED, d.speed_kmh, 0);
        jnum(&w, "clt", mask, CH_CLT, d.clt_c, 0);
        jnum(&w, "iat", mask, CH_IAT, d.iat_c, 0);
        jnum(&w, "bat", mask, CH_BATT, d.batt_v, 1);
        jnum(&w, "map", mask, CH_MAP, d.map_kpa, 0);
        jnum(&w, "tps", mask, CH_TPS, d.tps_pct, 0);
        jnum(&w, "load", mask, CH_LOAD, d.load_pct, 0);
        jnum(&w, "adv", mask, CH_ADV, d.ign_adv_deg, 1);
        jnum(&w, "afr", mask, CH_AFR, lam ? d.lambda * st_afr : NAN, 1);
        jnum(&w, "o2", mask, CH_O2V, d.o2_v, 2);
        jnum(&w, "o2b", mask, CH_O2V2, d.o2b_v, 2);
        jnum(&w, "stft", mask, CH_STFT, d.ego_corr_pct - 100.0f, 1);
        jnum(&w, "ltft", mask, CH_LTFT, d.ltft_pct, 1);
        jnum(&w, "maf", mask, CH_MAF, d.maf_gs, 1);
        jnum(&w, "oil", mask, CH_OIL, d.oil_c, 0);
        jnum(&w, "fuel", mask, CH_FUEL, d.fuel_pct, 0);
        jnum(&w, "lh", mask, CH_FUEL_LH, d.fuel_lh, 1);
        jnum(&w, "l100", mask, CH_FUEL_L100, d.fuel_l100, 1);
        jw(&w, ",\"mil\":%s,\"codes\":%d", d.mil_on ? "true" : "false", d.mil_codes);
    }
    jw(&w, "}\n");
    return w.len;
}

static size_t build_log(char *buf, size_t cap)
{
    jw_t w = {buf, 0, cap};
    srclog_line_t lines[12];
    uint32_t seq;
    const int n = srclog_read(lines, 12, &seq);
    jw(&w, "{\"t\":\"log\",\"log\":[");
    for (int i = 0; i < n; i++) {
        jw(&w, "%s[", i ? "," : "");
        jstr(&w, lines[i].text);
        jw(&w, ",");
        jstr(&w, lines[i].result);
        jw(&w, ",%u]", lines[i].state);
    }
    jw(&w, "]}\n");
    return w.len;
}

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------

/** Mesajı MTU'ya göre parçalayıp bildirir. Tampon dolarsa kısa bekleyip tekrar dener. */
static bool send_msg(const char *p, size_t len)
{
    const uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_sub) {
        return false;
    }
    const size_t chunk = ble_att_mtu(conn) - 3;
    for (size_t off = 0; off < len;) {
        const size_t n = len - off < chunk ? len - off : chunk;
        struct os_mbuf *om = NULL;
        for (int tries = 0; !om && tries < 20; tries++) {
            om = ble_hs_mbuf_from_flat(p + off, n);
            if (!om) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        if (!om || ble_gatts_notify_custom(conn, s_stream_handle, om) != 0) {
            return false;
        }
        off += n;
    }
    return true;
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    char cmd[16] = {0};
    uint16_t len;
    if (ble_hs_mbuf_to_flat(ctxt->om, cmd, sizeof(cmd) - 1, &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    xQueueSend(s_cmdq, cmd, 0);     // uzun işler (DTC ~4 sn) panel görevinde
    return 0;
}

static const struct ble_gatt_svc_def SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = &STREAM_UUID.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_stream_handle},
            {.uuid = &CMD_UUID.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
            {0},
        },
    },
    {0},
};

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;     // tek bağlantı: bağlıyken reklam yok
    }
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    struct ble_hs_adv_fields rsp = {0};     // ad tarama cevabında (reklam paketi UUID ile dolu)
    rsp.name = (const uint8_t *)CONFIG_VEMS_BLE_NAME;
    rsp.name_len = strlen(CONFIG_VEMS_BLE_NAME);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params ap = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "reklam başlamadı: %d", rc);
    }
}

// Bağlantı tutamacını ilk gördüğümüz olayda kaydet: CONNECT olayı her yığın sürümünde uygulamaya ulaşmayabiliyor
static void conn_up(uint16_t conn)
{
    if (s_conn == conn) {
        return;
    }
    s_conn = conn;
    ESP_LOGI(TAG, "telefon bağlandı");
    // gecikme: 15-30 ms bağlantı aralığı iste (Android varsayılanı ~45 ms)
    struct ble_gap_upd_params up = {.itvl_min = 12, .itvl_max = 24, .latency = 0, .supervision_timeout = 400};
    ble_gap_update_params(conn, &up);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            conn_up(ev->connect.conn_handle);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_LINK_ESTAB:
        if (ev->link_estab.status == 0) {
            conn_up(ev->link_estab.conn_handle);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "telefon ayrıldı (%d)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_sub = false;
        vems_set_wanted_channels(0);    // telefon yok: varsayılan sorgu listesi
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "abonelik: tutamaç %d (akış %d) bildirim %d", ev->subscribe.attr_handle, s_stream_handle,
                 ev->subscribe.cur_notify);
        if (ev->subscribe.attr_handle == s_stream_handle) {
            conn_up(ev->subscribe.conn_handle);
            s_sub = ev->subscribe.cur_notify;
            s_fresh = s_sub;
        }
        break;
    case BLE_GAP_EVENT_MTU:
        conn_up(ev->mtu.conn_handle);
        ESP_LOGI(TAG, "MTU %d", ev->mtu.value);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            advertise();
        }
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// Panel görevi: yeni veri gelince canlı mesaj, açılış günlüğü değişince log, komutlar
// ---------------------------------------------------------------------------
static void panel_task(void *arg)
{
    static char buf[1024];
    uint32_t last_seq = 0, last_log = 0;
    int64_t last_live = 0;

    while (1) {
        char cmd[16];
        if (xQueueReceive(s_cmdq, cmd, pdMS_TO_TICKS(20)) == pdTRUE) {
            jw_t w = {buf, 0, sizeof(buf)};
            if (strcmp(cmd, "dtc") == 0) {
                uint16_t codes[VEMS_DTC_MAX];
                const int n = vems_dtc_read(codes, VEMS_DTC_MAX);
                jw(&w, "{\"t\":\"dtc\",\"n\":%d,\"codes\":[", n);
                for (int i = 0; i < n; i++) {
                    jw(&w, "%s\"%c%04X\"", i ? "," : "", "PCBU"[codes[i] >> 14], codes[i] & 0x3FFF);
                }
                jw(&w, "]}\n");
            } else if (strcmp(cmd, "clear") == 0) {
                jw(&w, "{\"t\":\"clear\",\"ok\":%s}\n", vems_dtc_clear() ? "true" : "false");
            } else if (strcmp(cmd, "rescan") == 0) {
                vems_rescan();
            } else if (strncmp(cmd, "want ", 5) == 0) {
                // telefonda öne alınan kutucukların kanalları (CH_BIT, onaltılık): K-Line sorgusu bunlara ağırlık verir
                vems_set_wanted_channels(strtoul(cmd + 5, NULL, 16));
            }
            send_msg(buf, w.len);
        }

        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_sub) {
            continue;
        }
        if (s_fresh) {
            s_fresh = false;
            last_seq = last_log = 0;
            last_live = 0;
        }

        uint32_t lseq;
        srclog_read(NULL, 0, &lseq);
        if (lseq != last_log && send_msg(buf, build_log(buf, sizeof(buf)))) {
            last_log = lseq;
        }

        // yeni ECU cevabında en fazla 10 Hz, veri yokken 1 sn'de bir (durum / yaş)
        vems_data_t d;
        const uint32_t seq = vems_get_latest(&d) ? d.seq : 0;
        const int64_t now = esp_timer_get_time();
        if ((seq != last_seq && now - last_live >= 100000) || now - last_live >= 1000000) {
            if (send_msg(buf, build_live(buf, sizeof(buf)))) {
                last_seq = seq;
                last_live = now;
            }
        }
    }
}

esp_err_t ble_panel_start(void)
{
    s_cmdq = xQueueCreate(4, 16);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(SVCS) == 0 ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ble_gatts_add_svcs(SVCS) == 0 ? ESP_OK : ESP_FAIL);
    ble_svc_gap_device_name_set(CONFIG_VEMS_BLE_NAME);
    nimble_port_freertos_init(host_task);
    xTaskCreatePinnedToCore(panel_task, "panel", 4096, NULL, 3, NULL, 0);

    ESP_LOGI(TAG, "telefon paneli: Bluetooth \"%s\"", CONFIG_VEMS_BLE_NAME);
    srclog_step("bluetooth %s", CONFIG_VEMS_BLE_NAME);
    srclog_done(SRCLOG_OK, "OK");
    return ESP_OK;
}
