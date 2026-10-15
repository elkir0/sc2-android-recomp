#!/usr/bin/env python3
"""d3d9-vers-spirv.py — traduction HORS-LIGNE des shaders D3D9 (SM 2.x/3.0) de SC2 en SPIR-V (passe 5d).

    scripts/d3d9-vers-spirv.py <dossier de .bin> <sortie.h> [--glsl <dossier>] [--liste hash,…]

Chaque bytecode (vs_<hash>.bin / ps_<hash>.bin, hash = FNV-1a 64 du bytecode, celui du
runtime) → GLSL 450 → glslangValidator -V → spirv-val. Les traductions VALIDÉES sont
embarquées dans <sortie.h> (table SC2_SHADERS_SPV, clé = hash), avec leurs métadonnées
d'interface. Une instruction non gérée (prédicats, call/label, boucles loop…) → shader
NON traduit, listé dans le rapport : le runtime garde alors le draw « lumière ».

Interface commune (doit rester alignée sur runtime/sc2_d3d9v.c) :
  set 0 binding 0  UBO constantes VS : vec4 c[256] ; ivec4 i[16] ; ivec4 b[16]   (std140)
  set 0 binding 1  UBO constantes PS : même structure
  set 0 binding 2+n  sampler n du PS (2D / cube / 3D selon dcl)
  push constants   VS : vec4 hp (correction demi-pixel D3D9, 1/L, 1/H) ;
                   PS (offset 16) : vec4 at (test alpha : actif, fonction D3DCMP, référence)
  entrées VS       location = n° du registre v ; (usage, index) dans les métadonnées
  varyings         location fixe par (usage, index) : color i → i, texcoord i → 2+i, fog → 10,
                   normal → 11, tangent → 12, binormal → 13, autres → 14 ; le VS les déclare
                   TOUTES (16, à zéro par défaut) pour que tout PS trouve ses entrées.
  y : gl_Position.y inversé (repère Vulkan) ; z D3D9 [0,1] = Vulkan.
"""
import hashlib, os, struct, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d9sm as sm

NVARY = 16


def slot_varying(usage, idx):
    u = sm.USAGES[usage] if usage < len(sm.USAGES) else ''
    if u == 'color':
        return idx if idx < 2 else None
    if u == 'texcoord':
        return 2 + idx if idx < 8 else None
    return {'fog': 10, 'normal': 11, 'tangent': 12, 'binormal': 13}.get(u, 14)


class NonGere(Exception):
    pass


class Traducteur:
    def __init__(self, code):
        self.ps, self.ver, self.ins, self.com = sm.decoder(code)
        if self.ver[0] < 2:
            raise NonGere('SM1')
        self.defs = {}           # c n → vec4 littéral
        self.defi, self.defb = {}, {}
        self.entrees = []        # VS : (reg, usage, idx) ; PS : (reg, usage, idx)
        self.sorties = {}        # VS SM3 : reg o → (usage, idx)
        self.samplers = {}       # n → 2 / 3 / 4
        self.temps, self.lignes = set(), []
        self.utilise_a0 = False
        self.entrees_t = []
        self.indent = 1

    # ---- noms --------------------------------------------------------------
    def reg(self, p, ecriture=False):
        t, n = p.type, p.num
        if t == sm.TEMP:
            self.temps.add(n); return f'r{n}'
        if t == sm.INPUT:
            return f'v{n}'
        if t == sm.CONST:
            if p.rel is not None:
                self.utilise_a0 = True
                rc = 'xyzw'[p.rel.swz & 3]
                base = 'aL' if p.rel.type == sm.LOOP else f'a{p.rel.num}.{rc}'
                return f'K.c[clamp({base} + {n}, 0, 255)]'
            if n in self.defs and not ecriture:
                return f'k{n}'
            return f'K.c[{n}]'
        if t == sm.ADDR:
            if self.ps:
                if self.ver[0] >= 3:
                    raise NonGere('registre t en ps_3')
                return f't{n}'                          # ps_2_x : coordonnées de texture n
            self.utilise_a0 = True; return f'a{n}'
        if t == sm.RASTOUT:
            if n == 0: return 'oPos'
            raise NonGere('oFog/oPts')
        if t == sm.ATTROUT:
            return f'ov[{n}]'                       # SM2 oD n → color n
        if t == sm.OUTPUT:
            if self.ver[0] >= 3:
                return f'o{n}'
            return f'ov[{2 + n}]'                   # SM2 oT n → texcoord n
        if t == sm.COLOROUT:
            return f'oC{n}'
        if t == sm.DEPTHOUT:
            return 'oDepth'
        if t == sm.MISC:
            if n == 0: return 'vPos'
            if n == 1: return 'vFace'
        if t == sm.CONSTINT:
            return f'K.i[{n}]' if n not in self.defi else f'ki{n}'
        if t == sm.CONSTBOOL:
            return f'K.b[{n}]' if n not in self.defb else f'kb{n}'
        if t == sm.LOOP:
            return 'aL'
        if t == sm.SAMPLER:
            return f's{n}'
        raise NonGere(f'registre type {t}')

    def source(self, s, scalaire=False):
        r = self.reg(s)
        if s.type in (sm.CONSTINT,):
            r = f'vec4({r})'
        elif s.type == sm.MISC and s.num == 1:
            r = 'vec4(vFace)'
        elif s.type == sm.ADDR or s.type == sm.LOOP:
            r = f'vec4({r})'
        sw = ''.join('xyzw'[(s.swz >> (2 * k)) & 3] for k in range(4))
        e = r if sw == 'xyzw' else f'{r}.{sw}'
        m = s.smod
        e = {0: e, 1: f'(-{e})', 2: f'({e} - 0.5)', 3: f'(-({e} - 0.5))', 4: f'({e} * 2.0 - 1.0)',
             5: f'(-({e} * 2.0 - 1.0))', 6: f'(1.0 - {e})', 7: f'({e} * 2.0)', 8: f'(-({e} * 2.0))',
             11: f'abs({e})', 12: f'(-abs({e}))'}.get(m)
        if e is None:
            raise NonGere(f'modificateur de source {m}')
        return f'({e}).x' if scalaire else e

    def ecrire(self, d, expr, vec=True):
        """d ← expr (vec4), masque, saturation."""
        m = sm.masque(d.mask)
        if d.mod & 1:
            expr = f'clamp({expr}, 0.0, 1.0)'
        if d.shift:
            sh = d.shift if d.shift < 8 else d.shift - 16
            expr = f'({expr}) * {2.0 ** sh}'
        nom = self.reg(d, ecriture=True)
        if d.type == sm.ADDR and not self.ps:
            self.ligne(f'{nom}.{m} = ivec4(floor(({expr}) + 0.5)).{m};')
            return
        if nom == 'oDepth':
            self.ligne(f'gl_FragDepth = ({expr}).x;'); return
        self.ligne(f'{nom}.{m} = ({expr}).{m};')

    def ligne(self, t):
        self.lignes.append('    ' * self.indent + t)

    # ---- instructions -------------------------------------------------------
    def traduire(self):
        for i in self.ins:
            op, n = i.op, i.name
            if i.pred is not None:
                raise NonGere('instruction prédiquée')
            if op == 31:
                self.dcl(i); continue
            if op == 81:
                self.defs[i.dst.num] = i.extra; continue
            if op == 48:
                self.defi[i.dst.num] = i.extra; continue
            if op == 47:
                self.defb[i.dst.num] = i.extra; continue
            if op == 0:
                continue
            S = [self.source(s) for s in i.srcs]
            x = lambda k: self.source(i.srcs[k], scalaire=True)
            if n == 'mov': e = S[0]
            elif n == 'add': e = f'{S[0]} + {S[1]}'
            elif n == 'sub': e = f'{S[0]} - {S[1]}'
            elif n == 'mul': e = f'{S[0]} * {S[1]}'
            elif n == 'mad': e = f'{S[0]} * {S[1]} + {S[2]}'
            elif n == 'min': e = f'min({S[0]}, {S[1]})'
            elif n == 'max': e = f'max({S[0]}, {S[1]})'
            elif n == 'rcp': e = f'vec4(1.0 / {x(0)})'
            elif n == 'rsq': e = f'vec4(inversesqrt(abs({x(0)})))'
            elif n == 'exp' or n == 'expp': e = f'vec4(exp2({x(0)}))'
            elif n == 'log' or n == 'logp': e = f'vec4(log2(abs({x(0)})))'
            elif n == 'pow': e = f'vec4(pow(abs({x(0)}), {x(1)}))'
            elif n == 'dp3': e = f'vec4(dot(({S[0]}).xyz, ({S[1]}).xyz))'
            elif n == 'dp4': e = f'vec4(dot({S[0]}, {S[1]}))'
            elif n == 'dp2add': e = f'vec4(dot(({S[0]}).xy, ({S[1]}).xy) + {x(2)})'
            elif n == 'slt': e = f'vec4(lessThan({S[0]}, {S[1]}))'
            elif n == 'sge': e = f'vec4(greaterThanEqual({S[0]}, {S[1]}))'
            elif n == 'lrp': e = f'mix({S[2]}, {S[1]}, {S[0]})'
            elif n == 'frc': e = f'fract({S[0]})'
            elif n == 'abs': e = f'abs({S[0]})'
            elif n == 'sgn': e = f'sign({S[0]})'
            elif n == 'crs': e = f'vec4(cross(({S[0]}).xyz, ({S[1]}).xyz), 0.0)'
            elif n == 'nrm':
                e = f'vec4(normalize(({S[0]}).xyz), ({S[0]}).w * inversesqrt(dot(({S[0]}).xyz, ({S[0]}).xyz)))'
            elif n == 'sincos': e = f'vec4(cos({x(0)}), sin({x(0)}), 0.0, 0.0)'
            elif n == 'cmp': e = f'mix({S[2]}, {S[1]}, greaterThanEqual({S[0]}, vec4(0.0)))'
            elif n == 'cnd': e = f'mix({S[2]}, {S[1]}, greaterThan({S[0]}, vec4(0.5)))'
            elif n == 'dsx': e = f'dFdx({S[0]})'
            elif n == 'dsy': e = f'dFdy({S[0]})'
            elif n == 'lit':
                a = S[0]
                e = (f'vec4(1.0, max(({a}).x, 0.0), (({a}).x > 0.0 ? pow(max(({a}).y, 0.0), clamp(({a}).w, -127.9961, 127.9961)) : 0.0), 1.0)')
            elif n == 'dst': e = f'vec4(1.0, ({S[0]}).y * ({S[1]}).y, ({S[0]}).z, ({S[1]}).w)'
            elif n in ('m4x4', 'm4x3', 'm3x4', 'm3x3', 'm3x2'):
                lig = int(n[3]); dim = int(n[1])
                c0 = i.srcs[1]
                comps = []
                for k in range(lig):
                    q = sm.Param(); q.type, q.num, q.swz, q.smod, q.rel = c0.type, c0.num + k, 0xE4, 0, c0.rel
                    q.mask, q.mod, q.shift = 0xF, 0, 0
                    rk = self.source(q)
                    comps.append(f'dot(({S[0]}).{"xyzw"[:dim]}, ({rk}).{"xyzw"[:dim]})')
                comps += ['0.0'] * (4 - lig)
                e = f'vec4({", ".join(comps)})'
            elif n == 'mova':
                self.ecrire(i.dst, S[0]); continue
            elif n in ('texld', 'texldl', 'texldd'):
                self.texld(i, S); continue
            elif n == 'texkill':
                self.ligne(f'if (any(lessThan(({S[0]}).xyz, vec3(0.0)))) discard;'); continue
            elif n == 'if':
                self.ligne(f'if ({self.reg(i.srcs[0])} != 0) {{'); self.indent += 1; continue
            elif n == 'ifc':
                cmp = {1: '>', 2: '==', 3: '>=', 4: '<', 5: '!=', 6: '<='}[i.ctrl & 7]
                self.ligne(f'if ({x(0)} {cmp} {x(1)}) {{'); self.indent += 1; continue
            elif n == 'else':
                self.indent -= 1; self.ligne('} else {'); self.indent += 1; continue
            elif n == 'endif' or n == 'endrep' or n == 'endloop':
                self.indent -= 1; self.ligne('}'); continue
            elif n == 'rep':
                k = len(self.lignes)
                self.ligne(f'for (int rep{k} = 0; rep{k} < {self.reg(i.srcs[0])}.x; rep{k}++) {{'); self.indent += 1; continue
            elif n == 'break':
                self.ligne('break;'); continue
            elif n == 'breakc':
                cmp = {1: '>', 2: '==', 3: '>=', 4: '<', 5: '!=', 6: '<='}[i.ctrl & 7]
                self.ligne(f'if ({x(0)} {cmp} {x(1)}) break;'); continue
            else:
                raise NonGere(f'instruction {n}')
            self.ecrire(i.dst, e)

    def texld(self, i, S):
        if i.name == 'texld':
            coord, smp = S[0], i.srcs[1]
        elif i.name == 'texldl':
            coord, smp = S[0], i.srcs[1]
        else:
            coord, smp = S[0], i.srcs[1]
        if smp.type != sm.SAMPLER:
            raise NonGere('texld sans sampler')
        sn = smp.num
        dim = self.samplers.get(sn, 2)
        cc = {2: 'xy', 3: 'xyz', 4: 'xyz'}[dim]
        sw = ''.join('xyzw'[(smp.swz >> (2 * k)) & 3] for k in range(4))
        if i.name == 'texld':
            if i.ctrl == 1:                     # texldp
                e = f'textureProj(s{sn}, ({coord}).{cc}w)' if dim == 2 else f'texture(s{sn}, ({coord}).{cc} / ({coord}).w)'
            elif i.ctrl == 2:                   # texldb
                e = f'texture(s{sn}, ({coord}).{cc}, ({coord}).w)'
            else:
                e = f'texture(s{sn}, ({coord}).{cc})'
        elif i.name == 'texldl':
            e = f'textureLod(s{sn}, ({coord}).{cc}, ({coord}).w)'
        else:
            e = f'textureGrad(s{sn}, ({coord}).{cc}, ({S[2]}).{cc}, ({S[3]}).{cc})'
        if sw != 'xyzw':
            e = f'({e}).{sw}'
        self.ecrire(i.dst, e)

    def dcl(self, i):
        d, u = i.dst, i.extra
        if d.type == sm.SAMPLER:
            self.samplers[d.num] = {2: 2, 3: 3, 4: 4}.get((u >> 27) & 0xF, 2)
        elif d.type == sm.INPUT:
            self.entrees.append((d.num, u & 0x1F, (u >> 16) & 0xF))
        elif d.type == sm.OUTPUT and not self.ps:
            self.sorties[d.num] = (u & 0x1F, (u >> 16) & 0xF)
        elif d.type == sm.ADDR and self.ps:
            self.entrees_t.append(d.num)            # ps_2_x : dcl t n → texcoord n

    # ---- assemblage GLSL ----------------------------------------------------
    def regs_utilises(self, typ):
        r = set()
        for i in self.ins:
            if i.op == 31:
                continue
            for p in ([i.dst] if i.dst is not None else []) + list(i.srcs):
                if p.type == typ:
                    r.add(p.num)
        return r

    def glsl(self):
        self.traduire()
        declares = {reg for reg, _, _ in self.entrees}
        for n in sorted(self.regs_utilises(sm.INPUT) - declares):     # lus sans dcl
            if self.ps and self.ver[0] < 3:
                self.entrees.append((n, 10, n))                       # ps_2 : v n = color n
            else:
                raise NonGere(f'v{n} lu sans dcl')
        if not self.ps and self.ver[0] >= 3:
            for n in self.regs_utilises(sm.OUTPUT) - set(self.sorties):
                self.sorties_locales = getattr(self, 'sorties_locales', set()) | {n}
        tete = ['#version 450', '// traduit de D3D9 ' + ('ps' if self.ps else 'vs') + f'_{self.ver[0]}_{self.ver[1]} par scripts/d3d9-vers-spirv.py',
                f'layout(set = 0, binding = {1 if self.ps else 0}, std140) uniform Consts {{ vec4 c[256]; ivec4 i[16]; ivec4 b[16]; }} K;']
        corps = []
        if self.ps:
            tete.append('layout(push_constant) uniform PC { layout(offset = 16) vec4 at; } pc;')
            for n, dim in sorted(self.samplers.items()):
                t = {2: 'sampler2D', 3: 'samplerCube', 4: 'sampler3D'}[dim]
                tete.append(f'layout(set = 0, binding = {2 + n}) uniform {t} s{n};')
            vus = set()
            for reg, us, ix in self.entrees:
                if self.ver[0] >= 3:
                    sl = slot_varying(us, ix)
                else:                               # ps_2 : v n = color n
                    sl = reg
                if sl is None or sl in vus:
                    raise NonGere('varying d’entrée hors table')
                vus.add(sl)
                tete.append(f'layout(location = {sl}) in vec4 v{reg};')
            for n in self.entrees_t:
                if 2 + n in vus:
                    raise NonGere('varying t en double')
                vus.add(2 + n)
                tete.append(f'layout(location = {2 + n}) in vec4 t{n};')
            couts = sorted({i.dst.num for i in self.ins if i.dst is not None and i.dst.type == sm.COLOROUT})
            for k in couts:
                tete.append(f'layout(location = {k}) out vec4 oC{k};')
            if any(i.dst is not None and i.dst.type == sm.MISC for i in self.ins) or 'vPos' in ' '.join(self.lignes):
                pass
            corps.append('    vec4 vPos = vec4(gl_FragCoord.xy - 0.5, 0.0, 0.0); float vFace = gl_FrontFacing ? 1.0 : -1.0;')
            for k in couts:
                corps.append(f'    oC{k} = vec4(0.0);')
        else:
            tete.append('layout(push_constant) uniform PC { vec4 hp; } pc;')
            for reg, us, ix in self.entrees:
                tete.append(f'layout(location = {reg}) in vec4 v{reg};')
            tete.append(f'layout(location = 0) out vec4 ov[{NVARY}];')
            corps.append(f'    for (int k = 0; k < {NVARY}; k++) ov[k] = vec4(0.0);')
            corps.append('    vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);')
            if self.ver[0] >= 3:
                for reg in sorted(set(self.sorties) | getattr(self, 'sorties_locales', set())):
                    corps.append(f'    vec4 o{reg} = vec4(0.0);')
            corps.append('    ivec4 a0 = ivec4(0); int aL = 0;')
        for n, v in sorted(self.defs.items()):
            corps.append(f'    const vec4 k{n} = vec4({", ".join(repr(float(x)) for x in v)});')
        for n, v in sorted(self.defi.items()):
            corps.append(f'    const ivec4 ki{n} = ivec4({", ".join(str(x) for x in v)});')
        for n, v in sorted(self.defb.items()):
            corps.append(f'    const int kb{n} = {1 if v else 0};')
        for t in sorted(self.temps):
            corps.append(f'    vec4 r{t} = vec4(0.0);')
        fin = []
        if self.ps:
            fin.append('    if (pc.at.x > 0.5) {')
            fin.append('        float a = oC0.a, r = pc.at.z; int f = int(pc.at.y);')
            fin.append('        bool ok = f == 1 ? false : f == 2 ? a < r : f == 3 ? a == r : f == 4 ? a <= r : f == 5 ? a > r : f == 6 ? a != r : f == 7 ? a >= r : true;')
            fin.append('        if (!ok) discard;')
            fin.append('    }')
        else:
            if self.ver[0] >= 3:
                for reg, (us, ix) in sorted(self.sorties.items()):
                    u = sm.USAGES[us] if us < len(sm.USAGES) else ''
                    if u in ('position', 'positiont') and ix == 0:
                        fin.append(f'    oPos = o{reg};')
                    elif u == 'psize':
                        continue
                    else:
                        sl = slot_varying(us, ix)
                        if sl is None:
                            raise NonGere('varying de sortie hors table')
                        fin.append(f'    ov[{sl}] = o{reg};')
            fin.append('    gl_Position = vec4(oPos.x, -oPos.y, oPos.z, oPos.w);')
            fin.append('    gl_Position.xy += pc.hp.xy * gl_Position.w;   // demi-pixel D3D9')
        if self.ps and 'oC0' not in ' '.join(tete):
            raise NonGere('PS sans oC0')
        return '\n'.join(tete + ['void main() {'] + corps + self.lignes + fin + ['}', ''])


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for x in b:
        h ^= x
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def compiler(glsl, ps, tmp):
    src = os.path.join(tmp, 'x.' + ('frag' if ps else 'vert'))
    spv = os.path.join(tmp, 'x.spv')
    open(src, 'w').write(glsl)
    r = subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-o', spv, src], capture_output=True, text=True)
    if r.returncode:
        return None, r.stdout + r.stderr
    v = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', spv], capture_output=True, text=True)
    if v.returncode:
        return None, 'spirv-val : ' + v.stdout + v.stderr
    return open(spv, 'rb').read(), ''


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    din, hout = sys.argv[1], sys.argv[2]
    gdir = None
    if '--glsl' in sys.argv:
        gdir = sys.argv[sys.argv.index('--glsl') + 1]; os.makedirs(gdir, exist_ok=True)
    fichiers = sorted(f for f in os.listdir(din) if f.endswith('.bin') and f[:3] in ('vs_', 'ps_'))
    ok, ko = [], []
    with tempfile.TemporaryDirectory() as tmp:
        for f in fichiers:
            code = open(os.path.join(din, f), 'rb').read()
            h = fnv1a64(code)
            if f'{h:016x}' not in f:
                ko.append((f, 'hash du nom ≠ hash du contenu')); continue
            try:
                t = Traducteur(code)
                g = t.glsl()
            except (NonGere, KeyError, IndexError) as e:
                ko.append((f, f'non géré : {e}')); continue
            if gdir:
                open(os.path.join(gdir, f[:-4] + ('.frag' if t.ps else '.vert')), 'w').write(g)
            spv, err = compiler(g, t.ps, tmp)
            if spv is None:
                ko.append((f, err.strip().splitlines()[-1] if err.strip() else 'échec')); continue
            ok.append((h, t, spv, f))
    with open(hout, 'w') as o:
        o.write('/* sc2_shaders_spv.h — GÉNÉRÉ par scripts/d3d9-vers-spirv.py (passe 5d) : ne pas éditer.\n')
        o.write(f' * {len(ok)} shaders D3D9 de SC2 traduits en SPIR-V (validés spirv-val), {len(ko)} non traduits. */\n')
        o.write('#include <stdint.h>\n\n')
        for h, t, spv, f in ok:
            w = struct.unpack(f'<{len(spv) // 4}I', spv)
            o.write(f'static const uint32_t spv_{h:016x}[{len(w)}] = {{')
            o.write(','.join(f'0x{x:08x}' for x in w))
            o.write('};\n')
        o.write('\ntypedef struct { uint8_t reg, usage, index; } sc2_spv_entree;\n')
        o.write('typedef struct {\n    uint64_t hash; uint8_t ps; uint8_t nentrees; sc2_spv_entree entrees[16];\n'
                '    uint8_t samplers[16];   /* 0 aucun, 2 2D, 3 cube, 4 volume */\n'
                '    const uint32_t *spv; uint32_t nmots;\n} sc2_spv_shader;\n\n')
        o.write(f'#define SC2_NB_SPV {len(ok)}\n')
        o.write('static const sc2_spv_shader SC2_SHADERS_SPV[SC2_NB_SPV > 0 ? SC2_NB_SPV : 1] = {\n')
        for h, t, spv, f in ok:
            ent = ', '.join(f'{{ {r}, {u}, {x} }}' for r, u, x in t.entrees[:16]) or '{ 0, 0, 0 }'
            smp = ', '.join(str(t.samplers.get(k, 0)) for k in range(16))
            o.write(f'    {{ 0x{h:016x}ull, {1 if t.ps else 0}, {min(len(t.entrees), 16)}, {{ {ent} }}, {{ {smp} }}, '
                    f'spv_{h:016x}, {len(spv) // 4} }},\n')
        if not ok:
            o.write('    { 0 }\n')
        o.write('};\n')
    print(f'{len(ok)} traduits et validés (spirv-val), {len(ko)} non traduits → {hout}')
    for f, e in ko:
        print(f'  NON TRADUIT {f} : {e}')


if __name__ == '__main__':
    main()
