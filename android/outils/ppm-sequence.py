#!/usr/bin/env python3
"""ppm-sequence.py — séquence visuelle d'un run (passe 7f) : pour chaque PPM (P6) d'un dossier, dans l'ordre
des numéros de frame, moyenne R G B, part de pixels non noirs, empreinte (sha1 tronquée) et part de pixels
qui ont changé depuis le PPM précédent. Sert à documenter ce qui vient après l'écran de connexion.

    python3 -I android/outils/ppm-sequence.py <dossier frames/> [--carte N]
--carte N : imprime aussi une carte grossière (blocs 80x80, caractère selon la luminance) du N-ième PPM."""
import hashlib, os, re, sys

def lire(chemin):
    with open(chemin, 'rb') as f:
        d = f.read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', d)
    if not m:
        raise ValueError(chemin)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, d[m.end():m.end() + w * h * 3]

def main():
    dossier = sys.argv[1]
    carte = int(sys.argv[sys.argv.index('--carte') + 1]) if '--carte' in sys.argv else None
    fichiers = sorted((int(re.search(r'(\d+)', f).group(1)), f) for f in os.listdir(dossier) if f.endswith('.ppm'))
    prec = None
    print(f"{'frame':>6} {'R':>7} {'G':>7} {'B':>7} {'non noirs':>10} {'changés':>9}  empreinte")
    for n, f in fichiers:
        w, h, px = lire(os.path.join(dossier, f))
        np_ = w * h
        s = [sum(px[k::3]) / np_ for k in range(3)]
        nn = sum(1 for i in range(0, len(px), 3) if px[i] or px[i + 1] or px[i + 2]) / np_
        ch = '—' if prec is None else f"{sum(1 for i in range(0, len(px), 3) if px[i:i+3] != prec[i:i+3]) / np_:8.2%}"
        print(f"{n:6d} {s[0]:7.2f} {s[1]:7.2f} {s[2]:7.2f} {nn:9.2%} {ch:>9}  {hashlib.sha1(px).hexdigest()[:12]}")
        if carte is not None and n == carte:
            rampe = ' .:-=+*#%@'
            for by in range(0, h, 80):
                ligne = ''
                for bx in range(0, w, 80):
                    t = c = 0
                    for y in range(by, min(by + 80, h), 8):
                        for x in range(bx, min(bx + 80, w), 8):
                            i = (y * w + x) * 3; t += px[i] + px[i + 1] + px[i + 2]; c += 3
                    ligne += rampe[min(9, int(t / c / 25.6))]
                print('        |' + ligne + '|')
        prec = px

main()
