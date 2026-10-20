#!/usr/bin/env python3
"""test-x87-decalages.py — test différentiel ciblé des corrections de la passe 7k.

    PCRECOMP_FPU_CW=1 .venv/bin/python test-x87-decalages.py recomp-sc2/runtime/cpu.h

Chaque cas est une SÉQUENCE d'instructions, traduite par lift32_cpu (lifter pcrecomp,
importé tel quel) puis compilée avec le cpu.h donné — gcc x86-64 ET aarch64-linux-gnu-gcc
sous qemu-aarch64 — et comparée à Unicorn (mot de contrôle x87 de Windows, 0x027F) :
  * mot de contrôle x87 : fldcw + fistp (16/32/64) sous les 4 modes d'arrondi RC, valeurs
    hors plage et NaN (« integer indefinite »), frndint, fnstcw, fninit ;
  * fnstsw ax : champ TOP (bits 11-13) ;
  * OF des décalages de compte 1 (shl/shr/sar/shld/shrd) et compte cl = 1.
"""
import math, os, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.expanduser("~/tools/pcrecomp/tools/lift"))
if os.environ.get("PCRECOMP_FPU_CW") != "1":
    print("PCRECOMP_FPU_CW absent : formes x87 d'AVANT la passe 7k (mesure du « avant »)")
import lift32_cpu as L
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import *

CPUH = os.path.abspath(sys.argv[1])
SCR, CODE = 0x00100000, 0x00001000
REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
UREG = {r: globals()[f"UC_X86_REG_{r.upper()}"] for r in REGS}
FL = dict(cf=0, pf=2, zf=6, sf=7, of=11)

def d(v): return struct.pack("<d", v)
def w(v): return struct.pack("<H", v)

CAS = []
FLDCW_FLD = "d96e10dd06"            # fldcw [esi+0x10] ; fld qword [esi]
for rc, nom_rc in ((0x027F, "proche"), (0x067F, "bas"), (0x0A7F, "haut"), (0x0E7F, "zero")):
    for v in (2.7, -2.7, 2.5, 3.5, -0.5, 1e10, -3e9, float("nan"), 40000.5):
        mem = d(v) + bytes(8) + w(rc) + bytes(14)
        CAS.append((f"fistp32 {nom_rc} {v}", FLDCW_FLD + "db5e08", {}, mem, ()))
        CAS.append((f"fistp16 {nom_rc} {v}", FLDCW_FLD + "df5e08", {}, mem, ()))
        CAS.append((f"fistp64 {nom_rc} {v}", FLDCW_FLD + "df7e08", {}, mem, ()))
    CAS.append((f"frndint {nom_rc}", FLDCW_FLD + "d9fcdd5e08", {}, d(-2.5) + bytes(8) + w(rc) + bytes(14), ()))
    CAS.append((f"fnstcw {nom_rc}", "d96e10d97e18", {}, bytes(16) + w(rc) + bytes(14), ()))
CAS.append(("fnstcw défaut", "d97e18", {}, bytes(32), ()))
CAS.append(("fninit+fnstcw", "dbe3d97e18", {}, bytes(32), ()))
CAS.append(("fld+fnstsw (TOP=7)", "dd06dfe0", dict(eax=0xAAAA5555), d(1.0) + bytes(24), ()))
CAS.append(("fld fld fstp fnstsw", "dd06dd06ddd8dfe0", dict(eax=0), d(1.0) + bytes(24), ()))
# fcom → C0..C3 + TOP
CAS.append(("fcomp+fnstsw", "dd06dc5e08dfe0", dict(eax=0), d(1.0) + d(2.0) + bytes(16), ()))
F = ("cf", "pf", "zf", "sf", "of")
for nom, hexa, regs in (("shl 1", "d1e0", dict(eax=0x40000000)), ("shl 1 b", "d1e0", dict(eax=0xC0000000)),
                        ("shl 1 c", "d1e0", dict(eax=0x80000000)), ("shr 1", "d1e8", dict(eax=0x80000001)),
                        ("shr 1 b", "d1e8", dict(eax=0x7FFFFFFF)), ("sar 1", "d1f8", dict(eax=0x80000001)),
                        ("shl cl=1", "d3e0", dict(eax=0x40000000, ecx=1)),
                        ("shr cl=1", "d3e8", dict(eax=0x80000000, ecx=0x21)),
                        ("shl8 1", "d0e0", dict(eax=0x40)), ("shr16 1", "66d1e8", dict(eax=0x8000)),
                        ("shld 1", "0fa4c201", dict(eax=0x80000000, edx=0x40000000)),
                        ("shrd 1", "0facc201", dict(eax=0x00000001, edx=0x00000002)),
                        ("shrd 1 b", "0facc201", dict(eax=0x00000000, edx=0x80000000))):
    for ef in (0x202, 0xA03):
        CAS.append((f"{nom} ef{ef:X}", hexa, regs, bytes(32), F, ef))

def ref(code, regs, mem, ef):
    u = Uc(UC_ARCH_X86, UC_MODE_32); u.mem_map(0, 0x200000)
    u.mem_write(CODE, code); u.mem_write(SCR, mem)
    for r in REGS: u.reg_write(UREG[r], regs.get(r, 0))
    u.reg_write(UC_X86_REG_ESI, regs.get("esi", SCR)); u.reg_write(UC_X86_REG_EFLAGS, ef)
    u.reg_write(UC_X86_REG_FPCW, 0x027F)
    u.emu_start(CODE, CODE + len(code))
    out = {r: u.reg_read(UREG[r]) for r in REGS}
    e = u.reg_read(UC_X86_REG_EFLAGS); out.update({f: (e >> b) & 1 for f, b in FL.items()})
    return out, bytes(u.mem_read(SCR, len(mem)))

L.IMAGE_BASE = 0x00400000
lf = L.Lifter(None, 0x1000)
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
src = [f'#include "{CPUH}"', "#include <stdio.h>", "uintptr_t g_hote_delta; uint32_t g_image_delta;",
       "static unsigned char mem[64];"]
mainb = ["int main(void){ CPU c; g_hote_delta = (uintptr_t)mem - 0x%Xu;" % SCR]
attendu = []
for i, cas in enumerate(CAS):
    nom, hexa, regs, m, flags = cas[:5]
    ef = cas[5] if len(cas) > 5 else 0x202
    code = bytes.fromhex(hexa)
    lignes = []
    for insn in md.disasm(code, CODE):
        lignes += lf.translate(insn, {})
    src.append(f"static void f{i}(CPU *c) {{ " + " ".join(lignes) + " }")
    r = dict(regs); r.setdefault("esi", SCR)
    init = " ".join(f"c.{x}=0x{r.get(x, 0):X}u;" for x in REGS)
    init += " " + " ".join(f"c.{f}={(ef >> b) & 1};" for f, b in FL.items())
    lit = "".join("\\x%02x" % x for x in m)
    mainb.append(f" memset(&c,0,sizeof c); memset(mem,0,64); memcpy(mem,\"{lit}\",{len(m)}); {init} f{i}(&c);"
                 f' printf("{nom}"); ' + " ".join(f'printf(" %X", (unsigned)c.{x});' for x in REGS + list(flags)) +
                 f' for(int k=0;k<{len(m)};k++) printf(" m%02X", mem[k]); printf("\\n");')
    o, mm = ref(code, regs, m, ef)
    attendu.append(f"{nom} " + " ".join("%X" % o[x] for x in REGS + list(flags)) + "".join(" m%02X" % b for b in mm))
mainb.append(" return 0; }")
dd = tempfile.mkdtemp(); c = os.path.join(dd, "t.c"); open(c, "w").write("\n".join(src + mainb) + "\n")
rc_global = 0
for cc in (["gcc", "-O2"], ["aarch64-linux-gnu-gcc", "-O2", "-static"]):
    exe = os.path.join(dd, "t-" + cc[0])
    rc = subprocess.run(cc + ["-w", "-o", exe, c, "-lm"]).returncode
    if rc:
        print("compilation échouée", cc[0]); rc_global = 1; continue
    run = [exe] if cc[0] == "gcc" else ["qemu-aarch64", exe]
    obtenu = subprocess.run(run, capture_output=True, text=True).stdout.strip().split("\n")
    ko = [(a, b) for a, b in zip(attendu, obtenu) if a != b]
    print(f"[{cc[0]}] {len(attendu) - len(ko)}/{len(attendu)} identiques à Unicorn")
    for a, b in ko:
        print("   attendu", a, "\n   obtenu ", b)
    rc_global |= bool(ko)
sys.exit(rc_global)
