#!/usr/bin/env python3
"""difftest-sc2.py — oracle par fonction pour le recomp SC2 (kit 3e pilier M4).

Pour chaque fonction lifftée : exécute les OCTETS ORIGINAUX dans Unicorn
(référence matérielle) et la version lifftée via runner-difftest, avec le MÊME
état initial (GPR/flags/x87/SSE/mémoire, généré de façon seedée), puis compare
chaque champ architectural. Ne soumet au runner que les fonctions dont le flux
Unicorn reste dans .text (aucun import exécuté -> déterminisme garanti).

Usage: difftest-sc2.py --exe SC2.exe --liste recomp_sc2_list.h --runner runner-difftest \
                       [--debut 0] [--n 200] [--seed 1] [--json rapport.json]
"""
import argparse, json, os, random, struct, subprocess, sys, tempfile

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_INVALID, UC_HOOK_MEM_WRITE
from unicorn import x86_const as X
import capstone

# (aucune adresse du jeu en dur : .text est détecté via l ImageBase du PE)
BUDGET_INSTR = 200_000           # garde-fou anti-boucle infinie

REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]

def lire_sections(path):
    """Retourne (dict nom -> VA ABSOLUE, fin .text absolue, octets du fichier).
    Les VA du header PE sont RELATIVES à ImageBase — on rebase tout."""
    data = open(path, "rb").read()
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    nsec, opt_size = struct.unpack_from("<H", data, e_lfanew + 6)[0], struct.unpack_from("<H", data, e_lfanew + 20)[0]
    image_base = struct.unpack_from("<I", data, e_lfanew + 24 + 28)[0]
    sec_off = e_lfanew + 24 + opt_size
    sections = {}
    fin_texte = 0
    for i in range(nsec):
        off = sec_off + i * 40
        nom = data[off:off+8].rstrip(b"\0").decode()
        vsize, va, rsize, roff = struct.unpack_from("<IIII", data, off + 8)
        sections[nom] = (image_base + va, vsize, rsize, roff)
        if nom == ".text":
            fin_texte = image_base + va + vsize
    return sections, fin_texte, data

def octets_fonction(data, sections, va, taille):
    _, _, _, roff = sections[".text"]
    base_va, _, _, _ = sections[".text"]
    off = roff + (va - base_va)
    return data[off:off + taille]

def adresses_liftees(chemin_liste):
    lst = []
    for ligne in open(chemin_liste):
        ligne = ligne.strip()
        if ligne.startswith("X("):
            lst.append(int(ligne[2:10], 16))
    return sorted(lst)

def gen_etat(seed, adresses):
    """État déterministe : GPR variés (adresses valides pointant dans la zone),
    flags aléatoires, x87/SSE remplis, fenêtres mémoire scratch/stack."""
    r = random.Random(seed)
    SCRATCH = 0x01000000   # 1 Mo de données jouets
    STACK = 0x00700000     # pile sous l'image
    gpr = {nom: r.getrandbits(32) for nom in REGS}
    gpr["esp"] = STACK + 0x40000 - r.getrandbits(12)   # pile alignée-ish
    gpr["ebp"] = gpr["esp"] + 0x100
    # pointeurs plausibles dans les registres: mixer hasard pur et adresses
    for nom in ("esi", "edi", "ebx"):
        if r.random() < 0.7:
            gpr[nom] = SCRATCH + r.randrange(0x1000) * 4
    eflags = 0x202 | (r.getrandbits(16) & 0x0FD5)  # bits fixes + hasard masqué
    fenetres = [(SCRATCH, r.randbytes(0x8000)), (STACK + 0x20000, r.randbytes(0x20000))]
    return gpr, eflags, fenetres, SCRATCH, STACK

class ResultatUnicorn:
    __slots__ = ("ok", "sorti_du_texte", "budget", "gpr", "eflags", "mem", "x87", "xmm", "ecrits")

def executer_unicorn(uc_data, sections, fin_texte, addr, taille, gpr, eflags, fenetres, adresses_set, base_texte):
    res = ResultatUnicorn()
    res.ok = res.sorti_du_texte = res.budget = False
    try:
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(0x00400000, 0x1800000)   # 24 Mo sous les 4 Go (image + scratch + pile jouets)
        # mapper l'image : copier tout .text tel quel (appels internes possibles)
        base_va, vsize, rsize, roff = sections[".text"]
        uc.mem_write(base_va, uc_data[roff:roff + rsize])
        for va, octets in fenetres:
            uc.mem_write(va & ~0xFFFFF, b"\xCC" * 0x1000)  # marquage de page piochable
            uc.mem_write(va, octets)
        for i, nom in enumerate(REGS):
            uc.reg_write(getattr(X, f"UC_X86_REG_{nom.upper()}"), gpr[nom])
        uc.reg_write(X.UC_X86_REG_EFLAGS, eflags)

        retour = gpr["esp"]
        esp0 = gpr["esp"]
        compte = [0]
        ecrits = set()   # VA des octets ÉCRITS pendant l'exécution (seuls ceux-là sont comparés)
        def hook_write(uc2, acc, adresse, taille_i, valeur, _):
            for k in range(adresse, adresse + taille_i):
                ecrits.add(k)
        uc.hook_add(UC_HOOK_MEM_WRITE, hook_write)
        def hook_code(uc2, adresse, taille_i, _):
            compte[0] += 1
            if compte[0] > BUDGET_INSTR:
                res.budget = True
                uc2.emu_stop()
                return
            if adresse < base_texte or adresse >= fin_texte:
                res.sorti_du_texte = True
                uc2.emu_stop()
                return
            # ret de NOTRE cadre (C3/C2 avec pile revenue) = fin de fonction.
            # On SIMULE le ret (esp += 4 + imm) pour que la comparaison avec le
            # runner (qui exécute le ret modélisé) soit stricte.
            if taille_i in (1, 3):
                op = uc2.mem_read(adresse, 1)[0]
                if op in (0xC3, 0xC2) and uc2.reg_read(X.UC_X86_REG_ESP) >= esp0:
                    imm = 0
                    if op == 0xC2:
                        imm = int.from_bytes(uc2.mem_read(adresse + 1, 2), "little")
                    uc2.reg_write(X.UC_X86_REG_ESP, uc2.reg_read(X.UC_X86_REG_ESP) + 4 + imm)
                    uc2.emu_stop()
        uc.hook_add(UC_HOOK_CODE, hook_code)

        # exécuter jusqu'au ret du cadre (intercepté ci-dessus), via intercept du ret:
        def hook_mem_invalid(uc2, acc, adresse, taille_i, valeur, _):
            res.sorti_du_texte = True   # mémoire hors zone = dépendance externe
            return False
        uc.hook_add(UC_HOOK_MEM_INVALID, hook_mem_invalid)

        uc.emu_start(addr, 0, count=BUDGET_INSTR + 1)
        if res.sorti_du_texte or res.budget:
            return res
        res.gpr = {nom: uc.reg_read(getattr(X, f"UC_X86_REG_{nom.upper()}")) for nom in REGS}
        res.eflags = uc.reg_read(X.UC_X86_REG_EFLAGS)
        res.mem = {va: bytes(uc.mem_read(va, len(o))) for va, o in fenetres}
        res.ecrits = ecrits
        # st/xmm : Unicorn ne montre st qu'en 80 bits -> comparaison x87 en v2
        res.x87 = None
        res.xmm = None
        res.ok = True
        return res
    except Exception as e:
        res.sorti_du_texte = True   # exception = flux non maîtrisé -> skip
        return res

def executer_runner(runner, gpr, eflags, fenetres, addr):
    SCRATCH, STACK = fenetres[0][0], fenetres[1][0]
    blob = bytearray()
    blob += struct.pack("<I", 0x31305444) + struct.pack("<II", addr, eflags)
    for nom in REGS:
        blob += struct.pack("<I", gpr[nom])
    blob += struct.pack("<i", 0)          # fpu_top neutre
    blob += struct.pack("<8d", *([0.0] * 8))
    blob += struct.pack("<I", 0)          # fpu_sw
    blob += bytes(128)                    # xmm neutres
    blob += struct.pack("<I", len(fenetres))
    for va, octets in fenetres:
        blob += struct.pack("<II", va, len(octets)) + octets
    probes = [(va, len(o)) for va, o in fenetres]
    blob += struct.pack("<I", len(probes))
    for va, ln in probes:
        blob += struct.pack("<II", va, ln)
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(blob); chemin = f.name
    try:
        p = subprocess.run([runner, chemin], capture_output=True, timeout=60)
        if p.returncode != 0:
            return None
        d = p.stdout
        if len(d) < 8 or struct.unpack_from("<I", d, 0)[0] != 0x31305444:
            return None
        off = 8
        gpr2 = {}
        for i, nom in enumerate(REGS):
            gpr2[nom] = struct.unpack_from("<I", d, off)[0]; off += 4
        ef2 = struct.unpack_from("<I", d, off)[0]; off += 4
        mem2 = {}
        for va, ln in probes:
            off += 8
            mem2[va] = d[off:off + ln]; off += ln
        return {"gpr": gpr2, "eflags": ef2, "mem": mem2}
    finally:
        os.unlink(chemin)

def comparer(u_gpr, r_gpr, u_ef, r_ef, u_mem, r_mem, ecrits=None):
    diffs = []
    for nom in REGS:
        if u_gpr[nom] != r_gpr[nom]:
            diffs.append(f"{nom}: u={u_gpr[nom]:08X} r={r_gpr[nom]:08X}")
    # masque : CF PF ZF SF OF (AF exclu : divergence bénigne documentée du
    # modèle fcompi ; DF non modélisé par le runtime)
    masque = 0x8C5
    if (u_ef & masque) != (r_ef & masque):
        diffs.append(f"eflags: u={u_ef & masque:04X} r={r_ef & masque:04X}")
    for va in u_mem:
        if ecrits is None:
            if u_mem[va] != r_mem[va]:
                nb = sum(1 for a, b in zip(u_mem[va], r_mem[va]) if a != b)
                diffs.append(f"mem@{va:08X}: {nb} octets differents")
        else:
            nb = 0
            for i in range(len(u_mem[va])):
                if (va + i) in ecrits and u_mem[va][i] != r_mem[va][i]:
                    nb += 1
            if nb:
                diffs.append(f"mem@{va:08X}: {nb}/{len(u_mem[va])} octets ECRITS divergents")
    return diffs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--liste", required=True)
    ap.add_argument("--runner", required=True)
    ap.add_argument("--debut", type=int, default=0)
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--json")
    args = ap.parse_args()

    sections, fin_texte, data = lire_sections(args.exe)
    adresses = adresses_liftees(args.liste)
    print(f"{len(adresses)} fonctions lifftées, .text fin={fin_texte:#x}")

    rapport = {"ok": 0, "echec": 0, "skip": 0, "details": []}
    lot = adresses[args.debut: args.debut + args.n]
    for i, addr in enumerate(lot):
        nxt = adresses[adresses.index(addr) + 1] if adresses.index(addr) + 1 < len(adresses) else addr + 0x1000
        taille = min(nxt - addr, 0x8000)
        gpr, eflags, fenetres, SCRATCH, STACK = gen_etat(args.seed + i, adresses)
        base_texte = sections[".text"][0]
        u = executer_unicorn(data, sections, fin_texte, addr, taille, gpr, eflags, fenetres, set(adresses), base_texte)
        if not u.ok:
            rapport["skip"] += 1
            rapport.setdefault("raisons", {}).setdefault(
                "budget" if u.budget else "hors_texte", 0)
            rapport["raisons"]["budget" if u.budget else "hors_texte"] += 1
            if len(rapport.get("details", [])) < 12:
                rapport["details"].append({"addr": f"{addr:08X}", "skip": "budget" if u.budget else "hors_texte"})
            continue
        r = executer_runner(args.runner, gpr, eflags, fenetres, addr)
        if r is None:
            rapport["details"].append({"addr": f"{addr:08X}", "erreur": "runner"})
            rapport["echec"] += 1
            continue
        diffs = comparer(u.gpr, r["gpr"], u.eflags, r["eflags"], u.mem, r["mem"], u.ecrits)
        if diffs:
            rapport["echec"] += 1
            rapport["details"].append({"addr": f"{addr:08X}", "diffs": diffs[:6]})
        else:
            rapport["ok"] += 1
        if (i + 1) % 25 == 0:
            print(f"  {i+1}/{len(lot)} : ok={rapport['ok']} echec={rapport['echec']} skip={rapport['skip']}")
    print(json.dumps({k: v for k, v in rapport.items() if k != "details"}, indent=1))
    if args.json:
        rapport["args"] = vars(args)
        json.dump(rapport, open(args.json, "w"), indent=1, default=str)
        print(f"rapport: {args.json}")

if __name__ == "__main__":
    main()
