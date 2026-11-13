// lv_font_conv çıktısında rakamları sabit genişliğe (tabular) getirir: değer değişince sayı yerinde titremez.
// Her rakamın ilerleme genişliği en genişine eşitlenir, glif aradaki farkın yarısı kadar sağa kaydırılarak ortalanır.
//   node tools/font_tnum.js main/ui_font_big.c
const fs = require('fs');
const path = process.argv[2];
let src = fs.readFileSync(path, 'utf8');

// cmap kayıtları: hangi glif kimliği hangi kod noktası
const cmaps = [...src.matchAll(/\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+), \.list_length = \d+, \.type = (\w+)/g)];
const list = name => {
  const m = src.match(new RegExp(name + '\\[\\] = \\{([^}]*)\\}'));
  return m[1].split(',').map(s => s.trim()).filter(Boolean).map(s => parseInt(s));
};
const gid = cp => {
  for (const c of cmaps) {
    const [, start, len, g0, ulist, olist, type] = c;
    const s = +start, n = +len;
    if (cp < s || cp >= s + n) continue;
    if (type.endsWith('FORMAT0_TINY')) return +g0 + (cp - s);
    if (type.endsWith('FORMAT0_FULL')) return +g0 + list(olist)[cp - s];
    const i = list(ulist).indexOf(cp - s);      // SPARSE: kod noktası farkları listesi
    if (i >= 0) return +g0 + i;
  }
  return -1;
};

const lines = src.split('\n');
const first = lines.findIndex(l => l.includes('glyph_dsc[] = {'));
const dsc = id => first + 1 + id;      // 0. satır "reserved"
const re = /\.adv_w = (\d+),.*\.ofs_x = (-?\d+)/;
const digits = [...'0123456789'].map(ch => gid(ch.charCodeAt(0)));
if (digits.some(g => g < 0)) { console.error('rakam glifleri bulunamadı: ' + path); process.exit(1); }
const adv = digits.map(g => +lines[dsc(g)].match(re)[1]);
const max = Math.max(...adv);
digits.forEach((g, i) => {
  const shift = Math.round((max - adv[i]) / 16 / 2);
  lines[dsc(g)] = lines[dsc(g)].replace(re, (m, a, x) => m.replace(`.adv_w = ${a}`, `.adv_w = ${max}`).replace(`.ofs_x = ${x}`, `.ofs_x = ${+x + shift}`));
});
fs.writeFileSync(path, lines.join('\n'));
console.log(`${path}: rakamlar ${max / 16} px sabit genişlik`);
