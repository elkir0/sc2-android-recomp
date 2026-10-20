#!/usr/bin/env python3
"""difftest-sc2.py — oracle par fonction pour le recomp SC2 (kit 3e pilier M4 ; v2 passe 7k).

Pour chaque fonction lifftée : exécute les OCTETS ORIGINAUX dans Unicorn
(référence matérielle) et la version lifftée via runner-difftest, avec le MÊME
état initial (GPR/flags/mémoire, générés de façon seedée), puis compare chaque
champ architectural. Ne soumet au runner que les fonctions dont le flux Unicorn
reste dans .text (aucun import exécuté -> déterminisme garanti).

Usage: difftest-sc2.py --exe SC2.exe --liste recomp_sc2_list.h --runner runner-difftest \
                       [--debut 0] [--n 200] [--seed 1] [--json rapport.json]
                       [--adresses lot.txt] [--jobs 3] [--disposition v2|m4] [--masques ...]

v2 (passe 7k, DIVERGENCES.md) :
  * cadre de retour COMMUN : une adresse de retour sentinelle est posée à [esp0-4] dans
    l'état initial des deux côtés ; Unicorn s'arrête quand il y saute, le lifté revient
    de fn() après son `ret` — esp final comparable sans simulation.
  * disposition v2 : fenêtre scratch hors de l'image (la v1/M4 la posait à 0x01000000,
    DANS .text : les pointeurs du scratch visaient du code) ; l'image (en-têtes + toutes
    les sections, octets du fichier) est la même des deux côtés (le runner la lit via
    SC2_DT_IMAGE) ; pas de page 0xCC posée d'un seul côté.
  * diagnostic : dernier écrivain Unicorn de chaque registre / drapeau / octet divergent
    (adresse + désassemblage) ; arrêt du runner (signal, [ARRÊT] MANQUE/TODO, délai).
  * masques de CONVENTION (comptés à part, jamais silencieux) : drapeau dont le dernier
    écrivain le laisse INDÉFINI selon Intel (capstone UNDEFINED_*), écarts sous l'esp
    final (pile morte) seulement si --masques pile-morte.
  * disposition v2 : mot de contrôle x87 de Windows (0x027F) côté Unicorn, comme le runtime.
  * --adresses : lot ciblé, une ligne « ADDR [SEED] » ; --jobs : processus parallèles.
"""
import argparse, json, os, random, struct, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_INVALID, UC_HOOK_MEM_WRITE
from unicorn import x86_const as X
import capstone
from capstone import x86_const as CX

BASE_TEXTE = 0x00801000          # .text VA (base image 0x00800000)
BUDGET_INSTR = 200_000           # garde-fou anti-boucle infinie
SENTINELLE = 0x00401000          # adresse de retour du cadre de test (mappée, hors .text)

REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
PRIVILEGIEES = {"in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd", "hlt", "cli", "sti",
                "lgdt", "lidt", "lldt", "ltr", "invd", "wbinvd", "rdmsr", "wrmsr", "int", "into",
                "iretd", "sysenter", "syscall"}
DRAPEAUX = {"cf": 0, "pf": 2, "af": 4, "zf": 6, "sf": 7, "of": 11}
MASQUE_EFL = 0xFD5               # bits comparés (CF PF AF ZF SF TF IF DF OF)

# disposition des fenêtres de données
DISPO = {
    # m4 : celle de la campagne M4 (rapports-difftest-masse) — scratch DANS .text
    "m4": {"scratch": 0x01000000, "pile": 0x00700000, "image": "texte-unicorn", "cc": True, "fpcw": None},
    # v2 : scratch hors image, image complète des deux côtés
    "v2": {"scratch": 0x04000000, "pile": 0x00700000, "image": "complete", "cc": False, "fpcw": 0x027F},
}

def lire_pe(path):
    """(sections {nom: (va_abs, vsize, rsize, roff)}, fin .text, octets, image_base, size_of_image, size_of_headers)."""
    data = open(path, "rb").read()
    e = struct.unpack_from("<I", data, 0x3C)[0]
    nsec, opt_size = struct.unpack_from("<H", data, e + 6)[0], struct.unpack_from("<H", data, e + 20)[0]
    image_base = struct.unpack_from("<I", data, e + 24 + 28)[0]
    size_image = struct.unpack_from("<I", data, e + 24 + 56)[0]
    size_hdr = struct.unpack_from("<I", data, e + 24 + 60)[0]
    sec_off = e + 24 + opt_size
    sections, fin_texte = {}, 0
    for i in range(nsec):
        off = sec_off + i * 40
        nom = data[off:off + 8].rstrip(b"\0").decode()
        vsize, va, rsize, roff = struct.unpack_from("<IIII", data, off + 8)
        sections[nom] = (image_base + va, vsize, rsize, roff)
        if nom == ".text":
            fin_texte = image_base + va + vsize
    return sections, fin_texte, data, image_base, size_image, size_hdr

IMPORT_BASE = 0x00402000        # slots d'IAT : sentinelles hors .text, une par import

def image_plate(pe):
    """Image chargée telle que le chargeur PE la pose (sans relocations : base préférée).
    Chaque slot d'IAT reçoit une sentinelle distincte hors .text (IMPORT_BASE + 4k) : le fichier
    y porte la RVA du nom de l'import (lue telle quelle, un `jmp [IAT]` sautait au MILIEU de .text)
    et la v1 n'y avait que des zéros (saut vers 0 → 1 805 « runner » de la campagne M4)."""
    sections, _, data, base, taille, hdr = pe
    img = bytearray(taille)
    img[:hdr] = data[:hdr]
    for nom, (va, vsize, rsize, roff) in sections.items():
        n = min(rsize, vsize) if vsize else rsize
        img[va - base: va - base + n] = data[roff: roff + n]
    e = struct.unpack_from("<I", img, 0x3C)[0]
    imp_rva = struct.unpack_from("<I", img, e + 24 + 96 + 8)[0]     # répertoire 1 : imports
    k = 0
    while imp_rva:
        oft, _, _, nom_rva, ft = struct.unpack_from("<IIIII", img, imp_rva)
        if not ft:
            break
        slot = ft
        while struct.unpack_from("<I", img, slot)[0]:
            struct.pack_into("<I", img, slot, IMPORT_BASE + 4 * k)
            k += 1; slot += 4
        imp_rva += 20
    return bytes(img)

def segments_image(pe, img):
    """Parties non nulles de l'image (en-têtes + octets du fichier de chaque section, IAT
    patchée comprise) : (va, octets). Le reste (BSS, bourrage) est nul des deux côtés — inutile
    d'écrire 46 Mo par test (temps système de la v2 initiale)."""
    sections, _, data, base, taille, hdr = pe
    segs = [(base, img[:hdr])]
    for nom, (va, vsize, rsize, roff) in sorted(sections.items(), key=lambda kv: kv[1][0]):
        n = min(rsize, vsize) if vsize else rsize
        if n:
            segs.append((va, img[va - base: va - base + n]))
    return segs

def adresses_liftees(chemin_liste):
    lst = []
    for ligne in open(chemin_liste):
        ligne = ligne.strip()
        if ligne.startswith("X("):
            lst.append(int(ligne[2:10], 16))
    return sorted(lst)

def gen_etat(seed, dispo):
    """État déterministe (identique à la v1 pour une même graine et la disposition m4)."""
    r = random.Random(seed)
    SCRATCH, STACK = dispo["scratch"], dispo["pile"]
    gpr = {nom: r.getrandbits(32) for nom in REGS}
    gpr["esp"] = STACK + 0x40000 - r.getrandbits(12)
    gpr["ebp"] = gpr["esp"] + 0x100
    for nom in ("esi", "edi", "ebx"):
        if r.random() < 0.7:
            gpr[nom] = SCRATCH + r.randrange(0x1000) * 4
    eflags = 0x202 | (r.getrandbits(16) & 0x0FD5)
    # ABI Win32 : DF = 0 à l'entrée de toute fonction ; TF (pas à pas) n'a pas de sens ici.
    # La v1 les tirait au hasard ; le runner ne modélise ni l'un ni l'autre (cpu.h).
    eflags &= ~0x500
    fenetres = [(SCRATCH, bytearray(r.randbytes(0x8000))), (STACK + 0x20000, bytearray(r.randbytes(0x20000)))]
    # cadre de retour commun : [esp0-4] = sentinelle, esp = esp0-4
    esp0 = gpr["esp"]
    pile_va, pile = fenetres[1]
    struct.pack_into("<I", pile, esp0 - 4 - pile_va, SENTINELLE)
    gpr["esp"] = esp0 - 4
    return gpr, eflags, [(va, bytes(o)) for va, o in fenetres], esp0

# ------------------------------------------------------------------ capstone ---
_MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
_MD.detail = True
_CACHE = {}
_REG32 = {}
for _n, _r in (("eax", ("eax", "ax", "al", "ah")), ("ecx", ("ecx", "cx", "cl", "ch")),
               ("edx", ("edx", "dx", "dl", "dh")), ("ebx", ("ebx", "bx", "bl", "bh")),
               ("esp", ("esp", "sp")), ("ebp", ("ebp", "bp")), ("esi", ("esi", "si")),
               ("edi", ("edi", "di"))):
    for s in _r:
        _REG32[s] = _n
_EFL = {f: (getattr(CX, f"X86_EFLAGS_MODIFY_{f.upper()}") | getattr(CX, f"X86_EFLAGS_RESET_{f.upper()}")
            | getattr(CX, f"X86_EFLAGS_SET_{f.upper()}") | getattr(CX, f"X86_EFLAGS_UNDEFINED_{f.upper()}"),
            getattr(CX, f"X86_EFLAGS_UNDEFINED_{f.upper()}")) for f in DRAPEAUX}

def decode(texte, pc):
    """(texte asm, registres 32 écrits, {drapeau: indéfini?} écrits, compte dynamique) — cache par pc."""
    d = _CACHE.get(pc)
    if d is None:
        base_va, _, _, roff, data = texte
        o = roff + pc - base_va
        try:
            i = next(_MD.disasm(data[o:o + 16], pc, 1))
            asm = f"{i.mnemonic} {i.op_str}".strip()
            _, ecr = i.regs_access()
            regs = sorted({_REG32[i.reg_name(r)] for r in ecr if i.reg_name(r) in _REG32})
            efl = {f: bool(i.eflags & und) for f, (tout, und) in _EFL.items() if i.eflags & tout}
            # Corrections à capstone 5 d'après le SDM Intel (vol. 2) :
            #  - imul : SF (et ZF, AF, PF) indéfinis ; capstone dit SF « modifié » ;
            #  - décalages/rotations : OF défini seulement pour un compte de 1 (capstone : shl/shr/sar
            #    toujours « modifié », rol/ror toujours « indéfini ») ; un compte nul ne touche aucun
            #    drapeau. Compte immédiat : tranché ici ; compte cl : tranché à l'exécution (dyn).
            dyn = None
            if i.mnemonic in ("fcomi", "fucomi", "fcomip", "fucomip", "fcompi", "fucompi"):
                # Intel : OF = SF = AF = 0. QEMU (donc Unicorn) ne touche que ZF/PF/CF et garde
                # les trois autres : écart de l'ORACLE, pas du lifté (masque « oracle-qemu »).
                for f in ("of", "sf", "af"):
                    efl[f] = "qemu"
            if i.mnemonic in ("and", "or", "xor", "test"):
                # SDM : OF = CF = 0, SF/ZF/PF selon le résultat, AF indéfini — pour TOUTES les formes
                # (capstone 5 ne déclare aucun drapeau pour `test m32, r32`, opcode 85 /r mémoire).
                efl.update({"of": False, "cf": False, "sf": False, "zf": False, "pf": False, "af": True})
            if i.mnemonic in ("imul",) and "sf" in efl:
                efl["sf"] = True
            if i.mnemonic in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr", "shld", "shrd"):
                ops = i.operands
                cpt = ops[-1]
                if len(ops) == 1:
                    n = 1
                elif cpt.type == CX.X86_OP_IMM:
                    n = cpt.imm & 31
                else:
                    n, dyn = None, "cl"
                if n is not None:
                    if n == 0:
                        efl = {}
                    elif "of" in efl or i.mnemonic in ("rol", "ror", "rcl", "rcr"):
                        efl["of"] = n != 1
            d = (asm, regs, efl, dyn)
        except StopIteration:
            d = ("(indécodable)", [], {}, None)
        _CACHE[pc] = d
    return d

# ------------------------------------------------------------------- Unicorn ---
class ResU:
    __slots__ = ("ok", "raison", "gpr", "eflags", "mem", "der_reg", "der_efl", "der_mem", "n_instr", "pcs")

def executer_unicorn(ctx, addr, gpr, eflags, fenetres):
    texte, fin_texte, dispo, img = ctx["texte"], ctx["fin_texte"], ctx["dispo"], ctx["image"]
    res = ResU()
    res.ok, res.raison = False, None
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    if dispo["image"] == "complete":
        uc.mem_map(0x00400000, 0x00400000)                       # pile + sentinelle
        uc.mem_map(0x00800000, (len(img) + 0xFFF) & ~0xFFF)
        for va, octets in ctx["segments"]:
            uc.mem_write(va, octets)
        uc.mem_map(dispo["scratch"], 0x00100000)
    else:
        uc.mem_map(0x00400000, 0x1800000)
        base_va, vsize, roff, rsize = texte[0], texte[1], texte[3], texte[2]
        uc.mem_write(base_va, texte[4][roff:roff + rsize])
    for va, octets in fenetres:
        if dispo["cc"]:
            uc.mem_write(va & ~0xFFFFF, b"\xCC" * 0x1000)
        uc.mem_write(va, octets)
    for nom in REGS:
        uc.reg_write(getattr(X, f"UC_X86_REG_{nom.upper()}"), gpr[nom])
    uc.reg_write(X.UC_X86_REG_EFLAGS, eflags)
    if dispo["fpcw"] is not None:   # mot de contrôle x87 d'un fil Windows (Unicorn démarre à 0x037F)
        uc.reg_write(X.UC_X86_REG_FPCW, dispo["fpcw"])

    der_reg, der_efl, der_mem = {}, {}, {}
    etat = {"n": 0, "pc": addr, "stop": None}
    pcs = []
    def hook_code(u, pc, taille_i, _):
        etat["n"] += 1
        if pc == SENTINELLE:
            etat["stop"] = "fin"; u.emu_stop(); return
        if etat["n"] > BUDGET_INSTR:
            etat["stop"] = "budget"; u.emu_stop(); return
        if pc < BASE_TEXTE or pc >= fin_texte:
            etat["stop"] = "import" if IMPORT_BASE <= pc < IMPORT_BASE + 0x2000 else "hors_texte"
            u.emu_stop(); return
        etat["pc"] = pc
        if len(pcs) < 4096:
            pcs.append(pc)
        asm, regs, efl, dyn = decode(texte, pc)
        if asm == "rdtsc":
            # compteur de cycles : la valeur (et tout ce qui en dérive, mémoire comprise) diffère
            # d'une exécution à l'autre des DEUX côtés — fonction non déterministe, pas comparable.
            etat["stop"] = "non_deterministe"; u.emu_stop(); return
        if asm.split(" ")[0] in PRIVILEGIEES:
            # en mode utilisateur Windows : exception (#GP) ; Unicorn les exécute. Ce n'est pas
            # du code que le jeu atteint (données désassemblées, ou sonde anti-VM `in eax, dx`).
            etat["stop"] = "privilegiee"; u.emu_stop(); return
        for r in regs:
            der_reg[r] = pc
        if dyn == "cl":
            n = u.reg_read(X.UC_X86_REG_ECX) & 31
            if n == 0:
                efl = {}
            else:
                efl = dict(efl)
                efl["of"] = n != 1
        for f, ind in efl.items():
            der_efl[f] = (pc, ind)
    def hook_write(u, acc, adresse, taille_i, valeur, _):
        pc = etat["pc"]
        for k in range(taille_i):
            der_mem[adresse + k] = pc
    def hook_invalid(u, acc, adresse, taille_i, valeur, _):
        etat["stop"] = "hors_texte"
        return False
    uc.hook_add(UC_HOOK_CODE, hook_code)
    uc.hook_add(UC_HOOK_MEM_WRITE, hook_write)
    uc.hook_add(UC_HOOK_MEM_INVALID, hook_invalid)
    try:
        uc.emu_start(addr, 0, count=BUDGET_INSTR + 2)
    except UcError:
        if etat["stop"] is None:
            etat["stop"] = "exception"
    if etat["stop"] is None and uc.reg_read(X.UC_X86_REG_EIP) == 0:
        # emu_start(…, until=0) : un saut/appel vers 0 (pointeur nul) ARRÊTE Unicorn sans erreur.
        # La v1 le prenait pour une fin normale et soumettait au runner, qui s'arrête en MANQUE.
        etat["stop"] = "saut_vers_0"
    if etat["stop"] != "fin":
        res.raison = etat["stop"] or "sans_ret"
        return res
    res.gpr = {nom: uc.reg_read(getattr(X, f"UC_X86_REG_{nom.upper()}")) for nom in REGS}
    res.eflags = uc.reg_read(X.UC_X86_REG_EFLAGS)
    res.mem = {va: bytes(uc.mem_read(va, len(o))) for va, o in fenetres}
    res.der_reg, res.der_efl, res.der_mem, res.n_instr, res.pcs = der_reg, der_efl, der_mem, etat["n"], pcs
    res.ok = True
    return res

# -------------------------------------------------------------------- runner ---
def executer_runner(ctx, gpr, eflags, fenetres, addr):
    blob = bytearray()
    blob += struct.pack("<I", 0x31305444) + struct.pack("<II", addr, eflags)
    for nom in REGS:
        blob += struct.pack("<I", gpr[nom])
    blob += struct.pack("<i", 0)
    blob += struct.pack("<8d", *([0.0] * 8))
    blob += struct.pack("<I", 0)
    blob += bytes(128)
    poses = list(fenetres)
    if ctx["dispo"]["cc"]:   # m4 : la page 0xCC n'était posée que côté Unicorn — reproduit tel quel
        pass
    blob += struct.pack("<I", len(poses))
    for va, octets in poses:
        blob += struct.pack("<II", va, len(octets)) + octets
    probes = [(va, len(o)) for va, o in fenetres]
    blob += struct.pack("<I", len(probes))
    for va, ln in probes:
        blob += struct.pack("<II", va, ln)
    with tempfile.NamedTemporaryFile(delete=False, dir=ctx.get("tmp")) as f:
        f.write(blob); chemin = f.name
    env = dict(os.environ)
    if ctx["dispo"]["image"] == "complete":
        env["SC2_DT_IMAGE"] = ctx["image_fichier"]
    try:
        try:
            p = subprocess.run(ctx["runner_cmd"] + [chemin], capture_output=True, timeout=20, env=env)
        except subprocess.TimeoutExpired:
            return None, {"type": "delai"}
        d = p.stdout
        if p.returncode != 0:
            txt = d.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace")
            arret = next((l.strip() for l in txt.splitlines() if l.startswith("[ARRÊT] [") or l.startswith("[SIGNAL]")), "")
            if "[ARRÊT]" in arret:
                arret = arret.split("] ", 2)[-1]
            return None, {"type": "rc", "rc": p.returncode, "arret": arret[:200]}
        if len(d) < 8 or struct.unpack_from("<I", d, 0)[0] != 0x31305444:
            return None, {"type": "sortie"}
        off = 8
        gpr2 = {}
        for nom in REGS:
            gpr2[nom] = struct.unpack_from("<I", d, off)[0]; off += 4
        ef2 = struct.unpack_from("<I", d, off)[0]; off += 4
        off += 4 + 64 + 4 + 128
        mem2 = {}
        for va, ln in probes:
            off += 8
            mem2[va] = d[off:off + ln]; off += ln
        return {"gpr": gpr2, "eflags": ef2, "mem": mem2}, None
    finally:
        os.unlink(chemin)

# ---------------------------------------------------------------- comparaison ---
def comparer(ctx, u, r, fenetres, esp0, masques):
    """Rend (diffs bruts, diffs masqués) — chaque diff est un dict avec le dernier écrivain."""
    texte = ctx["texte"]
    def ecrivain(pc):
        return None if pc is None else {"pc": f"{pc:08X}", "asm": decode(texte, pc)[0]}
    bruts, masques_app = [], []
    for nom in REGS:
        if u.gpr[nom] != r["gpr"][nom]:
            w = ecrivain(u.der_reg.get(nom))
            dd = {"quoi": nom, "u": f"{u.gpr[nom]:08X}", "r": f"{r['gpr'][nom]:08X}", "ecrivain": w}
            asm = (w or {}).get("asm", "")
            if asm == "rdtsc" and "rdtsc" in masques:
                dd["masque"] = "rdtsc"          # compteur de cycles : non déterministe des deux côtés
            elif (nom == "esp" and asm.startswith("ret 0x") and int(asm[6:], 16) >= 0x8000
                  and "oracle-qemu" in masques):
                dd["masque"] = "oracle-qemu"    # QEMU lit l'imm16 de `ret` SIGNÉ ; Intel : non signé
            (masques_app if dd.get("masque") else bruts).append(dd)
    xe = (u.eflags ^ r["eflags"]) & MASQUE_EFL
    for f, b in DRAPEAUX.items():
        if xe >> b & 1:
            pc, ind = u.der_efl.get(f, (None, False))
            dd = {"quoi": f, "u": u.eflags >> b & 1, "r": r["eflags"] >> b & 1, "ecrivain": ecrivain(pc),
                  "indefini": ind}
            masque = (ind is True and "drapeaux-indefinis" in masques) or (ind == "qemu" and "oracle-qemu" in masques)
            (masques_app if masque else bruts).append(dd)
    for b, nom in ((8, "tf"), (9, "if"), (10, "df")):
        if xe >> b & 1:
            dd = {"quoi": nom, "u": u.eflags >> b & 1, "r": r["eflags"] >> b & 1}
            (masques_app if nom == "df" and "df" in masques else bruts).append(dd)
    esp_fin = u.gpr["esp"]
    for va, o in fenetres:
        a, b2 = u.mem[va], r["mem"][va]
        if a == b2:
            continue
        octs = [k for k in range(len(a)) if a[k] != b2[k]]
        # regrouper en plages contiguës (écart ≤ 3)
        plages, deb, prec = [], octs[0], octs[0]
        for k in octs[1:]:
            if k - prec > 3:
                plages.append((deb, prec)); deb = k
            prec = k
        plages.append((deb, prec))
        for d0, d1 in plages:
            adr = va + d0
            pc = u.der_mem.get(adr)
            if pc is None:   # le lifté a écrit là où Unicorn n'a rien écrit
                pc_any = next((u.der_mem.get(va + k) for k in range(d0, d1 + 1) if u.der_mem.get(va + k)), None)
            else:
                pc_any = pc
            dd = {"quoi": "mem", "adr": f"{adr:08X}", "n": d1 - d0 + 1,
                  "u": a[d0:d1 + 1][:16].hex(), "r": b2[d0:d1 + 1][:16].hex(),
                  "init": dict(fenetres)[va][d0:d1 + 1][:16].hex(),
                  "rel_esp0": adr - esp0 if va == ctx["dispo"]["pile"] + 0x20000 else None,
                  "sous_esp_final": va == ctx["dispo"]["pile"] + 0x20000 and adr + (d1 - d0) < esp_fin,
                  "ecrivain": ecrivain(pc_any)}
            (masques_app if dd["sous_esp_final"] and "pile-morte" in masques else bruts).append(dd)
    return bruts, masques_app

# ---------------------------------------------------------------- un test ---
_CTX = None
def init_worker(ctx):
    global _CTX
    _CTX = ctx

def tester(args):
    addr, seed = args
    ctx = _CTX
    gpr, eflags, fenetres, esp0 = gen_etat(seed, ctx["dispo"])
    u = executer_unicorn(ctx, addr, gpr, eflags, fenetres)
    if not u.ok:
        return {"addr": f"{addr:08X}", "seed": seed, "statut": "skip", "raison": u.raison}
    r, err = executer_runner(ctx, gpr, eflags, fenetres, addr)
    if r is None:
        return {"addr": f"{addr:08X}", "seed": seed, "statut": "echec", "runner": err,
                "n_instr": u.n_instr, "trace_fin": [f"{p:08X}" for p in u.pcs[-6:]]}
    bruts, masq = comparer(ctx, u, r, fenetres, esp0, ctx["masques"])
    out = {"addr": f"{addr:08X}", "seed": seed, "statut": "echec" if bruts else "ok", "n_instr": u.n_instr}
    if bruts:
        out["diffs"] = bruts[:12]
        out["n_diffs"] = len(bruts)
    if masq:
        out["masques"] = masq[:6]
        out["n_masques"] = len(masq)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--liste", required=True)
    ap.add_argument("--runner", required=True, help="binaire ; préfixe possible via --runner-prefixe")
    ap.add_argument("--runner-prefixe", default="", help="ex. 'qemu-aarch64 -L /racine' (ARM64)")
    ap.add_argument("--debut", type=int, default=0)
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--adresses", help="lot ciblé : lignes « ADDR [SEED] » (hex) ; remplace --debut/--n")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--disposition", choices=sorted(DISPO), default="v2")
    ap.add_argument("--masques", default="drapeaux-indefinis,oracle-qemu,rdtsc",
                    help="conventions masquées (comptées à part) : drapeaux-indefinis, oracle-qemu, rdtsc, df, pile-morte ; 'aucun'")
    ap.add_argument("--image-runner", choices=("segments", "plate"), default="segments",
                    help="format du fichier SC2_DT_IMAGE (plate : runner antérieur au format DTI1)")
    ap.add_argument("--tout", action="store_true", help="détails aussi pour les ok et les skip")
    ap.add_argument("--json")
    args = ap.parse_args()

    pe = lire_pe(args.exe)
    sections, fin_texte, data = pe[0], pe[1], pe[2]
    tva, tvs, trs, tro = sections[".text"]
    ctx = {"texte": (tva, tvs, trs, tro, data), "fin_texte": fin_texte, "dispo": DISPO[args.disposition],
           "masques": set() if args.masques == "aucun" else set(args.masques.split(",")),
           "runner_cmd": args.runner_prefixe.split() + [os.path.abspath(args.runner)], "image": None}
    tmpdir = tempfile.mkdtemp(prefix="difftest-")
    ctx["tmp"] = tmpdir
    if ctx["dispo"]["image"] == "complete":
        img = image_plate(pe)
        ctx["image"] = img
        ctx["segments"] = segments_image(pe, img)
        ctx["image_fichier"] = os.path.join(tmpdir, "image.bin")
        with open(ctx["image_fichier"], "wb") as f:      # lu par runner-difftest (SC2_DT_IMAGE)
            if args.image_runner == "plate":               # runners antérieurs au format DTI1
                f.write(img)
            else:
                f.write(struct.pack("<II", 0x31495444, len(ctx["segments"])))
                for va, octets in ctx["segments"]:
                    f.write(struct.pack("<II", va, len(octets)) + octets)

    adresses = adresses_liftees(args.liste)
    if args.adresses:
        lot = []
        for ligne in open(args.adresses):
            ch = ligne.split("#")[0].split()
            if ch:
                lot.append((int(ch[0], 16), int(ch[1]) if len(ch) > 1 else args.seed))
    else:
        lot = [(a, args.seed + i) for i, a in enumerate(adresses[args.debut: args.debut + args.n])]
    print(f"{len(adresses)} fonctions lifftées, .text fin={fin_texte:#x}, lot {len(lot)}, "
          f"disposition {args.disposition}, masques {sorted(ctx['masques'])}", flush=True)

    rapport = {"ok": 0, "echec": 0, "skip": 0, "masques": 0, "raisons": {}, "details": []}
    if args.jobs > 1:
        ex = ProcessPoolExecutor(args.jobs, initializer=init_worker, initargs=(ctx,))
        it = ex.map(tester, lot, chunksize=4)
    else:
        init_worker(ctx)
        it = map(tester, lot)
    for i, res in enumerate(it):
        st = res["statut"]
        rapport[st] += 1
        if st == "skip":
            rapport["raisons"][res["raison"]] = rapport["raisons"].get(res["raison"], 0) + 1
            if args.tout:
                rapport["details"].append(res)
        else:
            if res.get("n_masques"):
                rapport["masques"] += 1
            if st == "echec" or res.get("n_masques") or args.tout:
                rapport["details"].append(res)
        if (i + 1) % 100 == 0:
            print(f"  {i+1}/{len(lot)} : ok={rapport['ok']} echec={rapport['echec']} skip={rapport['skip']}", flush=True)
    print(json.dumps({k: v for k, v in rapport.items() if k != "details"}, indent=1))
    if args.json:
        rapport["args"] = vars(args)
        json.dump(rapport, open(args.json, "w"), indent=1, default=str)
        print(f"rapport: {args.json}")
    try:
        if ctx.get("image_fichier"):
            os.unlink(ctx["image_fichier"])
        os.rmdir(tmpdir)
    except OSError:
        pass

if __name__ == "__main__":
    main()
