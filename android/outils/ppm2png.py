#!/usr/bin/env python3
"""ppm2png.py — PPM (P6) → PNG sans dépendance (zlib) ; gain optionnel pour les frames sombres (passe 7g).
    python3 -I android/outils/ppm2png.py <entrée.ppm> <sortie.png> [gain]
Les images tirées des assets du jeu restent HORS git."""
import sys, zlib, struct, re
src, dst, gain = sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
d = open(src, 'rb').read(); m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', d)
w, h = int(m.group(1)), int(m.group(2)); px = d[m.end():]
if gain != 1.0: px = bytes(min(255, int(b * gain)) for b in px)
raw = b''.join(b'\0' + px[y*w*3:(y+1)*w*3] for y in range(h))
def ch(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
