#!/usr/bin/env python3
"""Passe 7w — statistiques des zones du HUD (1280×720) d'une frame PPM : minicarte, panneau central, carte de commande.
Pour chaque zone : % de pixels non noirs (somme RVB > 30), moyenne RVB, nombre de blocs 8×8 de moyennes distinctes
(un aplat ou un fond noir donne 1 à 3 ; du terrain, des dizaines). Usage : zones-hud-7w.py frame.ppm [...]"""
import sys

ZONES = {"minicarte": (28, 540, 190, 705), "panneau-central": (250, 590, 905, 712), "carte-commande": (1030, 555, 1265, 712)}

def lire_ppm(chemin):
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

for chemin in sys.argv[1:]:
    w, h, px = lire_ppm(chemin)
    print(chemin.rsplit("/", 1)[-1])
    for nom, (x0, y0, x1, y1) in ZONES.items():
        n = nn = 0; s = [0, 0, 0]; blocs = set()
        for by in range(y0, y1 - 7, 8):
            for bx in range(x0, x1 - 7, 8):
                b = [0, 0, 0]
                for y in range(by, by + 8):
                    o = 3 * (y * w + bx)
                    for x in range(8):
                        r, g, bb = px[o], px[o + 1], px[o + 2]; o += 3
                        b[0] += r; b[1] += g; b[2] += bb; n += 1
                        if r + g + bb > 30: nn += 1
                for k in range(3): s[k] += b[k]
                blocs.add(tuple(v // 64 // 8 for v in b))
        print(f"  {nom:16s} non noirs {100 * nn / max(n, 1):5.1f} %  moyenne {s[0] / n:5.1f} {s[1] / n:5.1f} {s[2] / n:5.1f}"
              f"  blocs distincts {len(blocs)}")
