#!/usr/bin/env python3
"""drapeaux-morts.py — part des écritures de drapeaux x86 mortes dans le code C lifté.

    python3 drapeaux-morts.py <fichier.c> [<fichier.c> …] [--json sortie.json] [--top N]
    python3 drapeaux-morts.py --appliquer <src/lifted> <sortie>   (réécriture)

Analyse statique intra-procédurale du C généré par lift32_cpu.py (une instruction x86
par ligne, commentaire « /* ADRESSE: asm */ » en fin de ligne) : vivacité arrière des
six drapeaux cf/zf/sf/of/af/pf sur le graphe de flot de chaque fonction L_XXXXXXXX.
Une écriture est morte quand le drapeau est réécrit avant toute lecture, ou n'atteint
ni lecture ni sortie. Conventions conservatrices :

  - les six drapeaux sont vivants à toute sortie de fonction (return, SC2_SAUT,
    dispatch_jmp, chute de fin) : x86 ne les sauve pas, la continuation peut les lire ;
  - un appel qui reçoit c sans être une aide de cpu.h (dispatch, SC2_APPEL, appel
    direct L_XXXXXXXX(c)) lit puis tue les six ;
  - une aide à écriture conditionnelle — op_shl/op_shr/op_sar/op_rol… quand le compte
    vaut 0 — ne compte ni lecture ni écriture : elle ne tue jamais, donc elle
    surestime la vivacité ;
  - toute construction non reconnue (identifiant appelé hors des tables ci-dessous)
    lit les six et est comptée à part.

Le mode analyse sert à décider s'il vaut la peine d'ajouter une analyse de vivacité
au lifter : part d'écritures mortes, appels d'aide supprimables en entier (seul le
résultat arithmétique sert), appels où seuls {zf, sf} ou cf survivent (aides
spécialisées possibles).

Le mode --appliquer fait la réécriture : chaque appel d'aide entièrement mort
(flags_sub/add/logicz/incs/decs/adc/sbb, taille littérale) est remplacé par
l'arithmétique seule ((A op B) & masque — même valeur de retour, drapeaux en moins) ;
un cmp/test mort devient un commentaire, en gardant l'évaluation des arguments à
effet de bord. Aucune lecture ne voyait ces écritures, donc le comportement observé
est inchangé. Les autres fichiers du dossier sont recopiés ; un fichier n'est écrit
que si son contenu change.
"""
import argparse
import collections
import json
import os
import re
import sys

DRAPEAUX = ('cf', 'zf', 'sf', 'of', 'af', 'pf')
SIX = frozenset(DRAPEAUX)
BIT = {f: 1 << i for i, f in enumerate(DRAPEAUX)}
MASQUE6 = 0b111111
TOUT_SAUF_CF = frozenset(('zf', 'sf', 'of', 'af', 'pf'))
ZS = frozenset(('cf', 'of', 'sf', 'zf', 'pf'))          # flags_logicz : pas d'af

# ---- sémantique des aides de runtime/cpu.h : ident -> (lus, écrasés) ----
# écrites à la main depuis le header ; rcl/rcr lisent cf mais ne l'écrivent
# que pour un compte non nul, donc leur écriture est en PEUT_ETRE ci-dessous.
AIDES = {
    'flags_sub':        (frozenset(), SIX),
    'flags_add':        (frozenset(), SIX),
    'flags_sbb':        (frozenset(('cf',)), SIX),
    'flags_adc':        (frozenset(('cf',)), SIX),
    'flags_sub32':      (frozenset(), SIX),
    'flags_add32':      (frozenset(), SIX),
    'flags_logicz':     (frozenset(), ZS),
    'flags_logic32':    (frozenset(), ZS),
    'flags_logic8':     (frozenset(), ZS),
    'flags_incs':       (frozenset(('cf',)), TOUT_SAUF_CF),
    'flags_decs':       (frozenset(('cf',)), TOUT_SAUF_CF),
    'flags_inc32':      (frozenset(('cf',)), TOUT_SAUF_CF),
    'flags_dec32':      (frozenset(('cf',)), TOUT_SAUF_CF),
    'eflags_unpack':    (frozenset(), SIX),
    'fcompare_eflags':  (frozenset(), SIX),
    'sse_compare':      (frozenset(), SIX),
    'do_sahf':          (frozenset(), frozenset(('cf', 'pf', 'af', 'zf', 'sf'))),
    'op_bsf':           (frozenset(), frozenset(('zf',))),
    'op_bsr':           (frozenset(), frozenset(('zf',))),
    'op_bittest':       (frozenset(), frozenset(('cf',))),
    'bit_string_op':    (frozenset(), frozenset(('cf',))),
    'bit_reg_op':       (frozenset(), frozenset(('cf',))),
    'eflags_pack':      (SIX, frozenset()),             # pushf : lit les six
    'op_rcl':           (frozenset(('cf',)), frozenset()),
    'op_rcr':           (frozenset(('cf',)), frozenset()),
}

# ---- écritures conditionnelles (compte de décalage nul) : ni lecture ni écriture sûre ----
PEUT_ETRE = {
    'op_shl': ZS, 'op_shr': ZS, 'op_sar': ZS, 'op_shld': ZS, 'op_shrd': ZS,
    'op_rol': frozenset(('cf', 'of')), 'op_ror': frozenset(('cf', 'of')),
    'op_rcl': frozenset(('cf', 'of')), 'op_rcr': frozenset(('cf', 'of')),
    'shl32': frozenset(('cf', 'zf', 'sf', 'pf')), 'shr32': frozenset(('cf', 'zf', 'sf', 'pf')),
    'shr8': frozenset(('cf', 'zf', 'sf', 'pf')),
}

# ---- aides de cpu.h (et macros) sans effet sur les drapeaux ----
SANS_EFFET = frozenset((
    'push32', 'pop32', 'fpush', 'fpop', 'fst', 'fcompare', 'fprem_op',
    'fpu_fnstcw', 'fpu_fldcw', 'fpu_fnstsw', 'fpu_fninit', 'fpu_rndint', 'fpu_i64',
    'wri32', 'wri16', 'wri64', 'wri32r', 'wri16r', 'wri64r',
    'rd8', 'rd16', 'rd32', 'rd64', 'wr8', 'wr16', 'wr32', 'wr64',
    'rdf32', 'rdf64', 'rdi16', 'rdi32', 'rdi64', 'rdss', 'rdsd', 'rdxm',
    'wrss', 'wrsd', 'wrxm', 'rdf80', 'wrf80', 'wrf32', 'wrf64',
    'sse_load_ss', 'sse_load_sd', 'sse_minf', 'sse_maxf', 'sse_mind', 'sse_maxd',
    'sse_cvtt_i32', 'sse_sat_i8', 'sse_sat_i16', 'sse_sat_u8', 'sse_sat_u16',
    'op_bswap32', 'parity8', 'mask_sz', 'sign_sz',
    'atomic_xadd32', 'atomic_cmpxchg32', 'atomic_xchg32',
    'RECOMP_TODO', 'GVA', 'GPTR', 'STACK_BASE',
    'R8L', 'R8H', 'R16', 'SET8L', 'SET8H', 'SET16',
    'FS_RD8', 'FS_RD16', 'FS_RD32', 'FS_WR8', 'FS_WR16', 'FS_WR32',
    'fabs', 'floor', 'ceil', 'trunc', 'nearbyint', 'fmod', 'ldexp', 'frexp',
    'signbit', 'memcpy', 'memset', 'abort',
))
PREFIXES_SANS_EFFET = ('mmx_',)                    # toute la famille MMX de cpu.h

SORTIES = ('SC2_SAUT', 'dispatch_jmp')             # saut hors fonction (toujours suivi de return)

# macros d'appel du runtime qui reçoivent c : lisent puis tuent les six, comme un call
APPELS_RUNTIME = ('dispatch', 'SC2_APPEL', 'SC2_SETJMP3')

# ---- mode --appliquer : aides réécrivables en arithmétique seule ----
REECRIVABLES = frozenset(('flags_sub', 'flags_add', 'flags_logicz',
                          'flags_incs', 'flags_decs', 'flags_adc', 'flags_sbb'))
MASQUES_SZ = {'1': '0xFFu', '2': '0xFFFFu', '4': '0xFFFFFFFFu'}

MOTS_CLES = frozenset(('if', 'else', 'while', 'for', 'do', 'switch', 'case', 'default',
                       'break', 'continue', 'goto', 'return', 'sizeof'))

RE_CORPS = re.compile(r'^void (L_[0-9A-F]{8})\(CPU \*c\)\n\{\n(.*?)^\}', re.M | re.S)
RE_COMMENTAIRE = re.compile(r'/\*.*?\*/')
RE_ETIQUETTE = re.compile(r'^(L_[0-9A-F]{8}):$')
RE_SI_GOTO = re.compile(r'^if\s*\((.*)\)\s*goto\s+(L_[0-9A-F]{8})\s*;$')
RE_GOTO = re.compile(r'^goto\s+(L_[0-9A-F]{8})\s*;$')
RE_SI_BLOC = re.compile(r'^if\s*\((.*)\)\s*\{(.*)\}\s*;$')
RE_GOTO_QUELCONQUE = re.compile(r'\bgoto\s+(L_[0-9A-F]{8})')
RE_RETURN_FIN = re.compile(r'\breturn\s*;\s*\}*\s*$')
RE_WHILE = re.compile(r'^while\s*\(')
RE_APPEL = re.compile(r'([A-Za-z_]\w*)\s*\(')
RE_DRAP_ECRIT = re.compile(r'\bc->(cf|zf|sf|of|af|pf)\s*=(?!=)')
RE_DRAP_LEC = re.compile(r'\bc->(cf|zf|sf|of|af|pf)\b')
RE_LFONC = re.compile(r'^L_[0-9A-F]{8}$')

EXIT = -1


class Fonction:
    """Résultat de l'analyse d'une fonction : appels mesurés et anomalies."""
    __slots__ = ('nom', 'appels', 'non_reconnues', 'peut_etre', 'nbinstr', 'instrs')

    def __init__(self, nom):
        self.nom = nom
        self.appels = []          # [(aide, écrits frozenset, vivants frozenset), …]
        self.non_reconnues = []   # [texte de ligne, …]
        self.peut_etre = []       # [nom d'aide, …]
        self.nbinstr = 0
        self.instrs = []          # instructions (lignes) : pour le mode --appliquer


def _sans_effet(ident):
    return ident in SANS_EFFET or ident.startswith(PREFIXES_SANS_EFFET)


def _lire_drapeaux(code):
    """Lectures directes de c->f (une écriture directe c->f = n'est pas une lecture)."""
    ecrits = [(m.start(), m.end()) for m in RE_DRAP_ECRIT.finditer(code)]
    lus = set()
    for m in RE_DRAP_LEC.finditer(code):
        if any(a <= m.start() < b for a, b in ecrits):
            continue
        lus.add(m.group(1))
    return lus, {f for f in (m.group(1) for m in RE_DRAP_ECRIT.finditer(code))}


def _fin_appel(code, apres_ouvrante):
    """Index juste après la ) fermant l'appel (parenthèses appariées)."""
    prof, i = 1, apres_ouvrante
    while i < len(code) and prof:
        if code[i] == '(':
            prof += 1
        elif code[i] == ')':
            prof -= 1
        i += 1
    return i


def _ligne_en_evenements(code, fonc):
    """Classe les appels d'une ligne : rend (use, def, aides_def, use_apres, non_reconnue).

    aides_def = [(ident, écrasés, déb, fin), …] où déb:fin est le span TEXTUEL de tout
    l'appel dans `code` (servi au mode --appliquer ; -1:-1 pour une écriture directe).
    use_apres = lectures qui peuvent survenir APRÈS les aides de la ligne (lectures
    directes c->f de l'expression, appels inconnus) ; les lectures propres d'une aide
    (flags_adc lit cf) sont toujours AVANT ses écritures et ne les rendent pas vivantes.
    """
    use, df, aides_def, non_rec = set(), set(), [], False
    for m in RE_APPEL.finditer(code):
        ident = m.group(1)
        if ident in MOTS_CLES:                        # if (…), while (…) : pas des appels
            continue
        if ident in AIDES:
            lus, ecras = AIDES[ident]
            use |= lus
            if ecras:
                df |= ecras
                aides_def.append((ident, ecras, m.start(), _fin_appel(code, m.end())))
        elif ident in PEUT_ETRE:
            fonc.peut_etre.append(ident)
        elif _sans_effet(ident) or ident in SORTIES:
            pass
        elif ident in APPELS_RUNTIME or RE_LFONC.match(ident):
            use |= SIX                      # appel qui reçoit c : lit puis tue les six
            df |= SIX
        else:
            use |= SIX                      # inconnu : conservateur, lu partout
            non_rec = True
    lus_d, ecrits_d = _lire_drapeaux(code)
    use |= lus_d
    df |= ecrits_d
    for f in ecrits_d:
        aides_def.append(('(écriture directe)', frozenset((f,)), -1, -1))
    use_apres = set(lus_d) | (SIX if non_rec else frozenset())
    return use, df, aides_def, use_apres, non_rec


def _analyser_fonction(nom, corps):
    fonc = Fonction(nom)
    instrs = []
    etiquettes = {}
    for nligne, brut in enumerate(corps.split('\n')):
        code = RE_COMMENTAIRE.sub('', brut).strip()
        if not code:
            continue
        m = RE_ETIQUETTE.match(code)
        if m:
            etiquettes[m.group(1)] = len(instrs)      # cible = instruction suivante
            continue
        ins = {'succ': [], 'use': 0, 'def': 0, 'aides': [], 'udir': 0, 'rec': [],
               'txt': code, 'ligne': brut, 'nligne': nligne,
               'prefixe': brut[:len(brut) - len(brut.lstrip())],
               'suffixe': brut[brut.index(code) + len(code):] if code in brut else ''}
        if RE_SI_GOTO.match(code):                    # if (cond) goto L_X ; (cond lit les drapeaux)
            ins['succ'] = ['suiv', RE_SI_GOTO.match(code).group(2)]
            use, _, _, _, nr = _ligne_en_evenements(code, fonc)
            ins['use'] = sum(BIT[f] for f in use)
            if nr:
                fonc.non_reconnues.append(code)
        elif RE_GOTO.match(code):
            ins['succ'] = [RE_GOTO.match(code).group(1)]
        elif RE_SI_BLOC.match(code):                  # if (cond) { corps } : arêtes du corps + chute
            mb = RE_SI_BLOC.match(code)
            corps_b = mb.group(2)
            use, _, _, _, nr = _ligne_en_evenements(code, fonc)
            ins['use'] = sum(BIT[f] for f in use)
            ins['succ'] = ['suiv']
            ins['succ'] += RE_GOTO_QUELCONQUE.findall(corps_b)
            if re.search(r'\breturn\b|' + '|'.join(SORTIES), corps_b):
                ins['succ'].append(EXIT)
            if nr:
                fonc.non_reconnues.append(code)
        else:
            use, df, aides_def, udir, nr = _ligne_en_evenements(code, fonc)
            ins['use'] = sum(BIT[f] for f in use)
            ins['def'] = sum(BIT[f] for f in df)
            ins['aides'] = aides_def
            ins['udir'] = sum(BIT[f] for f in udir)
            ins['succ'] = ['suiv']
            if RE_WHILE.match(code):                  # rep movs & co : la ligne boucle sur elle-même
                ins['succ'].append(len(instrs))
            if RE_RETURN_FIN.search(code) or any(s in code for s in SORTIES):
                ins['succ'] = [EXIT] + [g for g in RE_GOTO_QUELCONQUE.findall(code)]
            elif 'switch' in code:                    # non émis par le lifter : conservateur
                ins['use'] |= MASQUE6
                ins['succ'].append(EXIT)
                fonc.non_reconnues.append(code)
                nr = False
            if nr:
                fonc.non_reconnues.append(code)
        instrs.append(ins)

    # résolution des successeurs : 'suiv' -> index+1 ; étiquette -> index ; orphelin -> EXIT
    fonc.nbinstr = len(instrs)
    fonc.instrs = instrs
    for i, ins in enumerate(instrs):
        succ = []
        for s in ins['succ']:
            if s == 'suiv':
                succ.append(i + 1 if i + 1 < len(instrs) else EXIT)
            elif s in etiquettes:
                succ.append(etiquettes[s])
            elif isinstance(s, int):                  # arête self-loop (while)
                succ.append(s)
            else:
                succ.append(EXIT)                     # goto vers étiquette absente : prudent
                fonc.non_reconnues.append(ins['txt'])
        ins['succ'] = succ

    _vivacite(instrs)

    for i, ins in enumerate(instrs):
        vivant_out = ins['live_out'] | ins['udir']    # lecture intra-ligne, après l'aide : conservateur
        for aide, ecrasees, deb, fin in ins['aides']:
            vivants = ecrasees & {f for f in DRAPEAUX if vivant_out & BIT[f]}
            fonc.appels.append((aide, ecrasees, vivants))
            if aide in REECRIVABLES:
                ins['rec'].append((aide, vivants, deb, fin))
    return fonc


def _vivacite(instrs):
    """Point fixe arrière par instruction : live_in = (live_out & ~def) | use."""
    n = len(instrs)
    preds = [[] for _ in range(n)]
    for i, ins in enumerate(instrs):
        for s in ins['succ']:
            if s != EXIT:
                preds[s].append(i)
    live_in = [0] * n
    pile = list(range(n))
    dans = [True] * n
    while pile:
        i = pile.pop()
        dans[i] = False
        lo = 0
        for s in instrs[i]['succ']:
            lo |= MASQUE6 if s == EXIT else live_in[s]
        ins = instrs[i]
        li = (lo & ~ins['def']) | ins['use']
        if li != live_in[i]:
            live_in[i] = li
            for p in preds[i]:
                if not dans[p]:
                    pile.append(p)
                    dans[p] = True
    for i, ins in enumerate(instrs):                  # passe finale : live_out convergé
        ins['live_out'] = 0
        for s in ins['succ']:
            ins['live_out'] |= MASQUE6 if s == EXIT else live_in[s]


def analyser_source(src, nom_fichier=''):
    """Analyse toutes les fonctions L_XXXXXXXX d'un source C lifté."""
    foncs = []
    for m in RE_CORPS.finditer(src):
        foncs.append(_analyser_fonction(m.group(1), m.group(2)))
    for f in foncs:                                   # les exemples gardent leur provenance
        f.non_reconnues = [f'{nom_fichier}:{f.nom}: {t}' if nom_fichier else t
                           for t in f.non_reconnues]
    return foncs


def agreger(foncs):
    """Total et par aide : écritures par drapeau, appels entiers/catégorisés, anomalies."""
    r = {'fonctions': len(foncs), 'instructions': sum(f.nbinstr for f in foncs),
         'ecritures': {f: {'total': 0, 'mortes': 0} for f in DRAPEAUX},
         'appels': {'total': 0, 'entierement_morts': 0, 'zf_sf_seuls': 0, 'cf_seul': 0},
         'peut_etre': {'total': 0, 'par_aide': {}}, 'non_reconnues': {'total': 0, 'exemples': []},
         'par_aide': {}}
    for f in foncs:
        r['non_reconnues']['total'] += len(f.non_reconnues)
        r['non_reconnues']['exemples'] += f.non_reconnues[:10]
        for a in f.peut_etre:
            r['peut_etre']['total'] += 1
            r['peut_etre']['par_aide'][a] = r['peut_etre']['par_aide'].get(a, 0) + 1
        for aide, ecrasees, vivants in f.appels:
            r['appels']['total'] += 1
            st = r['par_aide'].setdefault(
                aide, {'appels': 0, 'ecritures': 0, 'mortes': 0, 'entierement_morts': 0,
                       'zf_sf_seuls': 0, 'cf_seul': 0})
            st['appels'] += 1
            mortes = ecrasees - vivants
            st['ecritures'] += len(ecrasees)
            st['mortes'] += len(mortes)
            for d in ecrasees:
                r['ecritures'][d]['total'] += 1
            for d in mortes:
                r['ecritures'][d]['mortes'] += 1
            if not vivants:
                st['entierement_morts'] += 1
                r['appels']['entierement_morts'] += 1
            if vivants == frozenset(('zf', 'sf')):
                st['zf_sf_seuls'] += 1
                r['appels']['zf_sf_seuls'] += 1
            if vivants == frozenset(('cf',)):
                st['cf_seul'] += 1
                r['appels']['cf_seul'] += 1
    r['non_reconnues']['exemples'] = r['non_reconnues']['exemples'][:10]
    return r


def _split_args(appel_txt):
    """'aide(a, b, c)' -> ['a', 'b', 'c'], virgules séparées par profondeur de parenthèses."""
    ouvrante = appel_txt.index('(')
    corps = appel_txt[ouvrante + 1:appel_txt.rindex(')')]
    args, prof, deb = [], 0, 0
    for i, ch in enumerate(corps):
        if ch == '(':
            prof += 1
        elif ch == ')':
            prof -= 1
        elif ch == ',' and prof == 0:
            args.append(corps[deb:i])
            deb = i + 1
    args.append(corps[deb:])
    return [a.strip() for a in args]


def _formule(aide, args):
    """Expression C équivalente à l'appel, drapeaux en moins ; None si non réécrivable.

    Équivalence avec runtime/cpu.h : le masque y est appliqué aux opérandes AVANT
    l'opération, mais (x op y) mod 2^sz ne dépend que de x et y mod 2^sz — le cast
    (uint32_t) suivi du & du masque rend donc exactement la même valeur. adc/sbb
    gardent la lecture de c->cf, qui a lieu même quand toutes les écritures meurent.
    """
    if len(args) < 3 or args[0] != 'c':
        return None
    sz = args[-1]
    if sz not in MASQUES_SZ:
        return None                       # SZ non littéral : ne pas réécrire
    m = MASQUES_SZ[sz]
    a = args[1]
    if aide == 'flags_logicz':
        return f'(((uint32_t)({a})) & {m})'
    if aide in ('flags_incs', 'flags_decs'):
        op = '+' if aide == 'flags_incs' else '-'
        return f'(((uint32_t)({a}) {op} 1u) & {m})'
    if len(args) != 4:
        return None
    b = args[2]
    if aide == 'flags_sub':
        return f'(((uint32_t)({a}) - (uint32_t)({b})) & {m})'
    if aide == 'flags_add':
        return f'(((uint32_t)({a}) + (uint32_t)({b})) & {m})'
    if aide == 'flags_adc':
        return f'(((uint32_t)({a}) + (uint32_t)({b}) + (c->cf & 1u)) & {m})'
    if aide == 'flags_sbb':
        return f'(((uint32_t)({a}) - (uint32_t)({b}) - (c->cf & 1u)) & {m})'
    return None


def _reecritures(fonc, stats):
    """Lignes réécrites de la fonction : {nligne: nouveau texte} + stats incrémentées."""
    remplacements = {}
    for ins in fonc.instrs:
        subs = []
        for aide, vivants, deb, fin in ins['rec']:
            if vivants or deb < 0:
                continue                  # seulement les appels entièrement morts
            formule = _formule(aide, _split_args(ins['txt'][deb:fin]))
            if formule is not None:
                subs.append((deb, fin, formule, aide))
        if not subs:
            continue
        code = ins['txt']
        # texte restant hors appels substitués : '' ou ';' → instruction seule (cmp/test)
        morceaux, pos = [], 0
        for deb, fin, _, _ in subs:
            morceaux.append(code[pos:deb])
            pos = fin
        morceaux.append(code[pos:])
        if ''.join(morceaux).strip() in ('', ';'):
            gardes = []
            for deb, fin, _, aide in subs:
                for arg in _split_args(code[deb:fin])[1:-1]:
                    if re.search(r'[A-Za-z_]\w*\s*\(', arg):   # effet de bord : garder l'évaluation
                        gardes.append(f'(void)({arg});')
            marque = ' '.join(gardes + ['/* drapeaux morts */'])
            remplacements[ins['nligne']] = ins['prefixe'] + marque + ins['suffixe']
            stats['instructions'] += 1
            stats['evaluations'] += len(gardes)
        else:
            nouveau, pos = '', 0
            for deb, fin, formule, _ in subs:
                nouveau += code[pos:deb] + formule
                pos = fin
            remplacements[ins['nligne']] = ins['prefixe'] + nouveau + code[pos:] + ins['suffixe']
        for _, _, _, aide in subs:
            stats['appels'][aide] += 1
    return remplacements


def recrire_source(src):
    """Réécrit les appels d'aide entièrement morts d'un source lifté.

    Fonction par fonction (le vrai lift fait 404 Mo : une seule fonction en mémoire
    d'analyse à la fois). Rend (nouveau_src, stats) avec stats = {'appels': Counter
    par aide, 'instructions': lignes cmp/test supprimées, 'evaluations': (void) gardés}.
    """
    stats = {'appels': collections.Counter(), 'instructions': 0, 'evaluations': 0}
    morceaux, pos = [], 0
    for m in RE_CORPS.finditer(src):
        fonc = _analyser_fonction(m.group(1), m.group(2))
        lignes = m.group(2).split('\n')
        for nl, texte in _reecritures(fonc, stats).items():
            lignes[nl] = texte
        nouveau = '\n'.join(lignes)
        if nouveau != m.group(2):
            morceaux.append(src[pos:m.start(2)])
            morceaux.append(nouveau)
            pos = m.end(2)
    morceaux.append(src[pos:])
    return ''.join(morceaux), stats


def appliquer(entree, sortie):
    """Réécrit les recomp_*.c de `entree` et copie tout le dossier vers `sortie`.

    Un fichier n'est écrit que si son contenu diffère de la destination — relancer
    deux fois ne réécrit rien la seconde fois.
    """
    os.makedirs(sortie, exist_ok=True)
    tot = collections.Counter()
    tot_i = tot_e = 0
    for nom in sorted(os.listdir(entree)):
        chemin = os.path.join(entree, nom)
        if not os.path.isfile(chemin):
            continue
        with open(chemin, 'rb') as f:
            data = f.read()
        if re.match(r'^recomp_.*\.c$', nom):
            try:
                src = data.decode('utf-8')
            except UnicodeDecodeError:
                src = data.decode('latin-1')
            nouveau, stats = recrire_source(src)
            contenu = nouveau.encode('utf-8')
            detail = ', '.join(f'{k} {_fm(v)}' for k, v in stats['appels'].most_common())
            print(f'{nom} : ' + (f'{detail} réécrits ; {stats["instructions"]} instructions '
                                f'supprimées, {stats["evaluations"]} évaluations gardées'
                                if detail else 'rien à réécrire'))
            tot.update(stats['appels'])
            tot_i += stats['instructions']
            tot_e += stats['evaluations']
        else:
            contenu = data
        dest = os.path.join(sortie, nom)
        if os.path.exists(dest):
            with open(dest, 'rb') as f:
                if f.read() == contenu:
                    continue              # inchangé : on ne le réécrit pas
        with open(dest, 'wb') as f:
            f.write(contenu)
    detail = ', '.join(f'{k} {_fm(v)}' for k, v in tot.most_common())
    print(f'TOTAL --appliquer : {sum(tot.values())} appels réécrits'
          + (f' ({detail})' if detail else '')
          + f', {tot_i} instructions supprimées, {tot_e} évaluations gardées')
    return 0


def _fm(n):
    return f'{n:,}'.replace(',', ' ')


def _pct(a, b):
    return f'{100 * a / b:5.1f} %' if b else '  —  '


def imprimer(rapports, total, top):
    """Rapport lisible : un résumé par fichier puis le détail global."""
    for chemin, r in rapports:
        ec = sum(v['total'] for v in r['ecritures'].values())
        mo = sum(v['mortes'] for v in r['ecritures'].values())
        print(f'{chemin}: {_fm(r["fonctions"])} fonctions, {_fm(r["instructions"])} instructions, '
              f'{_fm(r["appels"]["total"])} appels à écriture sûre, '
              f'{_fm(mo)}/{_fm(ec)} écritures mortes ({_pct(mo, ec).strip()})')

    ec = sum(v['total'] for v in total['ecritures'].values())
    mo = sum(v['mortes'] for v in total['ecritures'].values())
    a = total['appels']
    print(f'\nTOTAL : {_fm(total["fonctions"])} fonctions, {_fm(total["instructions"])} instructions')
    print(f'écritures de drapeaux (aides à écriture sûre) : {_fm(ec)}, dont mortes {_fm(mo)} '
          f'({_pct(mo, ec).strip()})')
    print('\n  par drapeau          total     mortes    % mort')
    for d in DRAPEAUX:
        v = total['ecritures'][d]
        print(f'  {d:<14} {_fm(v["total"]):>10} {_fm(v["mortes"]):>10}   {_pct(v["mortes"], v["total"]).strip()}')
    print(f'\nappels d\'aide à écriture sûre : {_fm(a["total"])}')
    print(f'  entièrement morts (supprimables) : {_fm(a["entierement_morts"])} '
          f'({_pct(a["entierement_morts"], a["total"]).strip()})')
    print(f'  vivants = {{zf, sf}} seulement    : {_fm(a["zf_sf_seuls"])}')
    print(f'  vivant = cf seulement            : {_fm(a["cf_seul"])}')
    pe = total['peut_etre']
    detail_pe = ', '.join(f'{k}×{_fm(v)}' for k, v in
                          sorted(pe['par_aide'].items(), key=lambda kv: -kv[1]))
    print(f'sites à écriture conditionnelle ignorés (compte 0 → conservateur) : {_fm(pe["total"])}'
          + (f' ({detail_pe})' if detail_pe else ''))
    nr = total['non_reconnues']
    print(f'constructions non reconnues : {_fm(nr["total"])}')
    for e in nr['exemples']:
        print(f'    {e}')

    aides = sorted(total['par_aide'].items(), key=lambda kv: -kv[1]['mortes'])
    print(f'\npar aide ({min(top, len(aides))} premières sur {len(aides)}, triées par écritures mortes) :')
    print('  aide                 appels  écritures    mortes   % mort  ∅ morts  zf+sf  cf')
    for aide, st in aides[:top]:
        print(f'  {aide:<18} {_fm(st["appels"]):>8} {_fm(st["ecritures"]):>10} '
              f'{_fm(st["mortes"]):>10}   {_pct(st["mortes"], st["ecritures"]).strip():>6} '
              f'{_fm(st["entierement_morts"]):>8} {_fm(st["zf_sf_seuls"]):>6} '
              f'{_fm(st["cf_seul"]):>4}')
    return total


def principal(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('fichiers', nargs='*',
                    help='fichiers C liftés (mode analyse, une fonction L_* par fonction C)')
    ap.add_argument('--appliquer', nargs=2, metavar=('ENTREE', 'SORTIE'),
                    help='réécrire les recomp_*.c de ENTREE (aides entièrement mortes → '
                         'arithmétique) et copier le dossier vers SORTIE')
    ap.add_argument('--json', help='écrire le rapport machine dans ce fichier')
    ap.add_argument('--top', type=int, default=15, help='nb d\'aides détaillées (défaut 15)')
    a = ap.parse_args(argv)

    if a.appliquer:
        if a.fichiers:
            ap.error('--appliquer prend ENTREE et SORTIE, pas de fichiers en argument')
        return appliquer(a.appliquer[0], a.appliquer[1])
    if not a.fichiers:
        ap.error('indiquer des fichiers .c, ou --appliquer ENTREE SORTIE')

    rapports, toutes = [], []
    for chemin in a.fichiers:
        with open(chemin, encoding='utf-8', errors='replace') as f:
            src = f.read()
        foncs = analyser_source(src, chemin)
        rapports.append((chemin, agreger(foncs)))
        toutes += foncs
    total = imprimer(rapports, agreger(toutes), a.top)
    if a.json:
        sortie = {'fichiers': [c for c, _ in rapports], **total}
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump(sortie, f, ensure_ascii=False, indent=1, sort_keys=True)
        print(f'\nrapport JSON : {a.json}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(principal())
