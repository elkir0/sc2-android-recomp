#!/usr/bin/env python3
"""Passe 7v3 — A/B d'ombres : compare la frame A (sans SC2_OMBRES) et la frame B (avec) de même numéro.
Sortie : statistiques sur la zone « monde » (y < 520, au-dessus du HUD) — % de pixels ASSOMBRIS de plus de 24 (somme RVB)
et % ÉCLAIRCIS, blocs 16×16 dont ≥ 30 % des pixels sont assombris (une ombre portée est un amas LOCALISÉ de tels blocs,
un changement global de teinte les allume partout) — et une carte PNG : image B en gris, pixels assombris en rouge,
éclaircis en bleu. Pur Python (pas de numpy). Usage : ombres-ab-7v3.py a.ppm b.ppm carte.png"""
import sys, zlib, struct

def lire(chemin):
    d = open(chemin, "rb").read()
    champs, i = [], 0
    while len(champs) < 4:
        while d[i:i + 1].isspace(): i += 1
        if d[i:i + 1] == b"#":
            while d[i:i + 1] != b"\n": i += 1
            continue
        j = i
        while not d[j:j + 1].isspace(): j += 1
        champs.append(d[i:j]); i = j
    return int(champs[1]), int(champs[2]), d[i + 1:]

(w, h, a), (wb, hb, b) = lire(sys.argv[1]), lire(sys.argv[2])
assert (w, h) == (wb, hb), "tailles différentes"
YM, S = 520, 16
sombre = bytearray(w * h); clair = bytearray(w * h)
out = bytearray()
la = lb = 0
for y in range(h):
    out.append(0)
    for x in range(w):
        o = 3 * (y * w + x)
        sa, sb = a[o] + a[o + 1] + a[o + 2], b[o] + b[o + 1] + b[o + 2]
        g = sb // 3
        if y < YM:
            la += sa; lb += sb
            if sb < sa - 24: sombre[y * w + x] = 1
            elif sb > sa + 24: clair[y * w + x] = 1
        if sombre[y * w + x]: out += bytes((255, g // 3, g // 3))
        elif clair[y * w + x]: out += bytes((g // 3, g // 3, 255))
        else: out += bytes((g, g, g))
n = w * YM
blocs, tot = [], 0
for by in range(0, YM, S):
    for bx in range(0, w, S):
        k = sum(sombre[(by + j) * w + bx + i] for j in range(S) if by + j < YM for i in range(S) if bx + i < w)
        tot += 1
        if k >= 0.3 * S * S: blocs.append((bx, by))
ps, pc = 100 * sum(sombre[:n]) / n, 100 * sum(clair[:n]) / n
print(f"monde : luminance A {la / n / 3:.1f} → B {lb / n / 3:.1f} ; assombris {ps:.2f} % ; éclaircis {pc:.2f} % ; "
      f"blocs 16×16 assombris (≥ 30 %) : {len(blocs)} / {tot}")
if blocs:
    xs, ys = [x for x, _ in blocs], [y for _, y in blocs]
    print(f"  emprise des blocs assombris : x {min(xs)}–{max(xs) + S}, y {min(ys)}–{max(ys) + S}")
def ch(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[3], "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                              + ch(b"IDAT", zlib.compress(bytes(out), 6)) + ch(b"IEND", b""))
