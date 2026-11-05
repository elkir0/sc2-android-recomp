#!/usr/bin/env python3
"""test_drapeaux_morts.py — tests unitaires de tools/lift/drapeaux-morts.py.

Extraits C écrits à la main, forme du lifter (une instruction par ligne, étiquettes
L_XXXXXXXX). Chaque test vérifie la vivacité des écritures d'un appel d'aide :
appels[i] = (aide, écrits, vivants).
"""
import importlib.util
import os
import sys
import unittest

ICI = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    'drapeaux_morts', os.path.join(ICI, 'drapeaux-morts.py'))
dm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dm)

SIX = frozenset(dm.DRAPEAUX)


def appels(src):
    """Analyse une source à une fonction ; rend sa liste d'appels mesurés."""
    foncs = dm.analyser_source(src)
    assert len(foncs) == 1, f'1 fonction attendue, {len(foncs)} trouvées'
    return foncs[0].appels


class VivaciteDrapeaux(unittest.TestCase):

    def test_deux_flags_sub_successifs(self):
        """Le premier flags_sub est entièrement mort : réécrit avant toute lecture."""
        a = appels("""
void L_00AA0000(CPU *c)
{
    flags_sub(c, c->eax, 1u, 4);
    flags_sub(c, c->eax, 2u, 4);
    return;
}
""")
        self.assertEqual([x[0] for x in a], ['flags_sub', 'flags_sub'])
        self.assertEqual(a[0][2], frozenset(), 'premier flags_sub entièrement mort')
        self.assertEqual(a[1][2], SIX, 'dernière écriture vivante à la sortie')

    def test_cmp_pour_jae_cf_seul_vivant(self):
        """cmp puis jae : CF vivant, les cinq autres morts (réécrits sur chaque chemin)."""
        a = appels("""
void L_00AA0000(CPU *c)
{
    flags_sub(c, c->edi, 0xFu, 4);                                 /* cmp edi, 0xf */
    if (!c->cf) goto L_00AA0020;                                   /* jae */
    c->eax = (uint32_t)(flags_add(c, c->eax, 1u, 4));
    return;
L_00AA0020:
    c->eax = (uint32_t)(flags_add(c, c->eax, 2u, 4));
    return;
}
""")
        self.assertEqual(a[0][2], frozenset(('cf',)), 'seul CF survit au cmp')
        for autre in a[1:]:
            self.assertEqual(autre[2], SIX)

    def test_boucle_goto_arriere(self):
        """zf lu en tête d'une boucle (goto arrière) : vivant, les autres morts."""
        a = appels("""
void L_00AA0000(CPU *c)
{
L_00AA0010:
    if (!c->zf) goto L_00AA0030;
    c->eax = (uint32_t)(flags_add(c, c->eax, 1u, 4));
    goto L_00AA0010;
L_00AA0030:
    c->ebx = (uint32_t)(flags_add(c, c->ebx, 1u, 4));
    return;
}
""")
        self.assertEqual(a[0][2], frozenset(('zf',)),
                         "zf de la boucle lu au tour suivant via le back-edge")

    def test_dispatch_entre_deux_ecritures(self):
        """Un appel dispatch lit tout : la première écriture reste vivante."""
        a = appels("""
void L_00AA0000(CPU *c)
{
    flags_sub(c, c->eax, 1u, 4);
    push32(c, 0x00AA0040u); dispatch(c, 0x00BB0000u);
    flags_sub(c, c->eax, 2u, 4);
    return;
}
""")
        self.assertEqual(a[0][2], SIX, 'conservateur : vivant avant un appel')

    def test_flags_adc_lit_cf(self):
        """flags_adc lit CF : l'écriture de CF qui la précède est vivante."""
        a = appels("""
void L_00AA0000(CPU *c)
{
    flags_sub(c, c->eax, 1u, 4);
    c->eax = (uint32_t)(flags_adc(c, c->eax, 0u, 4));
    c->ebx = (uint32_t)(flags_add(c, c->ebx, 1u, 4));
    return;
}
""")
        self.assertIn('cf', a[0][2], "CF du flags_sub lu par flags_adc")
        self.assertNotIn('af', a[0][2])
        self.assertEqual(a[1][2], frozenset(), 'écritures du flags_adc tuées avant la sortie')

    def test_ecriture_juste_avant_return(self):
        """Convention conservatrice : tout est vivant à la sortie."""
        a = appels("""
void L_00AA0000(CPU *c)
{
    c->eax = (uint32_t)(flags_logicz(c, c->eax & c->eax, 4));
    return;
}
""")
        self.assertEqual(a[0][2], frozenset(('cf', 'of', 'sf', 'zf', 'pf')))

    def test_op_shr_transparent(self):
        """op_shr n'écrit rien de sûr (compte 0 possible) : il ne tue pas zf."""
        a = appels("""
void L_00AA0000(CPU *c)
{
    flags_logicz(c, c->eax & c->eax, 4);
    c->edx = (uint32_t)(op_shr(c, c->eax, R8L(c->ecx), 4));
    if (!c->zf) goto L_00AA0020;
    c->ebx = (uint32_t)(flags_add(c, c->ebx, 1u, 4));
    return;
L_00AA0020:
    c->ebx = (uint32_t)(flags_add(c, c->ebx, 2u, 4));
    return;
}
""")
        self.assertEqual(a[0][2], frozenset(('zf',)), 'zf traverse op_shr (compte 0)')

    def test_setcc_lit_zf(self):
        """setcc : la lecture c->zf dans une expression rend l'écriture vivante."""
        a = appels("""
void L_00AA0000(CPU *c)
{
    flags_logicz(c, c->eax & c->eax, 4);
    SET8L(c->edx, ((c->zf) ? 1 : 0));
    c->ebx = (uint32_t)(flags_add(c, c->ebx, 1u, 4));
    return;
}
""")
        self.assertEqual(a[0][2], frozenset(('zf',)))

    def test_saut_hors_fonction_sort_avec_drapeaux(self):
        """SC2_SAUT sort de la fonction : les drapeaux y sont vivants."""
        a = appels("""
void L_00AA0000(CPU *c)
{
    if (c->cf) { SC2_SAUT(c, 0x00BB0000u); return; }               /* jb */
    flags_logicz(c, c->eax & c->eax, 4);
    SC2_SAUT(c, 0x00BB0010u); return;
}
""")
        self.assertEqual(a[0][2], frozenset(('cf', 'of', 'sf', 'zf', 'pf')),
                         'vivants à la traversée du saut conditionnel')

    def test_non_reconnu_lit_tout(self):
        """Un identifiant inconnu lit les six et est compté à part."""
        foncs = dm.analyser_source("""
void L_00AA0000(CPU *c)
{
    flags_sub(c, c->eax, 1u, 4);
    aide_exotique(c, 3u);
    c->ebx = (uint32_t)(flags_add(c, c->ebx, 1u, 4));
    return;
}
""")
        f = foncs[0]
        self.assertEqual(len(f.non_reconnues), 1)
        self.assertEqual(f.appels[0][2], SIX, 'le non reconnu lit les six')

    def test_vivants_zf_sf_seuls(self):
        """zf et sf lus séparément, le reste réécrit : catégorie {zf, sf}."""
        src = """
void L_00AA0000(CPU *c)
{
    flags_logicz(c, c->eax & c->eax, 4);
    if (!c->zf) goto L_00AA0020;
    if (!c->sf) goto L_00AA0020;
    c->ebx = (uint32_t)(flags_add(c, c->ebx, 1u, 4));
    return;
L_00AA0020:
    c->ebx = (uint32_t)(flags_add(c, c->ebx, 2u, 4));
    return;
}
"""
        a = appels(src)
        self.assertEqual(a[0][2], frozenset(('zf', 'sf')))
        r = dm.agreger(dm.analyser_source(src))
        self.assertEqual(r['appels']['zf_sf_seuls'], 1)
        self.assertEqual(r['appels']['cf_seul'], 0)

    def test_agregation_par_aide(self):
        src = """
void L_00AA0000(CPU *c)
{
    flags_sub(c, c->eax, 1u, 4);
    flags_sub(c, c->eax, 2u, 4);
    return;
}
"""
        r = dm.agreger(dm.analyser_source(src))
        self.assertEqual(r['appels']['total'], 2)
        self.assertEqual(r['appels']['entierement_morts'], 1)
        self.assertEqual(r['par_aide']['flags_sub']['appels'], 2)
        self.assertEqual(r['ecritures']['cf'], {'total': 2, 'mortes': 1})


# ---- réimplémentations Python des aides de runtime/cpu.h (retour seulement) ----

def _mask(sz):
    return (1 << (8 * sz)) - 1

def cpu_flags_sub(a, b, sz):
    m = _mask(sz); a &= m; b &= m; return (a - b) & m

def cpu_flags_add(a, b, sz):
    m = _mask(sz); a &= m; b &= m; return (a + b) & m

def cpu_flags_logicz(r, sz):
    return r & _mask(sz)

def cpu_flags_incs(a, sz):
    return cpu_flags_add(a, 1, sz)

def cpu_flags_decs(a, sz):
    return cpu_flags_sub(a, 1, sz)

def cpu_flags_adc(a, b, cf, sz):
    m = _mask(sz); return ((a & m) + (b & m) + (cf & 1)) & m

def cpu_flags_sbb(a, b, cf, sz):
    m = _mask(sz); return ((a & m) - (b & m) - (cf & 1)) & m

# sémantique des formules C générées : arithmétique uint32 puis masque de taille
def formule(op, a, b=0, cf=0, sz=4):
    u = lambda x: x & 0xFFFFFFFF
    m = _mask(sz)
    if op == 'sub':   return (u(a) - u(b)) & m
    if op == 'add':   return (u(a) + u(b)) & m
    if op == 'logicz': return u(a) & m
    if op == 'incs':  return (u(a) + 1) & m
    if op == 'decs':  return (u(a) - 1) & m
    if op == 'adc':   return (u(a) + u(b) + u(cf)) & m
    if op == 'sbb':   return (u(a) - u(b) - u(cf)) & m
    raise ValueError(op)


class FormulesRewrite(unittest.TestCase):
    """Les formules C rendent exactement la valeur de retour de l'aide de cpu.h."""

    VALEURS = (0, 1, 2, 0x7E, 0x7F, 0x80, 0xFE, 0xFF, 0x100, 0x7FFF, 0x8000,
               0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF,
               0x12345678, 0xDEADBEEF)

    def test_sub_add_logicz_valeurs_limites(self):
        for sz in (1, 2, 4):
            for a in self.VALEURS:
                for b in self.VALEURS:
                    self.assertEqual(cpu_flags_sub(a, b, sz), formule('sub', a, b, sz=sz),
                                     f'sub {a:#x} {b:#x} sz={sz}')
                    self.assertEqual(cpu_flags_add(a, b, sz), formule('add', a, b, sz=sz),
                                     f'add {a:#x} {b:#x} sz={sz}')
                self.assertEqual(cpu_flags_logicz(a, sz), formule('logicz', a, sz=sz),
                                 f'logicz {a:#x} sz={sz}')

    def test_incs_decs_valeurs_limites(self):
        for sz in (1, 2, 4):
            for a in self.VALEURS:
                self.assertEqual(cpu_flags_incs(a, sz), formule('incs', a, sz=sz),
                                 f'incs {a:#x} sz={sz}')
                self.assertEqual(cpu_flags_decs(a, sz), formule('decs', a, sz=sz),
                                 f'decs {a:#x} sz={sz}')

    def test_adc_sbb_valeurs_limites(self):
        for sz in (1, 2, 4):
            for cf in (0, 1):
                for a in self.VALEURS[:8] + (0xFFFFFFFF, 0x80000000):
                    for b in self.VALEURS[:8] + (1, 0xFFFFFFFF):
                        self.assertEqual(cpu_flags_adc(a, b, cf, sz), formule('adc', a, b, cf, sz),
                                         f'adc {a:#x} {b:#x} cf={cf} sz={sz}')
                        self.assertEqual(cpu_flags_sbb(a, b, cf, sz), formule('sbb', a, b, cf, sz),
                                         f'sbb {a:#x} {b:#x} cf={cf} sz={sz}')

    def test_formule_sz_non_litteral(self):
        self.assertIsNone(dm._formule('flags_sub', ['c', 'a', 'b', 'sz']))
        self.assertIsNone(dm._formule('flags_sub', ['c', 'a', 'b', '3']))
        self.assertIsNone(dm._formule('flags_sub', ['c', 'a']))


class Reecriture(unittest.TestCase):
    """recrire_source : texte produit pour les appels entièrement morts."""

    def recrire(self, src):
        return dm.recrire_source(src)[0]

    def test_expression_remplacee(self):
        src = """
void L_00AA0000(CPU *c)
{
    c->eax = (uint32_t)(flags_add(c, c->eax, c->eax, 4));
    c->ebx = (uint32_t)(flags_sub(c, c->ebx, 1u, 4));
    return;
}
"""
        out = self.recrire(src)
        self.assertIn('c->eax = (uint32_t)((((uint32_t)(c->eax) + (uint32_t)(c->eax)) & 0xFFFFFFFFu));', out)
        self.assertIn('flags_sub(c, c->ebx, 1u, 4)', out)   # vivant à la sortie : intact

    def test_tailles_1_et_2(self):
        src = """
void L_00AA0000(CPU *c)
{
    c->eax = (uint32_t)(flags_logicz(c, rd8((c->esi)) & 0x1u, 1));
    c->ebx = (uint32_t)(flags_sub(c, c->ebx, c->ecx, 2));
    c->edi = (uint32_t)(flags_add(c, c->edi, 1u, 4));
    return;
}
"""
        out = self.recrire(src)
        self.assertIn('(((uint32_t)(rd8((c->esi)) & 0x1u)) & 0xFFu)', out)
        self.assertIn('(((uint32_t)(c->ebx) - (uint32_t)(c->ecx)) & 0xFFFFu)', out)
        self.assertIn('flags_add(c, c->edi, 1u, 4)', out)       # vivant à la sortie : intact

    def test_cmp_mort_supprime(self):
        src = """
void L_00AA0000(CPU *c)
{
    flags_sub(c, c->edi, 0xFu, 4);                               /* 009E42B0: cmp edi, 0xf */
    flags_sub(c, c->edi, 0x10u, 4);                              /* 009E42B1: cmp edi, 0x10 */
    return;
}
"""
        nouveau, stats = dm.recrire_source(src)
        self.assertNotIn('flags_sub(c, c->edi, 0xFu, 4);', nouveau)
        self.assertIn('/* drapeaux morts */', nouveau)
        self.assertIn('/* 009E42B0: cmp edi, 0xf */', nouveau)   # commentaire d'origine gardé
        self.assertEqual(stats['instructions'], 1)

    def test_argument_avec_appel_garde(self):
        src = """
void L_00AA0000(CPU *c)
{
    flags_sub(c, rd32((c->ebp + 0x00000008u)), 0xFu, 4);        /* cmp [ebp+8], 0xf */
    flags_sub(c, c->edi, 0x10u, 4);                             /* cmp edi, 0x10 */
    return;
}
"""
        nouveau, stats = dm.recrire_source(src)
        self.assertIn('(void)(rd32((c->ebp + 0x00000008u)));', nouveau)
        self.assertIn('/* drapeaux morts */', nouveau)
        self.assertEqual(stats['evaluations'], 1)
        self.assertEqual(stats['instructions'], 1)

    def test_aucune_reecriture_si_drapeau_vivant(self):
        src = """
void L_00AA0000(CPU *c)
{
    flags_sub(c, c->edi, 0xFu, 4);                               /* cmp edi, 0xf */
    if (!c->cf) goto L_00AA0020;                                 /* jae */
    c->eax = (uint32_t)(flags_add(c, c->eax, 1u, 4));
    return;
L_00AA0020:
    c->eax = (uint32_t)(flags_add(c, c->eax, 2u, 4));
    return;
}
"""
        nouveau, stats = dm.recrire_source(src)
        self.assertEqual(nouveau, src)
        self.assertEqual(sum(stats['appels'].values()), 0)

    def test_adc_garde_la_lecture_de_cf(self):
        src = """
void L_00AA0000(CPU *c)
{
    c->eax = (uint32_t)(flags_adc(c, c->eax, 0u, 2));
    c->ebx = (uint32_t)(flags_sub(c, c->ebx, 1u, 4));
    return;
}
"""
        out = self.recrire(src)
        self.assertIn('(((uint32_t)(c->eax) + (uint32_t)(0u) + (c->cf & 1u)) & 0xFFFFu)', out)

    def test_sc2_appel_traite_comme_appel(self):
        """SC2_APPEL lit les six : l'écriture qui le précède reste vivante, intacte."""
        src = """
void L_00AA0000(CPU *c)
{
    flags_sub(c, c->eax, 1u, 4);
    SC2_APPEL(c, L_00BB0000, 0x00AA0020u);
    c->eax = (uint32_t)(flags_sub(c, c->eax, 2u, 4));
    return;
}
"""
        nouveau, stats = dm.recrire_source(src)
        self.assertIn('flags_sub(c, c->eax, 1u, 4);', nouveau)
        self.assertEqual(sum(stats['appels'].values()), 0)


if __name__ == '__main__':
    unittest.main()
