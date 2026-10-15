#!/usr/bin/env python3
"""extraire-shaders.py — extrait les shaders D3D9 du cache de SC2 (passe 5d).

    scripts/extraire-shaders.py <baselineCache.bin> <dossier> [--boot <dossier de dump runtime>]

Le cache `Versions/Shaders14513/baselineCache.bin` (141 Mo, lu au boot depuis la passe 6a)
contient les bytecodes D3D9 précompilés de toutes les variantes de shaders du jeu. Son
conteneur (en-tête « DMSER », enregistrements non alignés) n'est pas décodé ici : on
cherche chaque jeton de version vs/ps 2.x/3.0 (0xFFFE02xx, 0xFFFE0300, 0xFFFF02xx,
0xFFFF0300) à toute position, puis on VALIDE le bytecode jeton par jeton jusqu'à
0x0000FFFF (opcodes connus, longueurs d'instruction cohérentes) — d3d9sm.longueur().

Sortie :
  <dossier>/cache/{vs,ps}_<hash>.bin   bytecodes distincts (hash = FNV-1a 64, celui du runtime)
  <dossier>/index-cache.tsv            hash, type, version, octets, 1er offset, occurrences, instructions
  --boot <dump>  (dump du runtime : SC2_SHADERS_DUMP=<dump> ./recomp-sc2) : copie dans
  <dossier>/boot/ les shaders créés par le boot et vérifie qu'ils viennent bien du cache.
"""
import os, re, shutil, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d9sm as sm


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for x in b:
        h ^= x
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    cache, out = sys.argv[1], sys.argv[2]
    d = open(cache, 'rb').read()
    os.makedirs(os.path.join(out, 'cache'), exist_ok=True)
    motif = re.compile(rb'[\x00\x01][\x02\x03]\xfe\xff|[\x00\x01][\x02\x03]\xff\xff', re.S)
    vus, cand = {}, 0
    for m in motif.finditer(d):
        cand += 1
        off = m.start()
        n = sm.longueur(d, off)
        if not n:
            continue
        code = d[off:off + n]
        h = fnv1a64(code)
        if h in vus:
            vus[h][1] += 1
            continue
        ps = code[2:4] == b'\xff\xff'
        ver = (code[1], code[0])
        try:
            ninstr = len(sm.decoder(code)[2])
        except (struct.error, IndexError):
            continue
        vus[h] = [off, 1, ps, ver, n, ninstr]
        with open(os.path.join(out, 'cache', f"{'ps' if ps else 'vs'}_{h:016x}.bin"), 'wb') as f:
            f.write(code)
    with open(os.path.join(out, 'index-cache.tsv'), 'w') as f:
        f.write('hash\ttype\tversion\toctets\toffset\toccurrences\tinstructions\n')
        for h, (off, occ, ps, ver, n, ni) in sorted(vus.items(), key=lambda kv: kv[1][0]):
            f.write(f"{h:016x}\t{'ps' if ps else 'vs'}\t{ver[0]}_{ver[1]}\t{n}\t{off}\t{occ}\t{ni}\n")
    nps = sum(1 for v in vus.values() if v[2])
    print(f'{cache} : {len(d)} octets, {cand} jetons de version candidats, '
          f'{len(vus)} bytecodes distincts valides ({nps} PS, {len(vus) - nps} VS)')
    if '--boot' in sys.argv:
        dump = sys.argv[sys.argv.index('--boot') + 1]
        os.makedirs(os.path.join(out, 'boot'), exist_ok=True)
        dans, hors = 0, []
        for fn in sorted(os.listdir(dump)):
            if not fn.endswith('.bin'):
                continue
            h = int(fn[3:19], 16)
            shutil.copy(os.path.join(dump, fn), os.path.join(out, 'boot', fn))
            if h in vus:
                dans += 1
            else:
                hors.append(fn)
        print(f'boot : {dans + len(hors)} shaders créés par le jeu, {dans} présents dans le cache, '
              f'{len(hors)} absents : {" ".join(hors)}')


if __name__ == '__main__':
    main()
