/*
 * K-Line simülatörü ve ESP-IDF / FreeRTOS taklitleri (bkz. sim.h).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "hal/gpio_ll.h"
#include "srclog.h"
#include "obd_pid.h"
#include "sim.h"

// ---------------------------------------------------------------------------
// libc ara katmanı (msvcrt.dll): C99 snprintf davranışı, MSVC hedefinin kayan nokta işareti
// ---------------------------------------------------------------------------
int _fltused = 1;
int _vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    if (n == 0) {
        return 0;
    }
    int r = _vsnprintf(buf, n, fmt, ap);
    if (r < 0 || (size_t)r >= n) {
        buf[n - 1] = 0;
        r = (int)n - 1;
    }
    return r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

// ---------------------------------------------------------------------------
// Saat, olaylar, görev
// ---------------------------------------------------------------------------
static int64_t s_now;
static struct { int64_t t; void (*fn)(uint32_t); uint32_t arg; bool done; } s_ev[16];
static int s_nev;
static void (*s_task)(void *);
static bool s_running;
static int64_t s_stop;

// obd_task bir Windows fiber'ında koşar: süre dolunca test fiber'ına döner, sonraki sim_run_task kaldığı yerden sürdürür
// (fiber tam bağlamı, Win64'ün korunan XMM yazmaçları dahil, saklar; setjmp/longjmp saklamaz)
void *__stdcall ConvertThreadToFiber(void *param);
void *__stdcall CreateFiberEx(size_t commit, size_t reserve, uint32_t flags, void(__stdcall *fn)(void *), void *param);
void __stdcall SwitchToFiber(void *fiber);
void __stdcall DeleteFiber(void *fiber);
static void *s_main_fiber, *s_task_fiber;

static void __stdcall task_fiber(void *param)
{
    (void)param;
    for (;;) {
        s_task(NULL);
    }
}

sim_ecu_t g_ecu;
sim_pid_stat_t g_pid_stat[SIM_MAX_PID];
uint32_t g_sid_seen[256];
int64_t g_max_req_gap_us;
uint32_t g_requests;

int64_t sim_now(void)
{
    return s_now;
}

int64_t esp_timer_get_time(void)
{
    return s_now;
}

void sim_at(int64_t t_us, void (*fn)(uint32_t), uint32_t arg)
{
    if (s_nev < (int)(sizeof(s_ev) / sizeof(s_ev[0]))) {
        s_ev[s_nev].t = t_us;
        s_ev[s_nev].fn = fn;
        s_ev[s_nev].arg = arg;
        s_ev[s_nev].done = false;
        s_nev++;
    }
}

void sim_advance(int64_t us)
{
    const int64_t target = s_now + (us > 0 ? us : 0);
    // olay zamanlarında durarak ilerle: sayfa değişimi tam istenen anda olur
    for (;;) {
        int next = -1;
        for (int i = 0; i < s_nev; i++) {
            if (!s_ev[i].done && s_ev[i].t <= target && (next < 0 || s_ev[i].t < s_ev[next].t)) {
                next = i;
            }
        }
        if (next < 0) {
            break;
        }
        if (s_ev[next].t > s_now) {
            s_now = s_ev[next].t;
        }
        s_ev[next].done = true;
        s_ev[next].fn(s_ev[next].arg);
    }
    s_now = target;
    if (s_running && s_now >= s_stop) {
        s_running = false;
        SwitchToFiber(s_main_fiber);    // teste dön; sonraki sim_run_task buradan sürdürür
    }
}

void vTaskDelay(TickType_t ticks)
{
    sim_advance((int64_t)ticks * 1000);
}

void vTaskDelayUntil(TickType_t *prev, TickType_t inc)
{
    *prev += inc;
    const int64_t t = (int64_t)*prev * 1000;
    if (t > s_now) {
        sim_advance(t - s_now);
    }
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(s_now / 1000);
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio,
                                   TaskHandle_t *out, int core)
{
    (void)name, (void)stack, (void)arg, (void)prio, (void)out, (void)core;
    s_task = fn;
    return pdPASS;
}

void sim_run_task(int64_t t_us)
{
    if (s_task == NULL) {
        return;
    }
    if (s_main_fiber == NULL) {
        s_main_fiber = ConvertThreadToFiber(NULL);
    }
    if (s_task_fiber == NULL) {
        s_task_fiber = CreateFiberEx(1 << 20, 1 << 20, 0, task_fiber, NULL);
    }
    s_stop = s_now + t_us;
    s_running = true;
    SwitchToFiber(s_task_fiber);
    s_running = false;
}

/** Görevi baştan başlatır (sonraki sim_run_task yeni bir obd_task çağrısıyla başlar). */
static void task_restart(void)
{
    if (s_task_fiber) {
        DeleteFiber(s_task_fiber);
        s_task_fiber = NULL;
    }
}

// semaforlar: muteks hep alınır, ikili semaforun sayacı var
static struct { bool binary; int count; } s_sem[8];
static int s_nsem;

static SemaphoreHandle_t sem_new(bool binary)
{
    if (s_nsem >= 8) {
        return NULL;
    }
    s_sem[s_nsem].binary = binary;
    s_sem[s_nsem].count = 0;
    return &s_sem[s_nsem++];
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return sem_new(false);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return sem_new(true);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t wait)
{
    (void)wait;
    struct { bool binary; int count; } *s = h;
    if (!s->binary) {
        return pdTRUE;
    }
    if (s->count > 0) {
        s->count--;
        return pdTRUE;
    }
    return pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t h)
{
    struct { bool binary; int count; } *s = h;
    if (s->binary) {
        s->count = 1;
    }
    return pdTRUE;
}

// ---------------------------------------------------------------------------
// Log ve açılış günlüğü
// ---------------------------------------------------------------------------
#define LOG_LINES 512
static char s_log[LOG_LINES][160];
static int s_nlog;
static char s_srclog_done[64];

void sim_log(char level, const char *tag, const char *fmt, ...)
{
    char *line = s_log[s_nlog++ % LOG_LINES];
    int k = snprintf(line, 160, "%c %s: ", level, tag);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + k, 160 - k, fmt, ap);
    va_end(ap);
#ifdef SIM_VERBOSE
    printf("[%9.3f] %s\n", s_now / 1e6, line);
#endif
}

bool sim_log_contains(const char *s)
{
    const int n = s_nlog < LOG_LINES ? s_nlog : LOG_LINES;
    for (int i = 0; i < n; i++) {
        if (strstr(s_log[i], s)) {
            return true;
        }
    }
    return false;
}

void srclog_clear(void)
{
    s_srclog_done[0] = 0;
}

void srclog_step(const char *fmt, ...)
{
    (void)fmt;
}

void srclog_done(srclog_state_t state, const char *fmt, ...)
{
    (void)state;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_srclog_done, sizeof(s_srclog_done), fmt, ap);
    va_end(ap);
}

const char *sim_last_srclog(void)
{
    return s_srclog_done;
}

// ---------------------------------------------------------------------------
// K hattı: TX baytı yankılanır (L9637D), ECU dinler; RX kuyruğu zamana göre sıralı
// ---------------------------------------------------------------------------
gpio_dev_t GPIO;

typedef struct { uint8_t b; int64_t t; } rxb_t;
static rxb_t s_rx[4096];
static int s_nrx;
static int64_t s_line_free;
static bool s_line_low;
static int64_t s_low_t;

static void rx_push(uint8_t b, int64_t t)
{
    if (!g_ecu.rx_ok || s_nrx >= (int)(sizeof(s_rx) / sizeof(s_rx[0]))) {
        return;
    }
    int i = s_nrx++;
    while (i > 0 && s_rx[i - 1].t > t) {
        s_rx[i] = s_rx[i - 1];
        i--;
    }
    s_rx[i].b = b;
    s_rx[i].t = t;
}

static void rx_pop(int n)
{
    memmove(s_rx, s_rx + n, (size_t)(s_nrx - n) * sizeof(s_rx[0]));
    s_nrx -= n;
}

// ---- ECU ----
static uint8_t s_req[24];
static int s_nreq;
static int64_t s_req_last_t;
static bool s_woke, s_session;
static int64_t s_last_req_t;

static bool ecu_supported(uint8_t pid)
{
    if (pid == 0) {
        return true;
    }
    if (pid > 0x60) {
        return false;
    }
    return (g_ecu.sup[(pid - 1) / 32] >> (31 - (pid - 1) % 32)) & 1;
}

/** ECU cevap çerçevesi; bitiş zamanını döner. Baytlar UART zaman aşımıyla (2 sembol) teslim edilir. */
static int64_t ecu_send(uint8_t src, const uint8_t *p, int n, int64_t t0)
{
    uint8_t f[40];
    int k = 0;
    f[k++] = 0x80 | n;
    f[k++] = 0xF1;
    f[k++] = src;
    memcpy(&f[k], p, n);
    k += n;
    uint8_t cs = 0;
    for (int i = 0; i < k; i++) {
        cs += f[i];
    }
    f[k++] = cs;
    const int64_t end = t0 + (int64_t)k * (SIM_BYTE_US + 500);   // P1: baytlar arası ~0.5 ms
    for (int i = 0; i < k; i++) {
        rx_push(f[i], end + 2 * SIM_BYTE_US);
    }
    return end;
}

static void pid_stat(uint8_t pid, int64_t t)
{
    sim_pid_stat_t *s = &g_pid_stat[pid];
    if (s->count) {
        const int64_t gap = t - s->last_us;
        if (gap > s->max_gap_us) {
            s->max_gap_us = gap;
        }
    } else {
        s->first_us = t;
    }
    s->last_us = t;
    s->count++;
}

static void ecu_handle(const uint8_t *d, int n, int64_t t)
{
    const uint8_t sid = d[0];
    g_sid_seen[sid]++;
    if (!g_ecu.on) {
        return;
    }
    if (s_session && t - s_last_req_t > 5000000) {
        s_session = false;          // P3max aşıldı: oturum kapandı, yeniden init gerekir
    }
    if (sid == 0x81) {
        if (s_woke) {
            s_woke = false;
            s_session = true;
            s_last_req_t = t;
            static const uint8_t ok[] = {0xC1, 0xEF, 0x8F};
            ecu_send(g_ecu.addr, ok, sizeof(ok), t + g_ecu.p2_us);
        }
        return;
    }
    if (!s_session) {
        return;
    }
    if (s_last_req_t && t - s_last_req_t > g_max_req_gap_us) {
        g_max_req_gap_us = t - s_last_req_t;
    }
    s_last_req_t = t;
    g_requests++;
    uint8_t r[16];
    if (sid == 0x01 && n >= 2) {
        const uint8_t pid = d[1];
        if (pid >= SIM_MAX_PID || !ecu_supported(pid) || g_ecu.mute[pid]) {
            return;
        }
        const obd_pid_def_t *def = obd_pid_def(pid);
        const int len = pid % 0x20 == 0 ? 4 : def ? def->len : 1;
        r[0] = 0x41;
        r[1] = pid;
        if (pid % 0x20 == 0) {      // destek haritası
            const uint32_t m = g_ecu.sup[pid / 0x20];
            r[2] = m >> 24;
            r[3] = m >> 16;
            r[4] = m >> 8;
            r[5] = m;
        } else {
            memcpy(&r[2], g_ecu.val[pid], len);
        }
        const int64_t end = ecu_send(g_ecu.addr, r, 2 + len, t + g_ecu.p2_us);
        pid_stat(pid, end);
        if (g_ecu.second_addr) {
            for (int i = 0; i < len; i++) {
                r[2 + i] ^= 0xFF;   // ikinci ECU farklı değer verir: kilitli ECU dışındaki kabul edilmemeli
            }
            ecu_send(g_ecu.second_addr, r, 2 + len, end + 25000);
        }
    } else if (sid == 0x03) {
        int64_t t0 = t + g_ecu.p2_us;
        if (g_ecu.dtc_pending_us) {
            static const uint8_t wait[] = {0x7F, 0x03, 0x78};
            ecu_send(g_ecu.addr, wait, sizeof(wait), t0);
            t0 += g_ecu.dtc_pending_us;
        }
        int i = 0;
        do {        // her çerçeve 3 kod, eksikler 0000
            r[0] = 0x43;
            for (int k = 0; k < 3; k++, i++) {
                const uint16_t c = i < g_ecu.ndtc ? g_ecu.dtc[i] : 0;
                r[1 + 2 * k] = c >> 8;
                r[2 + 2 * k] = c & 0xFF;
            }
            t0 = ecu_send(g_ecu.addr, r, 7, t0) + 1000;
        } while (i < g_ecu.ndtc);
    } else if (sid == 0x04) {
        g_ecu.ndtc = 0;
        r[0] = 0x44;
        ecu_send(g_ecu.addr, r, 1, t + g_ecu.p2_us);
    }
}

static void ecu_rx_byte(uint8_t b, int64_t t)
{
    if (s_nreq && t - s_req_last_t > 30000) {
        s_nreq = 0;     // çerçeve arası boşluk: yeni istek
    }
    s_req_last_t = t;
    if (s_nreq >= (int)sizeof(s_req)) {
        s_nreq = 0;
    }
    s_req[s_nreq++] = b;
    const uint8_t fmt = s_req[0];
    if ((fmt & 0x80) == 0) {
        s_nreq = 0;     // ISO 9141 başlığı bu ECU'da yok
        return;
    }
    const int len = fmt & 0x3F, total = 3 + len + 1;
    if (len == 0 || s_nreq < total) {
        return;
    }
    uint8_t cs = 0;
    for (int i = 0; i < total - 1; i++) {
        cs += s_req[i];
    }
    if (cs == s_req[total - 1]) {
        ecu_handle(&s_req[3], len, t);
    }
    s_nreq = 0;
}

// ---- UART sürücüsü ----
esp_err_t uart_driver_install(uart_port_t p, int rx, int tx, int qsize, QueueHandle_t *q, int flags)
{
    (void)p, (void)rx, (void)tx, (void)qsize, (void)q, (void)flags;
    return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t p, const uart_config_t *cfg)
{
    (void)p, (void)cfg;
    return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t p, int tx, int rx, int rts, int cts)
{
    (void)p, (void)tx, (void)rx, (void)rts, (void)cts;
    return ESP_OK;
}

esp_err_t uart_set_rx_timeout(uart_port_t p, int symbols)
{
    (void)p, (void)symbols;
    return ESP_OK;
}

esp_err_t uart_set_line_inverse(uart_port_t p, uint32_t mask)
{
    (void)p;
    const bool low = mask & UART_SIGNAL_TXD_INV;
    if (low && !s_line_low) {
        s_low_t = s_now;
    } else if (!low && s_line_low) {
        const int64_t dur = s_now - s_low_t;
        if (dur >= 20000 && dur <= 30000) {
            s_woke = true;      // TiniL 25 ms: fast init uyandırması
        }
    }
    s_line_low = low;
    return ESP_OK;
}

esp_err_t uart_flush_input(uart_port_t p)
{
    (void)p;
    int n = 0;
    while (n < s_nrx && s_rx[n].t <= s_now) {
        n++;
    }
    rx_pop(n);
    return ESP_OK;
}

esp_err_t uart_wait_tx_done(uart_port_t p, TickType_t wait)
{
    (void)p, (void)wait;
    if (s_line_free > s_now) {
        sim_advance(s_line_free - s_now);
    }
    return ESP_OK;
}

int uart_write_bytes(uart_port_t p, const void *src, size_t n)
{
    (void)p;
    const uint8_t *b = src;
    for (size_t i = 0; i < n; i++) {
        const int64_t start = s_line_free > s_now ? s_line_free : s_now;
        const int64_t end = start + SIM_BYTE_US;
        s_line_free = end;
        rx_push(b[i], end + 2 * SIM_BYTE_US);   // yankı
        ecu_rx_byte(b[i], end);
    }
    return (int)n;
}

int uart_read_bytes(uart_port_t p, void *buf, uint32_t n, TickType_t wait)
{
    (void)p;
    uint8_t *out = buf;
    const int64_t deadline = wait == portMAX_DELAY ? INT64_MAX : s_now + (int64_t)wait * 1000;
    uint32_t i = 0;
    while (i < n) {
        if (s_nrx > 0 && s_rx[0].t <= deadline) {
            if (s_rx[0].t > s_now) {
                sim_advance(s_rx[0].t - s_now);
            }
            out[i++] = s_rx[0].b;
            rx_pop(1);
        } else {
            if (deadline > s_now && deadline != INT64_MAX) {
                sim_advance(deadline - s_now);
            }
            break;
        }
    }
    return (int)i;
}

int gpio_get_level(int gpio)
{
    if (gpio == CONFIG_VEMS_OBD_KLINE_TX_GPIO) {
        return s_line_low ? 0 : 1;
    }
    if (!g_ecu.rx_ok) {
        return 1;               // K -> RX yolu kopuk: RX hep yüksek
    }
    return s_line_low ? 0 : 1;
}

int gpio_set_direction(int gpio, gpio_mode_t mode)
{
    (void)gpio, (void)mode;
    return 0;
}

// ---------------------------------------------------------------------------
// Kurulum
// ---------------------------------------------------------------------------
void sim_stats_reset(void)
{
    memset(g_pid_stat, 0, sizeof(g_pid_stat));
    memset(g_sid_seen, 0, sizeof(g_sid_seen));
    g_max_req_gap_us = 0;
    g_requests = 0;
}

const sim_pid_stat_t *sim_pid(uint8_t pid)
{
    return &g_pid_stat[pid];
}

static void set_val(uint8_t pid, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    g_ecu.val[pid][0] = a;
    g_ecu.val[pid][1] = b;
    g_ecu.val[pid][2] = c;
    g_ecu.val[pid][3] = d;
}

void sim_reset(void)
{
    task_restart();
    s_now = 0;
    s_nev = 0;
    s_nrx = 0;
    s_line_free = 0;
    s_line_low = false;
    s_nreq = 0;
    s_woke = s_session = false;
    s_last_req_t = 0;
    s_nlog = 0;
    s_srclog_done[0] = 0;
    sim_stats_reset();
    memset(&g_ecu, 0, sizeof(g_ecu));
    // Chevrolet Kalos 1.4 16V (ELM327 ile okunan profil): 01 00 = BE 3E B8 11, MAF yok, dar bant O2
    g_ecu.on = true;
    g_ecu.rx_ok = true;
    g_ecu.addr = 0x10;
    g_ecu.sup[0] = 0xBE3EB811;
    g_ecu.p2_us = 25000;
    set_val(0x01, 0x00, 0x07, 0x65, 0x00);     // MIL kapalı, 0 kod
    set_val(0x03, 0x02, 0x00, 0, 0);           // kapalı çevrim
    set_val(0x04, 0x66, 0, 0, 0);              // %40 yük
    set_val(0x05, 0x7B, 0, 0, 0);              // 83 °C
    set_val(0x06, 0x80, 0, 0, 0);              // STFT 0
    set_val(0x07, 0x84, 0, 0, 0);              // LTFT +3.1
    set_val(0x0B, 0x23, 0, 0, 0);              // 35 kPa
    set_val(0x0C, 0x0C, 0x80, 0, 0);           // 800 rpm
    set_val(0x0D, 0x00, 0, 0, 0);              // 0 km/h
    set_val(0x0E, 0x8C, 0, 0, 0);              // 6°
    set_val(0x0F, 0x41, 0, 0, 0);              // 25 °C
    set_val(0x11, 0x20, 0, 0, 0);              // %12.5
    set_val(0x13, 0x03, 0, 0, 0);
    set_val(0x14, 0x5A, 0x80, 0, 0);           // 0.45 V
    set_val(0x15, 0x8C, 0xFF, 0, 0);           // 0.70 V
    set_val(0x1C, 0x06, 0, 0, 0);              // EOBD
}
