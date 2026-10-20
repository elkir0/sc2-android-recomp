# passe 9t — difftest CIBLÉ des racines levées par la frontière gameplay (difftest-sc2.py tel quel ; gen_etat enveloppé,
# cadre descendu de 0x200 comme difftest-cible-9q.py). Généralisation de la 9q : l'état d'entrée de chaque adresse est
# décrit par un PROFIL (fichier, variable DT9T_PROFILS), une ligne par adresse :
#   <ADDR> [ecx=<v>] [edx=<v>] [eax=<v>] [args=<v>,<v>,…]
# où <v> vaut :
#   obj:<taille>        objet de <taille> octets MIS À ZÉRO dans la fenêtre scratch (pointeurs nuls : pas de libération)
#   rnd:<taille>        objet de <taille> octets aléatoires (graine du test) — pour les fonctions feuilles de calcul
#   flt:<taille>:<lo>:<hi>  objet de flottants NORMAUX dans [lo, hi]
#   int:<lo>:<hi>       entier aléatoire dans [lo, hi] (décimal ou 0x…)
#   f:<lo>:<hi>         flottant (bits IEEE) aléatoire dans [lo, hi]
#   <hex>               constante
# Usage : DT9T_PROFILS=<profils> difftest-cible-9t.py <difftest-sc2.py> [arguments de difftest-sc2.py…]
#         (lot --adresses : « ADDR SEED »)
import importlib.util, os, random, struct, sys
spec = importlib.util.spec_from_file_location("difftest_sc2", sys.argv[1]); dt = importlib.util.module_from_spec(spec)
sys.argv = [sys.argv[1]] + sys.argv[2:]
spec.loader.exec_module(dt)
gen0, t0 = dt.gen_etat, dt.tester
PROFILS = {}
for ligne in open(os.environ["DT9T_PROFILS"]):
    ligne = ligne.split("#")[0].split()
    if not ligne: continue
    p = {}
    for tok in ligne[1:]:
        k, v = tok.split("=", 1); p[k] = v.split(",") if k == "args" else v
    PROFILS[int(ligne[0], 16)] = p
COURANT = {}
def num(s): return int(s, 0)
def gen_etat(seed, dispo):
    gpr, eflags, fenetres, esp0 = gen0(seed, dispo)
    r = random.Random(9000 + seed); SCR = dispo["scratch"]; addr = COURANT["addr"]
    esp0 -= 0x200; gpr["esp"] = esp0 - 4; gpr["ebp"] = esp0 + 0x100
    if addr not in PROFILS: raise SystemExit(f"adresse sans profil : {addr:08X}")
    poser, libre = [], [SCR + 0x200]                      # (va, octets) à écrire dans les fenêtres ; allocateur
    def valeur(v):
        t = v.split(":")
        if t[0] in ("obj", "rnd", "flt"):
            n = num(t[1]); va = libre[0]; libre[0] += (n + 0x3F) & ~0x3F
            if t[0] == "obj": b = bytes(n)
            elif t[0] == "rnd": b = bytes(r.randrange(256) for _ in range(n))
            else: b = b"".join(struct.pack("<f", r.uniform(float(t[2]), float(t[3]))) for _ in range(n // 4))
            poser.append((va, b)); return va
        if t[0] == "int": return r.randint(num(t[1]), num(t[2])) & 0xFFFFFFFF
        if t[0] == "f": return struct.unpack("<I", struct.pack("<f", r.uniform(float(t[1]), float(t[2]))))[0]
        return int(v, 16)
    P = PROFILS[addr]
    for reg in ("ecx", "edx", "eax"):
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
    COURANT["addr"] = a[0]; return t0(a)
dt.gen_etat, dt.tester = gen_etat, tester
dt.main()
