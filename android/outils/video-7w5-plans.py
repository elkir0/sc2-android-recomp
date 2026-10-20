#!/usr/bin/env python3
"""Passe 7w5 — téléversements des plans vidéo (D3DFMT 50 L8 / 51 A8L8) vus par le backend Vulkan.

Entrée : le dossier SC2_TEX_DUMP d'un run (le runtime y écrit, à CHAQUE téléversement Vulkan d'une texture,
tex_<n>_<clé>_g<gen>_<w>x<h>_f<fmt>.ppm + _a.pgm ; clé = adresse invitée de l'objet texture, gen = O_GEN au
moment du draw, c.-à-d. le nombre de LockRect/UpdateTexture/UpdateSurface reçus par la texture) et, en option,
le run.log (inventaire du bilan : échantillonnages par texture).

Sortie : pour chaque texture L8/A8L8 : générations téléversées, statistiques des pixels (plan uniforme ou image),
et pour chaque plan Y (L8) un aperçu PNG en niveaux de gris (--png <dossier>).
Ni le jeu ni le runtime ne sont modifiés : lecture des fichiers seulement.

usage : video-7w5-plans.py <dossier SC2_TEX_DUMP> [run.log] [--png <dossier>]
"""
import os
import re
import sys
import zlib
import struct

NOM = re.compile(r'^tex_(\d{4})_([0-9a-f]+)_g(\d+)_(\d+)x(\d+)_f([0-9A-F]+)\.ppm$')


def lire_pnm(chemin):
    with open(chemin, 'rb') as f:
        d = f.read()
    champs, pos = [], 0
    while len(champs) < 4:
        while d[pos:pos + 1].isspace():
            pos += 1
        if d[pos:pos + 1] == b'#':
            pos = d.index(b'\n', pos) + 1
            continue
        fin = pos
        while not d[fin:fin + 1].isspace():
            fin += 1
        champs.append(d[pos:fin])
        pos = fin
    return int(champs[1]), int(champs[2]), d[pos + 1:]


def stats(octets):
    hist = [0] * 256
    for b in octets:
        hist[b] += 1
    n = len(octets)
    vals = [v for v in range(256) if hist[v]]
    moy = sum(v * hist[v] for v in vals) / n if n else 0
    dom = max(vals, key=lambda v: hist[v]) if vals else 0
    return {'min': vals[0] if vals else 0, 'max': vals[-1] if vals else 0, 'moy': moy, 'distinctes': len(vals),
            'dominante': dom, 'part_dom': hist[dom] / n if n else 0}


def png_gris(chemin, w, h, octets):
    brut = b''.join(b'\x00' + octets[y * w:(y + 1) * w] for y in range(h))
    def bloc(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xFFFFFFFF)
    with open(chemin, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + bloc(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
                + bloc(b'IDAT', zlib.compress(brut, 6)) + bloc(b'IEND', b''))


def main(argv):
    png = None
    if '--png' in argv:
        i = argv.index('--png'); png = argv[i + 1]; del argv[i:i + 2]
        os.makedirs(png, exist_ok=True)
    if not argv:
        print(__doc__); return 2
    dossier = argv[0]
    log = argv[1] if len(argv) > 1 else None
    tous = sorted(f for f in os.listdir(dossier) if NOM.match(f))
    print(f'[7W5] {dossier} : {len(tous)} téléversements vidés au total')
    par_tex = {}
    for f in tous:
        n, cle, gen, w, h, fmt = NOM.match(f).groups()
        fmt = int(fmt, 16)
        if fmt not in (50, 51):
            continue
        par_tex.setdefault(cle, []).append((int(n), int(gen), int(w), int(h), fmt, f))
    if not par_tex:
        print('[7W5] aucun téléversement L8/A8L8 vidé')
    for cle, l in sorted(par_tex.items(), key=lambda kv: kv[1][0][0]):
        n0, _, w, h, fmt, _ = l[0]
        print(f'[7W5] texture 0x{int(cle, 16):08X} {w}×{h} D3DFMT {fmt} ({"L8" if fmt == 50 else "A8L8"}) : '
              f'{len(l)} téléversement(s), générations {[g for _, g, *_ in l]}, rangs de vidage {[n for n, *_ in l]}')
        for n, gen, w, h, fmt, f in l:
            _, _, rgb = lire_pnm(os.path.join(dossier, f))
            lum = rgb[0::3][:w * h]
            s = stats(lum)
            ligne = (f'[7W5]   g{gen} (vidage {n}) L : min {s["min"]} max {s["max"]} moy {s["moy"]:.1f}, '
                     f'{s["distinctes"]} valeurs, dominante {s["dominante"]} sur {100 * s["part_dom"]:.1f} %')
            if fmt == 51:
                _, _, a = lire_pnm(os.path.join(dossier, f[:-4] + '_a.pgm'))
                sa = stats(a[:w * h])
                ligne += (f' ; A : min {sa["min"]} max {sa["max"]} moy {sa["moy"]:.1f}, {sa["distinctes"]} valeurs, '
                          f'dominante {sa["dominante"]} sur {100 * sa["part_dom"]:.1f} %')
            print(ligne)
            if png and fmt == 50:
                png_gris(os.path.join(png, f'plan-y-{int(cle, 16):08X}-g{gen}.png'), w, h, lum)
    if log:
        with open(log, 'rb') as f:
            for ligne in f.read().decode('utf-8', 'replace').splitlines():
                if re.match(r'^\[BILAN\] \[TEX\]   0x[0-9A-F]{8} 2D +(L8|A8L8) ', ligne):
                    print('[7W5] bilan :', ligne[len('[BILAN] [TEX]   '):])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
