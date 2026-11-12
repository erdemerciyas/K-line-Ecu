/*
 * Arıza kodları sayfası: kayıtlı kodları (OBD 03) açıklamalarıyla listeler, SİL (OBD 04) iki dokunuşla onaylanır.
 * ECU cevabı birkaç saniye sürebildiği için istek ayrı bir görevde çalışır, LVGL görevi beklemez.
 * Sayfa ilk açıldığında bağlantı varsa kodlar kendiliğinden okunur.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "ui_priv.h"

#define CONFIRM_US  (4 * 1000 * 1000)

// ortak açıklamalar: aynı neden metni birkaç kodda kullanılır
#define D_INJ   "Enjektör soketi gevşek, kablo kopuk/kısa devre veya enjektör bobini arızalı. Enjektör direncini ölç, soketi kontrol et."
#define D_MIS   "Bu silindirin bujisi, buji kablosu, ateşleme bobini veya enjektörü. Buji/kabloyu başka silindirle değiştirip arıza yer değiştiriyor mu bak; gerekirse kompresyon ölç."
#define D_INT   "Aralıklı (gelip giden) arıza: soket temassızlığı veya kablo sürtünmesi. Motor çalışırken soketi ve kabloları oynatarak kontrol et. MIL yakmaz."
#define D_RR    "ABS'den gelen bozuk yol bilgisi (tekleme algısını düzeltmek için) alınamıyor. ABS beyni ve kablolarını kontrol et. MIL yakmaz."
#define D_EGRV  "EGR valfi soketi, kablo kopukluğu/kısa devre veya valfin konum sensörü arızalı. Valf karbonla sıkışmış olabilir."
#define D_ECU   "Motor beyni (ECU) iç hatası. Önce akü, şase ve ECU beslemelerini kontrol et; kod kalıyorsa ECU onarım/değişim gerekir."

/*
 * Kod açıklamaları: Chevrolet Kalos 2005 1.4 SOHC/DOHC servis kılavuzu DTC tablosu (P0106...P1655, P1xxx üreticiye özel)
 * + sık görülen genel OBD-II kodları. title kısa başlık, detail olası nedenler ve kontrol.
 */
static const struct {
    uint16_t code;
    const char *title;
    const char *detail;
} DTC_TXT[] = {
    {0x0100, "Hava akış (MAF) devresi", "Bu motorda MAF yok, MAP kullanılır. Kod geliyorsa yanlış okuma olabilir; tekrar oku."},
    {0x0105, "MAP sensörü devresi", "MAP sensörü soketi, 5V besleme, şase veya sinyal kablosu. Vakum hortumunu da kontrol et."},
    {0x0106, "MAP sensörü mantık hatası", "MAP değeri gaz kelebeği değişimine uymuyor. Vakum hortumu çatlak/çıkmış, emme kaçağı, tıkalı MAP girişi veya arızalı MAP sensörü."},
    {0x0107, "MAP sensörü düşük voltaj", "Sinyal kablosu kopuk veya şaseye kısa, 5V besleme yok ya da sensör arızalı. ECU sabit MAP değeriyle çalışır, motor zayıflar."},
    {0x0108, "MAP sensörü yüksek voltaj", "Sinyal 5V'a kısa, sensör şasesi kopuk, vakum hortumu çıkmış veya sensör arızalı."},
    {0x0110, "Emme havası sıcaklık sensörü devresi", "IAT sensörü soketi, kablo veya sensör arızası."},
    {0x0112, "Emme havası sıcaklık sensörü düşük voltaj", "Sensör aşırı sıcak gösteriyor: sinyal kablosu şaseye kısa veya sensör içten kısa devre."},
    {0x0113, "Emme havası sıcaklık sensörü yüksek voltaj", "Sensör aşırı soğuk gösteriyor: soket çıkmış, kablo kopuk veya sensör açık devre."},
    {0x0115, "Hararet sensörü devresi", "Hararet (ECT) sensörü soketi, kablosu veya sensörü arızalı."},
    {0x0117, "Hararet sensörü düşük voltaj", "Motor aşırı sıcak görünüyor: sensör kablosu şaseye kısa veya sensör arızalı. Fan sürekli çalışabilir."},
    {0x0118, "Hararet sensörü yüksek voltaj", "Motor buz gibi görünüyor: soket çıkmış, kablo kopuk veya sensör arızalı. Rölanti yüksek, yakıt tüketimi fazla olur."},
    {0x0120, "Gaz kelebeği sensörü (TPS) devresi", "TPS soketi, 5V besleme, şase veya sinyal kablosu arızalı."},
    {0x0121, "TPS aralık/performans", "TPS değeri MAP ve devirle uyumsuz: sensör aşınmış veya kelebek gövdesi kirli."},
    {0x0122, "Gaz kelebeği sensörü (TPS) düşük voltaj", "TPS sinyali kopuk veya şaseye kısa ya da 5V besleme yok. Soket, kablo ve TPS'yi kontrol et."},
    {0x0123, "Gaz kelebeği sensörü (TPS) yüksek voltaj", "TPS sinyali 5V'a kısa veya TPS şasesi kopuk. Sensör aşınmış olabilir."},
    {0x0125, "Motor kapalı çevrime ısınmıyor", "Termostat açık kalmış veya hararet sensörü yanlış okuyor."},
    {0x0130, "Ön O2 (lambda) sensörü devresi", "Ön O2 sensörü soketi, kablosu veya sensör arızası."},
    {0x0131, "Ön O2 (lambda) sensörü düşük voltaj", "Sürekli fakir okuyor: egzoz manifoldu kaçağı, emme kaçağı, sinyal kablosu şaseye kısa veya sensör ölmüş."},
    {0x0132, "Ön O2 (lambda) sensörü yüksek voltaj", "Sürekli zengin okuyor: sinyal kablosu voltaja kısa, yakıt basıncı yüksek, damlatan enjektör veya kirli sensör."},
    {0x0133, "Ön O2 (lambda) sensörü yavaş tepki", "Sensör yaşlanmış veya yağ/antifriz ile kirlenmiş. Egzoz kaçağı da yapabilir. Çoğunlukla sensör değişir."},
    {0x0134, "Ön O2 sensörü sinyal yok / açık devre", "Sensör hiç sinyal vermiyor: soket çıkmış, kablo kopuk veya ısıtıcısı yanmış sensör soğuk kalıyor."},
    {0x0135, "Ön O2 sensörü ısıtıcı devresi arızalı", "Isıtıcı yanmış veya beslemesi yok. Sigortayı, soketi ve ısıtıcı direncini kontrol et."},
    {0x0136, "Arka O2 sensörü devresi", "Katalizör sonrası O2 sensörü soketi, kablosu veya sensör arızası."},
    {0x0137, "Arka O2 sensörü düşük voltaj", "Katalizör sonrası sensör sürekli fakir: egzoz kaçağı, kablo şaseye kısa veya sensör arızalı."},
    {0x0138, "Arka O2 sensörü yüksek voltaj", "Sinyal kablosu voltaja kısa veya sensör arızalı."},
    {0x0140, "Arka O2 sensörü sinyal yok / açık devre", "Soket çıkmış, kablo kopuk veya sensör arızalı."},
    {0x0141, "Arka O2 sensörü ısıtıcı devresi arızalı", "Isıtıcı yanmış, sigorta atmış veya besleme yok."},
    {0x0170, "Yakıt düzeltme arızası", "Karışım ayarı sınırda: emme kaçağı, yakıt basıncı veya O2 sensörü."},
    {0x0171, "Karışım çok fakir", "Emme/vakum kaçağı, tıkalı yakıt filtresi, zayıf yakıt pompası, kirli enjektör veya hatalı MAP sensörü."},
    {0x0172, "Karışım çok zengin", "Yakıt basıncı yüksek (regülatör), damlatan enjektör, tıkalı hava filtresi veya açık kalan kanister valfi."},
    {0x0201, "1. enjektör devresi", D_INJ},
    {0x0202, "2. enjektör devresi", D_INJ},
    {0x0203, "3. enjektör devresi", D_INJ},
    {0x0204, "4. enjektör devresi", D_INJ},
    {0x0230, "Yakıt pompası rölesi devresi", "Yakıt pompası rölesi, sigortası veya röle kablosu arızalı. Motor çalışmayabilir."},
    {0x0261, "1. enjektör devresi düşük", D_INJ},
    {0x0264, "2. enjektör devresi düşük", D_INJ},
    {0x0267, "3. enjektör devresi düşük", D_INJ},
    {0x0270, "4. enjektör devresi düşük", D_INJ},
    {0x0300, "Çoklu silindir tekleme", "Birden fazla silindir tekliyor: bujiler, buji kabloları, ateşleme bobini, düşük yakıt basıncı veya vakum kaçağı. Katalizöre zarar verir, uzun süre böyle sürme."},
    {0x0301, "1. silindir tekleme", D_MIS},
    {0x0302, "2. silindir tekleme", D_MIS},
    {0x0303, "3. silindir tekleme", D_MIS},
    {0x0304, "4. silindir tekleme", D_MIS},
    {0x0317, "Bozuk yol sensörü bulunamadı", D_RR},
    {0x0325, "Vuruntu sensörü iç arıza", "Vuruntu sensörü veya ECU içindeki vuruntu devresi. Ateşleme avansı geri çekilir, güç düşer. MIL yakmaz."},
    {0x0327, "Vuruntu sensörü devresi", "Sensör kablosu kopuk/kısa, sensör gevşek veya arızalı. Sensörün bloğa sıkılığını kontrol et. MIL yakmaz."},
    {0x0328, "Vuruntu sensörü yüksek voltaj", "Sensör kablosu kısa devre veya sensör arızalı."},
    {0x0335, "Krank sensörü devresi", "Krank konum sensörü, soketi veya kablosu. Motor çalışmayabilir ya da stop edebilir."},
    {0x0336, "Krank sensörü fazla/eksik sinyal", "58 dişli krank sinyali bozuk: sensör kirli/gevşek, dişli hasarlı veya kablo buji kablolarından parazit alıyor."},
    {0x0337, "Krank sensörü sinyal yok", "Krank sinyali yok: motor çalışmaz veya stop eder. Sensörü, soketi ve kabloyu kontrol et."},
    {0x0340, "Eksantrik sensörü devresi", "Eksantrik konum sensörü, soketi veya kablosu arızalı."},
    {0x0341, "Eksantrik sensörü mantık hatası", "Eksantrik sinyali krank sinyaliyle uyumsuz: triger kayışı diş atlamış olabilir, sensör veya kablo arızalı."},
    {0x0342, "Eksantrik (kam) konum sensörü sinyal yok", "ECU eksantrik sinyalini görmüyor. Sensör soketi çıkmış/oksitlenmiş, kablo kopuk veya kısa, sensör arızalı ya da beslemesi yok. Motor genelde çalışır ama ilk çalıştırma uzayabilir. MIL yakar (tip A)."},
    {0x0351, "Ateşleme devresi A (1. ve 4. silindir)", "Bobin paketinin 1-4 çıkışı: bobin soketi, kablo veya ateşleme bobini arızalı. Tekleme ile birlikte gelir."},
    {0x0352, "Ateşleme devresi B (2. ve 3. silindir)", "Bobin paketinin 2-3 çıkışı: bobin soketi, kablo veya ateşleme bobini arızalı. Tekleme ile birlikte gelir."},
    {0x0400, "EGR akış arızası", "EGR valfi veya kanalları karbonla tıkalı ya da valf arızalı."},
    {0x0401, "EGR akışı yetersiz", "EGR valfi veya kanalları karbonla tıkalı. Valfi ve kanalı temizle. MIL yakmaz."},
    {0x0402, "EGR akışı fazla", "EGR valfi açık kalmış. Rölantide sarsıntı ve stop etme yapar. Valfi temizle/değiştir."},
    {0x0404, "EGR açık konum hatası", "EGR valfi istenen konuma gelmiyor: karbon birikmesi veya valf arızası."},
    {0x0405, "EGR konum sensörü düşük voltaj", D_EGRV},
    {0x0406, "EGR konum sensörü yüksek voltaj", D_EGRV},
    {0x0420, "Katalizör verimi düşük", "Katalizör yaşlanmış/erimiş ya da arka O2 sensörü arızalı. Önce egzoz kaçağı ve tekleme olmadığından emin ol."},
    {0x0441, "Kanister (EVAP) akış hatası", "Kanister valfi, hortumları veya depo kapağı."},
    {0x0443, "Kanister (EVAP) valfi devresi", "Kanister boşaltma valfi soketi, kablosu veya valf bobini arızalı."},
    {0x0461, "Yakıt seviye sensörü takılı", "Depo şamandırası takılmış veya seviye sensörü arızalı. MIL yakmaz."},
    {0x0462, "Yakıt seviye sensörü düşük voltaj", "Seviye sensörü kablosu şaseye kısa veya sensör arızalı. MIL yakmaz."},
    {0x0463, "Yakıt seviye sensörü yüksek voltaj", "Seviye sensörü kablosu kopuk veya sensör açık devre. MIL yakmaz."},
    {0x0480, "Fan rölesi devresi", "Radyatör fanı rölesi, sigortası veya kablosu. Hararet yapabilir, fanı kontrol et."},
    {0x0500, "Araç hız sensörü", "Şanzımandaki hız sensörü, soketi veya kablosu."},
    {0x0501, "Hız sensörü aralık/performans", "Hız sinyali düzensiz: sensör, soket veya kablo."},
    {0x0502, "Araç hız sensörü sinyal yok", "Şanzımandaki hız sensörü, soketi veya kablosu. Kilometre saati de çalışmıyor olabilir."},
    {0x0505, "Rölanti kontrol sistemi", "Rölanti (IAC) valfi kirli/arızalı veya kablosu."},
    {0x0506, "Rölanti istenenden düşük", "Rölanti (IAC) valfi kirli/takılı veya gaz kelebeği gövdesi kirli. Kelebek gövdesini ve IAC valfini temizle."},
    {0x0507, "Rölanti istenenden yüksek", "Emme/vakum kaçağı, IAC valfi açık kalmış veya gaz teli gergin."},
    {0x0530, "Klima basınç sensörü devresi", "Klima basınç sensörü soketi, kablosu veya sensör."},
    {0x0532, "Klima basınç sensörü düşük voltaj", "Klima gazı azalmış, sensör kablosu kısa devre veya sensör arızalı. MIL yakmaz."},
    {0x0533, "Klima basınç sensörü yüksek voltaj", "Sensör kablosu kopuk, sensör arızalı veya sistem basıncı aşırı. MIL yakmaz."},
    {0x0560, "Sistem voltajı", "Akü, şarj dinamosu veya şase bağlantıları."},
    {0x0562, "Sistem voltajı düşük", "Şarj dinamosu, kayış, akü, akü kutup başları ve şase bağlantısını kontrol et. MIL yakmaz."},
    {0x0563, "Sistem voltajı yüksek", "Dinamo regülatörü (konjektör) arızalı olabilir. Motor çalışırken akü voltajını ölç. MIL yakmaz."},
    {0x0601, "ECU bellek (checksum) hatası", "ECU yazılımı bozulmuş. ECU yeniden programlanmalı veya değişmeli."},
    {0x0602, "ECU programlanmamış", D_ECU},
    {0x0604, "ECU RAM hatası", D_ECU},
    {0x0605, "ECU ROM hatası", D_ECU},
    {0x0607, "ECU düşük güç sayacı hatası", D_ECU},
    {0x0700, "Otomatik şanzıman beyni arızası", "Şanzıman beyninde (TCM) kayıtlı kod var. Manuel şanzımanlı araçta beklenmez; tekrar oku."},
    {0x1106, "MAP sensörü aralıklı yüksek voltaj", D_INT},
    {0x1107, "MAP sensörü aralıklı düşük voltaj", D_INT},
    {0x1111, "Emme havası sıcaklık aralıklı yüksek voltaj", D_INT},
    {0x1112, "Emme havası sıcaklık aralıklı düşük voltaj", D_INT},
    {0x1114, "Hararet sensörü aralıklı düşük voltaj", D_INT},
    {0x1115, "Hararet sensörü aralıklı yüksek voltaj", D_INT},
    {0x1121, "TPS aralıklı yüksek voltaj", D_INT},
    {0x1122, "TPS aralıklı düşük voltaj", D_INT},
    {0x1133, "Ön O2 sensörü az geçiş yapıyor", "Sensör zengin/fakir arasında yeterince gidip gelmiyor: yaşlanmış sensör veya egzoz kaçağı."},
    {0x1134, "Ön O2 sensörü geçiş oranı hatalı", "Zengin-fakir geçiş süreleri dengesiz: sensör yaşlanmış veya kirlenmiş."},
    {0x1167, "Ön O2 yakıt kesmede zengin", "Motor freninde yakıt kesikken sensör zengin gösteriyor: damlatan enjektör, yüksek yakıt basıncı veya sensör arızası."},
    {0x1171, "Hızlanmada karışım fakir", "Gaza yüklenince karışım fakirleşiyor: zayıf yakıt pompası, tıkalı yakıt filtresi veya düşük yakıt basıncı."},
    {0x1336, "Krank diş hatası öğrenilmemiş", "Krank dişli öğrenmesi yapılmamış (ECU, sensör veya volan değişiminden sonra). Servis cihazıyla öğretilmeli."},
    {0x1380, "Bozuk yol verisi geçersiz", D_RR},
    {0x1381, "Bozuk yol seri veri hatası", D_RR},
    {0x1391, "G sensörü (bozuk yol) mantık hatası", D_RR},
    {0x1392, "G sensörü (bozuk yol) düşük voltaj", D_RR},
    {0x1393, "G sensörü (bozuk yol) yüksek voltaj", D_RR},
    {0x1404, "EGR kapalı konum hatası", "EGR valfi tam kapanmıyor: karbon birikmesi veya valf arızası. Valfi temizle/değiştir."},
    {0x1601, "ECU - şanzıman beyni iletişim hatası", "Motor beyni ile şanzıman beyni arasında SPI iletişimi kesik. Manuel araçta ECU iç hatası olabilir."},
    {0x1607, "ECU düşük güç sayacı sıfırlandı", "ECU beslemesi kesilmiş (akü sökülmesi, sigorta vb.). Genelde önemsiz. MIL yakmaz."},
    {0x1626, "İmmobilizer cevap yok", "Anahtar çipi okunamadı: anahtar çipi, kontak etrafındaki anten bobini veya immobilizer kutusu. Motor çalışmayabilir."},
    {0x1631, "İmmobilizer yanlış cevap", "Anahtar tanınmadı: öğretilmemiş anahtar veya ECU-immobilizer eşleşmesi bozuk. Yedek anahtarla dene."},
    {0x1650, "ECU iç sürücü (SIDM) hatası", D_ECU},
    {0x1655, "ECU iç sürücü (PSVI) hatası", D_ECU},
};

static struct {
    lv_obj_t *status;
    lv_obj_t *list;
    lv_obj_t *clear_lbl;
    int64_t confirm_t;          // SİL'e ilk dokunuş, 0 = onay beklenmiyor
    bool read_once;             // bu bağlantıda kendiliğinden okundu
    uint16_t rpm;
    // istek görevi: busy iken sadece görev yazar, done olunca sadece LVGL okur
    volatile bool busy;
    volatile bool done;
    bool result;                // gösterilecek bir sonuç var (sayfa yeniden kurulunca da)
    bool cleared;
    bool clear_ok;
    int n;
    uint16_t codes[VEMS_DTC_MAX];
} t;

/** Kodun başlığı ve detayı; tabloda yoksa genel bir açıklama. */
static void dtc_text(uint16_t c, const char **title, const char **detail)
{
    for (size_t i = 0; i < sizeof(DTC_TXT) / sizeof(DTC_TXT[0]); i++) {
        if (DTC_TXT[i].code == c) {
            *title = DTC_TXT[i].title;
            *detail = DTC_TXT[i].detail;
            return;
        }
    }
    const bool maker = (c & 0x3000) == 0x1000 || (c & 0x3000) == 0x3000;
    *title = maker ? "Üreticiye özel kod" : "Tanımsız kod";
    *detail = "Bu kod Kalos 1.4 servis kılavuzu listesinde yok. Kodu not alıp servis cihazıyla kontrol ettir.";
}

static void status_set(const char *txt, lv_color_t col)
{
    ui_label_set(t.status, txt);
    lv_obj_set_style_text_color(t.status, col, 0);
}

static void worker(void *arg)
{
    t.cleared = arg != NULL;
    t.clear_ok = t.cleared && vems_dtc_clear();
    t.n = (!t.cleared || t.clear_ok) ? vems_dtc_read(t.codes, VEMS_DTC_MAX) : -1;
    t.done = true;
    t.busy = false;
    vTaskDelete(NULL);
}

static void start(bool clear)
{
    if (t.busy) {
        return;
    }
    t.busy = true;
    t.done = false;
    status_set(clear ? "SİLİNİYOR…" : "OKUNUYOR…", C_WARN);
    if (xTaskCreate(worker, "dtc", 3072, clear ? (void *)1 : NULL, 3, NULL) != pdPASS) {
        t.busy = false;
        status_set("BELLEK YOK", C_ALERT);
    }
}

static void show_result(void)
{
    lv_obj_clean(t.list);
    if (t.cleared && !t.clear_ok) {
        status_set("ECU SİLMEYİ ONAYLAMADI", C_ALERT);
        return;
    }
    if (t.n < 0) {
        status_set("ECU CEVAP VERMEDİ", C_ALERT);
        return;
    }
    char buf[40];
    if (t.n == 0) {
        status_set(t.cleared ? "SİLİNDİ · KAYITLI KOD YOK" : "KAYITLI ARIZA KODU YOK", C_OK);
        return;
    }
    snprintf(buf, sizeof(buf), "%d KAYITLI KOD", t.n);
    status_set(buf, C_WARN);
    for (int i = 0; i < t.n; i++) {
        lv_obj_t *row = lv_obj_create(t.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(row, 7, 0);
        lv_obj_set_style_pad_left(row, 6, 0);
        lv_obj_set_style_pad_row(row, 4, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, C_LINE, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        // üst satır: kod + başlık, alt satır: tam genişlikte olası nedenler
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);

        const char *title, *detail;
        dtc_text(t.codes[i], &title, &detail);
        // üst 2 bit sistem harfi, kalan 14 bit 4 hane: 0x0301 -> P0301
        snprintf(buf, sizeof(buf), "%c%04X", "PCBU"[t.codes[i] >> 14], t.codes[i] & 0x3FFF);
        lv_obj_t *code = ui_label(row, &ui_font_label_lg, C_WARN, buf);
        lv_obj_set_width(code, 90);
        lv_obj_t *txt = ui_label(row, &ui_font_label, C_SOFT, title);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(txt, 226);
        lv_obj_set_style_pad_top(txt, 3, 0);
        lv_obj_t *det = ui_label(row, &ui_font_label, C_MUTED, detail);
        lv_label_set_long_mode(det, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(det, 320);
    }
}

static void read_cb(lv_event_t *e)
{
    t.confirm_t = 0;
    ui_label_set(t.clear_lbl, "SİL");
    start(false);
}

static void clear_cb(lv_event_t *e)
{
    if (t.busy) {
        return;
    }
    if (t.rpm > 300) {
        status_set("MOTOR ÇALIŞIYOR · ÖNCE DURDUR", C_ALERT);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (t.confirm_t == 0 || now - t.confirm_t > CONFIRM_US) {
        t.confirm_t = now;
        ui_label_set(t.clear_lbl, "EMİN MİSİN?");
        status_set("SİLMEK İÇİN TEKRAR BAS", C_WARN);
        return;
    }
    t.confirm_t = 0;
    ui_label_set(t.clear_lbl, "SİL");
    start(true);
}

lv_obj_t *ui_dtc_create(int page, int pages)
{
    lv_obj_t *scr = ui_screen_create();
    ui_header(scr, "ARIZA KODLARI", 66);

    t.status = ui_label(scr, &ui_font_label, C_MUTED, "");
    lv_obj_set_style_text_letter_space(t.status, 1, 0);
    lv_obj_align(t.status, LV_ALIGN_TOP_MID, 0, 96);

    t.list = lv_obj_create(scr);
    lv_obj_remove_style_all(t.list);
    lv_obj_set_size(t.list, 332, 208);
    lv_obj_align(t.list, LV_ALIGN_TOP_LEFT, 74, 124);
    lv_obj_set_flex_flow(t.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(t.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(t.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(t.list, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *rd = ui_pill_button(scr, "OKU", 128);
    lv_obj_align(rd, LV_ALIGN_TOP_MID, -72, 346);
    lv_obj_add_event_cb(rd, read_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = ui_pill_button(scr, "SİL", 128);
    lv_obj_align(cl, LV_ALIGN_TOP_MID, 72, 346);
    lv_obj_add_event_cb(cl, clear_cb, LV_EVENT_CLICKED, NULL);
    t.clear_lbl = lv_obj_get_child(cl, 0);

    ui_page_dots(scr, page, pages);
    t.confirm_t = 0;
    if (t.result || t.done) {
        show_result();          // sayfalar yeniden kurulunca son sonuç kalsın
    } else {
        status_set(t.busy ? "OKUNUYOR…" : "OKU'YA BAS", t.busy ? C_WARN : C_MUTED);
    }
    return scr;
}

void ui_dtc_update(const vems_data_t *d, bool fresh)
{
    t.rpm = fresh ? d->rpm : 0;
    if (t.done && !t.busy) {
        show_result();
        t.done = false;
        t.result = true;
        t.read_once = true;
    }
    if (t.confirm_t && esp_timer_get_time() - t.confirm_t > CONFIRM_US) {
        t.confirm_t = 0;
        ui_label_set(t.clear_lbl, "SİL");
        status_set("", C_MUTED);
    }
    const bool on = vems_get_state() == VEMS_STATE_ECU_OK;
    if (!on) {
        t.read_once = false;    // yeni bağlantıda tekrar kendiliğinden oku
    } else if (!t.read_once && !t.busy) {
        t.read_once = true;
        start(false);
    }
}
