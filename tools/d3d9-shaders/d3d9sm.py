"""d3d9sm.py — décodeur du bytecode des shaders Direct3D 9 (SM 2.x / 3.0), passe 5d.

Format (référence : documentation publique « D3D9 shader bytecode / token format »,
d3d9types.h) :
  version     0xFFFE_MMmm (vertex) / 0xFFFF_MMmm (pixel)
  instruction bits 0-15 opcode, 16-23 contrôles (texld : 1 = p, 2 = b ; ifc/setp :
              comparaison), 24-27 longueur en jetons (SM2+), 28 prédiqué
  destination bit 31, n° bits 0-10, type bits 28-30 | (11-12) << 3, masque 16-19,
              modificateur 20-23 (1 sat, 2 pp, 4 centroid), décalage 24-27
  source      n° 0-10, type idem, adressage relatif bit 13 (+1 jeton en SM3),
              swizzle 16-23, modificateur 24-27
  commentaire 0xFFFE + longueur (bits 16-30) ; fin 0x0000FFFF
"""
import struct

OPS = {
    0: 'nop', 1: 'mov', 2: 'add', 3: 'sub', 4: 'mad', 5: 'mul', 6: 'rcp', 7: 'rsq', 8: 'dp3', 9: 'dp4',
    10: 'min', 11: 'max', 12: 'slt', 13: 'sge', 14: 'exp', 15: 'log', 16: 'lit', 17: 'dst', 18: 'lrp',
    19: 'frc', 20: 'm4x4', 21: 'm4x3', 22: 'm3x4', 23: 'm3x3', 24: 'm3x2', 25: 'call', 26: 'callnz',
    27: 'loop', 28: 'ret', 29: 'endloop', 30: 'label', 31: 'dcl', 32: 'pow', 33: 'crs', 34: 'sgn',
    35: 'abs', 36: 'nrm', 37: 'sincos', 38: 'rep', 39: 'endrep', 40: 'if', 41: 'ifc', 42: 'else',
    43: 'endif', 44: 'break', 45: 'breakc', 46: 'mova', 47: 'defb', 48: 'defi', 64: 'texcoord',
    65: 'texkill', 66: 'texld', 78: 'expp', 79: 'logp', 80: 'cnd', 81: 'def', 88: 'cmp', 90: 'dp2add',
    91: 'dsx', 92: 'dsy', 93: 'texldd', 94: 'setp', 95: 'texldl', 96: 'breakp',
}
# registres
TEMP, INPUT, CONST, ADDR, RASTOUT, ATTROUT, OUTPUT, CONSTINT, COLOROUT, DEPTHOUT, SAMPLER = range(11)
CONSTBOOL, LOOP, MISC, PRED = 14, 15, 17, 19
USAGES = ['position', 'blendweight', 'blendindices', 'normal', 'psize', 'texcoord', 'tangent',
          'binormal', 'tessfactor', 'positiont', 'color', 'fog', 'depth', 'sample']


class Param:
    __slots__ = ('type', 'num', 'mask', 'mod', 'shift', 'swz', 'smod', 'rel')

    def __repr__(self):
        return f'{self.type}:{self.num}'


def reg_type(t):
    return ((t >> 28) & 7) | (((t >> 11) & 3) << 3)


def dest(t):
    p = Param()
    p.type, p.num = reg_type(t), t & 0x7FF
    p.mask, p.mod, p.shift = (t >> 16) & 0xF, (t >> 20) & 0xF, (t >> 24) & 0xF
    p.swz, p.smod, p.rel = 0xE4, 0, None
    return p


def src(t):
    p = Param()
    p.type, p.num = reg_type(t), t & 0x7FF
    p.swz, p.smod = (t >> 16) & 0xFF, (t >> 24) & 0xF
    p.mask, p.mod, p.shift, p.rel = 0xF, 0, 0, None
    return p


class Instr:
    __slots__ = ('op', 'name', 'ctrl', 'dst', 'srcs', 'extra', 'pred')

    def __init__(self, op, ctrl):
        self.op, self.name, self.ctrl = op, OPS.get(op, f'op{op}'), ctrl
        self.dst, self.srcs, self.extra, self.pred = None, [], None, None


def longueur(d, off=0):
    """Taille en octets du bytecode commençant à off (fin comprise), ou 0 s'il est invalide."""
    if off + 4 > len(d):
        return 0
    v = struct.unpack_from('<I', d, off)[0]
    if (v & 0xFFFE0000) != 0xFFFE0000 or ((v >> 8) & 0xFF) not in (2, 3) or (v & 0xFF) > 1:
        return 0
    i = off + 4
    while i + 4 <= len(d) and i - off < 262144:
        t = struct.unpack_from('<I', d, i)[0]
        if t == 0x0000FFFF:
            return i + 4 - off
        if (t & 0xFFFF) == 0xFFFE:
            i += 4 * (1 + ((t >> 16) & 0x7FFF))
            continue
        op = t & 0xFFFF
        if op not in OPS:
            return 0
        i += 4 * (1 + ((t >> 24) & 0xF))
    return 0


def decoder(code):
    """→ (ps: bool, version (maj, min), [Instr], commentaires [bytes])."""
    w = struct.unpack_from(f'<{len(code) // 4}I', code)
    v = w[0]
    ps = (v >> 16) == 0xFFFF
    ver = ((v >> 8) & 0xFF, v & 0xFF)
    out, comments, i = [], [], 1
    while i < len(w):
        t = w[i]
        if t == 0x0000FFFF:
            break
        if (t & 0xFFFF) == 0xFFFE:
            n = (t >> 16) & 0x7FFF
            comments.append(struct.pack(f'<{n}I', *w[i + 1:i + 1 + n]))
            i += 1 + n
            continue
        op, ln = t & 0xFFFF, (t >> 24) & 0xF
        ins = Instr(op, (t >> 16) & 0xFF)
        args = list(w[i + 1:i + 1 + ln])
        i += 1 + ln
        if t & (1 << 28):                       # prédiqué : le registre p suit la destination
            pass
        if op == 31:                            # dcl : jeton d'usage puis destination
            ins.extra = args[0]
            ins.dst = dest(args[1])
        elif op in (81, 48):                    # def / defi
            ins.dst = dest(args[0])
            ins.extra = struct.unpack('<4f' if op == 81 else '<4i', struct.pack('<4I', *args[1:5]))
        elif op == 47:                          # defb
            ins.dst = dest(args[0])
            ins.extra = args[1]
        else:
            j = 0
            has_dst = op not in (25, 26, 27, 28, 29, 30, 38, 39, 40, 41, 42, 43, 44, 45, 96, 0, 65) or op == 65
            if op == 65:                        # texkill : un seul paramètre, traité en source
                has_dst = False
            if has_dst and args:
                ins.dst = dest(args[0])
                j = 1
                if ins.dst.type == PRED and False:
                    pass
            if t & (1 << 28) and j < len(args):
                ins.pred = src(args[j]); j += 1
            while j < len(args):
                p = src(args[j]); j += 1
                if (args[j - 1] >> 13) & 1 and j < len(args) and (args[j] >> 31) & 1:
                    p.rel = src(args[j]); j += 1
                ins.srcs.append(p)
        out.append(ins)
    return ps, ver, out, comments


def ctab(comments):
    """Table de constantes (commentaire CTAB) → [(nom, regset, index, nb)]."""
    for c in comments:
        if c[:4] != b'CTAB':
            continue
        b = c[4:]
        try:
            size, creator, version, nconst, cinfo, flags, target = struct.unpack_from('<7I', b, 0)
            res = []
            for k in range(nconst):
                name, regset, regidx, regcount, _res, typeinfo, defval = struct.unpack_from('<IHHHHII', b, cinfo + 20 * k)
                nm = b[name:b.index(b'\0', name)].decode('latin-1')
                res.append((nm, regset, regidx, regcount))
            return res
        except (struct.error, ValueError):
            return []
    return []


def nom_reg(p, ps):
    t, n = p.type, p.num
    return {TEMP: f'r{n}', INPUT: f'v{n}', CONST: f'c{n}', ADDR: (f't{n}' if ps else f'a{n}'),
            RASTOUT: ['oPos', 'oFog', 'oPts'][n] if n < 3 else f'ro{n}', ATTROUT: f'oD{n}',
            OUTPUT: (f'o{n}'), CONSTINT: f'i{n}', COLOROUT: f'oC{n}', DEPTHOUT: 'oDepth',
            SAMPLER: f's{n}', CONSTBOOL: f'b{n}', LOOP: 'aL', MISC: ['vPos', 'vFace'][n] if n < 2 else f'misc{n}',
            PRED: f'p{n}'}.get(t, f'?{t}:{n}')


def desassembler(code):
    ps, ver, ins, com = decoder(code)
    lignes = [f"{'ps' if ps else 'vs'}_{ver[0]}_{ver[1]}"]
    for i in ins:
        if i.op == 31:
            u = i.extra
            if i.dst.type == SAMPLER:
                lignes.append(f'dcl_{["?", "?", "2d", "cube", "volume"][(u >> 27) & 0xF] if ((u >> 27) & 0xF) < 5 else "?"} {nom_reg(i.dst, ps)}')
            else:
                us = USAGES[u & 0x1F] if (u & 0x1F) < len(USAGES) else f'u{u & 0x1F}'
                lignes.append(f'dcl_{us}{(u >> 16) & 0xF} {nom_reg(i.dst, ps)}.{masque(i.dst.mask)}')
            continue
        if i.op in (81, 48):
            lignes.append(f'{i.name} {nom_reg(i.dst, ps)}, {", ".join(f"{x:g}" for x in i.extra)}')
            continue
        parts = []
        if i.dst is not None:
            sat = '_sat' if i.dst.mod & 1 else ''
            parts.append(f'{nom_reg(i.dst, ps)}.{masque(i.dst.mask)}')
            nm = i.name + sat
        else:
            nm = i.name
        for s in i.srcs:
            parts.append(fmt_src(s, ps))
        lignes.append(f'{nm} {", ".join(parts)}')
    return '\n'.join(lignes)


def masque(m):
    return ''.join(c for k, c in enumerate('xyzw') if m & (1 << k)) or 'xyzw'


def swz_str(s):
    t = ''.join('xyzw'[(s >> (2 * k)) & 3] for k in range(4))
    return '' if t == 'xyzw' else '.' + t


def fmt_src(s, ps):
    r = nom_reg(s, ps)
    if s.rel is not None:
        r = f'{r[0]}[{nom_reg(s.rel, ps)}{swz_str(s.rel.swz)[:2]} + {s.num}]'
    r += swz_str(s.swz)
    mods = {1: '-{}', 2: '{}_bias', 3: '-{}_bias', 4: '{}_bx2', 5: '-{}_bx2', 6: '1-{}', 7: '{}_x2',
            8: '-{}_x2', 9: '{}_dz', 10: '{}_dw', 11: '{}_abs', 12: '-{}_abs', 13: '!{}'}
    return mods.get(s.smod, '{}').format(r)
