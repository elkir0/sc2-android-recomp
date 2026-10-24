#!/usr/bin/env python3
"""ppm-sequence.py — séquence visuelle d'un run (passe 7f) : pour chaque PPM (P6) d'un dossier, dans l'ordre
des numéros de frame, moyenne R G B, part de pixels non noirs, empreinte (sha1 tronquée) et part de pixels
qui ont changé depuis le PPM précédent. Sert à documenter ce qui vient après l'écran de connexion.

    python3 -I android/outils/ppm-sequence.py <dossier frames/> [--carte N]
--carte N : imprime aussi une carte grossière (blocs 80x80, caractère selon la luminance) du N-ième PPM.
V0.1 (22/10) : comptes par entiers longs (≈ 10x plus vite, sortie identique octet pour octet sur les 507 PPM
d'une TRaynor01) ; un PPM sans chiffre dans son nom (CAPTURE:<nom>) passe après les frames, son nom à la place
du numéro (il faisait planter le tri). PPM vide, à en-tête incomplet ou d'image vide (run tué par le garde-fou de
campagne-banc.sh, ou planté entre le fopen et le premier vidage du tampon : d3dv_present écrit frame_<n>.ppm en place) :
ligne « <n>  illisible (N octets) », et la comparaison suivante se fait avec le dernier PPM lisible ; PPM tronqué
(tampon vidé en partie) : ligne complète suivie de « tronqué (X/Y octets) », sans IndexError sur le pixel final partiel.
Un seul de ces PPM faisait échouer tout le compactage (banc_compacter garde alors tous les PPM de la mission). Sortie
inchangée pour des PPM complets."""
import hashlib, os, re, sys

def lire(chemin):
    """(w, h, pixels lus) ; ValueError(taille du fichier) si l'en-tête P6 manque ou est incomplet, ou si l'image a 0 pixel."""
    with open(chemin, 'rb') as f:
        d = f.read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', d)
    if not m or not int(m.group(1)) * int(m.group(2)):
        raise ValueError(len(d))
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, d[m.end():m.end() + w * h * 3]

def pixels_non_nuls(px, n):
    """Pixels (RGB, exactement n) dont un canal au moins est non nul : OU des trois plans, octets nuls comptés."""
    ou = int.from_bytes(px[0::3], 'little') | int.from_bytes(px[1::3], 'little') | int.from_bytes(px[2::3], 'little')
    return n - ou.to_bytes(n, 'little').count(0)

def cle(f):
    m = re.search(r'(\d+)', f)
    return (0, int(m.group(1)), f) if m else (1, 0, f)

def main():
    dossier = sys.argv[1]
    carte = int(sys.argv[sys.argv.index('--carte') + 1]) if '--carte' in sys.argv else None
    fichiers = sorted(cle(f) for f in os.listdir(dossier) if f.endswith('.ppm'))
    prec = None
    print(f"{'frame':>6} {'R':>7} {'G':>7} {'B':>7} {'non noirs':>10} {'changés':>9}  empreinte")
    for sans_numero, n, f in fichiers:
        num = f"{f[:-4]:>6}" if sans_numero else f"{n:6d}"
        try:
            w, h, px = lire(os.path.join(dossier, f))
        except ValueError as e:                      # rien à mesurer : ligne notée, prec intact
            print(f"{num}  illisible ({e.args[0]} octets)")
            continue
        np_ = w * h
        s = [sum(px[k::3]) / np_ for k in range(3)]
        complet = len(px) == np_ * 3                 # PPM complet : comptes rapides ; tronqué : boucles d'origine
        nn = (pixels_non_nuls(px, np_) if complet else
              sum(1 for i in range(0, len(px), 3) if any(px[i:i + 3]))) / np_   # any : pixel final partiel
        if prec is None:
            ch = '—'
        elif complet and len(prec) == len(px):       # pixel changé = un octet au moins du XOR non nul
            x = (int.from_bytes(px, 'little') ^ int.from_bytes(prec, 'little')).to_bytes(len(px), 'little')
            ch = f"{pixels_non_nuls(x, np_) / np_:8.2%}"
        else:
            ch = f"{sum(1 for i in range(0, len(px), 3) if px[i:i+3] != prec[i:i+3]) / np_:8.2%}"
        print(f"{num} {s[0]:7.2f} {s[1]:7.2f} {s[2]:7.2f} {nn:9.2%} {ch:>9}  {hashlib.sha1(px).hexdigest()[:12]}"
              + ("" if complet else f"  tronqué ({len(px)}/{np_ * 3} octets)"))
        if carte is not None and n == carte and not sans_numero:
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
