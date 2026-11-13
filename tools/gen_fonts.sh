#!/bin/sh
# Arayüz yazıları: Barlow Semi Condensed (SIL OFL 1.1, fonts.google.com/specimen/Barlow+Semi+Condensed).
# TTF'ler indirilir, lv_font_conv ile main/ui_font_*.c üretilir, rakamlar sabit genişliğe getirilir.
#   sh tools/gen_fonts.sh
set -e
cd "$(dirname "$0")/.."
TMP=${TMPDIR:-/tmp}/barlow
mkdir -p "$TMP"
for w in Medium SemiBold Bold; do
    [ -f "$TMP/BSC-$w.ttf" ] || curl -sSfL -o "$TMP/BSC-$w.ttf" \
        "https://github.com/google/fonts/raw/main/ofl/barlowsemicondensed/BarlowSemiCondensed-$w.ttf"
done
CONV="npx --yes lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --format lvgl --lv-include lvgl.h"
TR="0xB0,0xB7,0xC7,0xD6,0xDC,0xE7,0xF6,0xFC,0x11E,0x11F,0x130,0x131,0x15E,0x15F,0x2026,0x203A"
NUM="0x25,0x2B-0x2E,0x30-0x3A,0xB0"

# sayılar: kerning yok (rakam çiftleri kaymasın)
$CONV --no-kerning --font "$TMP/BSC-SemiBold.ttf" -r $NUM --size 104 -o main/ui_font_big.c
$CONV --no-kerning --font "$TMP/BSC-SemiBold.ttf" -r $NUM --size 34 -o main/ui_font_value.c
# yazılar: Latin + Türkçe harfler, ° · … ›
$CONV --font "$TMP/BSC-SemiBold.ttf" -r 0x20-0x7E -r $TR --size 16 -o main/ui_font_label.c
$CONV --font "$TMP/BSC-SemiBold.ttf" -r 0x20-0x7E -r $TR --size 22 -o main/ui_font_label_lg.c
$CONV --font "$TMP/BSC-SemiBold.ttf" -r 0x20-0x7E -r $TR --size 30 -o main/ui_font_title.c

for f in big value label label_lg title; do
    node tools/font_tnum.js main/ui_font_$f.c
done

# yazılarda eksik semboller (LV_SYMBOL_*) Montserrat'tan; lisans satırı başlığa
fallback() { sed -i "s|^    .dsc = &font_dsc |    .fallback = \&lv_font_montserrat_$2,     /*LV_SYMBOL_* and other missing glyphs*/\n    .dsc = \&font_dsc |" "main/ui_font_$1.c"; }
fallback label 14
fallback label_lg 16
fallback title 24
for f in big value label label_lg title; do
    sed -i '2i\ * Barlow Semi Condensed (c) The Barlow Project Authors, SIL Open Font License 1.1 (fonts.google.com/specimen/Barlow+Semi+Condensed)' "main/ui_font_$f.c"
done
