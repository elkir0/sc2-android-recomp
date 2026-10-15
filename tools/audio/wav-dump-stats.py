#!/usr/bin/env python3
"""extraire-audio-stats.py — vérification d'un WAV produit par SC2_AUDIO_DUMP (passe 5e).

    scripts/extraire-audio-stats.py <fichier.wav> [--secondes]

Rend : format, durée, taille, échantillons non nuls, crête, saturations (|x| = 32767),
entropie de Shannon des ÉCHANTILLONS 16 bits (bits/échantillon : 0 pour un silence parfait),
entropie des octets du fichier, et (option) l'énergie RMS par seconde — un son réel n'est
ni un aplat ni un bruit uniforme.
"""
import math, struct, sys
from collections import Counter


def main():
    p = sys.argv[1]
    d = open(p, 'rb').read()
    if d[:4] != b'RIFF' or d[8:16] != b'WAVEfmt ':
        print('pas un WAV RIFF'); sys.exit(1)
    fmt, nch, rate, _, _, bits = struct.unpack_from('<HHIIHH', d, 20)
    data = d[44:44 + struct.unpack_from('<I', d, 40)[0]]
    n = len(data) // 2
    s = struct.unpack(f'<{n}h', data[:2 * n])
    nz = sum(1 for x in s if x)
    crete = max((abs(x) for x in s), default=0)
    sat = sum(1 for x in s if abs(x) >= 32767)

    def entropie(c, total):
        return max(0.0, -sum(v / total * math.log2(v / total) for v in c.values())) if total else 0.0
    h_ech = entropie(Counter(s), n)
    h_oct = entropie(Counter(d), len(d))
    print(f'{p}')
    print(f'  format {fmt} (1 = PCM), {nch} canaux, {rate} Hz, {bits} bits ; durée {n / nch / rate:.2f} s ; {len(d)} octets')
    print(f'  échantillons non nuls {nz} / {n} ({100 * nz / max(n, 1):.1f} %) ; crête {crete} ; saturés {sat}')
    print(f'  entropie : échantillons {h_ech:.3f} bits, octets du fichier {h_oct:.3f} bits/octet')
    if '--secondes' in sys.argv:
        par = rate * nch
        lig = []
        for k in range(0, n, par):
            b = s[k:k + par]
            rms = math.sqrt(sum(x * x for x in b) / len(b)) if b else 0
            lig.append(f'{k // par:3d}s {rms:7.0f}')
        print('  RMS par seconde :')
        for i in range(0, len(lig), 6):
            print('    ' + '   '.join(lig[i:i + 6]))


if __name__ == '__main__':
    main()
