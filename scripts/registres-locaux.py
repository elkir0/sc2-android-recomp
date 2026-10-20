#!/usr/bin/env python3
"""registres-locaux.py — registres et drapeaux invités en variables locales (passe 10s).

    python3 scripts/registres-locaux.py <entrée> <sortie> [--cpu-h CHEMIN] [--verifier]
                                        [--vivacite-drapeaux] [--dump-vivacite FICHIER]

Dans chaque fonction void L_XXXXXXXX(CPU *c) des recomp_*.c liftés, les 8 registres
généraux et les 6 drapeaux sont lus et écrits dans une copie locale rl (SC2_RL_DECL,
runtime/sc2_lifted.h) au lieu de *c : seules les aides de cpu.h toujours intégrées la
reçoivent (premier argument c → rl), le compilateur peut la scalariser et tenir ces
valeurs en registres hôtes. *c n'est resynchronisé qu'aux frontières — appels
(dispatch, SC2_APPEL, L_XXXXXXXX(c)), sauts (SC2_SAUT, dispatch_jmp), retours,
runtime (RECOMP_TODO, SC2_SETJMP3), appels d'aides mixtes ou inconnues — par
{ SC2_RL_VIDE(c, masque); …frontière… SC2_RL_CHARGE(c); } où le masque (4 chiffres
hexa) est l'ensemble des champs modifiés depuis la dernière synchronisation, calculé
par une analyse de flot AVANT sur le graphe d'étiquettes (fusion = union, point fixe
par liste de travail : le masque sur-approche toujours les champs réellement sales).
RECOMP_TODO est une frontière sans c textuel : la macro capture c implicitement.

Les aides de cpu.h sont classées en relisant l'en-tête (pas de table codée en dur) :
« en cache » (ne touche que les 14 champs couverts par rl), « hors cache » (n'en
touche aucun : x87, SSE, mot de contrôle… reçoivent toujours c), « mixte » (les deux :
appel traité en frontière). Fermeture transitive sur les appels aide(c, …).

Une fonction non reconnue (frontière dans un bloc conditionnel sans return ou dans
un while, c utilisé autrement que c->champ ou premier argument, goto vers une
étiquette absente, switch/for/do non SSE, en-tête différent, identifiant rl déjà
présent, ligne incomprise) est recopiée sans aucun changement : chaque fonction se
synchronise seule à l'entrée et à la sortie, un mélange de fonctions transformées et
intactes est correct. Les autres fichiers sont recopiés tels quels ; un fichier de
sortie n'est écrit que si son contenu change.

--vivacite-drapeaux : vivacité interprocédurale des six drapeaux (sémantique des
aides de cpu.h relue depuis tools/lift/drapeaux-morts.py, tables AIDES/PEUT_ETRE/
SANS_EFFET). Aux SEULS appels directs (dispatch(c, CONSTANTE) liftée, SC2_APPEL,
L_XXXXXXXX(c)) et appels d'import ({ uint32_t _ct = rd32((GVA(slot IAT de
sc2_imports.h))) ; … ; dispatch(c, _ct) }), les drapeaux sales que ni l'appelée —
E(U) : drapeaux que U ou une fonction qu'elle appelle peut lire avant de les avoir
écrits, point fixe global sur toutes les fonctions, E_indirect = union des E pour
les cibles calculées — ni l'appelant après le retour (vivacité arrière dans la
fonction appelante) ne lisent ne sont plus vidés : SC2_RL_VIDEX(c, m, x) vide m et
laisse x en retard dans *c (inversé/empoisonné dans le build de contrôle -DSC2_RL_
CONTROLE, qui signale tout écart de synchronisation sur m | x). Frontières calculées,
sauts, retours et runtime : inchangés. Une fonction non analysée (repli) a E = D6.
Sans l'option, la sortie est identique octet pour octet.

--dump-vivacite FICHIER : E par fonction (L_XXXXXXXX 0xMASQUE, triées), pour inspection.

--x87 (passe 10t) : dans chaque fonction transformée qui utilise la pile x87, l'état x87
(8 registres, sommet, mot d'état) vit dans une variable locale `x87` (SC2_X87_DECL,
runtime/sc2_lifted.h) RELATIVE AU SOMMET : x87->s[k] (k CONSTANT 0..7) ≙
c->st[(fpu_top + k) & 7] — un accès à indice variable dans rl ferait abandonner SROA sur
toute la variable et perdrait le gain de la passe. Réécritures : fpush(c, E) →
SC2_X87_PUSH(E) (v évalué avant la rotation), fpop(c) → SC2_X87_POP() (la valeur dépilée
reste en s[7], même case physique), *fst(c, i) → SC2_X87_ST(i), fcompare(c, a, b) →
SC2_X87_FCOM(a, b), fpu_fnstsw(c) → SC2_X87_FNSTSW(). Synchronisation aux MÊMES frontières
que rl : SC2_X87_VIDE(c, m) juste après SC2_RL_VIDE/SC2_RL_VIDEX, SC2_X87_CHARGE(c) juste
après SC2_RL_CHARGE, masque 10 bits par flot de données sur le même graphe (fusion = union,
point fixe) en coordonnées relatives : s0..s7 = 0x001..0x080, t = 0x100, sw = 0x200 ;
PUSH : D' = ((D & 0x07F) << 1) | 0x001 | 0x100 | (D & 0x200) ; POP : D' = ((D & 0x0FE) >>
1) | ((D & 001) << 7) | 0x100 | (D & 0x200) ; écriture ST(i) : D |= 1 << i ; FCOM :
D |= 0x200. Toute autre aide de cpu.h touchant st, fpu_top ou fpu_sw (relue depuis
l'en-tête : fpu_fninit, fprem_op), une forme non listée (fst non déréférencé, indice non
littéral) ou un identifiant x87 déjà présent → repli x87 SEUL : la passe 10s s'applique,
les formes x87 restent sur c (motif compté). Sans l'option, la sortie est identique octet
pour octet.

--verifier : (a) le retrait de la passe (déballer les SC2_RL_VIDE/SC2_RL_VIDEX/
SC2_RL_CHARGE, supprimer SC2_RL_DECL et le vidage final, rl->F → c->F, rl → c ; avec
--x87, aussi déballer/reconstituer les SC2_X87_*) doit
redonner chaque fichier d'entrée octet pour octet ; (b) invariants : plus aucun
c->F en cache ni d'aide en cache appelée avec c dans une fonction transformée,
aucune aide hors cache ni frontière appelée avec rl, rl jamais passé ailleurs, x de
SC2_RL_VIDEX fait de drapeaux disjoints de m ; avec --x87 : dans une fonction
transformée côté x87 plus aucune forme x87 sur c, chaque SC2_RL_VIDE suivi de son
SC2_X87_VIDE et chaque SC2_RL_CHARGE de son SC2_X87_CHARGE ; (c) les masques écrits (et
les x de vivacité, avec la même analyse globale ; les masques x87 avec --x87) sont
recalculés depuis l'entrée et comparés.
Échec = code de retour 1, avec les écarts.
"""
import argparse
import importlib.util
import re
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# champs couverts par rl et bits du masque (ordre de sc2_lifted.h, figé)
CHAMPS = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi',
          'cf', 'zf', 'sf', 'of', 'pf', 'af')
BIT = {f: 1 << i for i, f in enumerate(CHAMPS)}
ENSEMBLE_CHAMPS = frozenset(CHAMPS)
RX_CHAMPS = '|'.join(CHAMPS)
MASQUE_D6 = 0x3F00          # cf|zf|sf|of|pf|af (bits de sc2_lifted.h)

# frontières nommées du runtime : ident → catégorie de statistiques
FRONTIERES = {'dispatch': 'appels', 'dispatch_jmp': 'sauts', 'SC2_SAUT': 'sauts',
              'SC2_APPEL': 'appels', 'SC2_SETJMP3': 'runtime'}
# frontière dont la macro capture c implicitement (aucun c textuel en argument)
FRONTIERES_SANS_C = frozenset(('RECOMP_TODO',))
# macros de cpu.h qui reçoivent c mais ne touchent aucun champ en cache
MACROS_HORS_CACHE = ('FS_RD8', 'FS_RD16', 'FS_RD32', 'FS_WR8', 'FS_WR16', 'FS_WR32', 'STACK_BASE')

LIGNE_DECL = '    SC2_RL_DECL(c);'
LIGNE_BOUCLE = '    SC2_RL_BOUCLE();'
LIGNE_DECL_X87 = '    SC2_X87_DECL(c);'

# ---- x87 : pile locale relative au sommet (--x87, passe 10t) ----
CHAMPS_X87 = frozenset(('st', 'fpu_top', 'fpu_sw'))   # état x87 de *c couvert par la variable locale
FORMES_X87 = ('fpush', 'fpop', 'fst', 'fcompare', 'fpu_fnstsw')   # réécrites en SC2_X87_*
X87_S, X87_T, X87_SW = 0x0FF, 0x100, 0x200            # bits du masque : s0..s7, sommet, mot d'état

MOTS_CLES_APPEL = frozenset(('if', 'while', 'for', 'switch', 'return', 'sizeof'))

# ---- cpu.h : classement des aides dont le premier paramètre est CPU *c ----
RX_DEF_CPU = re.compile(r'(?:SC2_EN_LIGNE|static\s+inline)\s+[^;{()]*?\b(\w+)\s*\(\s*CPU\s*\*\s*c\s*[,)]')
RX_C_CHAMP = re.compile(r'\bc->(\w+)\b')
RX_C_ECRIT = re.compile(r'\bc->(\w+)\s*(?:\+\+|--|(?:<<|>>|[-+*/%&|^])?=)(?!=)')
RX_C_PRE_INCR = re.compile(r'(?:\+\+|--)\s*\bc->(\w+)\b')
RX_APPEL_CPU = re.compile(r'\b(\w+)\s*\(\s*c\s*[,)]')


class PasReconnu(Exception):
    """Repli sûr : la fonction est recopiée telle quelle, comptée sous ce motif."""

    def __init__(self, motif):
        super().__init__(motif)
        self.motif = motif
        self.ligne = None          # ligne fautive (renseignée par analyser_corps)


def classer_aides(texte):
    """Classement des fonctions de cpu.h : (classes, masques écrits).

    classes[ident] ∈ {'cache', 'hors', 'mixte'} selon les champs c->X touchés
    (lecture ou écriture), avec fermeture transitive sur les appels aide(c, …) ;
    ecrits[ident] = masque des champs en cache ÉCRITS (pour l'analyse de flot).
    """
    classes, ecr, _aides_x87 = analyser_cpu_h(texte)
    return classes, ecr


def analyser_cpu_h(texte):
    """Classement complet de cpu.h : (classes, masques écrits, aides touchant l'état x87).

    Même calcul que classer_aides, plus aides_x87 : identifiants dont la fermeture
    transitive touche st, fpu_top ou fpu_sw (passe 10t : seuls fpush, fpop, fst,
    fcompare et fpu_fnstsw y sont réécrits, les autres refusent la transformation x87
    de la fonction ; fpu_fldcw/fpu_fnstcw/wri*r ne touchent que fpu_cwx, hors périmètre).
    """
    corps = {}
    for m in RX_DEF_CPU.finditer(texte):
        debut = texte.find('{', m.end())
        if debut < 0:
            continue
        prof, i = 1, debut + 1
        while i < len(texte) and prof:
            if texte[i] == '{':
                prof += 1
            elif texte[i] == '}':
                prof -= 1
            i += 1
        corps.setdefault(m.group(1), []).append(texte[debut + 1:i - 1])
    touches, ecrits, appels = {}, {}, {}
    for nom, morceaux in corps.items():
        t, e, a = set(), set(), set()
        for b in morceaux:
            t.update(m.group(1) for m in RX_C_CHAMP.finditer(b))
            e.update(m.group(1) for m in RX_C_ECRIT.finditer(b))
            e.update(m.group(1) for m in RX_C_PRE_INCR.finditer(b))
            a.update(m.group(1) for m in RX_APPEL_CPU.finditer(b))
        touches[nom], ecrits[nom], appels[nom] = t, e, a
    noms = set(corps)
    encore = True                                   # fermeture transitive des appels
    while encore:
        encore = False
        for nom in noms:
            for callee in appels[nom] & noms:
                if not touches[callee] <= touches[nom]:
                    touches[nom] |= touches[callee]
                    encore = True
                if not ecrits[callee] <= ecrits[nom]:
                    ecrits[nom] |= ecrits[callee]
                    encore = True
    classes, ecr = {}, {}
    for nom in noms:
        t = touches[nom]
        en_cache, hors = t & ENSEMBLE_CHAMPS, t - ENSEMBLE_CHAMPS
        classes[nom] = 'mixte' if (en_cache and hors) else ('cache' if en_cache else 'hors')
        ecr[nom] = sum(BIT[f] for f in ecrits[nom] & ENSEMBLE_CHAMPS)
    for nom in MACROS_HORS_CACHE:
        classes[nom] = 'hors'
        ecr[nom] = 0
    return classes, ecr, {nom for nom in noms if touches[nom] & CHAMPS_X87}


# ---- analyse du code lifté ----
RX_FONC = re.compile(r'^void (L_[0-9A-F]{8})\(CPU \*c\)\n\{\n(.*?)^\}', re.M | re.S)
RX_FONC_LIGNE = re.compile(r'^void (L_[0-9A-F]{8})\([^;\n]*$', re.M)   # toute définition (pas les prototypes)
RX_ETIQUETTE = re.compile(r'(L_[0-9A-F]{8}):$')
RX_GOTO = re.compile(r'goto\s+(L_[0-9A-F]{8})\s*;')
RX_RETOUR = re.compile(r'return\s*;')
RX_RUPTURE = re.compile(r'(?:break|continue)\s*;')
RX_SI = re.compile(r'if\s*\(')
RX_TANTQUE = re.compile(r'while\s*\(')
RX_POUR = re.compile(r'for\s*\(\s*int\s+_i\b')
RX_POUR_ENTETE = re.compile(r'for\s*\(')
RX_INTERDITS = (re.compile(r'switch\s*\('), re.compile(r'do\s*\{'), re.compile(r'else\b'))
RX_CHAINE = re.compile(r'"(?:[^"\\]|\\.)*"')
RX_CHAINE_OU_COMMENTAIRE = re.compile(r'"(?:[^"\\]|\\.)*"|/\*.*?\*/')
RX_C_CHAMP_CACHE = re.compile(r'\bc->(' + RX_CHAMPS + r')\b')
RX_C_ECRIT_CACHE = re.compile(r'\bc->(' + RX_CHAMPS + r')\s*(?:\+\+|--|(?:<<|>>|[-+*/%&|^])?=)(?!=)')
RX_C_PRE_INCR_CACHE = re.compile(r'(?:\+\+|--)\s*\bc->(' + RX_CHAMPS + r')\b')
RX_SET = re.compile(r'\bSET(?:8L|8H|16)\s*\(\s*c->(' + RX_CHAMPS + r')\s*[,)]')
RX_C_ADRESSE = re.compile(r'&\s*\bc->(' + RX_CHAMPS + r')\b')
RX_APPEL = re.compile(r'([A-Za-z_]\w*)\s*\(')
RX_C_SEUL = re.compile(r'\bc\b')
RX_LFONC_NOM = re.compile(r'L_[0-9A-F]{8}$')
RX_RL_DEJA = re.compile(r'\brl\b|sc2_rl_|SC2_RL_')

# ---- formes x87 réécrites (--x87) : strictes, celles du lifter ; sinon refus x87 de la fonction ----
RX_FST_STRICT = re.compile(r'\*fst\(c, ([0-7])\)')
RX_FST_AFFECT = re.compile(r'\s*(?:\+\+|--|(?:<<|>>|[-+*/%&|^])?=(?!=))')
RX_X87_DEJA = re.compile(r'\bx87\b|sc2_x87_|SC2_X87_')

# ---- vivacité interprocédurale des drapeaux (--vivacite-drapeaux) ----
RX_ARG_CONST = re.compile(r'0x([0-9A-Fa-f]+)u')
RX_ARG_IDENT = re.compile(r'[A-Za-z_]\w*')
RX_ARG_LFONC = re.compile(r'L_[0-9A-F]{8}$')
RX_DECL_IAT = re.compile(r'\buint32_t\s+([A-Za-z_]\w*)\s*=\s*rd32\s*\(\s*\(\s*GVA\s*\(\s*0x([0-9A-Fa-f]+)u?\s*\)')
RX_IAT = re.compile(r'^\s*X\(\s*0x([0-9A-Fa-f]+)u?\s*,', re.M)

# ---- retrait de la passe (--verifier) ----
RX_DEBALLER = re.compile(r'\{ SC2_RL_VIDEX?\(c, 0x[0-9A-F]{4}u(?:, 0x[0-9A-F]{4}u)?\); (.*?) SC2_RL_CHARGE\(c\); \}')
RX_RETOUR_BALLE = re.compile(r'\{ SC2_RL_VIDE\(c, 0x[0-9A-F]{4}u\); return; \}')
RX_VIDE_SEUL = re.compile(r'    SC2_RL_VIDE\(c, 0x[0-9A-F]{4}u\);')
RX_RL_CHAMP = re.compile(r'\brl->(' + RX_CHAMPS + r')\b')
RX_RL_SEUL = re.compile(r'\brl\b')
RX_VIDE_XY = re.compile(r'SC2_RL_VIDEX?\(c, 0x([0-9A-F]{4})u(?:, 0x([0-9A-F]{4})u)?\)')
RX_VIDEX_PAIRE = re.compile(r'SC2_RL_VIDEX\(c, 0x([0-9A-F]{4})u, 0x([0-9A-F]{4})u\)')
RX_FIN_VIDE = re.compile(r'\{ SC2_RL_VIDEX?\(c, 0x[0-9A-F]{4}u(?:, 0x[0-9A-F]{4}u)?\); $')
RX_FIN_VIDE_X87 = re.compile(r'SC2_X87_VIDE\(c, 0x[0-9A-F]{3}u\); $')

# ---- retrait de la partie x87 (--verifier avec --x87) ----
RX_X87_DECL_LIGNE = re.compile(re.escape(LIGNE_DECL_X87) + r'\n')
RX_X87_VIDE_LIGNE = re.compile(r'    SC2_X87_VIDE\(c, 0x[0-9A-F]{3}u\);\n')
RX_X87_VIDE_ENLIGNE = re.compile(r'SC2_X87_VIDE\(c, 0x[0-9A-F]{3}u\); ')
RX_X87_VIDE = re.compile(r'SC2_X87_VIDE\(c, 0x([0-9A-F]{3})u\)')
RX_X87_ST = re.compile(r'SC2_X87_ST\(([0-7])\)')
RX_RL_VIDE_TOUT = re.compile(r'SC2_RL_VIDEX?\(c, 0x[0-9A-F]{4}u(?:, 0x[0-9A-F]{4}u)?\);')
RX_RL_CHARGE_TOUT = re.compile(r'SC2_RL_CHARGE\(c\);')
RX_X87_SUITE = re.compile(r'(?: SC2_X87_VIDE\(c, 0x[0-9A-F]{3}u\);|\n    SC2_X87_VIDE\(c, 0x[0-9A-F]{3}u\);)')
RX_X87_RESTE = re.compile(r'\b(?:fpush\(c,|fpop\(c|fst\(c,|fcompare\(c,|fpu_fnstsw\(c)')


def erreur(message):
    print(f"erreur : {message}", file=sys.stderr)
    raise SystemExit(1)


def est_recomp(chemin):
    return chemin.name.startswith("recomp_") and chemin.name.endswith(".c")


def lire_texte(chemin):
    try:
        with open(chemin, "r", encoding="utf-8", newline="") as f:
            return f.read()
    except UnicodeDecodeError as e:
        erreur(f"{chemin} : contenu non décodable en UTF-8 ({e})")


def fichiers_identiques(a, b):
    if a.stat().st_size != b.stat().st_size:
        return False
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(1 << 16), fb.read(1 << 16)
            if x != y:
                return False
            if not x:
                return True


def ecrire_si_different(chemin, contenu):
    """N'écrit que si le contenu change (évite les recompilations inutiles) ; True si écrit."""
    donnees = contenu.encode("utf-8")
    if chemin.is_file() and open(chemin, "rb").read() == donnees:
        return False
    chemin.write_bytes(donnees)
    return True


def copier_si_different(source, destination):
    if destination.is_file() and fichiers_identiques(source, destination):
        return False
    shutil.copyfile(source, destination)
    return True


def couper_commentaire(ligne):
    """(code, suffixe) : le suffixe est le commentaire FINAL de la ligne (« /* VA: asm */ » du lifter) et ce qui le suit.
    Un commentaire placé AVANT du code (« /* indirect jmp */ dispatch_jmp(c, c->eax); return; /* … */ », 804 lignes du
    lift) reste dans le code : masquer_chaines le blanchit pour l'analyse, la réécriture le recopie tel quel. Couper au
    premier « /* » faisait voir ces lignes vides : frontière et return non emballés, c->eax non réécrit."""
    debut = None
    for m in RX_CHAINE_OU_COMMENTAIRE.finditer(ligne):
        if m.group(0).startswith('/*') and not ligne[m.end():].strip():
            debut = m.start()
    if debut is None:
        return ligne, ''
    return ligne[:debut], ligne[debut:]


def masquer_chaines(code):
    """Intérieur des chaînes et commentaires internes remplacés par des espaces (longueur et offsets conservés)."""
    if '"' not in code and '/*' not in code:
        return code
    return RX_CHAINE_OU_COMMENTAIRE.sub(
        lambda m: ('"' + ' ' * (len(m.group(0)) - 2) + '"') if m.group(0)[0] == '"' else ' ' * len(m.group(0)), code)


def _parenthese_fermante(masque, ouvrante):
    prof, i = 1, ouvrante + 1
    while prof:
        if masque[i] == '(':
            prof += 1
        elif masque[i] == ')':
            prof -= 1
        i += 1
    return i - 1


def _accolade_fermante(masque, ouvrante):
    prof, i = 1, ouvrante + 1
    while prof:
        if masque[i] == '{':
            prof += 1
        elif masque[i] == '}':
            prof -= 1
        i += 1
    return i - 1


def _fin_instruction(masque, i, fin):
    """Index du prochain ; à profondeur de parenthèses nulle, ou -1."""
    prof = 0
    while i < fin:
        ch = masque[i]
        if ch == '(':
            prof += 1
        elif ch == ')':
            prof -= 1
        elif ch == ';' and prof <= 0:
            return i
        i += 1
    return -1


class LigneInstr:
    """Une ligne d'instruction analysée : statements, jetons de réécriture, emplacements."""

    __slots__ = ('brut', 'indice', 'code', 'suffixe', 'masque', 'stmts', 'args_c',
                 'premiers_c', 'slots', 'x87_jeton', 'refus_x87')

    def __init__(self, brut, indice, code, suffixe, masque):
        self.brut = brut
        self.indice = indice
        self.code = code
        self.suffixe = suffixe
        self.masque = masque
        self.stmts = []
        self.args_c = []      # spans du c en tête des aides en cache → rl
        self.premiers_c = []  # spans de tout premier argument c (audit)
        self.slots = []       # frontières et retours, dans l'ordre du texte
        self.x87_jeton = []   # (début, fin, texte, genre) des formes x87 réécrites
        self.refus_x87 = None # motif de repli x87 de la fonction (premier rencontré)


class Contexte:
    __slots__ = ('classes', 'ecrits', 'iat', 'x87', 'formes_x87', 'aides_x87_refus')

    def __init__(self, classes, ecrits, iat=frozenset(), x87=False, aides_x87=frozenset()):
        self.classes = classes
        self.ecrits = ecrits
        self.iat = iat        # slots d'IAT de sc2_imports.h (adresses invitées)
        self.x87 = x87        # option --x87 active
        self.formes_x87 = frozenset(FORMES_X87) & aides_x87      # réécrites (présentes dans cpu.h)
        self.aides_x87_refus = aides_x87 - self.formes_x87       # touchent st/fpu_top/fpu_sw, non réécrites


class Analyse:
    """Résultat de l'analyse d'une fonction : lignes, masques, statistiques."""

    __slots__ = ('lignes', 'masque_final', 'masques', 'frontieres', 'retours', 'boucles',
                 'etiquettes', 'masque_final_x', 'masques_x', 'refus_x87', 'x87_formes', 'x87_ok')

    def __init__(self):
        self.lignes = []       # LigneInstr indicées, dans l'ordre du corps
        self.masque_final = 0
        self.masques = []      # tous les masques, dans l'ordre du texte (+ final)
        self.frontieres = {}   # genre → nombre
        self.retours = 0
        self.boucles = set()   # étiquettes cibles d'un goto arrière : SC2_RL_BOUCLE() après elles
        self.etiquettes = {}   # étiquette → indice de ligne (n = fin virtuelle, pour la vivacité)
        self.masque_final_x = 0   # x87 : idem, 10 bits (s0..s7, t, sw)
        self.masques_x = []       # x87 : tous les masques, dans l'ordre du texte (+ final)
        self.refus_x87 = None     # x87 : motif du repli x87 seul (la passe 10s s'applique quand même)
        self.x87_formes = 0       # x87 : formes réécrites dans la fonction
        self.x87_ok = False       # x87 : la fonction est transformée côté x87


def _args_appel(masque, ouvrante, fermante):
    """Arguments d'un appel, découpés aux virgules de profondeur 0 (masque démasqué)."""
    args, prof, deb = [], 0, ouvrante - 1
    for i in range(ouvrante, fermante):
        ch = masque[i]
        if ch == '(':
            prof += 1
        elif ch == ')':
            prof -= 1
        elif ch == ',' and prof == 0:
            args.append(masque[deb + 1:i])
            deb = i
    args.append(masque[deb + 1:fermante])
    return [a.strip() for a in args]


def _info_frontiere(masque, m, nom, fin, ctx):
    """(genre brut, cible) d'une frontière d'appel/saut, pour la vivacité des drapeaux.

    dispatch_const/saut_const : cible = adresse (int) ; appel_nom : cible = L_XXXXXXXX ;
    import : dispatch(c, _ct) où _ct est lu dans un slot d'IAT de sc2_imports.h sur la
    même ligne ({ uint32_t _ct = rd32((GVA(slot))) ; … ; dispatch(c, _ct) }) ;
    dispatch_expr : argument non constant autre ; calcule : cible inconnue (dispatch_jmp,
    expression, SC2_APPEL déformé) ; runtime : RECOMP_TODO/SC2_SETJMP3 ; saut_autre :
    SC2_SAUT vers une adresse non liftée.
    """
    ferm = _parenthese_fermante(masque, m.end() - 1)
    args = _args_appel(masque, m.end(), ferm)
    if nom == 'dispatch':
        a = args[1] if len(args) > 1 else ''
        if RX_ARG_CONST.fullmatch(a):
            return ('dispatch_const', int(a[2:-1], 16))
        if ctx.iat and RX_ARG_IDENT.fullmatch(a):
            for md in RX_DECL_IAT.finditer(masque, 0, m.start()):
                if md.group(1) == a and int(md.group(2), 16) in ctx.iat:
                    return ('import', None)
        return ('dispatch_expr', a)
    if nom == 'dispatch_jmp':
        return ('calcule', None)
    if nom == 'SC2_SAUT':
        a = args[1] if len(args) > 1 else ''
        if RX_ARG_CONST.fullmatch(a):
            return ('saut_const', int(a[2:-1], 16))
        return ('saut_autre', None)
    if nom == 'SC2_APPEL':
        for a in args[1:]:
            if RX_ARG_LFONC.fullmatch(a):
                return ('appel_nom', a)
        return ('calcule', None)
    if nom in FRONTIERES_SANS_C or nom == 'SC2_SETJMP3':
        return ('runtime', None)
    return ('appel_nom', nom)                      # appel direct L_XXXXXXXX(c)


def _refuser_x87(ligne, motif):
    """Repli x87 seul : la passe 10s s'applique, les formes x87 restent sur c (premier motif gardé)."""
    if ligne.refus_x87 is None:
        ligne.refus_x87 = motif


def _forme_x87(masque, m, nom, ligne):
    """Forme x87 réécrite (passe 10t) : jeton de réécriture sur la ligne, événement de flot renvoyé.

    Formes strictes du lifter, sinon refus x87 de la fonction : fpush(c, E) (v évalué avant
    la rotation), fpop(c);, *fst(c, i) déréférencé (i littéral 0-7, adresse jamais prise),
    fcompare(c, a, b), fpu_fnstsw(c) (lecture : jamais sale). Événement (pos, 'X', op, bit) :
    push/pop tournent les bits s du masque, écriture ST(i) salit 1 << i, fcom salit sw.
    """
    debut = m.start()
    if nom == 'fst':
        mm = RX_FST_STRICT.match(masque, debut - 1) if debut > 0 and masque[debut - 1] == '*' else None
        if mm is None:
            _refuser_x87(ligne, 'x87 : fst non déréférencé' if debut == 0 or masque[debut - 1] != '*'
                         else 'x87 : indice fst non littéral')
            return None
        if debut >= 2 and masque[debut - 2] == '&':
            _refuser_x87(ligne, 'x87 : adresse de fst prise')
            return None
        i = int(mm.group(1))
        ligne.x87_jeton.append((debut - 1, mm.end(), 'SC2_X87_ST(%d)' % i, 'st'))
        if RX_FST_AFFECT.match(masque, mm.end()):        # cible d'affectation (simple ou composée)
            return (debut, 'X', 'stw', 1 << i)
        return None                                       # lecture
    if nom == 'fpush':
        if not masque.startswith('fpush(c, ', debut):
            _refuser_x87(ligne, 'x87 : forme fpush inattendue')
            return None
        ligne.x87_jeton.append((debut, debut + 9, 'SC2_X87_PUSH(', 'push'))
        return (debut, 'X', 'push', None)
    if nom == 'fpop':
        if not masque.startswith('fpop(c);', debut):
            _refuser_x87(ligne, 'x87 : forme fpop inattendue')
            return None
        ligne.x87_jeton.append((debut, debut + 8, 'SC2_X87_POP();', 'pop'))
        return (debut, 'X', 'pop', None)
    if nom == 'fcompare':
        if not masque.startswith('fcompare(c, ', debut):
            _refuser_x87(ligne, 'x87 : forme fcompare inattendue')
            return None
        ligne.x87_jeton.append((debut, debut + 12, 'SC2_X87_FCOM(', 'fcom'))
        return (debut, 'X', 'fcom', X87_SW)
    if not masque.startswith('fpu_fnstsw(c)', debut):    # lecture de sw et t : jamais sale
        _refuser_x87(ligne, 'x87 : forme fpu_fnstsw inattendue')
        return None
    ligne.x87_jeton.append((debut, debut + 13, 'SC2_X87_FNSTSW()', 'fnstsw'))
    return None


def _extraire(masque, s, e, ligne, ctx, dans_cond=False):
    """Événements ordonnés de masque[s:e] : (pos, 'W', bit, aide|None), (pos, 'R', bit,
    None) pour les lectures c->F hors cible d'affectation, (pos, 'B', genre, info)."""
    evts = []
    cibles = []                                    # spans « c->F » cibles d'une écriture
    for m in RX_C_ECRIT_CACHE.finditer(masque, s, e):
        evts.append((m.start(), 'W', BIT[m.group(1)], None))
        cibles.append((m.start(), m.start(1) + len(m.group(1))))
    for m in RX_C_PRE_INCR_CACHE.finditer(masque, s, e):
        evts.append((m.start(1) - 3, 'W', BIT[m.group(1)], None))
        cibles.append((m.start(1) - 3, m.start(1) + len(m.group(1))))
    for m in RX_SET.finditer(masque, s, e):
        evts.append((m.start(1) - 3, 'W', BIT[m.group(1)], None))
        cibles.append((m.start(1) - 3, m.start(1) + len(m.group(1))))
    for m in RX_C_ADRESSE.finditer(masque, s, e):
        q = m.start() - 1                          # & unaire ou et binaire ?
        while q >= 0 and masque[q] in ' \t':
            q -= 1
        if q >= 0 and (masque[q].isalnum() or masque[q] in '_)]\'"'):
            continue                               # « a & c->eax » : lecture, pas une adresse
        evts.append((m.start(1) - 3, 'W', BIT[m.group(1)], None))
        cibles.append((m.start(1) - 3, m.start(1) + len(m.group(1))))
    for m in RX_C_CHAMP_CACHE.finditer(masque, s, e):
        if not any(a <= m.start() < b for a, b in cibles):
            evts.append((m.start(), 'R', BIT[m.group(1)], None))
    for m in RX_APPEL.finditer(masque, s, e):
        nom = m.group(1)
        if nom in MOTS_CLES_APPEL:
            continue
        j = m.end()                                # juste après '('
        while j < e and masque[j] in ' \t':
            j += 1
        premier = None
        if j < e and masque[j] == 'c' and not (j + 1 < e and (masque[j + 1].isalnum() or masque[j + 1] == '_')):
            k = j + 1
            while k < e and masque[k] in ' \t':
                k += 1
            if k < e and masque[k] in ',)':
                premier = (j, j + 1)
                ligne.premiers_c.append((j, j + 1))
        if nom in FRONTIERES_SANS_C:
            evts.append((m.start(), 'B', 'runtime', _info_frontiere(masque, m, nom, e, ctx)))
        elif nom in FRONTIERES or RX_LFONC_NOM.fullmatch(nom):
            if premier is None:
                raise PasReconnu('c autrement')
            evts.append((m.start(), 'B', FRONTIERES.get(nom, 'appels'),
                         _info_frontiere(masque, m, nom, e, ctx)))
        elif ctx.x87 and premier is not None and (nom in ctx.formes_x87 or nom in ctx.aides_x87_refus):
            if nom in ctx.formes_x87:
                ev = _forme_x87(masque, m, nom, ligne)
                if ev is not None:
                    evts.append(ev)
            else:                                          # touche st/fpu_top/fpu_sw, non réécrite
                _refuser_x87(ligne, 'x87 : aide x87 non réécrite (%s)' % nom)
        elif nom in ctx.classes:
            cl = ctx.classes[nom]
            if cl == 'cache':
                if premier is None:
                    raise PasReconnu('c autrement')
                evts.append((m.start(), 'W', ctx.ecrits[nom], nom))
                ligne.args_c.append((j, j + 1))
            elif cl == 'mixte':
                if premier is None:
                    raise PasReconnu('c autrement')
                evts.append((m.start(), 'B', 'inconnues', ('inconnue', None)))
            # 'hors' : reste avec c, rien à faire
        elif premier is not None:
            evts.append((m.start(), 'B', 'inconnues', ('inconnue', None)))
    evts.sort(key=lambda t: t[0])
    if dans_cond and any(g == 'B' for _, g, _, _ in evts):
        raise PasReconnu('frontière dans une condition')
    return evts


def _stmt_simple(masque, i, fin, ligne, ctx):
    """Instruction ordinaire jusqu'au ; : événements, borne posée sur la ligne."""
    fin_i = _fin_instruction(masque, i, fin)
    if fin_i < 0:
        raise PasReconnu('ligne non reconnue')
    evts = _extraire(masque, i, fin_i + 1, ligne, ctx)
    bornes = [(pos, genre, info) for pos, g, genre, info in evts if g == 'B']
    if len(bornes) > 1:
        raise PasReconnu('frontières multiples dans une instruction')
    if bornes and bornes[0][0] != i:                # x = f(c) : SC2_RL_CHARGE écraserait l'affectation
        raise PasReconnu('frontière dans une expression')
    if bornes and any(pos > bornes[0][0] for pos, g, _d, _i in evts if g == 'W'):
        raise PasReconnu('écriture après une frontière')
    if bornes and ctx.x87 and any(pos > bornes[0][0] for pos, g, _d, _i in evts if g == 'X'):
        _refuser_x87(ligne, 'x87 : écriture après une frontière')   # le CHARGE écraserait l'écriture locale
    if bornes:
        slot = {'span': (i, fin_i + 1), 'genre': bornes[0][1], 'masque': None,
                'info': bornes[0][2]}
        ligne.slots.append(slot)
        return ('simple', i, fin_i + 1, evts, slot)
    return ('simple', i, fin_i + 1, evts, None)


def _parser_statements(masque, s, e, contexte, ligne, ctx):
    """Découpe masque[s:e] en statements reconnus (contexte 'ligne', 'si' ou 'tantque')."""
    stmts = []
    i = s
    while i < e:
        while i < e and masque[i] in ' \t':
            i += 1
        if i >= e:
            break
        ch = masque[i]
        if ch == '{':                               # bloc anonyme (multi-lignes _jt, xadd…)
            i += 1
            continue
        if ch == '}':                               # fermeture en fin de ligne
            i += 1
            continue
        for rx in RX_INTERDITS:
            if rx.match(masque, i, e):
                raise PasReconnu('contrôle switch/for/do/else')
        m = RX_GOTO.match(masque, i, e)
        if m:
            if contexte != 'ligne':
                raise PasReconnu('contrôle goto imbriqué')
            stmts.append(('goto', i, m.end(), m.group(1)))
            i = m.end()
            continue
        m = RX_RETOUR.match(masque, i, e)
        if m:
            if contexte == 'tantque':
                raise PasReconnu('contrôle return dans un while')
            slot = {'span': (i, m.end()), 'genre': 'retours', 'masque': None, 'info': None}
            ligne.slots.append(slot)
            stmts.append(('retour', i, m.end(), slot))
            i = m.end()
            continue
        m = RX_RUPTURE.match(masque, i, e)
        if m:
            if contexte != 'tantque':
                raise PasReconnu('contrôle break/continue hors while')
            stmts.append(('rupture', i, m.end()))
            i = m.end()
            continue
        m = RX_SI.match(masque, i, e)
        if m:
            i = _parser_si(masque, i, e, m, contexte, ligne, ctx, stmts)
            continue
        m = RX_TANTQUE.match(masque, i, e)
        if m:
            i = _parser_tantque(masque, i, e, m, contexte, ligne, ctx, stmts)
            continue
        if RX_POUR_ENTETE.match(masque, i, e):
            i = _parser_pour(masque, i, e, contexte, ligne, ctx, stmts)
            continue
        st = _stmt_simple(masque, i, e, ligne, ctx)
        stmts.append(st)
        i = st[2]
    return stmts


def _parser_si(masque, i, e, m, contexte, ligne, ctx, stmts):
    cond_s = m.end()
    cond_e = _parenthese_fermante(masque, m.end() - 1)
    apres = cond_e + 1
    while apres < e and masque[apres] in ' \t':
        apres += 1
    mg = RX_GOTO.match(masque, apres, e)
    if mg:                                          # if (COND) goto L_X ;
        if contexte != 'ligne':
            raise PasReconnu('contrôle goto imbriqué')
        stmts.append(('si_goto', i, mg.end(),
                      _extraire(masque, cond_s, cond_e, ligne, ctx, True), mg.group(1)))
        return mg.end()
    mr = RX_RUPTURE.match(masque, apres, e)
    if mr:                                          # if (COND) break; / continue; (rep movs)
        if contexte != 'tantque':
            raise PasReconnu('contrôle break/continue hors while')
        stmts.append(('si_rupture', i, mr.end(),
                      _extraire(masque, cond_s, cond_e, ligne, ctx, True)))
        return mr.end()
    if apres < e and masque[apres] == '{':          # if (COND) { CORPS }
        if contexte != 'ligne':
            raise PasReconnu('bloc if imbriqué')
        corps_e = _accolade_fermante(masque, apres)
        cond_evts = _extraire(masque, cond_s, cond_e, ligne, ctx, True)
        n_avant = len(ligne.slots)
        corps = _parser_statements(masque, apres + 1, corps_e, 'si', ligne, ctx)
        for cst in corps:
            if cst[0] not in ('simple', 'retour'):
                raise PasReconnu('bloc if non reconnu')
        finit_retour = bool(corps) and corps[-1][0] == 'retour'
        if len(ligne.slots) > n_avant and not finit_retour:
            raise PasReconnu('frontière conditionnelle')
        stmts.append(('si_bloc', i, corps_e + 1, cond_evts, corps, finit_retour))
        j = corps_e + 1
        while j < e and masque[j] in ' \t':
            j += 1
        if masque.startswith('else', j):
            raise PasReconnu('contrôle switch/for/do/else')
        if j < e and masque[j] == ';':
            j += 1
        return j
    if contexte == 'ligne':                         # if (COND) INSTRUCTION ; (lock cmpxchg)
        if RX_RETOUR.match(masque, apres, e):       # return sans accolades : non émis, prudent
            raise PasReconnu('ligne non reconnue')
        cond_evts = _extraire(masque, cond_s, cond_e, ligne, ctx, True)
        n_avant = len(ligne.slots)
        st = _stmt_simple(masque, apres, e, ligne, ctx)
        finit_retour = st[0] == 'retour'
        if len(ligne.slots) > n_avant and not finit_retour:
            raise PasReconnu('frontière conditionnelle')
        stmts.append(('si_bloc', i, st[2], cond_evts, [st], finit_retour))
        return st[2]
    raise PasReconnu('ligne non reconnue')


def _parser_tantque(masque, i, e, m, contexte, ligne, ctx, stmts):
    if contexte != 'ligne':
        raise PasReconnu('contrôle while imbriqué')
    cond_s = m.end()
    cond_e = _parenthese_fermante(masque, m.end() - 1)
    apres = cond_e + 1
    while apres < e and masque[apres] in ' \t':
        apres += 1
    if apres >= e or masque[apres] != '{':
        raise PasReconnu('ligne non reconnue')
    corps_e = _accolade_fermante(masque, apres)
    ecrits = 0
    cond_evts = _extraire(masque, cond_s, cond_e, ligne, ctx, True)
    for _, g, d, _info in cond_evts:
        if g == 'W':
            ecrits |= d
    n_avant = len(ligne.slots)
    corps = _parser_statements(masque, apres + 1, corps_e, 'tantque', ligne, ctx)
    if len(ligne.slots) > n_avant:
        raise PasReconnu('frontière dans un while')
    for cst in corps:
        if cst[0] == 'simple':
            for _, g, d, _info in cst[3]:
                if g == 'W':
                    ecrits |= d
        elif cst[0] == 'si_rupture':
            for _, g, d, _info in cst[3]:
                if g == 'W':
                    ecrits |= d
        elif cst[0] != 'rupture':
            raise PasReconnu('ligne non reconnue')
    stmts.append(('tantque', i, corps_e + 1, ecrits, cond_evts, corps))
    return corps_e + 1


def _parser_pour(masque, i, e, contexte, ligne, ctx, stmts):
    """Boucle for SSE (for (int _i …)) : admise seulement si elle ne touche rien en cache."""
    if contexte != 'ligne' or not RX_POUR.match(masque, i, e):
        raise PasReconnu('contrôle switch/for/do/else')
    cond_e = _parenthese_fermante(masque, masque.index('(', i))
    apres = cond_e + 1
    while apres < e and masque[apres] in ' \t':
        apres += 1
    if apres < e and masque[apres] == '{':
        fin_corps = _accolade_fermante(masque, apres) + 1
    else:
        fin_i = _fin_instruction(masque, apres, e)
        if fin_i < 0:
            raise PasReconnu('ligne non reconnue')
        fin_corps = fin_i + 1
    segment = masque[i:fin_corps]
    if RX_C_CHAMP_CACHE.search(segment):
        raise PasReconnu('contrôle switch/for/do/else')
    for m in RX_APPEL.finditer(segment):
        if m.group(1) in MOTS_CLES_APPEL:
            continue
        j = m.end()
        while j < len(segment) and segment[j] in ' \t':
            j += 1
        if j < len(segment) and segment[j] == 'c':
            k = j + 1
            while k < len(segment) and segment[k] in ' \t':
                k += 1
            if k < len(segment) and segment[k] in ',)':
                raise PasReconnu('contrôle switch/for/do/else')
    stmts.append(('pour', i, fin_corps))
    return fin_corps


def analyser_ligne(brut, indice, ctx):
    code, suffixe = couper_commentaire(brut)
    masque = masquer_chaines(code)
    texte = masque.strip()
    if not texte:
        return None
    m = RX_ETIQUETTE.fullmatch(texte)
    if m:
        return ('etiquette', m.group(1))
    ligne = LigneInstr(brut, indice, code, suffixe, masque)
    ligne.stmts = _parser_statements(masque, 0, len(masque), 'ligne', ligne, ctx)
    for mm in RX_C_SEUL.finditer(masque):           # audit : c n'apparaît que c->X ou en tête d'appel
        p = mm.start()
        if masque.startswith('c->', p):
            continue
        if any(a <= p < b for a, b in ligne.premiers_c):
            continue
        raise PasReconnu('c autrement')
    return ligne


def _x87_pas(x, op, d):
    """Un pas du masque x87, en coordonnées relatives au sommet (spéc. passe 10t).

    PUSH : les bits s0..s6 montent d'un cran (s7 est écrasé par la poussée, couvert par
    0x001), t salit, sw traverse ; POP : rotation cyclique (s0 part en s7 : la valeur
    dépilée reste dans la même case physique, le canon ne l'efface pas), t salit, sw
    traverse ; écriture ST(i) : 1 << i ; fcom : sw.
    """
    if op == 'push':
        return ((x & 0x07F) << 1) | 0x001 | X87_T | (x & X87_SW)
    if op == 'pop':
        return ((x & 0x0FE) >> 1) | ((x & 0x001) << 7) | X87_T | (x & X87_SW)
    return x | d                                      # 'stw' : d = 1 << i ; 'fcom' : d = X87_SW


def _x87_possible(stmts, cond_evts):
    """Effets x87 POSSIBLES d'un while (0 à n tours) : union conservatrice.

    Une rotation ou une écriture de slot peut avoir eu lieu n'importe quel nombre de
    fois : les 8 slots et t sont potentiellement sales (une poussée de trop écrase le
    retard) ; fcom ne salit que sw. Les while réels (rep movs/stos) n'ont pas de x87.
    """
    x = 0
    for evts in [cond_evts] + [st[3] for st in stmts if st[0] in ('simple', 'si_rupture')]:
        for _pos, g, op, _arg in evts:
            if g == 'X':
                x |= (X87_S | X87_T) if op != 'fcom' else X87_SW
    return x


def _executer(stmts, D, enr, X=0):
    """Marche les statements depuis (D, X) ; renvoie (chute ou None, [(étiquette, D, X)], chutex).

    D est le masque rl, X le masque x87 (10 bits), synchronisés aux mêmes frontières.
    enr=True remplit les masques des emplacements (frontières, retours). La chute
    vaut None quand la ligne ne tombe pas (return/goto) ; les écritures d'un bloc
    if non terminé par return se fondent dans la chute (sur-approximation).
    """
    chute, chutex, sorties, tombe = D, X, [], True
    for st in stmts:
        genre = st[0]
        if genre == 'simple':
            for _pos, g, d, _info in st[3]:
                if g == 'W':
                    chute |= d
                elif g == 'X':
                    chutex = _x87_pas(chutex, d, _info)
                elif g == 'B':                  # frontière : masque, puis rechargement complet
                    if enr:
                        st[4]['masque'] = chute
                        st[4]['masque_x'] = chutex
                    chute = 0
                    chutex = 0
        elif genre == 'retour':
            if enr:
                st[3]['masque'] = chute
                st[3]['masque_x'] = chutex
            chute = 0
            chutex = 0
            tombe = False
        elif genre == 'goto':
            sorties.append((st[3], chute, chutex))
            chute = 0
            chutex = 0
            tombe = False
        elif genre == 'si_goto':
            for _pos, g, d, _info in st[3]:
                if g == 'W':
                    chute |= d
                elif g == 'X':
                    chutex = _x87_pas(chutex, d, _info)
            sorties.append((st[4], chute, chutex))
        elif genre == 'si_bloc':
            for _pos, g, d, _info in st[3]:
                if g == 'W':
                    chute |= d
                elif g == 'X':
                    chutex = _x87_pas(chutex, d, _info)
            dcorps, dcorpsx = chute, chutex
            for cst in st[4]:
                if cst[0] == 'simple':
                    for _pos, g, d, _info in cst[3]:
                        if g == 'W':
                            dcorps |= d
                        elif g == 'X':
                            dcorpsx = _x87_pas(dcorpsx, d, _info)
                        elif g == 'B':               # frontière du bloc
                            if enr:
                                cst[4]['masque'] = dcorps
                                cst[4]['masque_x'] = dcorpsx
                            dcorps = 0
                            dcorpsx = 0
                else:                               # retour
                    if enr:
                        cst[3]['masque'] = dcorps
                        cst[3]['masque_x'] = dcorpsx
                    dcorps = 0
                    dcorpsx = 0
            if not st[5]:                           # le corps participe à la chute
                chute |= dcorps
                chutex |= dcorpsx
        elif genre == 'tantque':
            chute |= st[3]
            chutex |= _x87_possible(st[5], st[4])
        elif genre in ('pour', 'rupture', 'si_rupture'):
            pass
    return (chute if tombe else None), sorties, (chutex if tombe else None)


def analyser_corps(corps, ctx):
    """Analyse une fonction complète ; lève PasReconnu pour le repli sûr."""
    m_rl = RX_RL_DEJA.search(corps)
    if m_rl:
        exc = PasReconnu('identifiant rl déjà présent')
        exc.ligne = next((l.strip() for l in corps.split('\n') if RX_RL_DEJA.search(l)), '')
        raise exc
    analyse = Analyse()
    noeuds, etiquettes, pendantes = [], {}, []
    for indice, brut in enumerate(corps.split('\n')):
        try:
            res = analyser_ligne(brut, indice, ctx)
        except PasReconnu as exc:
            exc.ligne = brut.strip()[:120] or '(ligne vide)'
            raise
        if res is None:
            continue
        if isinstance(res, tuple):
            pendantes.append(res[1])
            continue
        idx = len(noeuds)
        noeuds.append(res)
        for lbl in pendantes:
            etiquettes[lbl] = idx
        pendantes.clear()
    n = len(noeuds)
    fin = n                                        # nœud virtuel de fin de fonction
    for lbl in pendantes:
        etiquettes[lbl] = fin
    for ligne in noeuds:                           # gotos vers étiquettes absentes
        for st in ligne.stmts:
            if st[0] == 'goto' and st[3] not in etiquettes:
                raise PasReconnu('goto absent')
            if st[0] == 'si_goto' and st[4] not in etiquettes:
                raise PasReconnu('goto absent')

    d_in = [None] * n
    pile = []
    if n:
        d_in[0] = (0, 0)
        pile.append(0)
    d_fin, d_fin_x, fin_atteinte = 0, 0, False
    while pile:                                    # point fixe avant, fusion = union
        i = pile.pop()
        chute, sorties, chutex = _executer(noeuds[i].stmts, d_in[i][0], False, d_in[i][1])
        aretes = []
        if chute is not None:
            aretes.append((i + 1 if i + 1 < n else fin, chute, chutex))
        for lbl, d, dx in sorties:
            aretes.append((etiquettes[lbl], d, dx))
        for t, d, dx in aretes:
            if t == fin:
                fin_atteinte = True
                d_fin |= d
                d_fin_x |= dx
            elif d_in[t] is None:
                d_in[t] = (d, dx)
                pile.append(t)
            elif d & ~d_in[t][0] or dx & ~d_in[t][1]:
                d_in[t] = (d_in[t][0] | d, d_in[t][1] | dx)
                pile.append(t)

    analyse.lignes = noeuds
    analyse.etiquettes = etiquettes
    for i, ligne in enumerate(noeuds):             # têtes de boucle : tout cycle contient un goto arrière
        for st in ligne.stmts:
            if st[0] in ('goto', 'si_goto'):
                lbl = st[3] if st[0] == 'goto' else st[4]
                if etiquettes[lbl] <= i:
                    analyse.boucles.add(lbl)
    for i, ligne in enumerate(noeuds):             # passe finale : enregistre les masques
        entree = d_in[i] if d_in[i] is not None else (0, 0)
        _executer(ligne.stmts, entree[0], True, entree[1])
        for slot in ligne.slots:
            if slot['genre'] == 'retours':
                analyse.retours += 1
            else:
                analyse.frontieres[slot['genre']] = analyse.frontieres.get(slot['genre'], 0) + 1
            analyse.masques.append(slot['masque'])
            analyse.masques_x.append(slot['masque_x'])
    analyse.masque_final = d_fin if fin_atteinte else 0
    analyse.masques.append(analyse.masque_final)
    analyse.masque_final_x = d_fin_x if fin_atteinte else 0
    analyse.masques_x.append(analyse.masque_final_x)
    if ctx.x87:                                    # éligibilité x87 (passe 10t)
        refus = next((l.refus_x87 for l in noeuds if l.refus_x87 is not None), None)
        if refus is None and RX_X87_DEJA.search(masquer_chaines(corps)):
            refus = 'x87 : identifiant x87 déjà présent'
        analyse.refus_x87 = refus
        analyse.x87_formes = sum(len(l.x87_jeton) for l in noeuds)
        analyse.x87_ok = refus is None and analyse.x87_formes > 0
    return analyse


# ---- vivacité interprocédurale des drapeaux (--vivacite-drapeaux) ----

def charger_semantique_drapeaux():
    """Tables de tools/lift/drapeaux-morts.py : ident → (lus, écrasés certains, possibles).

    Importé par importlib (pas de recopie) : AIDES = (lus, écrasés) certain, PEUT_ETRE =
    écriture possible (compte de décalage nul : n'écrase pas), SANS_EFFET/prefixe mmx_ =
    rien. Un ident absent des tables est une inconnue : lit D6.
    """
    chemin = REPO / 'recomp-sc2' / 'tools' / 'lift' / 'drapeaux-morts.py'
    spec = importlib.util.spec_from_file_location('drapeaux_morts', chemin)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    sem = {}
    for ident, (lus, ecrases) in module.AIDES.items():
        sem[ident] = (sum(BIT[f] for f in lus), sum(BIT[f] for f in ecrases),
                      sum(BIT[f] for f in module.PEUT_ETRE.get(ident, ())))
    for ident, possibles in module.PEUT_ETRE.items():
        if ident not in sem:
            sem[ident] = (0, 0, sum(BIT[f] for f in possibles))
    for ident in module.SANS_EFFET:
        if ident not in sem:
            sem[ident] = (0, 0, 0)
    return sem, module.PREFIXES_SANS_EFFET


def lire_iat(chemin):
    """Slots d'IAT de sc2_imports.h (X(0x…u, "DLL", "nom")) : adresses invitées (int)."""
    if not chemin.is_file():
        erreur(f"sc2_imports.h introuvable : {chemin} (requis avec --vivacite-drapeaux "
               f"pour distinguer les appels d'import des appels calculés)")
    return {int(a, 16) for a in RX_IAT.findall(lire_texte(chemin))}


class ResumeDrapeaux:
    """Résumé d'usage d'une fonction : contributions statiques et références symboliques."""
    __slots__ = ('statique', 'refs')

    def __init__(self):
        self.statique = 0    # masque des contributions connues (lectures ∩ P, D6 ∩ P)
        self.refs = []       # (genre brut, cible, P au point d'usage) — résolues après le balayage


class Vivacite:
    """États de la vivacité interprocédurale : E(U) par fonction, E_indirect, définies."""
    __slots__ = ('sem', 'sans_effet', 'E', 'e_indirect', 'definis')

    def __init__(self, sem, sans_effet):
        self.sem = sem                      # ident → (lus, écrasés, possibles)
        self.sans_effet = sans_effet        # préfixes (mmx_) sans effet sur les drapeaux
        self.E = {}                         # L_XXXXXXXX → masque lu à l'entrée
        self.e_indirect = 0                 # union des E de toutes les fonctions liftées
        self.definis = set()

    def E_de(self, nom):
        return self.E.get(nom, MASQUE_D6)   # fonction non analysée (repli) : peut tout lire

    def sem_aide(self, ident):
        """(lus, écrasés certains, possibles, inconnue) pour une aide appelée avec c."""
        s = self.sem.get(ident)
        if s is not None:
            return s + (False,)
        if ident.startswith(self.sans_effet):
            return (0, 0, 0, False)
        return (MASQUE_D6, 0, 0, True)      # absente des tables : lit D6, n'écrase rien

    def resoudre(self, info):
        """(genre, cible) final : direct/import/calcule/saut/saut_autre/runtime/inconnue."""
        genre, cible = info
        if genre == 'dispatch_const':
            nom = 'L_%08X' % cible
            return ('direct', nom) if nom in self.definis else ('calcule', None)
        if genre == 'appel_nom':
            return ('direct', cible) if cible in self.definis else ('calcule', None)
        if genre == 'saut_const':
            nom = 'L_%08X' % cible
            return ('saut', nom) if nom in self.definis else ('saut_autre', None)
        if genre == 'import':
            return ('import', None)
        if genre == 'runtime':
            return ('runtime', None)
        if genre == 'inconnue':
            return ('inconnue', None)
        return ('calcule', None)            # dispatch_expr hors IAT, dispatch_jmp


def _marche_evts(evts, p, certain, viva, resume):
    """P avant (drapeaux possiblement non écrits depuis l'entrée) à travers des événements.

    Écriture certaine : retire ; écriture possible : ne retire pas ; lectures et appels
    collectés dans le résumé (lecture ∩ P ; E(cible) ∩ P ; E_indirect ∩ P ; D6 ∩ P).
    resume=None : marche de point fixe, sans collecte.
    Dans une instruction, toutes les lectures sont comptées AVANT toutes les écritures (la partie droite d'une
    affectation est évaluée d'abord : l'ordre du texte tromperait pour « c->cf = !c->cf ») : conservateur.
    """
    p0, tue = p, 0
    for _pos, g, d, info in evts:                       # écritures certaines de l'instruction
        if g == 'W' and certain:
            if info is not None:
                _lus, ecrases, _possibles, inconnue = viva.sem_aide(info)
                if not inconnue:
                    tue |= ecrases
            else:
                tue |= d & MASQUE_D6
    p = p0 & ~tue
    for _pos, g, d, info in evts:                       # lectures et frontières avec le P d'entrée
        if g == 'R':
            if resume is not None:
                resume.statique |= d & p0
        elif g == 'W':
            if info is not None and resume is not None:  # aide de cpu.h : ses lectures
                lus, _ecrases, _possibles, inconnue = viva.sem_aide(info)
                resume.statique |= p0 if inconnue else (lus & p0)
        elif g == 'X':
            pass                                        # x87 : rien pour les drapeaux
        else:                                           # frontière : transparente pour P
            genre, cible = info
            if genre in ('dispatch_const', 'appel_nom', 'saut_const'):
                if resume is not None:
                    resume.refs.append((genre, cible, p0))
            elif genre in ('runtime', 'inconnue', 'saut_autre'):
                if resume is not None:
                    resume.statique |= p0               # lisent D6 ∩ P
            elif genre in ('calcule', 'dispatch_expr'):
                if resume is not None:                  # cible inconnue
                    resume.refs.append(('indirect', None, p0))
            # 'import' : ne lit aucun drapeau
    return p


def _marche(stmts, p, viva, resume):
    """P avant à travers les statements d'une ligne ; (chute|None, sorties [(étiquette, P)]).

    Les écritures d'un bloc conditionnel ou d'un while sont POSSIBLES : elles ne retirent
    rien ; les instructions y contribuent leurs lectures avec le P du point d'usage.
    """
    tombe, sorties = True, []
    for st in stmts:
        genre = st[0]
        if genre == 'simple':
            p = _marche_evts(st[3], p, True, viva, resume)
        elif genre == 'retour':
            tombe = False
        elif genre == 'goto':
            sorties.append((st[3], p))
            tombe = False
        elif genre == 'si_goto':
            p = _marche_evts(st[3], p, False, viva, resume)
            sorties.append((st[4], p))
        elif genre == 'si_bloc':
            p = _marche_evts(st[3], p, False, viva, resume)
            _marche(st[4], p, viva, resume)             # corps : contributions, P inchangé
        elif genre == 'tantque':
            _marche_evts(st[4], p, False, viva, resume)
            _marche(st[5], p, viva, resume)
        elif genre == 'si_rupture':
            _marche_evts(st[3], p, False, viva, resume)
        # 'pour', 'rupture' : rien en cache, rien pour les drapeaux
    return (p if tombe else None), sorties


def _resume_drapeaux(analyse, viva):
    """Résumé d'usage d'une fonction : point fixe avant de P par ligne, puis collecte."""
    noeuds, n = analyse.lignes, len(analyse.lignes)
    etiquettes = analyse.etiquettes
    p_in = [None] * n
    pile = []
    if n:
        p_in[0] = MASQUE_D6
        pile.append(0)
    while pile:                                        # point fixe avant, fusion = union
        i = pile.pop()
        chute, sorties = _marche(noeuds[i].stmts, p_in[i], viva, None)
        aretes = []
        if chute is not None:
            aretes.append((i + 1 if i + 1 < n else n, chute))
        for lbl, p in sorties:
            aretes.append((etiquettes[lbl], p))
        for t, p in aretes:
            if t == n:
                continue                               # sortie de fonction : rien
            if p_in[t] is None:
                p_in[t] = p
                pile.append(t)
            elif p & ~p_in[t]:
                p_in[t] |= p
                pile.append(t)
    resume = ResumeDrapeaux()
    for i, ligne in enumerate(noeuds):                 # collecte avec P convergé
        _marche(ligne.stmts, p_in[i] if p_in[i] is not None else MASQUE_D6, viva, resume)
    return resume


def construire_vivacite(source, fichiers, ctx):
    """Pré-calcul global : E(U) par point fixe sur toutes les fonctions de tous les fichiers.

    Seuls des résumés par fonction sont gardés (pas de textes). E initial = contributions
    statiques ; les références (appels directs, sauts, cibles calculées) propagent E par
    liste de travail sur le graphe des appelants ; E_indirect = union des E de toutes les
    fonctions définies (une cible indirecte est une fonction liftée ou un import).
    """
    sem, prefixes = charger_semantique_drapeaux()
    viva = Vivacite(sem, prefixes)
    resumes = {}
    for chemin in fichiers:
        if not est_recomp(chemin):
            continue
        texte = lire_texte(chemin)
        viva.definis.update(m.group(1) for m in RX_FONC_LIGNE.finditer(texte))
        for m in RX_FONC.finditer(texte):
            try:
                analyse = analyser_corps(m.group(2), ctx)
            except PasReconnu:
                continue                               # repli : E = D6, rien à calculer
            resumes[m.group(1)] = _resume_drapeaux(analyse, viva)
    refs_finales, appelants, avec_indirect = {}, {}, set()
    for nom, res in resumes.items():                   # résolution des cibles
        finaux = []
        for genre, cible, pm in res.refs:
            if genre in ('dispatch_const', 'saut_const'):
                cible = 'L_%08X' % cible
            if genre in ('dispatch_const', 'appel_nom', 'saut_const'):
                if cible in viva.definis:
                    finaux.append(('e', cible, pm))
                    appelants.setdefault(cible, []).append(nom)
                    continue
                if genre == 'saut_const':              # saut hors lift : D6 ∩ P
                    finaux.append(('d6', None, pm))
                    continue
            finaux.append(('indirect', None, pm))      # cible inconnue
            avec_indirect.add(nom)
        refs_finales[nom] = finaux
    for nom in viva.definis:
        if nom not in resumes:
            viva.e_indirect |= MASQUE_D6               # fonction intacte : E = D6
    viva.E = {nom: res.statique for nom, res in resumes.items()}
    for res in resumes.values():
        viva.e_indirect |= res.statique
    pile = list(resumes)
    dans = set(pile)
    while pile:                                        # point fixe global sur toutes les fonctions
        nom = pile.pop()
        dans.discard(nom)
        e = resumes[nom].statique
        for genre, cible, pm in refs_finales[nom]:
            if genre == 'e':
                e |= viva.E_de(cible) & pm
            elif genre == 'indirect':
                e |= viva.e_indirect & pm
            else:
                e |= pm
        if e != viva.E[nom]:
            viva.E[nom] = e
            if e & ~viva.e_indirect:
                viva.e_indirect |= e                   # E_indirect a bougé : tout revoir
                for autre in avec_indirect:
                    if autre != nom and autre not in dans:
                        dans.add(autre)
                        pile.append(autre)
            for appellant in appelants.get(nom, ()):
                if appellant not in dans:
                    dans.add(appellant)
                    pile.append(appellant)
    return viva


def _reculer_evts(evts, vivant, certain, viva, slot=None):
    """Vivacité arrière des drapeaux à travers les événements d'un statement.

    Enregistre slot['vivant'] (vivant juste APRÈS la frontière) puis traverse : appel
    direct → E(cible) ∪ vivant (n'écrase rien) ; calculé → E_indirect ∪ vivant ; import →
    vivant ; saut/runtime/inconnue → D6. Écriture certaine : retire ; possible : non.
    Instruction sans frontière : vivant = (vivant ∖ écritures certaines) ∪ toutes ses lectures (lectures avant
    écritures, comme dans _marche_evts ; une aide qui lit et écrit, flags_adc, garde sa lecture de cf).
    """
    if not any(g == 'B' for _pos, g, _d, _info in evts):
        tue = lit = 0
        for _pos, g, d, info in evts:
            if g == 'R':
                lit |= d
            elif g == 'W':
                if info is not None:
                    lus, ecrases, _possibles, inconnue = viva.sem_aide(info)
                    lit |= MASQUE_D6 if inconnue else lus
                    if certain and not inconnue:
                        tue |= ecrases
                elif certain:
                    tue |= d & MASQUE_D6
        return (vivant & ~tue) | lit
    for _pos, g, d, info in reversed(evts):
        if g == 'R':
            vivant |= d
        elif g == 'X':
            continue                                    # x87 : rien pour les drapeaux
        elif g == 'W':
            if info is not None:                        # retirer puis ajouter : la lecture précède
                lus, ecrases, _possibles, inconnue = viva.sem_aide(info)
                if certain and not inconnue:
                    vivant &= ~ecrases
                vivant |= MASQUE_D6 if inconnue else lus
            elif certain:
                vivant &= ~(d & MASQUE_D6)
        else:
            if slot is not None:
                slot['vivant'] = vivant
                slot = None
            genre, cible = viva.resoudre(info)
            if genre == 'direct':
                vivant |= viva.E_de(cible)
            elif genre == 'calcule':
                vivant |= viva.e_indirect
            elif genre != 'import':
                vivant = MASQUE_D6                      # saut, saut hors lift, runtime, inconnue
    return vivant


def _reculer(stmts, vivant, viva, vivant_cibles, certain=True):
    """Vivacité arrière à travers les statements d'une ligne ; renvoie le vivant d'entrée.

    return → D6 (appelants inconnus) ; goto/si_goto → vivant de la cible ; corps de if
    terminal → D6 avant lui ; while → lectures des deux (écritures possibles, ne tuent pas).
    """
    for st in reversed(stmts):
        genre = st[0]
        if genre == 'simple':
            vivant = _reculer_evts(st[3], vivant, certain, viva, st[4])
        elif genre == 'retour':
            vivant = MASQUE_D6
        elif genre == 'goto':
            vivant = vivant_cibles.get(st[3], MASQUE_D6)
        elif genre == 'si_goto':
            vivant |= vivant_cibles.get(st[4], MASQUE_D6)
            vivant = _reculer_evts(st[3], vivant, False, viva)
        elif genre == 'si_bloc':
            if st[5]:                                   # corps terminal : return y exige tout
                _reculer(st[4], MASQUE_D6, viva, {}, False)
                vivant = MASQUE_D6
            else:
                vivant = _reculer(st[4], vivant, viva, {}, False)
            vivant = _reculer_evts(st[3], vivant, False, viva)
        elif genre == 'tantque':
            vivant = _reculer(st[5], vivant, viva, {}, False)
            vivant = _reculer_evts(st[4], vivant, False, viva)
        elif genre == 'si_rupture':
            vivant = _reculer_evts(st[3], vivant, False, viva)
        # 'pour', 'rupture' : rien
    return vivant


def appliquer_vivacite(analyse, viva):
    """Vivacité arrière par fonction ; réduit les masques des appels directs et d'import.

    x = D ∩ D6 ∖ (E(cible) ∪ V_après) est retiré du masque (SC2_RL_VIDEX(c, m, x)) ; les
    autres frontières et les retours sont inchangés. Renvoie (sites direct/import, omis).
    """
    noeuds = analyse.lignes
    n = len(noeuds)
    etiquettes = analyse.etiquettes
    tombes, cibles = [], []
    for ligne in noeuds:
        chute, sorties, _chutex = _executer(ligne.stmts, 0, False)
        tombes.append(chute is not None)
        cibles.append([lbl for lbl, _d, _dx in sorties])
    succs, preds = [], [[] for _ in range(n + 1)]
    for i in range(n):
        s = ([i + 1] if tombes[i] else []) + [etiquettes[lbl] for lbl in cibles[i]]
        succs.append(s)
        for t in s:
            preds[t].append(i)

    def vivant_ligne(i, sortie_vivant):
        return _reculer(noeuds[i].stmts, sortie_vivant, viva,
                        {lbl: vivant[etiquettes[lbl]] for lbl in cibles[i]})

    vivant = [0] * (n + 1)
    vivant[n] = MASQUE_D6                               # chute de fin : appelants inconnus
    pile = list(range(n))
    while pile:                                        # point fixe arrière sur le graphe
        i = pile.pop()
        l = vivant_ligne(i, vivant[i + 1] if tombes[i] else 0)
        if l != vivant[i]:
            vivant[i] = l
            pile.extend(preds[i])
    sites = omis = 0
    for i in range(n):                                 # marche finale : vivant après chaque frontière
        vivant_ligne(i, vivant[i + 1] if tombes[i] else 0)
        for slot in noeuds[i].slots:
            info = slot['info']
            if info is None:
                continue
            genre, cible = viva.resoudre(info)
            if genre not in ('direct', 'import'):
                continue
            sites += 1
            non_lus = slot['vivant'] | (viva.E_de(cible) if genre == 'direct' else 0)
            x = slot['masque'] & MASQUE_D6 & ~non_lus
            if x:
                slot['x'] = x
                slot['masque'] &= ~x
                omis += bin(x).count('1')
    analyse.masques = [s['masque'] for ligne in noeuds for s in ligne.slots]
    analyse.masques.append(analyse.masque_final)
    return sites, omis


def _masque_hexa(m):
    return '0x%04Xu' % m


def _masque_hexa_x87(m):
    return '0x%03Xu' % m


def reecrire_ligne(ligne, x87=False):
    """Réécriture d'une ligne : c->F → rl->F, aide en cache c → rl, formes x87, ballotes de frontières."""
    jetons = [(m.start(), m.end(), 'rl->' + m.group(1)) for m in RX_C_CHAMP_CACHE.finditer(ligne.masque)]
    jetons += [(a, b, 'rl') for a, b in ligne.args_c]
    if x87:
        jetons += [(a, b, t) for a, b, t, _genre in ligne.x87_jeton]
    jetons.sort()

    def inner(s, e):
        morceaux, pos = [], s
        for a, b, t in jetons:
            if a >= s and b <= e:
                morceaux.append(ligne.code[pos:a])
                morceaux.append(t)
                pos = b
        morceaux.append(ligne.code[pos:e])
        return ''.join(morceaux)

    remp = []
    for slot in ligne.slots:
        s, e = slot['span']
        if slot['genre'] == 'retours':
            texte = '{ SC2_RL_VIDE(c, %s); return; }' % _masque_hexa(slot['masque'])
            if x87:
                texte = '{ SC2_RL_VIDE(c, %s); SC2_X87_VIDE(c, %s); return; }' % (
                    _masque_hexa(slot['masque']), _masque_hexa_x87(slot['masque_x']))
            remp.append((s, e, texte))
        else:
            x = slot.get('x', 0)
            ballot = ('SC2_RL_VIDEX(c, %s, %s)' % (_masque_hexa(slot['masque']), _masque_hexa(x))
                      if x else 'SC2_RL_VIDE(c, %s)' % _masque_hexa(slot['masque']))
            texte = '{ %s; %s SC2_RL_CHARGE(c); }' % (ballot, inner(s, e))
            if x87:
                texte = '{ %s; SC2_X87_VIDE(c, %s); %s SC2_RL_CHARGE(c); SC2_X87_CHARGE(c); }' % (
                    ballot, _masque_hexa_x87(slot['masque_x']), inner(s, e))
            remp.append((s, e, texte))
    for a, b, t in jetons:
        if not any(s <= a < e for s, e, _ in remp):
            remp.append((a, b, t))
    remp.sort()
    morceaux, pos = [], 0
    for a, b, t in remp:
        morceaux.append(ligne.code[pos:a])
        morceaux.append(t)
        pos = b
    morceaux.append(ligne.code[pos:])
    return ''.join(morceaux) + ligne.suffixe


def transformer_fonction(corps, ctx, viva=None):
    """(nouveau corps, analyse, sites direct/import, drapeaux omis) ; lève PasReconnu sinon."""
    analyse = analyser_corps(corps, ctx)
    sites = omis = 0
    if viva is not None:
        sites, omis = appliquer_vivacite(analyse, viva)
    reecrites = {l.indice: reecrire_ligne(l, analyse.x87_ok) for l in analyse.lignes}
    lignes = corps.split('\n')
    nouveaux = [LIGNE_DECL]
    if analyse.x87_ok:
        nouveaux.append(LIGNE_DECL_X87)
    for i, brut in enumerate(lignes):
        nouveaux.append(reecrites.get(i, brut))
        if i not in reecrites and brut.strip()[:-1] in analyse.boucles and brut.strip().endswith(':'):
            nouveaux.append(LIGNE_BOUCLE)              # attente active : la mémoire invitée est relue à chaque tour
    nouveaux.insert(len(nouveaux) - 1, '    SC2_RL_VIDE(c, %s);' % _masque_hexa(analyse.masque_final))
    if analyse.x87_ok:                                # vidage x87 juste après celui de rl
        nouveaux.insert(len(nouveaux) - 1, '    SC2_X87_VIDE(c, %s);' % _masque_hexa_x87(analyse.masque_final_x))
    return '\n'.join(nouveaux), analyse, sites, omis


def transformer_texte(texte, ctx, chemin, stats, viva=None):
    """Applique la passe à toutes les fonctions d'un recomp_*.c (le reste est intact)."""
    morceaux, pos = [], 0
    transformees = 0
    motifs = stats['motifs']
    fonctions = list(RX_FONC.finditer(texte))
    for m in fonctions:
        nom, corps = m.group(1), m.group(2)
        try:
            nouveau, analyse, sites, omis = transformer_fonction(corps, ctx, viva)
        except PasReconnu as exc:
            motifs[exc.motif] = motifs.get(exc.motif, 0) + 1
            exemples = stats['exemples'].setdefault(exc.motif, [])
            if len(exemples) < 5:
                fautive = exc.ligne or next((l.strip() for l in corps.split('\n') if l.strip()), '')
                exemples.append(f"{chemin}:{nom}: {fautive[:120]}")
            continue
        transformees += 1
        for genre, nombre in analyse.frontieres.items():
            stats['frontieres'][genre] = stats['frontieres'].get(genre, 0) + nombre
        stats['retours'] += analyse.retours
        stats['boucles'] += len(analyse.boucles)
        stats['masques'].extend(analyse.masques)
        stats['rl_sites'] += sites
        stats['rl_omis'] += omis
        if analyse.x87_ok:
            stats['x87_transformees'] += 1
            for ligne_a in analyse.lignes:
                for _a, _b, _t, genre in ligne_a.x87_jeton:
                    stats['x87_' + genre] += 1
            stats['x87_vidages'] += len(analyse.masques_x)
            stats['masques_x87'].extend(analyse.masques_x)
        elif ctx.x87 and analyse.refus_x87 is not None:
            motif = analyse.refus_x87
            stats['motifs_x87'][motif] = stats['motifs_x87'].get(motif, 0) + 1
            exemples = stats['exemples_x87'].setdefault(motif, [])
            if len(exemples) < 5:
                exemples.append(f"{chemin}:{nom}")
        morceaux.append(texte[pos:m.start(2)])
        morceaux.append(nouveau)
        pos = m.end(2)
    morceaux.append(texte[pos:])
    total_defs = len(RX_FONC_LIGNE.findall(texte))
    en_tete = max(0, total_defs - len(fonctions))
    if en_tete:
        motifs['en-tête différente'] = motifs.get('en-tête différente', 0) + en_tete
    stats['transformees'] += transformees
    stats['fonctions'] += total_defs
    return ''.join(morceaux)


# ---- vérification (--verifier) ----

def _retirer_x87(corps):
    """Inverse de la partie x87 : macros insérées retirées, formes réécrites reconstituées.

    SC2_X87_PUSH(E) → fpush(c, E) et SC2_X87_FCOM(a, b) → fcompare(c, a, b) exigent
    l'appariement des parenthèses (E contient des appels) ; ST/POP/FNSTSW sont littérales.
    """
    if 'SC2_X87_' not in corps:
        return corps
    corps = RX_X87_DECL_LIGNE.sub('', corps)
    corps = RX_X87_VIDE_LIGNE.sub('', corps)          # vidage final (ligne entière)
    corps = RX_X87_VIDE_ENLIGNE.sub('', corps)        # vidage en ligne, avant la frontière
    corps = corps.replace(' SC2_X87_CHARGE(c);', '')
    morceaux, pos = [], 0
    while True:                                        # PUSH/FCOM : parenthèse appariée
        i = corps.find('SC2_X87_', pos)
        if i < 0:
            morceaux.append(corps[pos:])
            break
        marque = next((mk for mk in ('SC2_X87_PUSH(', 'SC2_X87_FCOM(') if corps.startswith(mk, i)), None)
        if marque is None:                             # ST/POP/FNSTSW : traitées par remplacement après
            morceaux.append(corps[pos:i + 9])
            pos = i + 9
            continue
        ouvrante = i + len(marque) - 1
        fermante = _parenthese_fermante(corps, ouvrante)
        morceaux.append(corps[pos:i])
        morceaux.append('fpush(c, ' if marque == 'SC2_X87_PUSH(' else 'fcompare(c, ')
        morceaux.append(corps[ouvrante + 1:fermante])
        morceaux.append(')')
        pos = fermante + 1
    corps = ''.join(morceaux)
    corps = corps.replace('SC2_X87_POP();', 'fpop(c);')
    corps = RX_X87_ST.sub(r'*fst(c, \1)', corps)
    return corps.replace('SC2_X87_FNSTSW()', 'fpu_fnstsw(c)')


def retirer_fonction(corps):
    """Inverse exact de la passe sur un corps transformé."""
    corps = _retirer_x87(corps)
    corps = RX_DEBALLER.sub(r'\1', corps)
    corps = RX_RETOUR_BALLE.sub('return;', corps)
    lignes = [l for l in corps.split('\n')
              if l != LIGNE_DECL and l != LIGNE_BOUCLE and not RX_VIDE_SEUL.fullmatch(l)]
    corps = '\n'.join(lignes)
    corps = RX_RL_CHAMP.sub(r'c->\1', corps)
    return RX_RL_SEUL.sub('c', corps)


def retirer_passe(texte):
    """Retire la passe : seules les fonctions transformées (ligne SC2_RL_DECL) sont inversées."""
    morceaux, pos = [], 0
    for m in RX_FONC.finditer(texte):
        corps = m.group(2)
        if corps.startswith(LIGNE_DECL + '\n'):
            morceaux.append(texte[pos:m.start(2)])
            morceaux.append(retirer_fonction(corps))
            pos = m.end(2)
    morceaux.append(texte[pos:])
    return ''.join(morceaux)


def verifier_invariants(texte, classes, chemin, ecarts, x87=False):
    """(b) dans chaque fonction transformée : rl propre, c->F en cache disparu… (+ x87)"""
    caches = sorted(n for n, cl in classes.items() if cl == 'cache')
    autres = sorted(set(n for n, cl in classes.items() if cl != 'cache') | set(FRONTIERES)
                    | set(FRONTIERES_SANS_C))
    rx_aide_c = re.compile(r'\b(?:' + '|'.join(map(re.escape, caches)) + r')\s*\(\s*c\s*[,)]')
    rx_hors_rl = re.compile(r'\b(?:' + '|'.join(map(re.escape, autres)) + r')\s*\(\s*rl\s*[,)]')
    rx_aide_rl = re.compile(r'\b(' + '|'.join(map(re.escape, caches)) + r')\s*\(\s*rl\s*[,)]')
    hors = frozenset(n for n, cl in classes.items() if cl == 'hors')
    rx_frontiere = re.compile(r'\b(\w+)\s*\(\s*c\s*[,)]|\b(RECOMP_TODO)\s*\(|\b(return)\s*;')
    for m in RX_FONC.finditer(texte):
        corps = m.group(2)
        if not corps.startswith(LIGNE_DECL + '\n'):
            continue
        nom = m.group(1)
        est_x87 = corps.startswith(LIGNE_DECL + '\n' + LIGNE_DECL_X87 + '\n')
        if x87 and est_x87:
            corps_m = masquer_chaines(corps)
            for mm in RX_X87_RESTE.finditer(corps_m):     # plus aucune forme x87 sur c
                ecarts.append(f"{chemin}:{nom}: forme x87 subsistante sur c : {mm.group(0)}")
            for mm in RX_RL_VIDE_TOUT.finditer(corps_m):  # chaque vidage rl suivi du vidage x87
                if not RX_X87_SUITE.match(corps_m, mm.end()):
                    ecarts.append(f"{chemin}:{nom}: SC2_RL_VIDE non suivi de son SC2_X87_VIDE")
            for mm in RX_RL_CHARGE_TOUT.finditer(corps_m):
                if not corps_m.startswith(' SC2_X87_CHARGE(c);', mm.end()):
                    ecarts.append(f"{chemin}:{nom}: SC2_RL_CHARGE non suivi de son SC2_X87_CHARGE")
        elif x87 and 'SC2_X87_' in masquer_chaines(corps):
            ecarts.append(f"{chemin}:{nom}: SC2_X87_* hors fonction transformée côté x87")
        lignes_f = corps.split('\n')
        places = {}
        for k, brut in enumerate(lignes_f):
            me = RX_ETIQUETTE.fullmatch(brut.strip())
            if me:
                places[me.group(1)] = k
        for k, brut in enumerate(lignes_f):              # (e) tête de boucle sans barrière
            for mg in RX_GOTO.finditer(masquer_chaines(couper_commentaire(brut)[0])):
                j = places.get(mg.group(1))
                if j is not None and j <= k and (j + 1 >= len(lignes_f) or lignes_f[j + 1] != LIGNE_BOUCLE):
                    ecarts.append(f"{chemin}:{nom}: tête de boucle {mg.group(1)} sans SC2_RL_BOUCLE (ligne {j + 1})")
        for num, brut in enumerate(lignes_f, 1):
            code = masquer_chaines(couper_commentaire(brut)[0])
            for mm in RX_C_CHAMP_CACHE.finditer(code):
                ecarts.append(f"{chemin}:{nom}: c->{mm.group(1)} en cache subsistant (ligne {num})")
            for mm in rx_aide_c.finditer(code):
                ecarts.append(f"{chemin}:{nom}: {mm.group(0)} : aide en cache appelée avec c (ligne {num})")
            for mm in rx_hors_rl.finditer(code):
                ecarts.append(f"{chemin}:{nom}: {mm.group(0)} : hors cache ou frontière avec rl (ligne {num})")
            for mm in RX_VIDEX_PAIRE.finditer(code):       # x : drapeaux seulement, disjoints de m
                m_x, x_x = int(mm.group(1), 16), int(mm.group(2), 16)
                if x_x & ~MASQUE_D6:
                    ecarts.append(f"{chemin}:{nom}: SC2_RL_VIDEX x=0x{x_x:04X} hors drapeaux (ligne {num})")
                if x_x & m_x:
                    ecarts.append(f"{chemin}:{nom}: SC2_RL_VIDEX x=0x{x_x:04X} et m=0x{m_x:04X} "
                                  f"ne sont pas disjoints (ligne {num})")
            for mm in rx_frontiere.finditer(code):          # (d) frontière ou return non emballé
                if mm.group(1) in hors or (mm.group(1) or '').startswith(('SC2_RL_', 'SC2_X87_')):
                    continue
                if not (RX_FIN_VIDE.search(code, 0, mm.start())
                        or RX_FIN_VIDE_X87.search(code, 0, mm.start())):
                    ecarts.append(f"{chemin}:{nom}: {mm.group(0).strip()} non emballé par SC2_RL_VIDE (ligne {num})")
            restant = rx_aide_rl.sub(lambda mm: mm.group(1) + '(RLNEUTRALISE', code)
            restant = RX_RL_CHAMP.sub('RLCHAMP', restant)
            for mm in RX_RL_SEUL.finditer(restant):
                ecarts.append(f"{chemin}:{nom}: rl passé ailleurs qu'à une aide en cache (ligne {num})")


def verifier_masques(entree, sortie, ctx, chemin, ecarts, viva=None):
    """(c) masques (et x de vivacité, même analyse globale ; masques x87 avec --x87) recalculés == écrits."""
    foncs_sortie = list(RX_FONC.finditer(sortie))
    for mi, mo in zip(RX_FONC.finditer(entree), foncs_sortie):
        attendus = []
        attendus_x = []
        try:
            analyse = analyser_corps(mi.group(2), ctx)
        except PasReconnu:
            pass
        else:
            if viva is not None:
                appliquer_vivacite(analyse, viva)
            attendus = [(s['masque'], s.get('x', 0)) for l in analyse.lignes for s in l.slots]
            attendus.append((analyse.masque_final, 0))
            if analyse.x87_ok:
                attendus_x = [s['masque_x'] for l in analyse.lignes for s in l.slots]
                attendus_x.append(analyse.masque_final_x)
        ecrits = [(int(m_, 16), int(x_, 16) if x_ else 0)
                  for m_, x_ in RX_VIDE_XY.findall(mo.group(2))]
        if attendus != ecrits:
            ecarts.append(f"{chemin}:{mi.group(1)} : masques écrits {ecrits} ≠ recalculés {attendus}")
        if ctx.x87:
            ecrits_x = [int(m_, 16) for m_ in RX_X87_VIDE.findall(mo.group(2))]
            if attendus_x != ecrits_x:
                ecarts.append(f"{chemin}:{mi.group(1)} : masques x87 écrits {ecrits_x} ≠ recalculés {attendus_x}")


def verifier(source, sortie, ctx, classes, viva=None):
    """Chaque fichier de <sortie>, passe retirée, doit redonner son entrée octet pour octet."""
    if not sortie.is_dir():
        erreur(f"dossier de sortie introuvable : {sortie}")
    ecarts, verifies = [], 0
    for chemin in sorted(p for p in sortie.rglob("*") if p.is_file()):
        relatif = chemin.relative_to(sortie)
        origine = source / relatif
        if not origine.is_file():
            ecarts.append(f"{relatif} : aucun fichier d'entrée correspondant")
        elif est_recomp(chemin):
            texte_sortie = lire_texte(chemin)
            texte_entree = lire_texte(origine)
            if retirer_passe(texte_sortie) != texte_entree:
                ecarts.append(f"{relatif} : ne redevient pas le fichier d'origine sans la passe")
            else:
                verifier_invariants(texte_sortie, classes, str(relatif), ecarts, ctx.x87)
                verifier_masques(texte_entree, texte_sortie, ctx, str(relatif), ecarts, viva)
            verifies += 1
        elif fichiers_identiques(chemin, origine):
            verifies += 1
        else:
            ecarts.append(f"{relatif} : recopié tel quel mais différent de l'entrée")
    if ecarts:
        for e in ecarts[:50]:
            print(f"écart : {e}", file=sys.stderr)
        if len(ecarts) > 50:
            print(f"… et {len(ecarts) - 50} autre(s) écart(s)", file=sys.stderr)
        print(f"échec : {len(ecarts)} écart(s) sur {verifies} fichier(s) vérifié(s)", file=sys.stderr)
        return 1
    print(f"vérification : {verifies} fichier(s) conformes (retrait de la passe = entrée, "
          f"octet pour octet ; invariants et masques recalculés"
          + (", vivacité des drapeaux comprise" if viva is not None else "")
          + (", pile x87 comprise" if ctx.x87 else "") + ")")
    return 0


def principal():
    ap = argparse.ArgumentParser(
        description="Registres et drapeaux invités en variables locales dans le C lifté (passe 10s).")
    ap.add_argument("source", help="dossier d'entrée (les recomp_*.c, les autres fichiers recopiés)")
    ap.add_argument("sortie", help="dossier de sortie (créé si besoin)")
    ap.add_argument("--cpu-h", metavar="CHEMIN", default=str(REPO / "recomp-sc2/runtime/cpu.h"),
                    help="cpu.h à classifier (défaut : recomp-sc2/runtime/cpu.h du dépôt)")
    ap.add_argument("--verifier", action="store_true",
                    help="contrôle que <sortie> redevient <source> sans la passe, invariants et masques")
    ap.add_argument("--vivacite-drapeaux", action="store_true",
                    help="aux seuls appels directs et d'import, ne vider que les drapeaux lus "
                         "(vivacité interprocédurale, tables de tools/lift/drapeaux-morts.py) ; "
                         "sans l'option, sortie identique octet pour octet")
    ap.add_argument("--dump-vivacite", metavar="FICHIER",
                    help="écrire E par fonction (L_XXXXXXXX 0xMASQUE, triées) — exige --vivacite-drapeaux")
    ap.add_argument("--x87", action="store_true",
                    help="pile x87 (st, fpu_top, fpu_sw) en variable locale relative au sommet, "
                         "synchronisée aux mêmes frontières (passe 10t) ; sans l'option, "
                         "sortie identique octet pour octet")
    args = ap.parse_args()
    if args.dump_vivacite and not args.vivacite_drapeaux:
        erreur("--dump-vivacite exige --vivacite-drapeaux")

    source, sortie = Path(args.source), Path(args.sortie)
    if not source.is_dir():
        erreur(f"entrée introuvable ou pas un dossier : {source}")
    cpu_h = Path(args.cpu_h)
    if not cpu_h.is_file():
        erreur(f"cpu.h introuvable : {cpu_h}")
    classes, ecrits, aides_x87 = analyser_cpu_h(lire_texte(cpu_h))
    fichiers = sorted(p for p in source.rglob("*") if p.is_file())
    if not any(est_recomp(p) for p in fichiers):
        erreur(f"aucun recomp_*.c dans {source}")

    viva = None
    ctx = Contexte(classes, ecrits, x87=args.x87, aides_x87=aides_x87)
    if args.vivacite_drapeaux:
        ctx.iat = lire_iat(source / "sc2_imports.h")
        viva = construire_vivacite(source, fichiers, ctx)

    if args.verifier:
        return verifier(source, sortie, ctx, classes, viva)

    sortie.mkdir(parents=True, exist_ok=True)
    stats = {"recomps": 0, "autres": 0, "ecrits": 0, "transformees": 0, "fonctions": 0,
             "frontieres": {}, "retours": 0, "boucles": 0, "masques": [], "motifs": {}, "exemples": {},
             "rl_sites": 0, "rl_omis": 0,
             "x87_transformees": 0, "x87_push": 0, "x87_pop": 0, "x87_st": 0, "x87_fcom": 0,
             "x87_fnstsw": 0, "x87_vidages": 0, "masques_x87": [], "motifs_x87": {}, "exemples_x87": {}}
    for chemin in fichiers:                          # fichier par fichier : jamais tout chargé
        destination = sortie / chemin.relative_to(source)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if est_recomp(chemin):
            stats["recomps"] += 1
            nouveau = transformer_texte(lire_texte(chemin), ctx, chemin.name, stats, viva)
            ecrit = ecrire_si_different(destination, nouveau)
        else:
            stats["autres"] += 1
            ecrit = copier_si_different(chemin, destination)
        if ecrit:
            stats["ecrits"] += 1

    genres = stats["frontieres"]
    total_frontieres = (genres.get('appels', 0) + genres.get('sauts', 0) + stats["retours"]
                        + genres.get('runtime', 0) + genres.get('inconnues', 0))
    moyenne = (sum(bin(m).count('1') for m in stats["masques"]) / len(stats["masques"])
               if stats["masques"] else 0.0)
    print(f"{stats['recomps']} fichier(s) recomp_*.c traité(s), {stats['autres']} autre(s) recopié(s), "
          f"{stats['ecrits']} réécrit(s)")
    print(f"fonctions transformées : {stats['transformees']} sur {stats['fonctions']}")
    detail_motifs = ', '.join(f"{k} : {v}" for k, v in
                              sorted(stats['motifs'].items(), key=lambda kv: -kv[1]))
    if detail_motifs:
        print(f"fonctions laissées intactes : {stats['fonctions'] - stats['transformees']} ({detail_motifs})")
    else:
        print(f"fonctions laissées intactes : {stats['fonctions'] - stats['transformees']}")
    print(f"frontières : {total_frontieres} (appels {genres.get('appels', 0)}, "
          f"sauts {genres.get('sauts', 0)}, retours {stats['retours']}, "
          f"runtime {genres.get('runtime', 0)}, inconnues {genres.get('inconnues', 0)}), "
          f"champs vidés en moyenne : {moyenne:.2f}")
    n_cache = sum(1 for cl in classes.values() if cl == 'cache')
    n_hors = sum(1 for cl in classes.values() if cl == 'hors')
    n_mixte = sum(1 for cl in classes.values() if cl == 'mixte')
    print(f"aides de cpu.h : {n_cache} en cache, {n_hors} hors cache, {n_mixte} mixtes")
    if args.x87:
        bits_x = (sum(bin(m).count('1') for m in stats['masques_x87']) / len(stats['masques_x87'])
                  if stats['masques_x87'] else 0.0)
        detail_x = ', '.join(f"{k} : {v}" for k, v in
                             sorted(stats['motifs_x87'].items(), key=lambda kv: -kv[1]))
        print(f"x87 : fonctions transformées côté x87 : {stats['x87_transformees']} sur "
              f"{stats['transformees']}, replis x87 : {sum(stats['motifs_x87'].values())}"
              + (f" ({detail_x})" if detail_x else "")
              + f", PUSH {stats['x87_push']}, POP {stats['x87_pop']}, ST {stats['x87_st']}, "
              f"FCOM {stats['x87_fcom']}, FNSTSW {stats['x87_fnstsw']}, vidages x87 : "
              f"{stats['x87_vidages']}, bits par masque x87 : {bits_x:.2f}")
        for motif, exemples in sorted(stats['exemples_x87'].items(), key=lambda kv: -stats['motifs_x87'][kv[0]]):
            for exemple in exemples:
                print(f"  {motif} : {exemple}")
    if viva is not None:
        e_vides = sum(1 for nom in viva.definis if viva.E_de(nom) == 0)
        sites, omis = stats['rl_sites'], stats['rl_omis']
        print(f"vivacité des drapeaux : E vide pour {e_vides} fonctions sur {len(viva.definis)}, "
              f"E_indirect = 0x{viva.e_indirect:04X}, drapeaux omis aux appels : {omis} "
              f"(moyenne {(omis / sites) if sites else 0.0:.2f} par appel direct ou d'import), "
              f"champs vidés en moyenne : {moyenne:.2f}")
        if args.dump_vivacite:
            with open(args.dump_vivacite, "w", encoding="utf-8") as f:
                for nom in sorted(viva.definis):
                    f.write("%s 0x%04X\n" % (nom, viva.E_de(nom)))
    print(f"têtes de boucle (SC2_RL_BOUCLE) : {stats['boucles']}")
    for motif, exemples in sorted(stats['exemples'].items(), key=lambda kv: -stats['motifs'][kv[0]]):
        for exemple in exemples:
            print(f"  {motif} : {exemple}")
    return 0


if __name__ == "__main__":
    sys.exit(principal())
