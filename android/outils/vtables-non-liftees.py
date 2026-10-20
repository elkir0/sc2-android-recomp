#!/usr/bin/env python3
"""Passe 7v — méthodes virtuelles NON liftées, relevées par les vtables d'un PE 32 bits MSVC.

  vtables-non-liftees.py <exe> <recomp_sc2_list.h> [--va X]… [--famille X]… [--liste]

Débuts de vtable, de deux sources :
  - RTTI : un Complete Object Locator (signature 0, TypeDescriptor « .?AV… ») précède la vtable ;
  - constructeurs (classes SANS RTTI) : `mov dword ptr [reg(+disp)], imm32` (C7 /0) dont imm32 pointe en données
    sur deux pointeurs vers le code. Le site de l'instruction est noté (c'est lui qui pose la vtable que lit
    `mov edx, [ecx] ; call [edx+4·i]`).
Une vtable s'étend jusqu'au début suivant ou au premier mot qui ne pointe pas dans le code. Toute entrée absente de
la X-macro LIFTED_FUNCS (recomp_sc2_list.h) est une méthode virtuelle NON liftée : la découverte du lift ne suit pas
les tables de données.
  --va X       : vtables contenant X, leurs entrées et constructeurs ;
  --famille X  : classes sœurs de X = vtables posées par des constructeurs à moins de 64 Ko de X (même module),
                 et leurs entrées non liftées ;
  --liste      : toutes les méthodes virtuelles non liftées (adresse, vtable, index, constructeur, classe).
Heuristique (pas de désassembleur) : des faux débuts sont possibles (C7 vers une table de pointeurs de code qui
n'est pas une vtable) ; ils ne retirent rien, ils ajoutent des candidats. Bibliothèque standard seulement.
"""
import re
import struct
import sys


class PE:
    def __init__(self, chemin):
        self.d = open(chemin, 'rb').read()
        e = struct.unpack_from('<I', self.d, 0x3C)[0]
        nsec = struct.unpack_from('<H', self.d, e + 6)[0]
        opt = struct.unpack_from('<H', self.d, e + 20)[0]
        self.base = struct.unpack_from('<I', self.d, e + 24 + 28)[0]
        self.sec = []
        for i in range(nsec):
            o = e + 24 + opt + 40 * i
            vs, va, rs, ro = struct.unpack_from('<IIII', self.d, o + 8)
            car = struct.unpack_from('<I', self.d, o + 36)[0]
            self.sec.append((self.base + va, min(rs, len(self.d) - ro), ro, car))
        self.texte = [(a, a + n) for a, n, ro, car in self.sec if car & 0x20000000]

    def off(self, va):
        for a, n, ro, car in self.sec:
            if a <= va < a + n:
                return va - a + ro
        return None

    def rd(self, va):
        o = self.off(va) if va is not None else None
        return None if o is None or o + 4 > len(self.d) else struct.unpack_from('<I', self.d, o)[0]

    def code(self, va):
        return va is not None and any(a <= va < b for a, b in self.texte)

    def donnees(self, va):
        return va is not None and not self.code(va) and self.off(va) is not None

    def nom_td(self, td):
        o = self.off(td + 8) if td else None
        if o is None:
            return None
        s = self.d[o:o + 200].split(b'\0')[0]
        return s.decode('latin1') if s.startswith(b'.?A') else None


def debuts(pe):
    """-> {vtable: {'classe': nom|None, 'ctors': [sites]}}"""
    vt = {}
    for a, n, ro, car in pe.sec:                                    # RTTI
        if car & 0x20000000 or not car & 0x40000000:
            continue
        for off in range(ro, ro + n - 8, 4):
            col = struct.unpack_from('<I', pe.d, off)[0]
            if pe.rd(col) != 0:
                continue
            classe = pe.nom_td(pe.rd(col + 12))
            v = a + (off - ro) + 4
            if classe and pe.code(pe.rd(v)):
                vt.setdefault(v, {'classe': classe, 'ctors': []})
    for a, n, ro, car in pe.sec:                                    # constructeurs : C7 /0 imm32
        if not car & 0x20000000:
            continue
        d = pe.d
        o = d.find(b'\xC7', ro)
        while 0 <= o < ro + n - 6:
            m = d[o + 1]
            mod, rm = m >> 6, m & 7
            if not m & 0x38 and mod != 3:
                l = 2 + (rm == 4) + {0: 4 if rm == 5 else 0, 1: 1, 2: 4}[mod]
                if mod == 0 and rm == 4 and (d[o + 2] & 7) == 5:
                    l += 4
                x = struct.unpack_from('<I', d, o + l)[0]
                if pe.donnees(x) and pe.code(pe.rd(x)) and pe.code(pe.rd(x + 4)):
                    vt.setdefault(x, {'classe': None, 'ctors': []})['ctors'].append(a + (o - ro))
            o = d.find(b'\xC7', o + 1)
    return vt


def etendre(pe, vt):
    """-> {vtable: [entrées]} : jusqu'au début suivant ou au premier mot hors code."""
    tri = sorted(vt)
    res = {}
    for i, v in enumerate(tri):
        fin = tri[i + 1] if i + 1 < len(tri) else v + 4096
        ent = []
        while v + 4 * len(ent) < fin and len(ent) < 1024:
            f = pe.rd(v + 4 * len(ent))
            if not pe.code(f):
                break
            ent.append(f)
        res[v] = ent
    return res


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    pe = PE(a[0])
    lif = {int(x, 16) for x in re.findall(r'X\(([0-9A-F]{8})\)', open(a[1]).read())}
    opt = lambda o: [int(a[i + 1], 16) for i in range(len(a) - 1) if a[i] == o]
    vt = debuts(pe)
    ent = etendre(pe, vt)
    nl = {}
    for v, e in ent.items():
        for i, f in enumerate(e):
            if f not in lif:
                nl.setdefault(f, []).append((v, i))
    print(f'# {len(vt)} vtables ({sum(1 for x in vt.values() if x["classe"])} avec RTTI, '
          f'{sum(1 for x in vt.values() if x["ctors"])} posées par un constructeur), {sum(map(len, ent.values()))} entrées ;'
          f' {len(lif)} fonctions liftées ; {len(nl)} méthodes virtuelles distinctes NON liftées')

    def montrer(v):
        x = vt[v]
        print(f'vtable 0x{v:08X} {x["classe"] or "(sans RTTI)"} ; {len(ent[v])} entrées ; constructeurs : '
              + (' '.join(f'0x{c:08X}' for c in x['ctors']) or '—'))
        for i, f in enumerate(ent[v]):
            print(f'    [{i:2d}] +0x{4 * i:02X}  0x{f:08X}  {"liftée" if f in lif else "NON LIFTÉE"}')

    for x in opt('--va'):
        for v in sorted(ent):
            if x in ent[v]:
                montrer(v)
    for x in opt('--famille'):
        voisins = sorted(v for v in ent if any(abs(c - x) < 0x10000 for c in vt[v]['ctors']))
        nonl = sorted({f for v in voisins for f in ent[v] if f not in lif})
        print(f'# famille de 0x{x:08X} : {len(voisins)} vtables posées par des constructeurs à moins de 64 Ko ; '
              f'{len(nonl)} entrées non liftées : ' + ' '.join(f'0x{f:08X}' for f in nonl))
        for v in voisins:
            montrer(v)
    if '--liste' in a:
        for f in sorted(nl):
            v, i = nl[f][0]
            x = vt[v]
            print(f'0x{f:08X}  vtable 0x{v:08X} [{i}]  {x["classe"] or "(sans RTTI)"}  ctor '
                  + (f'0x{x["ctors"][0]:08X}' if x['ctors'] else '—')
                  + (f'  (+{len(nl[f]) - 1} autres vtables)' if len(nl[f]) > 1 else ''))


if __name__ == '__main__':
    main()
