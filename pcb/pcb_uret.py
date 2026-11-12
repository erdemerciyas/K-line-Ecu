# K-Line OBD adaptör kartı: tek yüzlü (alt bakır) THT PCB üreteci.
# Çıktılar: Gerber + Excellon (fabrika), 1:1 SVG/PDF (yollar, delme, yerleşim).
# Çalıştır: py pcb/pcb_uret.py
import math, os, zipfile

OUT = os.path.dirname(os.path.abspath(__file__))
U = 2.54                      # ızgara adımı (mm)
OX, OY = 5.54, 6.3            # ızgara (0,0) noktasının kart üzerindeki yeri
W, H = 74.5, 38.0             # kart ölçüsü (mm)
CLR_MIN = 0.4                 # en küçük bakır aralığı (ev yapımı için güvenli)

def G(x, y):
    return (OX + U * x, OY + U * y)

# ---------------------------------------------------------------- parçalar
# pad: (ref, pin, isim, net, (x,y) ızgara, şekil, boyut, delik)
#   şekil: C=yuvarlak (çap), R=kare (kenar), O=oval (gx, gy)
PADS = []
def pad(ref, pin, name, net, xy, shape, size, drill):
    PADS.append(dict(ref=ref, pin=pin, name=name, net=net, p=G(*xy), shape=shape, size=size, drill=drill))

# J2 ekran kablosu (Waveshare UART başlığına)
for i, (n, net) in enumerate([("RX", "RXL"), ("3V3", "3V3"), ("GND", "GND"), ("TX", "TXL")]):
    pad("J2", i + 1, n, net, (1, 5 + i), "R" if i == 0 else "C", 1.8, 1.0)
# U3 BSS138 seviye dönüştürücü modülü (LV sırası x=4, HV sırası x=8)
for i, (n, net) in enumerate([("LV1", "NC1"), ("LV2", "RXL"), ("LV", "3V3"), ("GND", "GND"), ("LV3", "TXL"), ("LV4", "NC2")]):
    pad("U3", "L%d" % i, n, net, (4, 4 + i), "C", 1.8, 1.0)
for i, (n, net) in enumerate([("HV1", "NC3"), ("HV2", "RXH"), ("HV", "5V"), ("GND", "GND"), ("HV3", "TXH"), ("HV4", "NC4")]):
    pad("U3", "H%d" % i, n, net, (8, 4 + i), "C", 1.8, 1.0)
# U1 L9637D (SO8→DIP adaptörü), pin 1-4 x=12, pin 8-5 x=15
for pin, n, net, xy in [(1, "RX", "RXH", (12, 5)), (2, "LO", "NC5", (12, 6)), (3, "VCC", "5V", (12, 7)), (4, "TX", "TXH", (12, 8)),
                        (5, "GND", "GND", (15, 8)), (6, "K", "K", (15, 7)), (7, "VS", "VBAT", (15, 6)), (8, "LI", "NC6", (15, 5))]:
    pad("U1", pin, n, net, xy, "R" if pin == 1 else "O", 1.6 if pin == 1 else (2.4, 1.6), 1.0)
# U2 78L05 (TO-92, düz yüz aşağı/çipe bakar): OUT, GND, IN
for pin, n, net, x in [(1, "OUT", "5V", 10), (2, "GND", "GND", 11), (3, "IN", "VBAT", 12)]:
    pad("U2", pin, n, net, (x, 3), "O", (1.5, 2.2), 0.8)
# kondansatörler, diyot, dirençler
pad("C2", 1, "", "GND", (8, 0), "C", 1.8, 0.8); pad("C2", 2, "", "5V", (8, 2), "C", 1.8, 0.8)
pad("C1", 1, "", "GND", (14, 0), "C", 1.8, 0.8); pad("C1", 2, "", "VBAT", (14, 2), "C", 1.8, 0.8)
pad("D1", 1, "K", "VBAT", (18, 0), "R", 1.8, 1.0); pad("D1", 2, "A", "V12", (22, 0), "C", 2.0, 1.0)
pad("R1", 1, "", "VBAT", (18, 2), "C", 1.8, 0.8); pad("R1", 2, "", "K", (18, 6), "C", 1.8, 0.8)
pad("R2", 1, "", "VBAT", (20, 2), "C", 1.8, 0.8); pad("R2", 2, "", "K", (20, 6), "C", 1.8, 0.8)
# J1 OBD kablosu: pin 16, 7, 4, 5
for i, (n, net) in enumerate([("+12V", "V12"), ("K", "K"), ("GND", "GND"), ("GND", "GND")]):
    pad("J1", i + 1, n, net, (24, 5 + i), "R" if i == 0 else "C", 1.8, 1.0)

MOUNT = [(3.5, 3.5), (W - 3.5, 3.5), (3.5, H - 3.5), (W - 3.5, H - 3.5)]   # M3, kaplamasız

# ---------------------------------------------------------------- yollar (ızgara)
WP, WS = 1.0, 0.8             # güç / sinyal yol genişliği
TRACKS = [
    ("GND", WP, [(1, 7), (8, 7)]),
    ("GND", WP, [(6, 7), (6, 0), (14, 0)]),
    ("GND", WP, [(11, 0), (11, 3)]),
    ("GND", WP, [(6, 7), (6, 10), (24, 10), (24, 7)]),
    ("GND", WP, [(15, 8), (15, 10)]),
    ("5V", WS, [(8, 6), (7, 6), (7, 2), (10, 2), (10, 3)]),
    ("5V", WS, [(8, 6), (10, 6), (11, 7), (12, 7)]),
    ("VBAT", WP, [(12, 3), (12, 2), (20, 2)]),
    ("VBAT", WP, [(16, 2), (16, 6), (15, 6)]),
    ("VBAT", WP, [(18, 0), (18, 2)]),
    ("V12", WP, [(22, 0), (24, 0), (24, 5)]),
    ("K", WS, [(15, 7), (16.5, 7), (17.5, 6), (24, 6)]),
    ("RXH", WS, [(8, 5), (12, 5)]),
    ("TXH", WS, [(8, 8), (12, 8)]),
    ("RXL", WS, [(1, 5), (4, 5)]),
    ("3V3", WS, [(1, 6), (4, 6)]),
    ("TXL", WS, [(1, 8), (4, 8)]),
]
TRACKS = [(n, w, [G(*p) for p in pts]) for n, w, pts in TRACKS]

# ---------------------------------------------------------------- çizgi yazı tipi (4x6)
FONT = {
    '0': [[(1,0),(3,0),(4,1),(4,5),(3,6),(1,6),(0,5),(0,1),(1,0)]], '1': [[(1,1),(2,0),(2,6)],[(1,6),(3,6)]],
    '2': [[(0,1),(1,0),(3,0),(4,1),(4,2),(0,6),(4,6)]], '3': [[(0,0),(4,0),(2,2.5),(3,2.5),(4,3.5),(4,5),(3,6),(1,6),(0,5)]],
    '4': [[(3,6),(3,0),(0,4),(4,4)]], '5': [[(4,0),(0,0),(0,2.5),(3,2.5),(4,3.5),(4,5),(3,6),(0,6)]],
    '6': [[(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5),(4,3.5),(3,2.5),(0,2.5)]], '7': [[(0,0),(4,0),(1.5,6)]],
    '8': [[(1,0),(3,0),(4,1),(4,2),(3,3),(1,3),(0,2),(0,1),(1,0)],[(1,3),(0,4),(0,5),(1,6),(3,6),(4,5),(4,4),(3,3)]],
    '9': [[(4,3.5),(1,3.5),(0,2.5),(0,1),(1,0),(3,0),(4,1),(4,5),(3,6),(1,6)]],
    'A': [[(0,6),(0,2),(2,0),(4,2),(4,6)],[(0,3.5),(4,3.5)]],
    'B': [[(0,0),(0,6),(3,6),(4,5),(4,4),(3,3),(0,3)],[(0,0),(3,0),(4,1),(4,2),(3,3)]],
    'C': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5)]], 'D': [[(0,0),(0,6),(2.5,6),(4,4.5),(4,1.5),(2.5,0),(0,0)]],
    'E': [[(4,0),(0,0),(0,6),(4,6)],[(0,3),(3,3)]], 'F': [[(4,0),(0,0),(0,6)],[(0,3),(3,3)]],
    'G': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5),(4,3.5),(2,3.5)]], 'H': [[(0,0),(0,6)],[(4,0),(4,6)],[(0,3),(4,3)]],
    'I': [[(1,0),(3,0)],[(2,0),(2,6)],[(1,6),(3,6)]], 'J': [[(4,0),(4,5),(3,6),(1,6),(0,5)]],
    'K': [[(0,0),(0,6)],[(4,0),(0,4)],[(1.4,2.6),(4,6)]], 'L': [[(0,0),(0,6),(4,6)]],
    'M': [[(0,6),(0,0),(2,3),(4,0),(4,6)]], 'N': [[(0,6),(0,0),(4,6),(4,0)]],
    'O': [[(1,0),(3,0),(4,1),(4,5),(3,6),(1,6),(0,5),(0,1),(1,0)]], 'P': [[(0,6),(0,0),(3,0),(4,1),(4,2),(3,3),(0,3)]],
    'R': [[(0,6),(0,0),(3,0),(4,1),(4,2),(3,3),(0,3)],[(2,3),(4,6)]],
    'S': [[(4,1),(3,0),(1,0),(0,1),(0,2),(1,3),(3,3),(4,4),(4,5),(3,6),(1,6),(0,5)]],
    'T': [[(0,0),(4,0)],[(2,0),(2,6)]], 'U': [[(0,0),(0,5),(1,6),(3,6),(4,5),(4,0)]], 'V': [[(0,0),(2,6),(4,0)]],
    'W': [[(0,0),(1,6),(2,3),(3,6),(4,0)]], 'X': [[(0,0),(4,6)],[(4,0),(0,6)]], 'Y': [[(0,0),(2,3),(4,0)],[(2,3),(2,6)]],
    'Z': [[(0,0),(4,0),(0,6),(4,6)]], '+': [[(0,3),(4,3)],[(2,1),(2,5)]], '-': [[(0.5,3),(3.5,3)]],
    '.': [[(2,5.6),(2,6)]], '/': [[(0,6),(4,0)]], '(': [[(2.5,0),(1.5,1),(1.5,5),(2.5,6)]],
    ')': [[(1.5,0),(2.5,1),(2.5,5),(1.5,6)]], '=': [[(0.5,2.2),(3.5,2.2)],[(0.5,3.8),(3.5,3.8)]], ' ': [],
}

def text(s, x, y, h, anchor="l", rot=0, mirror=False):
    """Metni çizgi listesine çevirir. (x,y): dikey orta noktası, rot=90 aşağıdan yukarı okunur."""
    k = h / 6.0
    width = (len(s) * 6 - 2) * k
    x0 = {"l": 0, "c": -width / 2, "r": -width}[anchor]
    out = []
    for i, ch in enumerate(s.upper()):
        for st in FONT[ch]:
            pts = []
            for gx, gy in st:
                dx, dy = x0 + (i * 6 + gx) * k, (gy - 3) * k
                if mirror:
                    dx = -dx
                if rot == 90:
                    dx, dy = dy, -dx
                pts.append((x + dx, y + dy))
            out.append(pts)
    return out

# bakır yazı: alt yüzde düz okunsun diye üstten bakışta aynalı
COPPER_TEXT = text("K-LINE", 8.6 + 10.2, 8.6, 1.8, "l", mirror=True) + text("OBD V1", 8.6 + 10.2, 11.6, 1.8, "l", mirror=True)

# ---------------------------------------------------------------- serigrafi (üst yüz)
SILK = []
def sl(*pts):
    SILK.append(list(pts))
def srect(x1, y1, x2, y2):
    sl((x1, y1), (x2, y1), (x2, y2), (x1, y2), (x1, y1))
def scircle(cx, cy, r, a0=0, a1=360, rx=None):
    rx = rx or r
    n = 36
    sl(*[(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)])
def stext(*a, **kw):
    SILK.extend(text(*a, **kw))

def silk():
    gx = lambda x: OX + U * x
    gy = lambda y: OY + U * y
    stext("K-LINE OBD  L9637D  V1.0", 12, 2.8, 1.5)
    stext("J2 RX=TXD(GPIO43)  TX=RXD(GPIO44)", 9, 35.0, 1.2)
    # J2
    srect(gx(1) - 1.27, gy(5) - 1.27, gx(1) + 1.27, gy(8) + 1.27)
    for i, n in enumerate(["RX", "3V3", "GND", "TX"]):
        stext(n, gx(1) - 1.6, gy(5 + i), 0.9, "r")
    stext("J2 EKRAN", 2.3, gy(6.5), 1.0, "c", rot=90)
    # U3 modül
    srect(gx(3.3), gy(3.3), gx(8.7), gy(9.7))
    for i, n in enumerate(["LV1", "LV2", "LV", "GND", "LV3", "LV4"]):
        stext(n, gx(4) + 1.3, gy(4 + i), 0.9, "l")
    for i, n in enumerate(["HV1", "HV2", "HV", "GND", "HV3", "HV4"]):
        stext(n, gx(8) - 1.3, gy(4 + i), 0.9, "r")
    stext("U3", gx(6), gy(2.6), 1.0, "c")
    # U1 L9637D
    x1, x2, y1, y2 = gx(12) + 1.5, gx(15) - 1.5, gy(4.4), gy(8.6)
    xm = (x1 + x2) / 2
    sl((xm - 0.8, y1), (x1, y1), (x1, y2), (x2, y2), (x2, y1), (xm + 0.8, y1))
    scircle(xm, y1, 0.8, 0, 180)
    stext("L9637D", xm, (y1 + y2) / 2, 1.3, "c", rot=90)
    for i, n in enumerate(["RX", "LO", "VCC", "TX"]):
        stext(n, gx(12) - 1.6, gy(5 + i), 0.9, "r")
    for i, n in enumerate(["LI", "VS", "K", "GND"]):
        stext(n, gx(15) + 1.6, gy(5 + i), 0.9, "l")
    stext("U1", xm, gy(9.2), 1.0, "c")
    # U2 78L05: düz yüz aşağıda
    fy = gy(3) + 1.3
    sl((gx(9.35), fy), (gx(12.65), fy))
    scircle(gx(11), fy, 3.3, 180, 360, rx=gx(12.65) - gx(11))
    stext("78L05", gx(11), fy + 1.1, 0.9, "c")
    # C1, C2
    for ref, x in (("C2", 8), ("C1", 14)):
        srect(gx(x) - 1.2, gy(0) + 1.3, gx(x) + 1.2, gy(2) - 1.3)
        stext(ref, gx(x) + 1.6, gy(1), 0.9, "l")
    # D1: bant katot (sol) tarafında
    srect(gx(18.6), gy(0) - 1.0, gx(21.4), gy(0) + 1.0)
    for d in (0, 0.2, 0.4):
        sl((gx(18.95) + d, gy(0) - 1.0), (gx(18.95) + d, gy(0) + 1.0))
    stext("D1", gx(18) - 1.5, gy(0), 0.9, "r")
    stext("1N4005", gx(20), gy(1), 0.9, "c")
    # R1, R2
    for ref, x in (("R1", 18), ("R2", 20)):
        srect(gx(x) - 0.9, gy(2.55), gx(x) + 0.9, gy(5.45))
        stext(ref + " 1K", gx(x), gy(4), 1.0, "c", rot=90)
    # J1 OBD
    srect(gx(24) - 1.27, gy(5) - 1.27, gx(24) + 1.27, gy(8) + 1.27)
    for i, (n, p) in enumerate([("+12V", "16"), ("K", "7"), ("GND", "4"), ("GND", "5")]):
        stext(n, gx(24) - 1.6, gy(5 + i), 0.9, "r")
        stext(p, gx(24) + 1.6, gy(5 + i), 0.9, "l")
    stext("J1 OBD", gx(24) + 5.0, gy(6.5), 1.0, "c", rot=90)
    for mx, my in MOUNT:
        scircle(mx, my, 2.9)
silk()

# ---------------------------------------------------------------- geometri / DRC
def seg_pt(p, a, b):
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)

def seg_seg(a, b, c, d):
    def ccw(p, q, r):
        return (r[1] - p[1]) * (q[0] - p[0]) - (q[1] - p[1]) * (r[0] - p[0])
    if (ccw(a, b, c) * ccw(a, b, d) < 0) and (ccw(c, d, a) * ccw(c, d, b) < 0):
        return 0.0
    return min(seg_pt(a, c, d), seg_pt(b, c, d), seg_pt(c, a, b), seg_pt(d, a, b))

def pad_capsules(p):
    x, y = p["p"]
    if p["shape"] == "C":
        return [((x, y), (x, y), p["size"] / 2)]
    if p["shape"] == "R":   # kare: iç daire + 4 kenar
        h = p["size"] / 2
        c = [(x - h, y - h), (x + h, y - h), (x + h, y + h), (x - h, y + h)]
        return [((x, y), (x, y), h)] + [(c[i], c[(i + 1) % 4], 0) for i in range(4)]
    w, h = p["size"]
    if w >= h:
        return [((x - (w - h) / 2, y), (x + (w - h) / 2, y), h / 2)]
    return [((x, y - (h - w) / 2), (x, y + (h - w) / 2), w / 2)]

def copper_objects():
    objs = []
    for p in PADS:
        for a, b, r in pad_capsules(p):
            objs.append((p["net"], a, b, r, "%s.%s" % (p["ref"], p["pin"])))
    for net, w, pts in TRACKS:
        for a, b in zip(pts, pts[1:]):
            objs.append((net, a, b, w / 2, "yol " + net))
    for st in COPPER_TEXT:
        for a, b in zip(st, st[1:]):
            objs.append(("TXT", a, b, 0.15, "yazı"))
    return objs

def drc():
    objs = copper_objects()
    worst, errs = 99, []
    for i in range(len(objs)):
        n1, a1, b1, r1, d1 = objs[i]
        for j in range(i + 1, len(objs)):
            n2, a2, b2, r2, d2 = objs[j]
            if n1 == n2:
                continue
            g = seg_seg(a1, b1, a2, b2) - r1 - r2
            worst = min(worst, g)
            if g < CLR_MIN:
                errs.append("aralık %.2f mm: %s <-> %s" % (g, d1, d2))
    for n, a, b, r, d in objs:
        for mx, my in MOUNT:
            if seg_seg(a, b, (mx, my), (mx, my)) - r < 2.9:
                errs.append("montaj deliğine yakın: " + d)
        e = min(min(a[0], b[0]) - r, min(a[1], b[1]) - r, W - max(a[0], b[0]) - r, H - max(a[1], b[1]) - r)
        if e < 1.0:
            errs.append("kenara yakın: " + d)
    # bağlantı: her net tek parça olmalı
    parent = list(range(len(objs)))
    def f(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return i
    for i in range(len(objs)):
        for j in range(i + 1, len(objs)):
            if objs[i][0] == objs[j][0] and objs[i][0] != "TXT":
                if seg_seg(objs[i][1], objs[i][2], objs[j][1], objs[j][2]) <= objs[i][3] + objs[j][3]:
                    parent[f(i)] = f(j)
    nets = {}
    for i, o in enumerate(objs):
        if o[0] != "TXT":
            nets.setdefault(o[0], set()).add(f(i))
    for n, s in nets.items():
        if len(s) != 1:
            errs.append("net %s %d parçaya bölünmüş" % (n, len(s)))
    return worst, errs

# ---------------------------------------------------------------- Gerber / Excellon
def gnum(v):
    return "%d" % round(v * 1e6)

class Gerber:
    def __init__(self, func):
        self.func, self.ap, self.body = func, {}, []
    def aperture(self, key):
        if key not in self.ap:
            self.ap[key] = 10 + len(self.ap)
        return self.ap[key]
    def sel(self, key):
        self.body.append("D%d*" % self.aperture(key))
    def xy(self, p):
        return "X%sY%s" % (gnum(p[0]), gnum(H - p[1]))
    def flash(self, key, p):
        self.sel(key); self.body.append(self.xy(p) + "D03*")
    def line(self, w, pts):
        self.sel(("C", round(w, 4)))
        self.body.append(self.xy(pts[0]) + "D02*")
        for p in pts[1:]:
            self.body.append(self.xy(p) + "D01*")
    def save(self, name):
        hdr = ["G04 K-Line OBD adaptor - %s*" % self.func, "%TF.FileFunction," + self.func + "*%",
               "%FSLAX46Y46*%", "%MOMM*%", "%LPD*%", "G01*"]
        for key, code in self.ap.items():
            if key[0] == "C":
                hdr.append("%%ADD%dC,%.4f*%%" % (code, key[1]))
            else:
                hdr.append("%%ADD%d%s,%.4fX%.4f*%%" % (code, key[0], key[1], key[2]))
        with open(os.path.join(OUT, "gerber", name), "w", newline="\n") as fh:
            fh.write("\n".join(hdr + self.body + ["M02*"]) + "\n")

def pad_key(p, grow=0.0):
    if p["shape"] == "C":
        return ("C", round(p["size"] + 2 * grow, 4))
    if p["shape"] == "R":
        return ("R", round(p["size"] + 2 * grow, 4), round(p["size"] + 2 * grow, 4))
    return ("O", round(p["size"][0] + 2 * grow, 4), round(p["size"][1] + 2 * grow, 4))

def write_fab():
    os.makedirs(os.path.join(OUT, "gerber"), exist_ok=True)
    cu = Gerber("Copper,L1,Bot")
    for net, w, pts in TRACKS:
        cu.line(w, pts)
    for p in PADS:
        cu.flash(pad_key(p), p["p"])
    for st in COPPER_TEXT:
        cu.line(0.3, st)
    cu.save("k-line-obd-B_Cu.gbl")
    mask = Gerber("Soldermask,Bot")
    for p in PADS:
        mask.flash(pad_key(p, 0.1), p["p"])
    mask.save("k-line-obd-B_Mask.gbs")
    silkg = Gerber("Legend,Top")
    for st in SILK:
        silkg.line(0.15, st)
    silkg.save("k-line-obd-F_Silkscreen.gto")
    edge = Gerber("Profile,NP")
    edge.line(0.1, [(0, 0), (W, 0), (W, H), (0, H), (0, 0)])
    edge.save("k-line-obd-Edge_Cuts.gko")

    def drill(name, holes, plated):
        tools = sorted(set(d for d, _ in holes))
        lines = ["M48", "; K-Line OBD adaptor - %s" % ("PTH" if plated else "NPTH"), "FMAT,2", "METRIC,TZ"]
        lines += ["T%dC%.3f" % (i + 1, d) for i, d in enumerate(tools)] + ["%", "G90", "G05"]
        for i, d in enumerate(tools):
            lines.append("T%d" % (i + 1))
            lines += ["X%.3fY%.3f" % (x, H - y) for dd, (x, y) in holes if dd == d]
        lines.append("M30")
        with open(os.path.join(OUT, "gerber", name), "w", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")
    drill("k-line-obd-PTH.drl", [(p["drill"], p["p"]) for p in PADS], True)
    drill("k-line-obd-NPTH.drl", [(3.2, m) for m in MOUNT], False)

    with zipfile.ZipFile(os.path.join(OUT, "k-line-obd-gerber.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(os.path.join(OUT, "gerber"))):
            z.write(os.path.join(OUT, "gerber", f), f)

# ---------------------------------------------------------------- SVG (1:1, mm)
def poly(pts):
    return "M" + " L".join("%.3f %.3f" % p for p in pts)

def svg_copper(ink="#000", paper="#fff", center_dot=0.5):
    s = []
    for net, w, pts in TRACKS:
        s.append('<path d="%s" fill="none" stroke="%s" stroke-width="%.2f" stroke-linecap="round" stroke-linejoin="round"/>' % (poly(pts), ink, w))
    for st in COPPER_TEXT:
        s.append('<path d="%s" fill="none" stroke="%s" stroke-width="0.3" stroke-linecap="round" stroke-linejoin="round"/>' % (poly(st), ink))
    for p in PADS:
        x, y = p["p"]
        if p["shape"] == "C":
            s.append('<circle cx="%.3f" cy="%.3f" r="%.3f" fill="%s"/>' % (x, y, p["size"] / 2, ink))
        elif p["shape"] == "R":
            a = p["size"]
            s.append('<rect x="%.3f" y="%.3f" width="%.3f" height="%.3f" fill="%s"/>' % (x - a / 2, y - a / 2, a, a, ink))
        else:
            w, h = p["size"]
            s.append('<rect x="%.3f" y="%.3f" width="%.3f" height="%.3f" rx="%.3f" fill="%s"/>' % (x - w / 2, y - h / 2, w, h, min(w, h) / 2, ink))
        if center_dot:
            s.append('<circle cx="%.3f" cy="%.3f" r="%.3f" fill="%s"/>' % (x, y, center_dot / 2, paper))
    return "\n".join(s)

def svg_outline(color="#000", wdt=0.15):
    return '<rect x="0" y="0" width="%.2f" height="%.2f" fill="none" stroke="%s" stroke-width="%.2f"/>' % (W, H, color, wdt) + \
        "".join('<circle cx="%.2f" cy="%.2f" r="1.6" fill="none" stroke="%s" stroke-width="0.15"/>' % (mx, my, color) for mx, my in MOUNT)

def scale_bar(x, y):
    return ('<g font-family="Arial" font-size="3"><path d="M%.1f %.1f h50 M%.1f %.1f v-2 M%.1f %.1f v-2 M%.1f %.1f v-1.2" stroke="#000" stroke-width="0.3" fill="none"/>'
            '<text x="%.1f" y="%.1f">50 mm ölçek: yazıcıda %%100 (gerçek boyut) bas, cetvelle kontrol et</text></g>') % (
        x, y, x, y, x + 50, y, x + 25, y, x, y + 4.5)

def svg_doc(wmm, hmm, body):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="%gmm" height="%gmm" viewBox="0 0 %g %g">\n'
            '<rect width="100%%" height="100%%" fill="#fff"/>\n%s\n</svg>\n') % (wmm, hmm, wmm, hmm, body)

def head(x, y, title, sub):
    return '<text x="%g" y="%g" font-family="Arial" font-size="5" font-weight="bold">%s</text><text x="%g" y="%g" font-family="Arial" font-size="3.2">%s</text>' % (x, y, title, x, y + 5.5, sub)

M = 15  # sayfa kenar boşluğu (mm)

def write_svgs():
    pages = []
    # 1) YOLLAR: alt bakır, üstten bakış = toner transferi için aynalamadan basılır
    pw = 180
    b = [head(M, 12, "1 · Bakır yollar (alt yüz) · 1:1", "Toner transferi / UV pozlama için aynalamadan bas. Ortadaki beyaz noktalar delme merkezidir. Alt yüzdeki yazı aynalı görünür; plakette düz okunur.")]
    for k in range(2):
        oy = 24 + k * (H + 16)
        b.append('<g transform="translate(%g %g)">%s%s</g>' % (M, oy, svg_copper(), svg_outline("#000", 0.12)))
    b.append(scale_bar(M, 24 + 2 * (H + 16) + 2))
    pages.append(("k-line-pcb-yollar.svg", pw, 24 + 2 * (H + 16) + 12, "\n".join(b)))

    # 2) DELME: iki görünüm, delik çapı renkli
    col = {0.8: "#d32f2f", 1.0: "#1565c0", 3.2: "#2e7d32"}
    def holes(mirror):
        s = [svg_outline("#000", 0.2)]
        allh = [(p["drill"], p["p"]) for p in PADS] + [(3.2, m) for m in MOUNT]
        for d, (x, y) in allh:
            if mirror:
                x = W - x
            c = col[d]
            s.append('<circle cx="%.3f" cy="%.3f" r="%.3f" fill="none" stroke="%s" stroke-width="0.2"/>' % (x, y, d / 2, c))
            s.append('<path d="M%.3f %.3f h%.3f M%.3f %.3f v%.3f" stroke="%s" stroke-width="0.12"/>' % (x - 1.2, y, 2.4, x, y - 1.2, 2.4, c))
        return "\n".join(s)
    cnt = {d: 0 for d in col}
    for p in PADS:
        cnt[p["drill"]] += 1
    cnt[3.2] = len(MOUNT)
    b = [head(M, 12, "2 · Delme noktaları · 1:1", "Artı işaretinin merkezinden del. Renk = matkap ucu. Plakayı hangi yüzü sana bakacak şekilde tutuyorsan o görünümü kullan.")]
    b.append('<text x="%g" y="%g" font-family="Arial" font-size="3.5" font-weight="bold">A · Bakır (lehim) tarafından bakış</text>' % (M, 25))
    b.append('<g transform="translate(%g %g)">%s</g>' % (M, 28, holes(True)))
    y2 = 28 + H + 12
    b.append('<text x="%g" y="%g" font-family="Arial" font-size="3.5" font-weight="bold">B · Parça tarafından bakış</text>' % (M, y2 - 3))
    b.append('<g transform="translate(%g %g)">%s</g>' % (M, y2, holes(False)))
    ly = y2 + H + 10
    names = {0.8: "dirençler, kondansatörler, 78L05", 1.0: "J1, J2, L9637D adaptörü, seviye modülü, 1N4005", 3.2: "montaj (M3, kaplamasız)"}
    for i, d in enumerate(sorted(col)):
        yy = ly + i * 6
        b.append('<circle cx="%g" cy="%g" r="1.6" fill="none" stroke="%s" stroke-width="0.5"/><text x="%g" y="%g" font-family="Arial" font-size="3.4">Ø%.1f mm · %d delik · %s</text>' % (M + 2, yy, col[d], M + 6, yy + 1.2, d, cnt[d], names[d]))
    b.append(scale_bar(M, ly + 22))
    pages.append(("k-line-pcb-delme.svg", pw, ly + 32, "\n".join(b)))

    # 3) YERLEŞİM: parça tarafı, bakır soluk
    sk = "\n".join('<path d="%s" fill="none" stroke="#000" stroke-width="0.18" stroke-linecap="round" stroke-linejoin="round"/>' % poly(st) for st in SILK)
    b = [head(M, 12, "3 · Parça yerleşimi (üst yüz) · 1:1", "Parçalar bu yüzden takılır, bacaklar alttaki bakır pedlere lehimlenir. Soluk turuncu: alttaki yollar (içinden görünüş).")]
    b.append('<g transform="translate(%g %g)"><g opacity="0.35">%s</g>%s%s</g>' % (M, 24, svg_copper("#e07b00", "#fff", 0.6), svg_outline(), sk))
    bom = [("U1", "L9637D (SOIC-8) + SO8→DIP adaptörü", "pin 1 sol üst, kare ped"),
           ("U2", "78L05 (TO-92)", "düz yüz L9637D'ye bakar"),
           ("U3", "BSS138 4 kanal seviye dönüştürücü", "HV sırası L9637D'ye bakar"),
           ("D1", "1N4005 / 1N4007", "bant kare pede (sol)"),
           ("R1, R2", "1 kΩ ¼ W (paralel ≈ 500 Ω)", "K-Line pull-up"),
           ("C1, C2", "100 nF seramik (104)", "5.08 mm bacak aralığı"),
           ("J1", "4 pin 2.54 mm · OBD 16/7/4/5", "kare ped = +12V"),
           ("J2", "4 pin 2.54 mm · ekran UART başlığı", "RX→TXD(G43), TX→RXD(G44)")]
    ty = 24 + H + 12
    b.append('<text x="%g" y="%g" font-family="Arial" font-size="3.5" font-weight="bold">Malzeme listesi</text>' % (M, ty))
    for i, (r, v, n) in enumerate(bom):
        yy = ty + 6 + i * 5
        b.append('<text x="%g" y="%g" font-family="Arial" font-size="3"><tspan font-weight="bold">%s</tspan><tspan x="%g">%s</tspan><tspan x="%g" fill="#555">%s</tspan></text>' % (M, yy, r, M + 14, v, M + 76, n))
    b.append(scale_bar(M, ty + 6 + len(bom) * 5 + 6))
    pages.append(("k-line-pcb-yerlesim.svg", pw, ty + 6 + len(bom) * 5 + 16, "\n".join(b)))

    for name, wmm, hmm, body in pages:
        with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(svg_doc(wmm, hmm, body))
    return pages

def write_pdf(pages):
    import fitz
    pdf = fitz.open()
    A4 = fitz.paper_rect("a4")
    for name, wmm, hmm, _ in pages:
        src = fitz.open(os.path.join(OUT, name))
        one = fitz.open("pdf", src.convert_to_pdf())
        pg = pdf.new_page(width=A4.width, height=A4.height)
        k = 72 / 25.4
        pg.show_pdf_page(fitz.Rect(0, 0, wmm * k, hmm * k), one, 0)
    pdf.save(os.path.join(OUT, "k-line-pcb-baski.pdf"))
    for i, pg in enumerate(pdf if os.environ.get("ONIZLEME") else []):
        pg.get_pixmap(dpi=150).save(os.path.join(OUT, "onizleme-%d.png" % (i + 1)))

if __name__ == "__main__":
    worst, errs = drc()
    print("DRC: en küçük bakır aralığı %.2f mm, %d hata" % (worst, len(errs)))
    for e in errs:
        print("  ", e)
    write_fab()
    write_pdf(write_svgs())
    print("Kart %.1f x %.1f mm, %d delik + %d montaj" % (W, H, len(PADS), len(MOUNT)))
