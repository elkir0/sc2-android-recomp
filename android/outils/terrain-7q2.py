#!/usr/bin/env python3
"""Passe 7q2 — le terrain 3D suit-il ? Pour chaque frame PPM 1280×720 d'un dossier (ou des fichiers donnés) :
zone MONDE = y 20..515 (au-dessus du HUD), x 0..1280. Mesures : % de pixels non noirs (R+V+B > 30), nombre de blocs
16×16 de moyennes distinctes (quantifiées /16 : un aplat donne 1), part de pixels qui changent vs la frame précédente
(> 24 de somme) = la caméra/le monde bougent. Usage : terrain-7q2.py <dossier|frames…>"""
import os, sys

def lire(p):
    d = open(p, "rb").read(); f = d.split(b"\n", 3); w, h = map(int, f[1].split()); return w, h, f[3]

fichiers = []
for a in sys.argv[1:]:
    fichiers += sorted(os.path.join(a, x) for x in os.listdir(a) if x.endswith(".ppm") and x.startswith("frame_")) if os.path.isdir(a) else [a]
prec = None
print(" frame   monde non noirs   blocs distincts   changés vs préc.")
for p in fichiers:
    w, h, px = lire(p)
    y0, y1 = 20, 515
    nn = n = 0; blocs = set(); ch = 0
    for by in range(y0, y1 - 15, 16):
        for bx in range(0, w - 15, 16):
            s = [0, 0, 0]
            for y in range(by, by + 16, 2):
                o = 3 * (y * w + bx)
                for x in range(0, 16, 2):
                    r, g, b = px[o + 3 * x], px[o + 3 * x + 1], px[o + 3 * x + 2]
                    s[0] += r; s[1] += g; s[2] += b; n += 1
                    if r + g + b > 30: nn += 1
                    if prec is not None:
                        q = o + 3 * x
                        if abs(r - prec[q]) + abs(g - prec[q + 1]) + abs(b - prec[q + 2]) > 24: ch += 1
            blocs.add(tuple(v // 64 // 16 for v in s))
    nom = os.path.basename(p).replace("frame_", "").replace(".ppm", "")
    print(f" {nom:>5}   {100 * nn / n:6.1f} %        {len(blocs):5d}          " + (f"{100 * ch / n:5.1f} %" if prec is not None else "   —"))
    prec = px
