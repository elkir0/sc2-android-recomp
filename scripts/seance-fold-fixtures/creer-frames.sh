#!/bin/bash
# creer-frames.sh — PPM 64×36 SYNTHÉTIQUES pour seance-fold.sh en ADB_FAKE (AUCUNE image du jeu) :
# damier dont la teinte dépend du n° de frame, pour vérifier que chaque frame tirée est la bonne.
#   creer-frames.sh <dossier> <n°> [n°…]
set -eu
[ $# -ge 2 ] || { echo "usage : creer-frames.sh <dossier> <n°>…" >&2; exit 2; }
D=$1; shift
mkdir -p "$D"
python3 - "$D" "$@" <<'FIN'
import sys
d, nums = sys.argv[1], [int(x) for x in sys.argv[2:]]
W, H = 64, 36
for n in nums:
    with open(f"{d}/frame_{n:05d}.ppm", "wb") as f:
        f.write(f"P6\n{W} {H}\n255\n".encode())
        base = (n % 128) * 60 // 128          # 0…59 selon le n° : chaque frame distincte
        for y in range(H):
            ligne = bytearray()
            for x in range(W):
                damier = ((x // 8 + y // 6) % 2) * 80
                v = damier + base
                ligne += bytes((v, (v * 3) % 256, (v * 7) % 256))
            f.write(ligne)
FIN
