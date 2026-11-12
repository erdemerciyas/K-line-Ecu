/*
 * main/obd.c host testleri: gerçek kaynak dosyası buraya dahil edilir (static fonksiyonlara erişim),
 * ESP-IDF / FreeRTOS / UART stub'larla, K hattı ve ECU sim.c'deki zamanlı simülatörle taklit edilir.
 *
 *   Birim:        çerçeve kurma / çözme, salt okunur servis koruması, PID haritası, değer çözme, tüketim
 *   Zamanlayıcı:  16 isteklik sıra tablosu, sayfa değişiminde anında geçiş, açlık (starvation) sınırı
 *   Entegrasyon:  fast init -> PID taraması -> canlı sorgu; sayfa geçiş gecikmesi ve yenilenme hızları (ölçüm),
 *                 cevapsız PID, kontak kapanıp açılması, iki ECU, arıza kodları (7F 78 bekletmeli), donanım arızası
 */
#include "../../main/obd.c"
#include "imu_math.h"
#include "sim.h"

void exit(int code);
int fflush(void *f);

// ---------------------------------------------------------------------------
// Mini test çerçevesi
// ---------------------------------------------------------------------------
static int s_fail, s_checks, s_tests, s_failed_tests;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; printf("    HATA %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { s_checks++; long long a_ = (long long)(a), b_ = (long long)(b); \
    if (a_ != b_) { s_fail++; printf("    HATA %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, a_, b_); } } while (0)
#define CHECK_NEAR(a, b, eps) do { s_checks++; double a_ = (a), b_ = (b); \
    if (!(a_ - b_ <= (eps) && b_ - a_ <= (eps))) { s_fail++; \
    printf("    HATA %s:%d: %s ~ %s (%.4f != %.4f)\n", __FILE__, __LINE__, #a, #b, a_, b_); } } while (0)

static void run(const char *name, void (*fn)(void))
{
    const int f0 = s_fail;
    s_tests++;
    fn();
    if (s_fail != f0) {
        s_failed_tests++;
    }
    printf("  %-4s %s\n", s_fail == f0 ? "OK" : "HATA", name);
}

// ölçüm raporu (sonda basılır)
static char s_report[4096];
static int s_nreport;
#define REPORT(...) (s_nreport += snprintf(s_report + s_nreport, sizeof(s_report) - s_nreport, __VA_ARGS__))

// ---------------------------------------------------------------------------
// Yardımcılar
// ---------------------------------------------------------------------------
#define KALOS_SUP0  0xBE3EB811u

/** obd.c modül durumunu ve simülatörü sıfırlar (her entegrasyon testi temiz bağlantıyla başlar). */
static void obd_reset(void)
{
    sim_reset();
    s_want = s_built_want = s_want_remote = 0;
    s_rpm_fast = s_speed_slot = s_clt_guard = false;
    s_slot = 0;
    s_nfast = s_nslow = s_fi = s_si = 0;
    s_rescan = false;
    s_dtc_req = 0;
    s_mode = VEMS_MODE_NONE;
    s_kl_ecu = 0;
    s_kl_quiet = false;
    s_have_data = false;
    s_channels = 0;
    s_state = VEMS_STATE_WAIT_LINK;
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_sup, 0, sizeof(s_sup));
    xSemaphoreTake(s_dtc_done, 0);
}

static void sched(uint32_t sup0, uint32_t sup1, uint32_t sup2, uint32_t want)
{
    s_sup[0] = sup0;
    s_sup[1] = sup1;
    s_sup[2] = sup2;
    s_want = want;
    build_poll_lists(want);
    s_slot = 0;
}

static int seq_run(uint8_t *seq, int n)
{
    for (int i = 0; i < n; i++) {
        seq[i] = next_pid();
    }
    return n;
}

static int seq_count(const uint8_t *seq, int n, uint8_t pid)
{
    int c = 0;
    for (int i = 0; i < n; i++) {
        c += seq[i] == pid;
    }
    return c;
}

/** Bir PID'in dizideki en uzun boşluğu (istek sayısı), hiç yoksa n. */
static int seq_max_gap(const uint8_t *seq, int n, uint8_t pid)
{
    int last = -1, gap = 0;
    for (int i = 0; i < n; i++) {
        if (seq[i] == pid) {
            const int g = i - last;
            gap = g > gap ? g : gap;
            last = i;
        }
    }
    return last < 0 ? n : gap;
}

static bool in_lists(uint8_t pid, bool fast)
{
    const uint8_t *l = fast ? s_fast : s_slow;
    const int n = fast ? s_nfast : s_nslow;
    return memchr(l, pid, n) != NULL;
}

// ---------------------------------------------------------------------------
// Birim testleri
// ---------------------------------------------------------------------------
static void t_readonly_guard(void)
{
    // salt okunur: sadece 01 (canlı veri), 03 / 04 (arıza kodu, kullanıcı isteğiyle) ve 81 (oturum açma) gönderilebilir
    uint8_t f[16];
    s_mode = VEMS_MODE_OBD_KWP_FAST;
    for (int sid = 0; sid < 256; sid++) {
        const uint8_t data[] = {(uint8_t)sid, 0x00};
        const size_t n = kl_build(f, data, 2);
        const bool allowed = sid == 0x01 || sid == 0x03 || sid == 0x04 || sid == 0x81;
        if (allowed != (n > 0)) {
            CHECK_EQ(sid, -1);      // hangi servisin kaçtığını göster
        }
    }
    CHECK(true);
}

static void t_kl_build(void)
{
    uint8_t f[16];
    const uint8_t rpm[] = {0x01, 0x0C};
    s_mode = VEMS_MODE_OBD_KWP_FAST;
    CHECK_EQ(kl_build(f, rpm, 2), 6);
    const uint8_t kwp[] = {0xC2, 0x33, 0xF1, 0x01, 0x0C, 0xF3};
    CHECK(memcmp(f, kwp, 6) == 0);
    s_mode = VEMS_MODE_OBD_ISO9141;
    CHECK_EQ(kl_build(f, rpm, 2), 6);
    const uint8_t iso[] = {0x68, 0x6A, 0xF1, 0x01, 0x0C, 0xD0};
    CHECK(memcmp(f, iso, 6) == 0);
    s_mode = VEMS_MODE_OBD_KWP_FAST;
    const uint8_t sc[] = {0x81};
    CHECK_EQ(kl_build(f, sc, 1), 5);
    const uint8_t start[] = {0xC1, 0x33, 0xF1, 0x81, 0x66};
    CHECK(memcmp(f, start, 5) == 0);
}

static void t_kl_find_kwp(void)
{
    uint8_t out[DATA_MAX], src = 0;
    size_t pos = 0;
    s_mode = VEMS_MODE_OBD_KWP_FAST;
    s_kl_ecu = 0;
    // yankı + gürültü + geçerli çerçeve: 84 F1 10 41 0C 1A F8 cs
    uint8_t b[] = {0xC2, 0x33, 0xF1, 0x01, 0x0C, 0xF3, 0x00, 0x84, 0xF1, 0x10, 0x41, 0x0C, 0x1A, 0xF8, 0x00};
    b[14] = sum8(&b[7], 7);
    CHECK_EQ(kl_find(b, sizeof(b), &pos, 0x41, 0x0C, out, &src), 2);
    CHECK_EQ(out[0], 0x1A);
    CHECK_EQ(out[1], 0xF8);
    CHECK_EQ(src, 0x10);
    CHECK_EQ(pos, sizeof(b));

    // bozuk sağlama: kabul edilmez
    pos = 0;
    b[14] ^= 1;
    CHECK_EQ(kl_find(b, sizeof(b), &pos, 0x41, 0x0C, out, &src), -1);
    b[14] ^= 1;

    // başka PID'in cevabı: kabul edilmez
    pos = 0;
    CHECK_EQ(kl_find(b, sizeof(b), &pos, 0x41, 0x0D, out, &src), -1);

    // ECU kilitliyken başka adresten gelen: kabul edilmez
    pos = 0;
    s_kl_ecu = 0x18;
    CHECK_EQ(kl_find(b, sizeof(b), &pos, 0x41, 0x0C, out, &src), -1);
    s_kl_ecu = 0;

    // eksik çerçeve (son bayt gelmedi)
    pos = 0;
    CHECK_EQ(kl_find(b, sizeof(b) - 1, &pos, 0x41, 0x0C, out, &src), -1);

    // ayrı uzunluk baytı biçimi: 80 F1 10 04 41 0D 32 00? -> 80 F1 10 03 41 0D 32 cs
    uint8_t lb[] = {0x80, 0xF1, 0x10, 0x03, 0x41, 0x0D, 0x32, 0x00};
    lb[7] = sum8(lb, 7);
    pos = 0;
    CHECK_EQ(kl_find(lb, sizeof(lb), &pos, 0x41, 0x0D, out, &src), 1);
    CHECK_EQ(out[0], 0x32);

    // çok çerçeveli cevap sırayla yürünür (arıza kodları)
    uint8_t m[16];
    int k = 0;
    for (int fr = 0; fr < 2; fr++) {
        const int s0 = k;
        m[k++] = 0x82;
        m[k++] = 0xF1;
        m[k++] = 0x10;
        m[k++] = 0x43;
        m[k++] = (uint8_t)(0x11 * (fr + 1));
        m[k] = sum8(&m[s0], 5);
        k++;
    }
    pos = 0;
    CHECK_EQ(kl_find(m, k, &pos, 0x43, -1, out, &src), 1);
    CHECK_EQ(out[0], 0x11);
    CHECK_EQ(kl_find(m, k, &pos, 0x43, -1, out, &src), 1);
    CHECK_EQ(out[0], 0x22);
    CHECK_EQ(kl_find(m, k, &pos, 0x43, -1, out, &src), -1);
}

static void t_kl_find_iso(void)
{
    uint8_t out[DATA_MAX], src = 0;
    size_t pos = 0;
    s_mode = VEMS_MODE_OBD_ISO9141;
    s_kl_ecu = 0;
    uint8_t b[] = {0x48, 0x6B, 0x10, 0x41, 0x0C, 0x1A, 0xF8, 0x00};
    b[7] = sum8(b, 7);
    CHECK_EQ(kl_find(b, sizeof(b), &pos, 0x41, 0x0C, out, &src), 2);
    CHECK_EQ((out[0] << 8 | out[1]) / 4, 1726);
    s_mode = VEMS_MODE_OBD_KWP_FAST;
}

static void t_pid_supported(void)
{
    memset(s_sup, 0, sizeof(s_sup));
    s_sup[0] = KALOS_SUP0;      // 01 03 04 05 06 07 0B 0C 0D 0E 0F 11 13 14 15 1C 20
    static const uint8_t yes[] = {0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x11,
                                  0x13, 0x14, 0x15, 0x1C, 0x20};
    int n = 0;
    for (int pid = 1; pid <= 0x60; pid++) {
        const bool exp = memchr(yes, pid, sizeof(yes)) != NULL;
        if (pid_supported(pid) != exp) {
            CHECK_EQ(pid, -1);
        }
        n += exp;
    }
    CHECK_EQ(n, sizeof(yes));
    CHECK(pid_supported(0));
    CHECK(!pid_supported(0x61));
    CHECK(!pid_supported(0xFF));
    pid_unsupport(0x15);
    CHECK(!pid_supported(0x15));
    CHECK(pid_supported(0x14));
}

static void t_decode(void)
{
    vems_data_t d;
    data_reset(&d);
    const uint8_t rpm[] = {0x1A, 0xF8};
    decode(&d, PID_RPM, rpm, 2);
    CHECK_EQ(d.rpm, 1726);
    CHECK(d.engine_status & VEMS_ENG_RUNNING);
    const uint8_t idle[] = {0x01, 0x00};
    decode(&d, PID_RPM, idle, 2);
    CHECK(!(d.engine_status & VEMS_ENG_RUNNING));
    uint8_t v[4] = {0x7B};
    decode(&d, PID_CLT, v, 1);
    CHECK_NEAR(d.clt_c, 83, 1e-4);
    v[0] = 0xFF;
    decode(&d, PID_TPS, v, 1);
    CHECK_NEAR(d.tps_pct, 100, 1e-4);
    v[0] = 0x64;
    decode(&d, PID_MAP, v, 1);
    CHECK_NEAR(d.map_kpa, 100, 1e-4);
    v[0] = 0x90;
    decode(&d, PID_ADV, v, 1);
    CHECK_NEAR(d.ign_adv_deg, 8, 1e-4);
    const uint8_t vb[] = {0x30, 0xD4};
    decode(&d, PID_VBAT, vb, 2);
    CHECK_NEAR(d.batt_v, 12.5, 1e-4);
    v[0] = 0x80;
    decode(&d, PID_STFT1, v, 1);
    CHECK_NEAR(d.ego_corr_pct, 100, 1e-4);
    v[0] = 0x90;
    decode(&d, PID_LTFT1, v, 1);
    CHECK_NEAR(d.ltft_pct, 12.5, 1e-4);
    const uint8_t wr[] = {0x80, 0x00, 0x80, 0x00};
    decode(&d, PID_O2S1_WR_V, wr, 4);
    CHECK_NEAR(d.lambda, 1.0, 1e-5);
    CHECK_NEAR(d.afr, 14.7, 1e-4);
    const uint8_t o2[] = {0x5A, 0x80};
    decode(&d, PID_O2S1, o2, 2);
    CHECK_NEAR(d.o2_v, 0.45, 1e-4);
    const uint8_t mil[] = {0x83, 0x07, 0x65, 0x00};
    decode(&d, PID_MIL, mil, 4);
    CHECK(d.mil_on);
    CHECK_EQ(d.mil_codes, 3);
    const uint8_t fs[] = {0x02, 0x00};
    decode(&d, PID_FUEL_SYS, fs, 2);
    CHECK(d.status1 & 0x04);
    // eksik cevap yok sayılır
    const float before = d.batt_v;
    decode(&d, PID_VBAT, vb, 1);
    CHECK_NEAR(d.batt_v, before, 0);
}

static void t_fuel_calc(void)
{
    vems_data_t d;
    data_reset(&d);
    d.rpm = 2000;
    d.maf_gs = 10;
    d.speed_kmh = 100;
    fuel_calc(&d);
    CHECK_NEAR(d.fuel_lh, 10 / 14.7 / 745.0 * 3600, 1e-3);
    CHECK_NEAR(d.fuel_l100, d.fuel_lh, 1e-4);

    // hız-yoğunluk (Kalos: MAF yok) 100 kPa, 25 °C, 3000 rpm, 1399 cc, VE 0.8
    data_reset(&d);
    d.rpm = 3000;
    d.map_kpa = 100;
    d.iat_c = 25;
    d.speed_kmh = 3;
    fuel_calc(&d);
    const double air = 100 / (0.287 * 298.15) * 1.399 * 3000 / 120 * 0.8;
    CHECK_NEAR(d.fuel_lh, air / 14.7 / 745 * 3600, 2e-3);
    CHECK(isnan(d.fuel_l100));          // 5 km/h altında L/100 km anlamsız

    d.rpm = 200;                        // motor duruyor
    fuel_calc(&d);
    CHECK_NEAR(d.fuel_lh, 0, 0);

    data_reset(&d);                     // ECU yakıt debisini veriyorsa o kullanılır
    d.fuel_rate_lh = 4.5f;
    d.maf_gs = 50;
    d.speed_kmh = 90;
    fuel_calc(&d);
    CHECK_NEAR(d.fuel_lh, 4.5, 1e-5);
    CHECK_NEAR(d.fuel_l100, 5.0, 1e-4);
}

static void t_polled_channels(void)
{
    sched(KALOS_SUP0, 0, 0, 0);
    const uint32_t m = polled_channels();
    static const vems_ch_t has[] = {CH_RPM, CH_SPEED, CH_MAP, CH_BOOST, CH_O2V, CH_CLT, CH_IAT, CH_TPS, CH_LOAD,
                                    CH_ADV, CH_STFT, CH_LTFT, CH_O2V2, CH_FUEL_LH, CH_FUEL_L100};
    static const vems_ch_t not[] = {CH_AFR, CH_BATT, CH_OIL, CH_FUEL, CH_MAF, CH_TARGET};
    for (size_t i = 0; i < sizeof(has) / sizeof(has[0]); i++) {
        if (!(m & CH_BIT(has[i]))) {
            CHECK_EQ(has[i], -1);
        }
    }
    for (size_t i = 0; i < sizeof(not) / sizeof(not[0]); i++) {
        if (m & CH_BIT(not[i])) {
            CHECK_EQ(not[i], -1);
        }
    }
}

// ---------------------------------------------------------------------------
// Zamanlayıcı testleri (Kalos profili)
// ---------------------------------------------------------------------------
#define N 1600
static uint8_t seq[N];

/** Desteklenen ve tabloda olan her canlı PID dizide en geç maxgap istekte bir görünür (açlık yok). */
static void check_no_starvation(int maxgap)
{
    for (int pid = 1; pid <= PID_LAST; pid++) {
        const obd_pid_def_t *def = obd_pid_def(pid);
        if (!pid_supported(pid) || !def || (def->flags & OPF_ONCE)) {
            continue;
        }
        const int g = seq_max_gap(seq, N, pid);
        if (g > maxgap) {
            printf("    PID %02X en uzun boşluk %d istek (sınır %d)\n", pid, g, maxgap);
            CHECK(g <= maxgap);
        }
    }
}

static void t_sched_default(void)
{
    sched(KALOS_SUP0, 0, 0, 0);
    seq_run(seq, N);
    for (int i = 0; i < N; i += 2) {
        if (seq[i] != PID_RPM) {
            CHECK_EQ(i, -1);
            break;
        }
    }
    // varsayılan hızlı liste (Kalos'ta olanlar): hararet, MAP, hız, TPS, dar bant O2
    CHECK(in_lists(PID_CLT, true) && in_lists(PID_MAP, true) && in_lists(PID_SPEED, true));
    CHECK(in_lists(PID_TPS, true) && in_lists(PID_O2S1, true));
    CHECK(!in_lists(PID_RPM, true) && !in_lists(PID_RPM, false));
    CHECK(!s_clt_guard);
    check_no_starvation(16 * s_nslow + 16);
}

static void t_sched_focus_single(void)
{
    // tek gösterge TPS: RPM ekranda değil, TPS isteklerin büyük kısmını alır
    sched(KALOS_SUP0, 0, 0, CH_BIT(CH_TPS));
    seq_run(seq, N);
    const double share = seq_count(seq, N, PID_TPS) / (double)N;
    REPORT("  tek gösterge TPS     istek payı %%%.0f (16'da %d)\n", share * 100, (int)(share * 16 + 0.5));
    CHECK(share >= 0.80);
    CHECK(seq_max_gap(seq, N, PID_TPS) <= 2);
    CHECK(in_lists(PID_RPM, false));                    // devir yavaş turda: uyarılar için tazelenir
    CHECK(s_clt_guard);
    CHECK_EQ(seq_max_gap(seq, N, PID_CLT), 16);         // hararet bekçisi: tam 16 istekte bir
    check_no_starvation(8 * s_nslow + 16);
}

static void t_sched_drive(void)
{
    // sürüş sayfası (Kalos): devir, hız, hararet, MAP + tüketim göstergesi
    sched(KALOS_SUP0, 0, 0, CH_BIT(CH_RPM) | CH_BIT(CH_SPEED) | CH_BIT(CH_CLT) | CH_BIT(CH_MAP) | CH_BIT(CH_FUEL_L100));
    seq_run(seq, N);
    CHECK(s_speed_slot);
    CHECK(!in_lists(PID_SPEED, true));                  // hız kendi sırasında, hızlı listeye ikinci kez girmez
    for (int i = 0; i < N; i++) {
        if ((i % 2 == 0 && seq[i] != PID_RPM) || (i % 4 == 3 && seq[i] != PID_SPEED)) {
            CHECK_EQ(i, -1);
            break;
        }
    }
    CHECK_EQ(seq_count(seq, N, PID_RPM), N / 2);
    CHECK_EQ(seq_count(seq, N, PID_SPEED), N / 4);
    // ekrandaki diğerleri (hararet, MAP, emme havası = tüketim girdisi) 16 istekte bir
    CHECK_EQ(seq_max_gap(seq, N, PID_CLT), 16);
    CHECK_EQ(seq_max_gap(seq, N, PID_MAP), 16);
    CHECK_EQ(seq_max_gap(seq, N, PID_IAT), 16);
    check_no_starvation(16 * s_nslow + 16);
}

static void t_sched_switch_immediate(void)
{
    sched(KALOS_SUP0, 0, 0, CH_BIT(CH_RPM) | CH_BIT(CH_SPEED) | CH_BIT(CH_CLT) | CH_BIT(CH_MAP));
    seq_run(seq, 7);
    vems_set_wanted_channels(CH_BIT(CH_IAT));           // sayfa değişti
    seq_run(seq, 2);
    CHECK(seq[0] == PID_IAT || seq[1] == PID_IAT);      // yeni ekranın verisi en geç 2. istekte
    vems_set_wanted_channels(CH_BIT(CH_ADV));
    CHECK(next_pid() == PID_ADV || next_pid() == PID_ADV);
}

static void t_sched_remote_merge(void)
{
    // LCD tek gösterge TPS + telefonda emme öne alınmış: ikisi de hızlı listede, telefon ayrılınca sadece TPS
    sched(KALOS_SUP0, 0, 0, CH_BIT(CH_TPS));
    vems_set_remote_channels(CH_BIT(CH_IAT));
    next_pid();
    CHECK(in_lists(PID_TPS, true) && in_lists(PID_IAT, true));
    CHECK(!s_rpm_fast);
    vems_set_remote_channels(0);
    next_pid();
    CHECK(in_lists(PID_TPS, true) && !in_lists(PID_IAT, true));
    // ekran varsayılanda (ayarlar / arıza kodu sayfası), telefon avansı öne almış: sadece o
    vems_set_wanted_channels(0);
    vems_set_remote_channels(CH_BIT(CH_ADV));
    seq_run(seq, 64);
    CHECK_EQ(s_nfast, 1);
    CHECK(seq_count(seq, 64, PID_ADV) >= 64 * 3 / 4);
    vems_set_remote_channels(0);
}

static void t_sched_fuel_inputs(void)
{
    sched(KALOS_SUP0, 0, 0, CH_BIT(CH_FUEL_LH));
    CHECK(s_rpm_fast);
    CHECK(in_lists(PID_MAP, true) && in_lists(PID_IAT, true));
}

static void t_sched_edge(void)
{
    // ekrandaki kanalı hiçbir PID beslemiyor: yavaş tur döner, takılmaz
    sched(KALOS_SUP0, 0, 0, CH_BIT(CH_BARO));
    CHECK_EQ(s_nfast, 0);
    seq_run(seq, N);
    check_no_starvation(s_nslow + 4);

    // sadece devir destekli
    sched(1u << (31 - (PID_RPM - 1)), 0, 0, CH_BIT(CH_TPS));
    seq_run(seq, 64);
    CHECK_EQ(seq_count(seq, 64, PID_RPM), 64);

    // geniş bant varken dar bant hızlı listeye girmez
    sched(KALOS_SUP0, 1u << (31 - (PID_O2S1_WR_V - 0x21)), 0, CH_BIT(CH_AFR) | CH_BIT(CH_O2V));
    CHECK(in_lists(PID_O2S1_WR_V, true));
    CHECK(in_lists(PID_O2S1, false) && !in_lists(PID_O2S1, true));

    // her şey destekli, her şey isteniyor: dizi sınırları aşılmaz
    sched(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
    CHECK(s_nfast + s_nslow <= PID_LAST);
    seq_run(seq, N);
    sched(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, CH_BIT(CH_TPS));
    CHECK(s_nfast + s_nslow <= PID_LAST);
    seq_run(seq, N);
    CHECK(seq_count(seq, N, PID_TPS) >= N * 3 / 4);
}

// ---------------------------------------------------------------------------
// Entegrasyon testleri (sanal zamanda gerçek obd_task)
// ---------------------------------------------------------------------------
static int64_t s_switch_t;

static void ev_want(uint32_t want)
{
    vems_set_wanted_channels(want);
    s_switch_t = sim_now();
    sim_stats_reset();
}

static void ev_ecu_on(uint32_t on)
{
    g_ecu.on = on;
}

static void ev_dtc(uint32_t sid)
{
    s_dtc_req = (uint8_t)sid;
}

/** Sayfa değişiminden sonra bir PID'in ilk cevabına kadar geçen süre ve ortalama yenilenme aralığı (ms). */
static void pid_rate(uint8_t pid, double *first_ms, double *avg_ms, double *max_ms)
{
    const sim_pid_stat_t *s = sim_pid(pid);
    *first_ms = s->count ? (s->first_us - s_switch_t) / 1000.0 : 1e9;
    *avg_ms = s->count > 1 ? (s->last_us - s->first_us) / 1000.0 / (s->count - 1) : 1e9;
    *max_ms = s->count > 1 ? s->max_gap_us / 1000.0 : 1e9;
}

static void t_int_connect(void)
{
    obd_reset();
    sim_run_task(4000000);
    CHECK_EQ(s_state, VEMS_STATE_ECU_OK);
    CHECK_EQ(s_mode, VEMS_MODE_OBD_KWP_FAST);
    CHECK_EQ(s_kl_ecu, 0x10);
    CHECK_EQ(s_sup[0], KALOS_SUP0);
    vems_data_t d;
    CHECK(vems_get_latest(&d));
    CHECK_EQ(d.rpm, 800);
    CHECK_NEAR(d.clt_c, 83, 1e-4);
    CHECK_NEAR(d.map_kpa, 35, 1e-4);
    CHECK_NEAR(d.tps_pct, 12.549, 1e-2);
    CHECK_NEAR(d.speed_kmh, 0, 1e-4);
    CHECK_NEAR(d.o2_v, 0.45, 1e-4);
    CHECK(d.rpm_us > 0 && d.rpm_us <= sim_now());
    CHECK_EQ(vems_get_channels(), polled_channels());
    vems_link_info_t li;
    vems_get_link_info(&li);
    CHECK(strcmp(li.ecu, "0x10") == 0);
    // değişmeyen bilgiler bağlantıda bir kez okundu, profil dolu
    vems_pid_info_t prof[64];
    const int n = vems_get_profile(prof, 64);
    CHECK(n >= 15);
    for (int i = 0; i < n; i++) {
        if (prof[i].pid == 0x1C) {
            CHECK(prof[i].once && prof[i].age_ms != UINT32_MAX);
        }
    }
    CHECK(g_sid_seen[0x81] == 1);
}

static void t_int_request_period(void)
{
    obd_reset();
    sim_at(2000000, ev_want, 0);
    sim_run_task(12000000);
    const double ms = (sim_now() - s_switch_t) / 1000.0 / g_requests;
    REPORT("  istek süresi         %.0f ms (P2 25 ms ECU, %.1f istek/sn)\n", ms, 1000 / ms);
    CHECK(ms > 100 && ms < 160);
    CHECK(g_max_req_gap_us < 1000000);      // oturum hiç P3max'a (5 sn) yaklaşmaz
    CHECK_EQ(s_stats.timeouts, 0);
}

static void t_int_page_switch(void)
{
    // sürüş sayfasından tek gösterge TPS'e geçiş
    obd_reset();
    vems_set_wanted_channels(CH_BIT(CH_RPM) | CH_BIT(CH_SPEED) | CH_BIT(CH_CLT) | CH_BIT(CH_MAP) | CH_BIT(CH_FUEL_L100));
    sim_at(5000000, ev_want, CH_BIT(CH_TPS));
    sim_run_task(15000000);
    double first, avg, max;
    pid_rate(PID_TPS, &first, &avg, &max);
    REPORT("  sürüş -> TPS geçişi  ilk TPS %.0f ms sonra, sonra %.0f ms'de bir (en uzun %.0f ms, %.1f Hz)\n",
           first, avg, max, 1000 / avg);
    CHECK(first < 350);
    CHECK(avg < 200);
    CHECK(max < 350);
    pid_rate(PID_CLT, &first, &avg, &max);
    REPORT("  ...hararet bekçisi   %.0f ms'de bir\n", avg);
    CHECK(max < 2600);
}

static void t_int_drive(void)
{
    obd_reset();
    sim_at(3000000, ev_want, CH_BIT(CH_RPM) | CH_BIT(CH_SPEED) | CH_BIT(CH_CLT) | CH_BIT(CH_MAP) | CH_BIT(CH_FUEL_L100));
    sim_run_task(23000000);
    double first, avg, max;
    pid_rate(PID_RPM, &first, &avg, &max);
    REPORT("  sürüş sayfası        devir %.0f ms", avg);
    CHECK(avg < 320);
    pid_rate(PID_SPEED, &first, &avg, &max);
    REPORT(", hız %.0f ms", avg);
    CHECK(avg < 640);
    pid_rate(PID_MAP, &first, &avg, &max);
    REPORT(", MAP %.0f ms", avg);
    CHECK(max < 2600);
    pid_rate(PID_IAT, &first, &avg, &max);
    REPORT(", emme %.0f ms", avg);
    CHECK(max < 2600);
    pid_rate(PID_CLT, &first, &avg, &max);
    REPORT(", hararet %.0f ms\n", avg);
    CHECK(max < 2600);
    // ekranda olmayanlar da (profil sayfası) tazelenir
    pid_rate(PID_ADV, &first, &avg, &max);
    CHECK(sim_pid(PID_ADV)->count >= 1);
}

static void t_int_list_page(void)
{
    // tüm veriler listesinde görünen 6 satır
    const uint32_t rows = CH_BIT(CH_IAT) | CH_BIT(CH_ADV) | CH_BIT(CH_LTFT) | CH_BIT(CH_STFT) | CH_BIT(CH_LOAD) |
                          CH_BIT(CH_O2V);
    obd_reset();
    sim_at(3000000, ev_want, rows);
    sim_run_task(23000000);
    static const uint8_t pids[] = {PID_IAT, PID_ADV, PID_LTFT1, PID_STFT1, PID_LOAD, PID_O2S1};
    double worst = 0, wfirst = 0;
    for (size_t i = 0; i < sizeof(pids); i++) {
        double first, avg, max;
        pid_rate(pids[i], &first, &avg, &max);
        worst = avg > worst ? avg : worst;
        wfirst = first > wfirst ? first : wfirst;
        CHECK(avg < 1200);
    }
    REPORT("  tüm veriler (6 satır) her satır en geç %.0f ms'de bir, kaydırınca ilk değer %.0f ms içinde\n",
           worst, wfirst);
    CHECK(wfirst < 1500);
}

static void t_int_mute_pid(void)
{
    obd_reset();
    g_ecu.mute[0x14] = true;            // destekliyor diyor, cevap vermiyor
    sim_run_task(20000000);
    CHECK(!pid_supported(0x14));
    CHECK(!in_lists(0x14, true) && !in_lists(0x14, false));
    CHECK(sim_log_contains("PID 14 cevap vermiyor"));
    CHECK_EQ(s_state, VEMS_STATE_ECU_OK);
    CHECK_EQ(s_stats.resyncs, 0);
}

static void t_int_ignition_cycle(void)
{
    obd_reset();
    sim_at(3000000, ev_ecu_on, 0);
    sim_at(14000000, ev_ecu_on, 1);
    sim_run_task(40000000);
    CHECK(s_stats.resyncs >= 1);
    CHECK(sim_log_contains("ECU stopped answering"));
    CHECK_EQ(s_state, VEMS_STATE_ECU_OK);
    vems_data_t d;
    CHECK(vems_get_latest(&d) && d.rpm == 800);
}

static void t_int_two_ecus(void)
{
    obd_reset();
    g_ecu.second_addr = 0x18;           // şanzıman ECU'su gibi ikinci bir cevap veren
    sim_run_task(6000000);
    vems_data_t d;
    CHECK(vems_get_latest(&d));
    CHECK_EQ(d.rpm, 800);               // ikinci ECU'nun ters çevrilmiş değerleri alınmadı
    CHECK_NEAR(d.clt_c, 83, 1e-4);
    CHECK_EQ(s_kl_ecu, 0x10);
}

static void t_int_dtc(void)
{
    obd_reset();
    g_ecu.dtc[0] = 0x0133;              // P0133
    g_ecu.dtc[1] = 0x0420;              // P0420
    g_ecu.dtc[2] = 0x4123;              // C0123
    g_ecu.dtc[3] = 0x0300;              // P0300
    g_ecu.ndtc = 4;                     // 2 çerçeve
    g_ecu.dtc_pending_us = 1500000;     // önce 7F 03 78
    sim_at(3000000, ev_dtc, SID_DTC_READ);
    sim_run_task(9000000);
    CHECK_EQ(s_ndtc, 4);
    CHECK_EQ(s_dtc[0], 0x0133);
    CHECK_EQ(s_dtc[1], 0x0420);
    CHECK_EQ(s_dtc[2], 0x4123);
    CHECK_EQ(s_dtc[3], 0x0300);
    CHECK(xSemaphoreTake(s_dtc_done, 0) == pdTRUE);     // ekran görevine haber verildi
    CHECK_EQ(s_dtc_req, 0);
    CHECK_EQ(s_state, VEMS_STATE_ECU_OK);               // ~1.5 sn bekleme bağlantıyı düşürmedi

    sim_at(sim_now() + 500000, ev_dtc, SID_DTC_CLEAR);
    sim_run_task(3000000);
    CHECK_EQ(s_ndtc, 0);
    CHECK_EQ(g_ecu.ndtc, 0);
    CHECK_EQ(g_sid_seen[0x04], 1);

    sim_at(sim_now() + 500000, ev_dtc, SID_DTC_READ);   // kod yok: tek çerçeve 0000
    sim_run_task(5000000);
    CHECK_EQ(s_ndtc, 0);
}

static void t_int_readonly_wire(void)
{
    // normal çalışmada hatta giden servisler: sadece 81 (bir kez) ve 01
    obd_reset();
    sim_run_task(20000000);
    for (int sid = 0; sid < 256; sid++) {
        if (g_sid_seen[sid] && sid != 0x01 && sid != 0x81) {
            CHECK_EQ(sid, -1);
        }
    }
    CHECK(g_sid_seen[0x01] > 100);
}

static void t_int_rx_broken(void)
{
    // L9637D -> RX yolu kopuk ("K inmiyor"): ölçüm modu, 5 baud denemesi, çökmeden tekrar
    obd_reset();
    g_ecu.rx_ok = false;
    sim_run_task(25000000);
    CHECK_EQ(s_kl_diag, KL_DIAG_NO_LOW);
    CHECK(sim_log_contains("K ölç TX LOW"));
    CHECK(s_state == VEMS_STATE_WAIT_LINK);
    vems_data_t d;
    CHECK(!vems_get_latest(&d));
    CHECK_EQ(vems_get_channels(), 0);
}

static void t_int_ecu_silent(void)
{
    // devre sağlam, kontak kapalı: "ECU sessiz", yeniden dener, kontak açılınca bağlanır
    obd_reset();
    g_ecu.on = false;
    sim_at(8000000, ev_ecu_on, 1);
    sim_run_task(6000000);
    CHECK_EQ(s_kl_diag, KL_DIAG_SILENT);
    CHECK(s_state == VEMS_STATE_WAIT_LINK);
    sim_run_task(14000000);
    CHECK_EQ(s_state, VEMS_STATE_ECU_OK);
}

static void t_int_rescan(void)
{
    obd_reset();
    sim_run_task(3000000);
    CHECK_EQ(s_state, VEMS_STATE_ECU_OK);
    vems_rescan();
    sim_run_task(1000000);
    CHECK(s_state != VEMS_STATE_ECU_OK);    // P3max beklemesinde "hazır" görünmez
    CHECK_EQ(vems_get_channels(), 0);
    sim_run_task(12000000);
    CHECK_EQ(s_state, VEMS_STATE_ECU_OK);
    CHECK_EQ(g_sid_seen[0x81], 2);          // yeni oturum açıldı
}

// ---------------------------------------------------------------------------
// Eğim (imu_math.c)
// ---------------------------------------------------------------------------
static const float SR[3] = {1, 0, 0}, SU[3] = {0, 1, 0}, SIN[3] = {0, 0, -1};  // ekranın sağı / yukarısı / içi

static void vmix(float out[3], float a, const float x[3], float b, const float y[3])
{
    for (int i = 0; i < 3; i++) {
        out[i] = a * x[i] + b * y[i];
    }
}

#define RAD(d) ((d) / 57.29578f)

static void t_incl_level_screen(void)
{
    // kart dik, sıfırlanmamış: ekranın yukarısı = aracın yukarısı
    incl_axes_t ax;
    CHECK(incl_axes(&ax, NULL, SIN, SU));
    float g[3], roll, pitch;
    incl_angles(SU, &ax, &roll, &pitch);
    CHECK_NEAR(roll, 0, 1e-3);
    CHECK_NEAR(pitch, 0, 1e-3);
    vmix(g, cosf(RAD(15)), ax.u, -sinf(RAD(15)), ax.x);     // sağ taraf 15° aşağıda: dünyanın yukarısı sola kayar
    incl_angles(g, &ax, &roll, &pitch);
    CHECK_NEAR(roll, 15, 0.01);
    CHECK_NEAR(pitch, 0, 0.01);
    vmix(g, cosf(RAD(10)), ax.u, sinf(RAD(10)), ax.f);      // burun 10° yukarıda
    incl_angles(g, &ax, &roll, &pitch);
    CHECK_NEAR(roll, 0, 0.01);
    CHECK_NEAR(pitch, 10, 0.01);
    CHECK_NEAR(ax.f[2], -1, 1e-5);                          // ileri = ekranın içi
}

static void t_incl_mounted(void)
{
    // kart 25° arkaya yatık ve ekranda 20° döndürülmüş monte; düz zeminde sıfırlanır
    const float th = RAD(20);
    float scr_up_world[3], level[3];
    vmix(scr_up_world, sinf(th), SR, cosf(th), SU);         // ekran düzlemindeki dünya yukarısı
    const float out[3] = {0, 0, 1};
    vmix(level, cosf(RAD(25)), scr_up_world, sinf(RAD(25)), out);
    incl_axes_t ax;
    CHECK(incl_axes(&ax, level, SIN, SU));
    // bağımsız kontrol: aracın gerçek sağı ekran düzleminde -20° dönmüş ekran sağıdır (arkaya yatma onu değiştirmez)
    float right[3];
    vmix(right, cosf(th), SR, -sinf(th), SU);
    CHECK_NEAR(ax.x[0] * right[0] + ax.x[1] * right[1] + ax.x[2] * right[2], 1, 1e-4);
    float roll, pitch, g[3];
    incl_angles(level, &ax, &roll, &pitch);
    CHECK_NEAR(roll, 0, 0.01);
    CHECK_NEAR(pitch, 0, 0.01);
    vmix(g, cosf(RAD(-12)), ax.u, -sinf(RAD(-12)), ax.x);   // sol taraf 12° aşağıda
    incl_angles(g, &ax, &roll, &pitch);
    CHECK_NEAR(roll, -12, 0.01);
    CHECK_NEAR(pitch, 0, 0.01);
    vmix(g, cosf(RAD(-8)), ax.u, sinf(RAD(-8)), ax.f);      // burun 8° aşağıda
    incl_angles(g, &ax, &roll, &pitch);
    CHECK_NEAR(pitch, -8, 0.01);
    CHECK_NEAR(roll, 0, 0.01);
    // sıfırlanmamış olsaydı montaj açısı eğim gibi görünürdü
    incl_axes_t raw;
    incl_axes(&raw, NULL, SIN, SU);
    incl_angles(level, &raw, &roll, &pitch);
    CHECK(fabsf(pitch) > 20);
}

static void t_incl_filter(void)
{
    incl_axes_t ax;
    incl_axes(&ax, NULL, SIN, SU);
    incl_filter_t f = {0};
    const float zero[3] = {0};
    float roll, pitch, a[3];
    // düz, durağan: hemen 0
    for (int i = 0; i < 50; i++) {
        incl_filter_step(&f, SU, zero, 0.02f);
    }
    incl_angles(f.g, &ax, &roll, &pitch);
    CHECK_NEAR(roll, 0, 0.1);

    // 50 km/h sola dönüş, sapma 0.3 rad/s: 0.43 g yanal ivme. Telafi ile eğim ~0
    const float v = 50 / 3.6f, yaw = 0.3f;
    float gyro[3];
    vmix(gyro, yaw, ax.u, 0, ax.u);
    for (int i = 0; i < 250; i++) {     // 5 sn
        vmix(a, 1, ax.u, -yaw * v / 9.80665f, ax.x);            // ivmeölçer: yukarı + merkezcil (sola)
        incl_compensate(a, gyro, &ax, v, 0);
        incl_filter_step(&f, a, gyro, 0.02f);
    }
    incl_angles(f.g, &ax, &roll, &pitch);
    CHECK(fabsf(roll) < 0.5f);
    // telafi olmasaydı ~23° yatık görünürdü
    incl_filter_t raw = {0};
    for (int i = 0; i < 250; i++) {
        vmix(a, 1, ax.u, -yaw * v / 9.80665f, ax.x);
        incl_filter_step(&raw, a, gyro, 0.02f);
    }
    incl_angles(raw.g, &ax, &roll, &pitch);
    CHECK(roll > 15);
    const float raw_roll = roll;

    // frenleme 0.5 g (hız değişimi telafisi): burun eğimi ~0
    incl_filter_t fb = {0};
    for (int i = 0; i < 150; i++) {
        vmix(a, 1, ax.u, -0.5f, ax.f);
        incl_compensate(a, zero, &ax, 20, -0.5f * 9.80665f);
        incl_filter_step(&fb, a, zero, 0.02f);
    }
    incl_angles(fb.g, &ax, &roll, &pitch);
    CHECK(fabsf(pitch) < 0.5f);

    // gerçek yan yatma: 2 sn'de 0 -> 20°, jiroskop ve ivmeölçer birlikte; sonunda 20°'ye oturur
    incl_filter_t fr = {0};
    for (int i = 0; i < 50; i++) {
        incl_filter_step(&fr, SU, zero, 0.02f);
    }
    const float rate = RAD(10);         // rad/s, ileri eksen etrafında
    for (int i = 1; i <= 150; i++) {
        const float ang = fminf(rate * i * 0.02f, RAD(20));
        vmix(a, cosf(ang), ax.u, -sinf(ang), ax.x);
        float w[3];
        vmix(w, ang < RAD(20) ? rate : 0, ax.f, 0, ax.f);      // d(g)/dt = -w x g -> w = +açısal hız * ileri
        incl_filter_step(&fr, a, w, 0.02f);
    }
    incl_angles(fr.g, &ax, &roll, &pitch);
    CHECK_NEAR(roll, 20, 0.5);

    // kartta ölçülen jiroskop sıfır kayması (X -3.3°/s): durağanken öğrenilir, eğim ivmeölçere oturur
    incl_filter_t fz = {0};
    const float drift[3] = {RAD(-3.3f), 0, 0};
    float tilted[3];
    vmix(tilted, cosf(RAD(5)), ax.u, sinf(RAD(5)), ax.f);
    for (int i = 0; i < 1500; i++) {    // 30 sn
        incl_filter_step(&fz, tilted, drift, 0.02f);
    }
    incl_angles(fz.g, &ax, &roll, &pitch);
    CHECK_NEAR(pitch, 5, 0.2);
    CHECK_NEAR(fz.bias[0], RAD(-3.3f), RAD(0.2f));
    REPORT("  eğim                 50 km/h viraj (0.43 g): telafiyle < 0.5°, telafisiz %.0f° yanlış yatma\n", raw_roll);
}

// ---------------------------------------------------------------------------
void test_entry(void)
{
    obd_start();        // muteks + ikili semafor + görev kaydı (bir kez)
    printf("obd.c host testleri\n");
    printf(" birim\n");
    run("salt okunur servis koruması (0x00-0xFF)", t_readonly_guard);
    run("istek çerçevesi KWP / ISO 9141", t_kl_build);
    run("cevap çözme KWP: sağlama, ECU kilidi, uzunluk baytı, çok çerçeve", t_kl_find_kwp);
    run("cevap çözme ISO 9141", t_kl_find_iso);
    run("PID destek haritası (Kalos BE3EB811)", t_pid_supported);
    run("değer çözme (SAE J1979 ölçekleri)", t_decode);
    run("anlık tüketim (MAF / hız-yoğunluk / PID 5E)", t_fuel_calc);
    run("ekrana sunulan kanal maskesi", t_polled_channels);
    printf(" zamanlayıcı\n");
    run("varsayılan liste, RPM her ikinci istek", t_sched_default);
    run("tek gösterge: veri isteklerin %80+'ı", t_sched_focus_single);
    run("sürüş sayfası: devir 1/2, hız 1/4, diğerleri 16'da bir", t_sched_drive);
    run("sayfa değişimi anında uygulanır", t_sched_switch_immediate);
    run("LCD + telefon istekleri birleşir", t_sched_remote_merge);
    run("tüketim girdileri hızlı turda", t_sched_fuel_inputs);
    run("uç durumlar: boş liste, tek PID, geniş bant, tam harita", t_sched_edge);
    printf(" entegrasyon (sanal zaman, gerçek obd_task)\n");
    run("fast init, PID taraması, canlı veri, profil", t_int_connect);
    run("istek süresi ve oturum (P3max)", t_int_request_period);
    run("sayfa geçişi: sürüş -> tek gösterge", t_int_page_switch);
    run("sürüş sayfası yenilenme hızları", t_int_drive);
    run("tüm veriler listesi yenilenme hızı", t_int_list_page);
    run("cevapsız PID profilden düşer", t_int_mute_pid);
    run("kontak kapanıp açılır: yeniden bağlanır", t_int_ignition_cycle);
    run("iki ECU cevap veriyor: kilitli ECU", t_int_two_ecus);
    run("arıza kodu oku (7F 78 bekletmeli) / sil", t_int_dtc);
    run("hatta sadece 81 ve 01 servisleri", t_int_readonly_wire);
    run("donanım arızası: RX yolu kopuk", t_int_rx_broken);
    run("kontak kapalı: ECU sessiz, açılınca bağlanır", t_int_ecu_silent);
    run("yeniden tarama", t_int_rescan);
    printf(" eğim (imu_math)\n");
    run("dik kart: yan yatma ve burun eğimi", t_incl_level_screen);
    run("yatık + döndürülmüş montaj, düz zeminde sıfır", t_incl_mounted);
    run("filtre: viraj ve fren telafisi, gerçek yatma", t_incl_filter);

    printf("\n ölçümler (sanal zaman, 10400 baud, ECU P2 25 ms)\n%s", s_report);
    printf("\n %d test, %d kontrol: %s\n", s_tests, s_checks,
           s_fail ? "BAŞARISIZ" : "hepsi geçti");
    if (s_fail) {
        printf(" %d test / %d kontrol başarısız\n", s_failed_tests, s_fail);
    }
    fflush(NULL);
    exit(s_fail ? 1 : 0);
}
