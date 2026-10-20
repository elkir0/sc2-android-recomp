# Lift d'UNE fonction de SC2.exe avec un lift32_cpu donné (lecture seule du PE) : même lecteur PE, mêmes
# relocations et même étendue par descente que scripts/lift-boot.py. Sortie : le C de L_<va> sur stdout.
#   lift-un.py <lift-boot.py> <dossier tools/lift> <SC2.exe> <va hex> [plafond]
import importlib.util, sys
lb_path, lift_dir, exe, va = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16)
plafond = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x4000
spec = importlib.util.spec_from_file_location("liftboot", lb_path); lb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lb)
sys.path.insert(0, lift_dir)
import lift32_cpu as L
pe = lb.PE(exe)
L.IMAGE_BASE = pe.image_base
lifter = L.Lifter(None, pe.size_of_image, pe.read_va)
lifter.reloc_vas = pe.relocs()
code = pe.read_va(va, plafond + 16)
insns = {i.address: i for i in lifter.md.disasm(code, va)}
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
n = fin - va
sys.stderr.write(f"[lift-un] 0x{va:08X} : {n} octets ({len(vus)} instructions atteintes)\n")
print(lifter.lift_function(pe.read_va(va, n), va))
