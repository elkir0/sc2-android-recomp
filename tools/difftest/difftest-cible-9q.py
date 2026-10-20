# passe 9q — difftest CIBLÉ de deux fonctions (difftest-sc2.py tel quel ; gen_etat enveloppé, cadre descendu de 0x200
# comme difftest-cible-c72900.py) :
#   0x0129DC30 (destructeur, thiscall) : ecx = objet de 0x100 octets mis à ZÉRO dans la fenêtre scratch
#              ([this+0x80] = 0 : pas de libération ; membre +0x54 puis destructeur parent 0x011E0F70) ;
#   0x00CE5610 (lerp, 2 arguments) : out[ecx] = a[edx] + t*(b[arg1] - a), 7 flottants ; a, b, out = flottants NORMAUX
#              aléatoires (pas de NaN ni de dénormal : charges utiles hors de propos), t dans [-2, 2].
# Usage : difftest-cible-9q.py <difftest-sc2.py> [arguments de difftest-sc2.py…]   (lot : « 0129DC30 n » / « 00CE5610 n »)
import importlib.util, random, struct, sys
spec = importlib.util.spec_from_file_location("difftest_sc2", sys.argv[1]); dt = importlib.util.module_from_spec(spec)
sys.argv = [sys.argv[1]] + sys.argv[2:]
spec.loader.exec_module(dt)
gen0, t0 = dt.gen_etat, dt.tester
COURANT = {}
def gen_etat(seed, dispo):
    gpr, eflags, fenetres, esp0 = gen0(seed, dispo)
    r = random.Random(9000 + seed); SCR = dispo["scratch"]; addr = COURANT["addr"]
    esp0 -= 0x200; gpr["esp"] = esp0 - 4; gpr["ebp"] = esp0 + 0x100
    poser = []                                           # (va, octets) à écrire dans les fenêtres
    if addr == 0x0129DC30:
        gpr["ecx"] = SCR + 0x200; poser.append((SCR + 0x200, bytes(0x100)))
        args = []
    elif addr == 0x00CE5610:
        fl = lambda: struct.pack("<f", r.uniform(-1000.0, 1000.0))
        A, Bv, OUT = SCR + 0x100, SCR + 0x200, SCR + 0x400
        poser += [(A, b"".join(fl() for _ in range(8))), (Bv, b"".join(fl() for _ in range(8))), (OUT, b"".join(fl() for _ in range(8)))]
        gpr["edx"], gpr["ecx"] = A, OUT
        args = [Bv, struct.unpack("<I", struct.pack("<f", r.uniform(-2.0, 2.0)))[0]]
    else:
        raise SystemExit(f"adresse non prévue : {addr:08X}")
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
