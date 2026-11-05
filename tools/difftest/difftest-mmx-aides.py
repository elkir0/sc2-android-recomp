#!/usr/bin/env python3
"""difftest-mmx-aides.py — passe 9r : chaque aide MMX de runtime/cpu.h (mmx_*) contre Unicorn, instruction par instruction.

Pour chaque opcode MMX traduit par lift32_cpu (0F xx /r, forme mm0, mm1 ; pshufw avec immédiat) : N couples (mm0, mm1)
aléatoires (comptes de décalage parfois petits), exécution de l'instruction seule dans Unicorn, résultat mm0 relevé.
Écrit les vecteurs « nom a b imm attendu » ; test-mmx-aides.c les rejoue avec les aides C et compare.
  difftest-mmx-aides.py <vecteurs.txt> [N=40] [graine=1]
"""
import random, sys
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn import x86_const as X
OPS = {  # nom : octet après 0F
    "paddb": 0xFC, "paddw": 0xFD, "paddd": 0xFE, "paddq": 0xD4, "psubb": 0xF8, "psubw": 0xF9, "psubd": 0xFA, "psubq": 0xFB,
    "paddsb": 0xEC, "paddsw": 0xED, "paddusb": 0xDC, "paddusw": 0xDD, "psubsb": 0xE8, "psubsw": 0xE9, "psubusb": 0xD8,
    "psubusw": 0xD9, "pmullw": 0xD5, "pmulhw": 0xE5, "pmulhuw": 0xE4, "pmaddwd": 0xF5, "pmuludq": 0xF4, "pmaxsw": 0xEE,
    "pminsw": 0xEA, "pmaxub": 0xDE, "pminub": 0xDA, "pavgb": 0xE0, "pavgw": 0xE3, "psadbw": 0xF6, "pcmpeqb": 0x74,
    "pcmpeqw": 0x75, "pcmpeqd": 0x76, "pcmpgtb": 0x64, "pcmpgtw": 0x65, "pcmpgtd": 0x66, "punpcklbw": 0x60,
    "punpcklwd": 0x61, "punpckldq": 0x62, "punpckhbw": 0x68, "punpckhwd": 0x69, "punpckhdq": 0x6A, "packsswb": 0x63,
    "packuswb": 0x67, "packssdw": 0x6B, "pand": 0xDB, "pandn": 0xDF, "por": 0xEB, "pxor": 0xEF,
    "psllw": 0xF1, "pslld": 0xF2, "psllq": 0xF3, "psrlw": 0xD1, "psrld": 0xD2, "psrlq": 0xD3, "psraw": 0xE1, "psrad": 0xE2,
    "pshufw": 0x70,
}
DECALAGES = {"psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"}
sortie = sys.argv[1]; N = int(sys.argv[2]) if len(sys.argv) > 2 else 40; r = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
CODE, DONNEES = 0x10000, 0x20000
with open(sortie, "w") as f:
    for nom, op in OPS.items():
        for k in range(N):
            a, b, imm = r.getrandbits(64), r.getrandbits(64), r.getrandbits(8)
            if nom in DECALAGES and k % 2 == 0: b = r.randrange(0, 80)       # comptes utiles, dont > largeur
            insn = bytes([0x0F, op, 0xC1]) + (bytes([imm]) if nom == "pshufw" else b"")   # modrm C1 : mm0, mm1
            # mm0/mm1 chargés et relus par la mémoire (movq) : Unicorn n'expose pas MM0-7 en registres
            code = (bytes([0x0F, 0x6F, 0x05]) + DONNEES.to_bytes(4, "little")          # movq mm0, [A]
                    + bytes([0x0F, 0x6F, 0x0D]) + (DONNEES + 8).to_bytes(4, "little")  # movq mm1, [B]
                    + insn
                    + bytes([0x0F, 0x7F, 0x05]) + (DONNEES + 16).to_bytes(4, "little"))  # movq [C], mm0
            u = Uc(UC_ARCH_X86, UC_MODE_32); u.mem_map(CODE, 0x1000); u.mem_map(DONNEES, 0x1000)
            u.mem_write(CODE, code); u.mem_write(DONNEES, a.to_bytes(8, "little") + b.to_bytes(8, "little"))
            u.emu_start(CODE, CODE + len(code))
            res = int.from_bytes(u.mem_read(DONNEES + 16, 8), "little")
            f.write(f"{nom} {a:016x} {b:016x} {imm:02x} {res:016x}\n")
print(f"{len(OPS)} opcodes × {N} = {len(OPS) * N} vecteurs → {sortie}")
