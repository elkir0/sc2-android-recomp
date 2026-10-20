#!/usr/bin/env python3
"""ppm-blocs.py — statistiques par blocs 16×16 de PPM (P6) : géométrie/texture ou aplat ? (passe 5d)

    scripts/ppm-blocs.py [--avant <dossier>] <dossier ou fichiers .ppm>…

Par image : moyenne RGB, nombre de blocs 16×16 (80×45 en 1280×720), blocs non noirs
(moyenne > 1/255), luminance des blocs (min / max / écart-type), nombre de moyennes de
blocs DISTINCTES (arrondies à l'entier) — un aplat uniforme en a 1. Avec --avant, chaque
image est comparée à son homonyme : blocs dont la moyenne a changé de plus de 2/255.
"""
import os, sys


def lire(p):
    d = open(p, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        parts.append(d[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i:i + w * h * 3]


def blocs(p, t=16):
    w, h, px = lire(p)
    res = []
    for by in range(h // t):
        for bx in range(w // t):
            s = [0, 0, 0]
            for y in range(by * t, by * t + t):
                o = (y * w + bx * t) * 3
                ligne = px[o:o + 3 * t]
                s[0] += sum(ligne[0::3]); s[1] += sum(ligne[1::3]); s[2] += sum(ligne[2::3])
            res.append(tuple(v / (t * t) for v in s))
    moy = tuple(sum(px[k::3]) / (w * h) for k in range(3))
    return w, h, moy, res


def stats(p):
    w, h, moy, bl = blocs(p)
    lum = [0.2126 * r + 0.7152 * g + 0.0722 * b for r, g, b in bl]
    m = sum(lum) / len(lum)
    et = (sum((x - m) ** 2 for x in lum) / len(lum)) ** 0.5
    nn = sum(1 for r, g, b in bl if max(r, g, b) > 1)
    dist = len({(round(r), round(g), round(b)) for r, g, b in bl})
    return moy, bl, lum, et, nn, dist


def fichiers(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            out += [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.endswith('.ppm')]
        else:
            out.append(a)
    return out


def main():
    args = sys.argv[1:]
    avant = None
    if '--avant' in args:
        k = args.index('--avant'); avant = args[k + 1]; del args[k:k + 2]
    print(f"{'image':38} {'moyenne RGB':>20} {'blocs≠noir':>11} {'lum min/max':>13} {'écart-type':>10} {'moy. distinctes':>15}"
          + ('   blocs changés vs avant' if avant else ''))
    for p in fichiers(args):
        moy, bl, lum, et, nn, dist = stats(p)
        ligne = (f"{os.path.relpath(p):38.38} {moy[0]:6.2f} {moy[1]:6.2f} {moy[2]:6.2f} {nn:5d}/{len(bl):<5d} "
                 f"{min(lum):5.1f}/{max(lum):6.1f} {et:10.2f} {dist:15d}")
        if avant:
            q = os.path.join(avant, os.path.basename(p))
            if os.path.exists(q):
                _, bl0, *_ = stats(q)
                ch = sum(1 for a, b in zip(bl, bl0) if max(abs(a[k] - b[k]) for k in range(3)) > 2)
                ligne += f"   {ch:5d}/{len(bl)}"
        print(ligne)


if __name__ == '__main__':
    main()
