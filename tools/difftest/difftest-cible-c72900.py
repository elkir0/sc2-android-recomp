# Difftest CIBLÉ de 0x00C72900 (YCbCr 4:2:0 → BGRA, MMX) : difftest-sc2.py tel quel, dont gen_etat est enveloppé pour
# poser 11 arguments cohérents (tampons dans la fenêtre scratch seedée, dimensions selon la graine). Comparaison inchangée
# (registres, drapeaux, octets écrits). Usage : difftest-cible-c72900.py <difftest-sc2.py> [arguments de difftest-sc2.py…]
import importlib.util, random, struct, sys
spec = importlib.util.spec_from_file_location("difftest_sc2", sys.argv[1]); dt = importlib.util.module_from_spec(spec)
sys.argv = [sys.argv[1]] + sys.argv[2:]
spec.loader.exec_module(dt)
gen0 = dt.gen_etat
PARAMS = {}
def gen_etat(seed, dispo):
    gpr, eflags, fenetres, esp0 = gen0(seed, dispo)
    r = random.Random(1000 + seed)
    SCR = dispo["scratch"]
    w, h, ligne = r.randint(1, 13), r.randint(1, 5), r.randint(0, 3)
    ystride, cstride, dpitch = 16, 8, 64
    Y, U, V, DST = SCR + 0x100, SCR + 0x400, SCR + 0x500, SCR + 0x1000
    args = [0, ligne, Y, ystride, U, V, cstride, DST, w, h, dpitch]     # [ebx+8] … [ebx+0x30]
    PARAMS[seed] = dict(w=w, h=h, ligne=ligne)
    # cadre déplacé de 0x200 vers le bas : les 11 arguments (44 o) au-dessus de esp0 restent dans la fenêtre de pile
    esp0 -= 0x200
    gpr["esp"] = esp0 - 4; gpr["ebp"] = esp0 + 0x100
    out = []
    for va, octets in fenetres:
        b = bytearray(octets)
        if va <= esp0 < va + len(b):
            struct.pack_into("<I", b, esp0 - 4 - va, dt.SENTINELLE)
            struct.pack_into("<11I", b, esp0 - va, *args)
        out.append((va, bytes(b)))
    return gpr, eflags, out, esp0
dt.gen_etat = gen_etat
t0 = dt.tester
def tester(a):
    res = t0(a); res.update(PARAMS.get(a[1], {})); return res
dt.tester = tester
dt.main()
