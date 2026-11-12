#pragma once
/*
 * Host testleri için K-Line simülatörü: sanal saat (µs), L9637D yankısı olan yarı çift yönlü UART
 * ve fast init / servis 01-03-04 cevaplayan bir ECU. Bayt zamanları 10400 baud'a göre, ECU P2 gecikmesi ayarlı.
 * Tek iş parçacıklı: obd_task aynı yığında koşar, süre dolunca sim_run'a geri sıçrar.
 */
#include <stdbool.h>
#include <stdint.h>

#define SIM_BYTE_US     962         // 10 bit / 10400 baud
#define SIM_MAX_PID     0x60

typedef struct {
    bool on;                        // kontak açık, ECU cevap verir
    bool rx_ok;                     // K hattı -> RX yolu sağlam (yankı ve cevaplar gelir)
    uint8_t addr;                   // ECU kaynak adresi
    uint32_t sup[3];                // 01 00 / 01 20 / 01 40 destek haritaları
    uint8_t val[SIM_MAX_PID][4];    // PID cevap verisi
    bool mute[SIM_MAX_PID];         // destekliyor der ama cevap vermez
    uint32_t p2_us;                 // istek sonu -> ilk cevap baytı
    uint8_t second_addr;            // != 0: ikinci ECU da cevaplar (farklı değerle)
    uint16_t dtc[8];
    int ndtc;
    uint32_t dtc_pending_us;        // != 0: önce 7F 03 78, asıl cevap bu kadar sonra
} sim_ecu_t;

typedef struct {
    uint32_t count;
    int64_t first_us, last_us;
    int64_t max_gap_us;
} sim_pid_stat_t;

extern sim_ecu_t g_ecu;
extern sim_pid_stat_t g_pid_stat[SIM_MAX_PID];
extern uint32_t g_sid_seen[256];    // ECU'ya ulaşan her servisin sayısı
extern int64_t g_max_req_gap_us;    // iki istek arası en uzun süre (oturum P3max 5 s)
extern uint32_t g_requests;

void sim_reset(void);               // saat 0, ECU varsayılan (Kalos 1.4 profili), istatistikler sıfır
void sim_stats_reset(void);         // sadece ölçüm istatistikleri
int64_t sim_now(void);
void sim_advance(int64_t us);

/** Süre olayı: belirtilen zamanda fn(arg) çağrılır (ör. sayfa değişimi). */
void sim_at(int64_t t_us, void (*fn)(uint32_t), uint32_t arg);

/** Kayıtlı görevi (obd_start ile oluşan) t_us sanal zaman boyunca koşturur. */
void sim_run_task(int64_t t_us);

/** ECU'nun bir PID için verdiği son cevabın sanal zamanı listesi yerine özet. */
const sim_pid_stat_t *sim_pid(uint8_t pid);

/** Son srclog_done metni ve log satırlarında arama. */
const char *sim_last_srclog(void);
bool sim_log_contains(const char *s);
