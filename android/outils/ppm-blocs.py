#!/usr/bin/env python3
"""ppm-blocs.py — mesure d'un dossier de frames PPM (passe 7l) : par frame, moyenne R G B, pixels non noirs,
blocs 16×16 non noirs, MOYENNES LOCALES DISTINCTES (moyenne RGB de chaque bloc, arrondie : mesure la richesse
de l'image — un aplat donne 1, une texture réelle des milliers), écart-type des luminances de blocs, empreinte.

    python3 -I android/outils/ppm-blocs.py <frames/> [--ref <frames-avant/>]
--ref : compare chaque frame à la frame de même numéro du dossier de référence (blocs dont la moyenne
        locale diffère de plus de 2 niveaux) — c'est la mesure « avant / après »."""
import hashlib, os, re, sys

B = 16

def lire(chemin):
    with open(chemin, 'rb') as f:
        d = f.read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', d)
    if not m:
        raise ValueError(chemin)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, d[m.end():m.end() + w * h * 3]

def blocs(w, h, px):
    """moyennes (R, G, B) des blocs B×B, échantillonnées 1 pixel sur 2 dans chaque sens."""
    out = []
    for by in range(0, h - B + 1, B):
        for bx in range(0, w - B + 1, B):
            r = g = b = n = 0
            for y in range(by, by + B, 2):
                i = (y * w + bx) * 3
                ligne = px[i:i + B * 3]
                r += sum(ligne[0::6]); g += sum(ligne[1::6]); b += sum(ligne[2::6]); n += B // 2
            out.append((r / n, g / n, b / n))
    return out

def mesure(chemin):
    w, h, px = lire(chemin)
    np_ = w * h
    moy = [sum(px[k::3]) / np_ for k in range(3)]
    nn = sum(1 for i in range(0, len(px), 3) if px[i] | px[i + 1] | px[i + 2]) / np_
    bl = blocs(w, h, px)
    lum = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in bl]
    m = sum(lum) / len(lum)
    et = (sum((x - m) ** 2 for x in lum) / len(lum)) ** 0.5
    return dict(w=w, h=h, moy=moy, nn=nn, bl=bl, bnn=sum(1 for x in lum if x > 1.0),
                dist=len({(round(r), round(g), round(b)) for r, g, b in bl}), et=et, sha=hashlib.sha1(px).hexdigest()[:12])

def main():
    d = sys.argv[1]
    ref = sys.argv[sys.argv.index('--ref') + 1] if '--ref' in sys.argv else None
    fs = sorted((int(re.search(r'(\d+)', f).group(1)), f) for f in os.listdir(d) if f.endswith('.ppm'))
    print(f"{'frame':>6} {'R':>6} {'G':>6} {'B':>6} {'non noirs':>9} {'blocs≠noir':>11} {'moy. locales distinctes':>23} "
          f"{'σ lum':>6}{'  blocs changés/réf' if ref else ''}  empreinte")
    for n, f in fs:
        m = mesure(os.path.join(d, f))
        nb = len(m['bl'])
        cmp_ = ''
        if ref and os.path.exists(os.path.join(ref, f)):
            r = mesure(os.path.join(ref, f))
            if len(r['bl']) == nb:
                ch = sum(1 for a, b in zip(m['bl'], r['bl']) if max(abs(a[k] - b[k]) for k in range(3)) > 2)
                cmp_ = f"  {ch:>6}/{nb:<6} ({r['dist']}→{m['dist']})"
        print(f"{n:6d} {m['moy'][0]:6.2f} {m['moy'][1]:6.2f} {m['moy'][2]:6.2f} {m['nn']:9.2%} {m['bnn']:>5}/{nb:<5} "
              f"{m['dist']:>23} {m['et']:6.2f}{cmp_}  {m['sha']}")

if __name__ == '__main__':
    main()
