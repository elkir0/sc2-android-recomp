# passe 9n — # désassemblage d'une fonction de SC2.exe (lecture seule) : étendue par descente récursive bornée (comme
# lift-boot.py etendue_descente), inventaire des instructions MMX (opérandes mm0-mm7).
import struct, sys, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_REG
E = sys.argv[1]; d = open(E, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; opt = pe + 24
base = struct.unpack_from('<I', d, opt + 28)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<8sIIII', d, opt + so + 40 * i) for i in range(nsec)]
def lire(va, n):
    for nm, vs, rva, rs, ro in secs:
        if rva <= va - base < rva + rs: o = ro + va - base - rva; return d[o:o + n]
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
def etendue(t, plafond=0x4000):
    insns = {i.address: i for i in md.disasm(lire(t, plafond + 16), t)}
    pile, vus, fin = [t], set(), t
    while pile:
        a = pile.pop()
        while a in insns and a < t + plafond and a not in vus:
            vus.add(a); i = insns[a]; nxt = a + i.size; fin = max(fin, nxt); m = i.mnemonic.split()[-1]
            if m in ('ret', 'retn', 'retf', 'iret', 'iretd', 'hlt', 'int3', 'ud2'): break
            if m.startswith('j') or m.startswith('loop'):
                op = i.operands[0] if i.operands else None
                if op is not None and op.type == X86_OP_IMM and t <= op.imm < t + plafond: pile.append(op.imm)
                if m == 'jmp': break
            a = nxt
    return fin - t, insns, vus
for arg in sys.argv[2:]:
    t = int(arg, 16); n, insns, vus = etendue(t)
    mmx = collections.Counter(); appels = []
    for a in sorted(vus):
        i = insns[a]
        if 'mm' in i.op_str.replace('xmm', 'XMM'): mmx[i.mnemonic] += 1
        if i.mnemonic == 'call': appels.append(i.op_str)
    print(f'== 0x{t:08X} : {n} octets, {len(vus)} instructions, MMX {sum(mmx.values())} : ' + ', '.join(f'{k}×{v}' for k, v in sorted(mmx.items())))
    print('   appels :', ' '.join(appels[:30]))
    if len(sys.argv) > 2 and sys.argv[-1] == arg and '--liste' in sys.argv: pass
