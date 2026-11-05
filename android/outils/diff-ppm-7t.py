#!/usr/bin/env python3
"""Passe 7t — compare deux frames PPM (1280×720) : écart moyen absolu par canal, % de pixels qui diffèrent de plus de
16 (somme RVB), luminance moyenne de chaque image sur la zone « monde » (au-dessus du HUD, y < 520) et sur l'image
entière. Des ombres apparues = luminance du monde plus basse, pixels plus sombres. Usage : diff-ppm-7t.py a.ppm b.ppm"""
import sys

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

(wa, ha, a), (wb, hb, b) = lire(sys.argv[1]), lire(sys.argv[2])
assert (wa, ha) == (wb, hb), "tailles différentes"
n = wa * ha; ecart = 0; diff = 0; plus_sombre = 0
lum = [[0, 0], [0, 0]]                       # [image][0 monde, 1 tout]
for p in range(n):
    o = 3 * p; y = p // wa
    ra, ga, ba = a[o], a[o + 1], a[o + 2]; rb, gb, bb = b[o], b[o + 1], b[o + 2]
    d = abs(ra - rb) + abs(ga - gb) + abs(ba - bb); ecart += d
    if d > 16:
        diff += 1
        if rb + gb + bb < ra + ga + ba: plus_sombre += 1
    la = 0.299 * ra + 0.587 * ga + 0.114 * ba; lb = 0.299 * rb + 0.587 * gb + 0.114 * bb
    lum[0][1] += la; lum[1][1] += lb
    if y < 520: lum[0][0] += la; lum[1][0] += lb
nm = wa * 520
print(f"écart moyen {ecart / n / 3:.2f}/canal ; pixels différents (>16) {100 * diff / n:.1f} % dont plus sombres dans B "
      f"{100 * plus_sombre / max(diff, 1):.1f} % ; luminance monde A {lum[0][0] / nm:.1f} → B {lum[1][0] / nm:.1f} ; "
      f"image A {lum[0][1] / n:.1f} → B {lum[1][1] / n:.1f}")
