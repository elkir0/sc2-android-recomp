# passe 9w — difftest CIBLÉ des fonctions reliftées (rsqrtss / rsqrtps / rcpps). difftest-sc2.py tel quel ; gen_etat et
# decode enveloppés (cadre descendu de 0x200 comme difftest-cible-9q/9t.py). Étend difftest-cible-9t.py :
#   - champs IMBRIQUÉS : <v>[<off>=<v>;<off>=<v>…] pose, dans l'objet alloué, des valeurs (4 octets) ou des pointeurs
#     vers d'autres objets, récursivement (offsets hexadécimaux) ;
#   - COUVERTURE : DT9W_COUVRE=<va>,<va>,… (hex) — chaque test compte les passages de l'ORACLE (Unicorn) sur ces
#     adresses (les instructions reliftées) ; résumé « couverture » sur stderr et dans le rapport de chaque test.
# Profils (DT9W_PROFILS), une ligne par adresse (plusieurs lignes = VARIANTES, choisies par graine modulo n) : <ADDR> [ecx=<v>] [edx=<v>] [eax=<v>] [esi=<v>] [edi=<v>] [args=<v>,<v>,…]
#   obj:<taille>  zéros     rnd:<taille>  octets aléatoires     flt:<taille>:<lo>:<hi>  flottants normaux
#   int:<lo>:<hi>  entier   f:<lo>:<hi>  flottant (bits IEEE)   u16:<n>:<lo>:<hi>  tableau d'indices   <hex>  constante
# Usage : DT9W_PROFILS=<profils> [DT9W_COUVRE=…] difftest-cible-9w.py <difftest-sc2.py> [arguments de difftest-sc2.py…]
import importlib.util, os, random, struct, sys
from collections import Counter
spec = importlib.util.spec_from_file_location("difftest_sc2", sys.argv[1]); dt = importlib.util.module_from_spec(spec)
sys.argv = [sys.argv[1]] + sys.argv[2:]
spec.loader.exec_module(dt)
gen0, t0, dec0 = dt.gen_etat, dt.tester, dt.decode

def decouper(s, sep):                                   # découpe au niveau 0 des crochets
    out, prof, cur = [], 0, ""
    for ch in s:
        if ch == "[": prof += 1
        elif ch == "]": prof -= 1
        if ch == sep and prof == 0: out.append(cur); cur = ""
        else: cur += ch
    return out + [cur]
PROFILS = {}
for ligne in open(os.environ["DT9W_PROFILS"]):
    ligne = ligne.split("#")[0].split()
    if not ligne: continue
    p = {}
    for tok in ligne[1:]:
        k, v = tok.split("=", 1); p[k] = decouper(v, ",") if k == "args" else v
    PROFILS.setdefault(int(ligne[0], 16), []).append(p)   # plusieurs lignes = variantes (graine modulo n)
COUVRE = [int(x, 16) for x in os.environ.get("DT9W_COUVRE", "").split(",") if x]
VUS = Counter(); BILAN = Counter(); TESTS = Counter()
def decode(texte, pc):
    if pc in COUVRE: VUS[pc] += 1
    return dec0(texte, pc)
COURANT = {}
def num(s): return int(s, 0)
def gen_etat(seed, dispo):
    gpr, eflags, fenetres, esp0 = gen0(seed, dispo)
    r = random.Random(9000 + seed); SCR = dispo["scratch"]; addr = COURANT["addr"]
    esp0 -= 0x200; gpr["esp"] = esp0 - 4; gpr["ebp"] = esp0 + 0x100
    if addr not in PROFILS: raise SystemExit(f"adresse sans profil : {addr:08X}")
    poser, libre = [], [SCR + 0x200]
    def valeur(v):
        champs = []
        if v.endswith("]"):
            i = v.index("["); champs = [c for c in decouper(v[i + 1:-1], ";") if c]; v = v[:i]
        t = v.split(":")
        if t[0] in ("obj", "rnd", "flt"):
            n = num(t[1]); va = libre[0]; libre[0] += (n + 0x3F) & ~0x3F
            if t[0] == "obj": b = bytearray(n)
            elif t[0] == "rnd": b = bytearray(r.randrange(256) for _ in range(n))
            else: b = bytearray(b"".join(struct.pack("<f", r.uniform(float(t[2]), float(t[3]))) for _ in range(n // 4)))
            for c in champs:
                off, sv = c.split("=", 1); struct.pack_into("<I", b, int(off, 16), valeur(sv))
            poser.append((va, bytes(b))); return va
        if t[0] == "u16":                                # tableau de <n> u16 dans [lo, hi] (indices)
            n = num(t[1]); va = libre[0]; libre[0] += (2 * n + 0x3F) & ~0x3F
            poser.append((va, b"".join(struct.pack("<H", r.randint(num(t[2]), num(t[3]))) for _ in range(n)))); return va
        if t[0] == "int": return r.randint(num(t[1]), num(t[2])) & 0xFFFFFFFF
        if t[0] == "f": return struct.unpack("<I", struct.pack("<f", r.uniform(float(t[1]), float(t[2]))))[0]
        return int(v, 16)
    P = PROFILS[addr][seed % len(PROFILS[addr])]
    for reg in ("ecx", "edx", "eax", "esi", "edi"):
        if reg in P: gpr[reg] = valeur(P[reg])
    args = [valeur(v) for v in P.get("args", [])]
    out = []
    for va, octets in fenetres:
        b = bytearray(octets)
        if va <= esp0 < va + len(b):
            struct.pack_into("<I", b, esp0 - 4 - va, dt.SENTINELLE)
            for k, v in enumerate(args): struct.pack_into("<I", b, esp0 - va + 4 * k, v)
        for pva, p in poser:
            if va <= pva and pva + len(p) <= va + len(b): b[pva - va:pva - va + len(p)] = p
        out.append((va, bytes(b)))
    return gpr, eflags, out, esp0
def tester(a):
    COURANT["addr"] = a[0]; VUS.clear()
    res = t0(a)
    vus = {f"{k:08X}": n for k, n in VUS.items()}
    res["couverture"] = vus
    TESTS[a[0]] += 1
    for k in VUS: BILAN[(a[0], k)] += 1
    return res
dt.gen_etat, dt.tester, dt.decode = gen_etat, tester, decode
try:
    dt.main()
finally:
    if COUVRE:
        for f in sorted(TESTS):
            atteints = {k: BILAN[(f, k)] for k in COUVRE if BILAN[(f, k)]}
            sys.stderr.write(f"[couverture] {f:08X} : {TESTS[f]} test(s) ; adresses reliftées atteintes par l'oracle : "
                             + (", ".join(f"{k:08X}×{n} tests" for k, n in atteints.items()) or "AUCUNE") + "\n")
