#!/usr/bin/env python3
"""closure-plus.py — complète une closure liftée EXISTANTE (pcrecomp, lift32_cpu) avec des racines manquantes,
sans relift global.

Pour chaque racine (et, récursivement, chaque cible directe de dispatch absente de la liste connue et située dans
.text) : étendue par descente bornée des sauts locaux, relocations du PE, lift par le lift32_cpu donné. Sortie : un
fichier C (les fonctions neuves) et, sur stdout, la liste des VA ajoutées.

  closure-plus.py [--include H]... [--known-re RE] <dossier tools/lift> <image.exe> <liste connue> <sortie.c> <racine hex>…

  <dossier tools/lift>  celui qui contient lift32_cpu.py (pcrecomp)
  <image.exe>           VOTRE image PE32 (jamais distribuée ; lue localement)
  <liste connue>        fichier texte des fonctions déjà liftées : toute VA capturée par --known-re
                        (défaut : X(XXXXXXXX), la X-macro de liste de pcrecomp) ; « - » = aucune
  --include H           en-tête ajouté au fichier C (répétable ; défaut : cpu.h)
"""
import argparse, importlib, re, struct, sys

DISPATCH_RE = re.compile(r"dispatch(?:_jmp)?\(c, (?:GVA\()?0x([0-9A-Fa-f]{8})u")


class PE:
    """Lecteur PE32 minimal : en-têtes, sections, lecture par VA, relocations HIGHLOW."""
    def __init__(self, path):
        r = self.raw = open(path, "rb").read()
        fh = struct.unpack_from("<I", r, 0x3C)[0] + 4
        assert r[fh - 4:fh] == b"PE\0\0", "signature PE absente"
        nsec, opt_size = struct.unpack_from("<H", r, fh + 2)[0], struct.unpack_from("<H", r, fh + 16)[0]
        oh = fh + 20
        assert struct.unpack_from("<H", r, oh)[0] == 0x10B, "PE32 attendu"
        self.image_base, = struct.unpack_from("<I", r, oh + 28)
        self.size_of_image, self.size_of_headers = struct.unpack_from("<II", r, oh + 56)
        ndirs, = struct.unpack_from("<I", r, oh + 92)
        self.dirs = [struct.unpack_from("<II", r, oh + 96 + 8 * i) for i in range(ndirs)]
        so = oh + opt_size
        self.sections = []
        for i in range(nsec):
            name, vsize, rva, rsize, roff = struct.unpack_from("<8sIIII", r, so + 40 * i)
            self.sections.append(dict(name=name.rstrip(b"\0").decode(), rva=rva, vsize=vsize, rsize=rsize, roff=roff))

    def rva_off(self, rva):
        for s in self.sections:
            if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["rsize"]):
                return None if rva - s["rva"] >= s["rsize"] else s["roff"] + rva - s["rva"]   # None : BSS
        return rva if rva < self.size_of_headers else None

    def read_va(self, va, n):
        out, rva = bytearray(), va - self.image_base
        while len(out) < n:
            off = self.rva_off(rva + len(out))
            if off is None:
                out += b"\0"
            else:
                out += self.raw[off:off + (n - len(out))]
                if off + (n - len(out)) > len(self.raw):
                    break
        return bytes(out[:n])

    def relocs(self):
        rva, size = self.dirs[5] if len(self.dirs) > 5 else (0, 0)
        vas, p = set(), 0
        while p < size:
            off = self.rva_off(rva + p)
            page, blk = struct.unpack_from("<II", self.raw, off)
            if blk < 8:
                break
            for k in range((blk - 8) // 2):
                e, = struct.unpack_from("<H", self.raw, off + 8 + 2 * k)
                if e >> 12 == 3:
                    vas.add(self.image_base + page + (e & 0xFFF))
            p += blk
        return vas


ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
ap.add_argument("--include", action="append")
ap.add_argument("--known-re", default=r"X\(([0-9A-Fa-f]{8})\)")
ap.add_argument("lift_dir"); ap.add_argument("exe"); ap.add_argument("liste"); ap.add_argument("sortie")
ap.add_argument("racines", nargs="+", type=lambda a: int(a, 16))
args = ap.parse_args()

sys.path.insert(0, args.lift_dir)
L = importlib.import_module("lift32_cpu")
pe = PE(args.exe)
L.IMAGE_BASE = pe.image_base                    # global lu par Lifter (in_image, GVA)
lifter = L.Lifter(None, pe.size_of_image, pe.read_va)
lifter.reloc_vas = pe.relocs()
text = next(s for s in pe.sections if s["name"] == ".text")
code_lo = pe.image_base + text["rva"]; code_hi = code_lo + text["vsize"]
connues = set() if args.liste == "-" else {int(m, 16) for m in re.findall(args.known_re, open(args.liste).read())}

def etendue(va, plafond=0x4000):
    insns = {i.address: i for i in lifter.md.disasm(pe.read_va(va, plafond + 16), va)}
    pile, vus, fin = [va], set(), va
    while pile:
        a = pile.pop()
        while a in insns and a < va + plafond and a not in vus:
            vus.add(a); i = insns[a]; nxt = a + i.size; fin = max(fin, nxt); m = i.mnemonic.split()[-1]
            if m in ("ret", "retn", "retf", "iret", "iretd", "hlt", "int3", "ud2"): break
            if m.startswith("j") or m.startswith("loop"):
                op = i.operands[0] if i.operands else None
                if op is not None and op.type == L.X86_OP_IMM and va <= op.imm < va + plafond: pile.append(op.imm)
                if m == "jmp": break
            a = nxt
    return fin - va

file, faites, sources, echecs = list(args.racines), set(), {}, {}
while file:
    a = file.pop(0)
    if a in faites or a in connues: continue
    faites.add(a)
    n = etendue(a)
    try:
        src = lifter.lift_function(pe.read_va(a, n), a)
    except Exception as e:
        msg = f"{type(e).__name__}: {e}".replace('"', "'")[:120]; echecs[a] = msg
        src = (f"void L_{a:08X}(CPU *c)\n{{\n    /* lift32_cpu a échoué : {msg} */\n"
               f'    RECOMP_TODO(0x{a:08X}u, "fonction non liftable ({msg})");\n}}')
    sources[a] = src
    cibles = sorted({int(m.group(1), 16) for m in DISPATCH_RE.finditer(src)})
    neuves = [t for t in cibles if code_lo <= t < code_hi and t not in connues and t not in faites]
    sys.stderr.write(f"[closure+] 0x{a:08X} : {n} o, {len(cibles)} cibles directes, {len(neuves)} absentes de la closure"
                     f"{' : ' + ' '.join('%08X' % t for t in neuves) if neuves else ''}{'  ÉCHEC ' + echecs[a] if a in echecs else ''}\n")
    file += neuves
with open(args.sortie, "w") as f:
    f.write("/* AUTO-GENERATED by lift32_cpu.py (pcrecomp) via closure-plus.py - do not edit */\n")
    f.write("".join(f'#include "{h}"\n' for h in (args.include or ["cpu.h"])) + "\n")
    for a in sorted(sources): f.write(sources[a] + "\n\n")
print(" ".join("%08X" % a for a in sorted(sources)))
sys.stderr.write(f"[closure+] {len(sources)} fonction(s) neuve(s), {len(echecs)} échec(s) de lift\n")
