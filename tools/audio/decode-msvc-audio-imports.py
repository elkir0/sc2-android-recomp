#!/usr/bin/env python3
"""extraire-audio-imports.py — inventaire des imports AUDIO de SC2.exe et table C de l'API FMOD Ex (passe 5e).

    scripts/extraire-audio-imports.py <sc2_imports.h> <sortie.h> [--tsv <inventaire.tsv>]

SC2.exe (build 15405) joue son son par FMOD Ex 4.28.08 (fmodex_4_28_08.dll) : 129 imports,
pour l'essentiel des méthodes C++ décorées MSVC (« ?init@System@FMOD@@QAG?AW4FMOD_RESULT@@HIPAX@Z »,
QAG = public __stdcall : `this` est le 1er argument sur la pile, l'appelé dépile). Le script
décode chaque nom décoré pour en tirer, sans aucun en-tête FMOD :
  - le nombre de MOTS de 32 bits à dépiler (this compris ; double = 2 mots) ;
  - la nature de chaque paramètre : valeur ('v'), double ('d'), pointeur d'entrée ('p'),
    pointeur de SORTIE vers un scalaire ('o', octets écrits), pointeur de sortie vers un objet
    FMOD ('O' : Sound**, Channel**…), chaîne ('s'), rappel ('f').
Le runtime (runtime/sc2_audio_fmod.c) s'en sert pour un FMOD sûr par défaut : chaque sortie
reçoit une valeur neutre ou un objet neuf, chaque appel est compté — avant les méthodes réelles.
Les imports DSOUND (par ordinal) et WINMM (mixer) sont listés dans l'inventaire.
"""
import re, sys

PRIM4 = set('CDEFGHIJKM')


class Lecteur:
    def __init__(self, s):
        self.s, self.i, self.backrefs = s, 0, []

    def c(self):
        return self.s[self.i]

    def nom(self):                              # fragments jusqu'à '@' final
        parts = []
        while self.c() != '@':
            if self.c().isdigit():
                parts.append(f'#{self.c()}'); self.i += 1
            else:
                j = self.s.index('@', self.i); parts.append(self.s[self.i:j]); self.i = j + 1
        self.i += 1
        return '::'.join(reversed(parts))

    def type(self):
        """→ (genre, taille, détail). genre : v valeur, d double, p pointeur, X void, F fonction."""
        d = self.i
        c = self.c()
        if c.isdigit():                         # référence arrière à un type d'argument
            self.i += 1
            return self.backrefs[int(c)]
        if c == 'X':
            self.i += 1; return ('X', 0, 'void')
        if c in PRIM4:
            self.i += 1; return ('v', 4, {'M': 'float', 'H': 'int', 'I': 'unsigned', 'D': 'char', 'C': 'signed char',
                                          'E': 'unsigned char', 'F': 'short', 'G': 'unsigned short'}.get(c, c))
        if c in 'NO':
            self.i += 1; return ('d', 8, 'double')
        if c == '_':
            k = self.s[self.i + 1]; self.i += 2
            return ('v', 8, 'int64') if k in 'JK' else ('v', 4, {'N': 'bool', 'W': 'wchar_t'}.get(k, '_' + k))
        if c == '?' and self.s[self.i + 1] == 'A':
            self.i += 2; return self.type()
        if c == 'W':
            self.i += 2                         # W4
            n = self.nom(); return ('v', 4, 'enum ' + n)
        if c in 'UV':
            self.i += 1; n = self.nom(); return ('v', 4, 'struct ' + n)
        if c in 'PQRS':
            if self.s[self.i + 1] == '6':       # pointeur de fonction
                self.i += 3                     # P6 + convention
                self.type()                     # retour
                self.params()                   # arguments, jusqu'à Z
                t = ('f', 4, 'rappel')
            else:
                cv = self.s[self.i + 1]; self.i += 2
                cible = self.type()
                t = ('p', 4, ('const ' if cv == 'B' else '') + cible[2] + '*', cible)
            if self.i - d > 1:
                self._ref(t)
            return t
        raise ValueError(f'type inconnu {c!r} dans {self.s} @ {self.i}')

    def _ref(self, t):
        if len(self.backrefs) < 10:
            self.backrefs.append(t)

    def params(self):
        if self.c() == 'X':
            self.i += 2 if self.s[self.i + 1] == 'Z' else 1
            return []
        out = []
        while self.c() != '@' and self.c() != 'Z':
            d = self.i
            t = self.type()
            if self.i - d > 1 and t not in self.backrefs and t[0] != 'p' and t[0] != 'f':
                self._ref(t)
            out.append(t)
        self.i += 1                             # '@' (puis 'Z') ou 'Z'
        if self.i < len(self.s) and self.s[self.i] == 'Z':
            self.i += 1
        return out


def nature(t):
    g = t[0]
    if g == 'd': return 'd'
    if g == 'f': return 'f'
    if g != 'p': return 'v'
    cible = t[3]
    if t[2] in ('const char*', 'const wchar_t*'):
        return 's'
    if t[2].startswith('const '):
        return 'p'
    if cible[0] == 'p' and cible[3][2].startswith('struct '):
        return 'O'                              # Sound**, Channel**… : objet FMOD rendu
    if cible[0] in 'vd' and not cible[2].startswith('struct') and cible[2] not in ('char', 'wchar_t'):
        return 'o'                              # int* float* bool* enum* : scalaire rendu
    return 'p'                                  # tampon, structure, void* : entrée/sortie à traiter au cas par cas


def decoder(nom):
    m = re.match(r'\?(\w+)@(\w+)@FMOD@@QAG\?AW4FMOD_RESULT@@(.*)$', nom)
    if not m:
        m2 = re.match(r'_(\w+)@(\d+)$', nom)
        if m2:
            return m2.group(1), '', int(m2.group(2)) // 4, 'v' * (int(m2.group(2)) // 4), []
        return None
    meth, cls, reste = m.groups()
    L = Lecteur(reste)
    ps = L.params()
    mots = 1 + sum(2 if p[0] == 'd' else 1 for p in ps)
    nat = ''.join(nature(p) for p in ps)
    return f'{cls}::{meth}', cls, mots, nat, ps


def main():
    src, out = sys.argv[1], sys.argv[2]
    tsv = sys.argv[sys.argv.index('--tsv') + 1] if '--tsv' in sys.argv else None
    imports = re.findall(r'X\(0x([0-9A-F]+)u, "([^"]+)", "([^"]+)"\)', open(src).read())
    lignes, table = [], []
    for va, dll, nom in imports:
        d = dll.lower()
        if not (d.startswith('fmodex') or d == 'dsound.dll' or d == 'winmm.dll'):
            continue
        if d.startswith('fmodex'):
            r = decoder(nom)
            if r is None:
                raise SystemExit(f'nom non décodé : {nom}')
            court, cls, mots, nat, ps = r
            table.append((nom, court, mots, nat))
            sig = ', '.join(p[2] for p in ps)
            lignes.append(f'0x{va}\t{dll}\t{court}({sig})\t{mots}\t{nat}\t{nom}')
        else:
            lignes.append(f'0x{va}\t{dll}\t{nom}\t\t\t{nom}')
    with open(out, 'w') as f:
        f.write('/* sc2_audio_fmod_table.h — GÉNÉRÉ par scripts/extraire-audio-imports.py (passe 5e) : ne pas éditer.\n'
                ' * Les imports fmodex_4_28_08.dll de SC2.exe, décodés depuis leurs noms décorés MSVC :\n'
                ' *   mots   = mots de 32 bits dépilés par l\'appelé (stdcall ; `this` compris)\n'
                ' *   nature = un caractère par paramètre (après this) : v valeur, d double, p pointeur\n'
                ' *            d\'entrée, s chaîne, f rappel, o sortie scalaire, O sortie objet FMOD */\n')
        f.write(f'#define SC2_FMOD_NB {len(table)}\n')
        f.write('static const struct { const char *nom, *court; int mots; const char *nature; } SC2_FMOD[SC2_FMOD_NB] = {\n')
        for nom, court, mots, nat in table:
            f.write(f'    {{ "{nom}", "{court}", {mots}, "{nat}" }},\n')
        f.write('};\n')
        f.write('/* un trampoline par import : le dispatcher d\'imports ne transmet pas leur identité */\n')
        f.write('#define SC2_FMOD_INDICES(X) ' + ' '.join(f'X({k})' for k in range(len(table))) + '\n')
    if tsv:
        with open(tsv, 'w') as f:
            f.write('iat\tdll\tsignature décodée\tmots\tnature\tnom décoré\n' + '\n'.join(lignes) + '\n')
    print(f'{len(table)} imports FMOD décodés, {len(lignes) - len(table)} DSOUND/WINMM → {out}')


if __name__ == '__main__':
    main()
