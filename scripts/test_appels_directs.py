#!/usr/bin/env python3
"""Tests unittest de scripts/appels-directs.py — petits dossiers temporaires, jamais les échantillons du jeu."""
import shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "appels-directs.py"

LISTE = (
    "/* liste de test */\n"
    "#define LIFTED_FUNCS(X) \\\n"
    "    X(00A00000) \\\n"
    "    X(00A00010) \\\n"
    "    X(00A00020) \\\n"
    "    X(00A00030) \\\n"
    "    X(00A00040) \\\n"
    "    X(00A00050)\n"
)

# recomp_00 : cible liftée définie ailleurs (00A00010), minuscule + return sur la même ligne
# (00a00020), auto-appel définie ici (00A00000), exclues (00A00040 avec 0x, 00A00050 sans),
# non liftée (00BEEF00), calculée, SC2_SAUT et dispatch_jmp intacts.
ENTREE_00 = """\
/* AUTO-GENERATED */
#include "cpu.h"
#include "sc2_lifted.h"

void L_00A00000(CPU *c)
{
    push32(c, 0x00A00060u); dispatch(c, 0x00A00010u);            /* 00A00004: call 0xa0010 */
    dispatch(c, 0x00a00020u); return;                            /* 00A00008: call 0xa0020 */
    dispatch(c, 0x00A00000u);                                    /* 00A0000C: call 0xa0000 */
    dispatch(c, 0x00A00040u);                                    /* exclue */
    dispatch(c, 0x00A00050u);                                    /* exclue (sans 0x dans exclus.txt) */
    dispatch(c, 0x00BEEF00u);                                    /* non liftée */
    dispatch(c, _ct);                                            /* calculée */
    if (c->cf) { SC2_SAUT(c, 0x00A00010u); return; }             /* saut : intact */
    dispatch_jmp(c, 0x00A00010u);                                /* saut : intact */
}
"""

# recomp_01 : appels vers une cible externe (00A00000) et des cibles définies sur place.
ENTREE_01 = """\
/* AUTO-GENERATED */
#include "cpu.h"
#include "sc2_lifted.h"

void L_00A00010(CPU *c)
{
    dispatch(c, 0x00A00000u);
}

void L_00A00020(CPU *c)
{
    if (c->zf) { dispatch(c, 0x00A00020u); }
}

void L_00A00030(CPU *c)
{
    dispatch(c, 0x00A00030u);
}
"""

ATTENDU_00 = """\
/* AUTO-GENERATED */
#include "cpu.h"
#include "sc2_lifted.h"
/* appels directs (scripts/appels-directs.py) */
void L_00A00000(CPU *c);
void L_00A00010(CPU *c);
void L_00A00020(CPU *c);

void L_00A00000(CPU *c)
{
    push32(c, 0x00A00060u); SC2_APPEL(c, L_00A00010, 0x00A00010u);            /* 00A00004: call 0xa0010 */
    SC2_APPEL(c, L_00A00020, 0x00a00020u); return;                            /* 00A00008: call 0xa0020 */
    SC2_APPEL(c, L_00A00000, 0x00A00000u);                                    /* 00A0000C: call 0xa0000 */
    dispatch(c, 0x00A00040u);                                    /* exclue */
    dispatch(c, 0x00A00050u);                                    /* exclue (sans 0x dans exclus.txt) */
    dispatch(c, 0x00BEEF00u);                                    /* non liftée */
    dispatch(c, _ct);                                            /* calculée */
    if (c->cf) { SC2_SAUT(c, 0x00A00010u); return; }             /* saut : intact */
    dispatch_jmp(c, 0x00A00010u);                                /* saut : intact */
}
"""

ATTENDU_01 = """\
/* AUTO-GENERATED */
#include "cpu.h"
#include "sc2_lifted.h"
/* appels directs (scripts/appels-directs.py) */
void L_00A00000(CPU *c);
void L_00A00020(CPU *c);
void L_00A00030(CPU *c);

void L_00A00010(CPU *c)
{
    SC2_APPEL(c, L_00A00000, 0x00A00000u);
}

void L_00A00020(CPU *c)
{
    if (c->zf) { SC2_APPEL(c, L_00A00020, 0x00A00020u); }
}

void L_00A00030(CPU *c)
{
    SC2_APPEL(c, L_00A00030, 0x00A00030u);
}
"""

EXCLUS = "# exclusions de test\n0x00A00040   # avec 0x et commentaire\n00A00050\n"


class TestAppelsDirects(unittest.TestCase):
    def setUp(self):
        self.racine = Path(tempfile.mkdtemp(prefix="appels-directs-test-"))
        self.addCleanup(shutil.rmtree, self.racine, True)
        self.entree = self.racine / "entree"
        self.entree.mkdir()
        (self.entree / "recomp_sc2_list.h").write_text(LISTE, encoding="utf-8")
        (self.entree / "recomp_00.c").write_text(ENTREE_00, encoding="utf-8")
        (self.entree / "recomp_01.c").write_text(ENTREE_01, encoding="utf-8")
        (self.entree / "README.md").write_text("notes de test : é\n", encoding="utf-8")
        self.exclus = self.racine / "exclus.txt"
        self.exclus.write_text(EXCLUS, encoding="utf-8")
        self.sortie = self.racine / "sortie"

    def lancer(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                              capture_output=True, text=True)

    def passage(self, sortie=None):
        resultat = self.lancer(self.entree, sortie or self.sortie, "--exclure", self.exclus)
        self.assertEqual(resultat.returncode, 0, resultat.stderr)
        return resultat

    def test_transformation_complete(self):
        """Cibles liftées transformées, prototype pour chaque cible référencée (même définie sur place), le reste intact."""
        resultat = self.passage()
        self.assertEqual((self.sortie / "recomp_00.c").read_text(encoding="utf-8"), ATTENDU_00)
        self.assertEqual((self.sortie / "recomp_01.c").read_text(encoding="utf-8"), ATTENDU_01)
        self.assertRegex(resultat.stdout, r"sites transformés\s*:\s*6\b")
        self.assertRegex(resultat.stdout, r"sites constants non liftés laissés\s*:\s*1\b")
        self.assertRegex(resultat.stdout, r"sites exclus\s*:\s*2\b")
        self.assertRegex(resultat.stdout, r"appels calculés laissés\s*:\s*1\b")

    def test_fichier_non_recomp_recopie(self):
        self.passage()
        self.assertEqual((self.sortie / "README.md").read_bytes(),
                         (self.entree / "README.md").read_bytes())

    def test_idempotence_sans_reecriture(self):
        """Relancer sur la sortie ne change rien et ne réécrit aucun fichier."""
        self.passage()
        avant = {p.name: p.stat().st_mtime_ns for p in self.sortie.iterdir()}
        contenus = {p.name: p.read_bytes() for p in self.sortie.iterdir()}
        resultat = self.passage()
        self.assertEqual({p.name: p.read_bytes() for p in self.sortie.iterdir()}, contenus)
        self.assertEqual({p.name: p.stat().st_mtime_ns for p in self.sortie.iterdir()}, avant)
        self.assertRegex(resultat.stdout, r"0 réécrit")

    def test_determinisme(self):
        """Deux exécutions indépendantes donnent des sorties identiques octet pour octet."""
        s1, s2 = self.racine / "s1", self.racine / "s2"
        self.passage(s1)
        self.passage(s2)
        self.assertEqual({p.name for p in s1.iterdir()}, {p.name for p in s2.iterdir()})
        for p in s1.iterdir():
            self.assertEqual(p.read_bytes(), (s2 / p.name).read_bytes())

    def test_verifier_conforme(self):
        self.passage()
        resultat = self.lancer(self.entree, self.sortie, "--exclure", self.exclus, "--verifier")
        self.assertEqual(resultat.returncode, 0, resultat.stderr)
        self.assertIn("conformes", resultat.stdout)

    def test_verifier_refuse_appel_altere(self):
        self.passage()
        cible = self.sortie / "recomp_00.c"
        cible.write_text(cible.read_text(encoding="utf-8").replace(
            "SC2_APPEL(c, L_00A00010, 0x00A00010u)",
            "SC2_APPEL(c, L_00A00010, 0x00A00099u)"), encoding="utf-8")
        resultat = self.lancer(self.entree, self.sortie, "--exclure", self.exclus, "--verifier")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("écart", resultat.stderr)

    def test_verifier_refuse_recopie_alteree(self):
        self.passage()
        with open(self.sortie / "README.md", "a", encoding="utf-8") as f:
            f.write("altéré\n")
        resultat = self.lancer(self.entree, self.sortie, "--exclure", self.exclus, "--verifier")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("écart", resultat.stderr)

    def test_entree_sans_liste(self):
        entree = self.racine / "sans-liste"
        entree.mkdir()
        shutil.copy(self.entree / "recomp_00.c", entree)
        resultat = self.lancer(entree, self.racine / "s")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("recomp_sc2_list.h", resultat.stderr)

    def test_entree_sans_recomp(self):
        entree = self.racine / "sans-recomp"
        entree.mkdir()
        shutil.copy(self.entree / "recomp_sc2_list.h", entree)
        resultat = self.lancer(entree, self.racine / "s")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("recomp_", resultat.stderr)

    def test_exclusion_mal_formee(self):
        mauvais = self.racine / "mauvais.txt"
        mauvais.write_text("0x12345\n", encoding="utf-8")
        resultat = self.lancer(self.entree, self.racine / "s", "--exclure", mauvais)
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("adresse invalide", resultat.stderr)


if __name__ == "__main__":
    unittest.main()
