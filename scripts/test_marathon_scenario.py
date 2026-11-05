#!/usr/bin/env python3
"""Tests unittest de scripts/marathon-scenario.py — plans et briques synthétiques, jamais de run du jeu.

Le module est chargé par chemin (nom à tirets, non importable), comme test_progression_oracle.py. Les
briques sont écrites sous un dossier temporaire (le générateur accepte un dossier de briques paramètre) :
les comptes attendus (23 événements de bar, 21 de chargement, 89 de triche dont un TEXTE à un événement
par caractère, 4 de victoire, 12 segments pour marsara.plan) supposent les briques réelles du dossier
scripts/entrees/marathon/, que les tests relisent par ailleurs."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "marathon-scenario.py"
spec = importlib.util.spec_from_file_location("marathon_scenario", SCRIPT)
marathon_scenario = importlib.util.module_from_spec(spec)
spec.loader.exec_module(marathon_scenario)

BRIQUES_REELLES = Path(__file__).resolve().parent / "entrees" / "marathon"

# Deux missions depuis le bar de Mar Sara — le plan de référence.
PLAN_MARSARA = """# marathon de référence
hub marsara
mission TRaynor02

hub marsara
mission TRaynor03
"""


class BriquesTmp:
    """Dossier de briques synthétique : bar-test.txt (2 événements), chargement/triche/victoire réduites."""

    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dossier = Path(self.tmp.name)
        (self.dossier / "bar-test.txt").write_text("+1:ESC\n+2:CLIC:1,2@ui\n")
        # hub à planète : le marqueur {planete} reçoit la coordonnée du plan
        (self.dossier / "bar-planete.txt").write_text("+1:ESC\n+5:CLIC:{planete}@ui\n")
        # triche réduite : 2 clics + Entrée + TEXTE « abc » (3 événements) + Entrée + garde = 8 événements
        (self.dossier / "triche.txt").write_text("+20:CLIC:953,588@ui\n+25:CLIC:953,588@ui\n"
                                                 "+40:KEY:0x0D\n+40.5:TEXTE:abc\n+47:KEY:0x0D\n+260:QUIT\n")
        (self.dossier / "chargement.txt").write_text("+20:KEY:0x20\n+25:KEY:0x20\n")
        (self.dossier / "victoire.txt").write_text("+3:CAPTURE:victoire-{n}\n+6:CLIC:998,674@ui\n")

    def close(self):
        self.tmp.cleanup()


class TestsGeneration(unittest.TestCase):

    def test_plan_deux_missions_segments_et_evenements(self):
        lignes, stats = marathon_scenario.generer(marathon_scenario.lire_plan(PLAN_MARSARA), BRIQUES_REELLES)
        self.assertEqual(stats["missions"], 2)
        self.assertEqual(stats["segments"], 12)      # bar + (hub + 4) par mission + arrêt final… voir generer
        # 12 segments : CARTE bar + [PRET hub + CARTE mission + PRET triche + VICTOIRE + CARTE retour] × 2 + PRET final
        self.assertEqual(stats["evenements"], 2 * 23 + 2 * (21 + 89 + 4 + 21) + 1)   # 2 hub + 2 missions + QUIT
        self.assertEqual(lignes[1], "REF:1280x720")
        self.assertTrue(lignes[2].startswith("GARDE:960"))   # 420 s × 2 missions + 120 s de marge
        self.assertEqual(sum(1 for l in lignes if l.startswith("QUAND:")), 12)

    def test_ordre_des_declencheurs(self):
        lignes, _ = marathon_scenario.generer(marathon_scenario.lire_plan(PLAN_MARSARA), BRIQUES_REELLES)
        quands = [l for l in lignes if l.startswith("QUAND:")]
        self.assertEqual(quands[:2], ["QUAND:CARTE:TStory01 MAX:60 SINON:QUIT", "QUAND:PRET MAX:90 SINON:QUIT"])
        self.assertEqual(quands[2:7], ["QUAND:CARTE:TRaynor02 MAX:150 SINON:QUIT", "QUAND:PRET MAX:150 SINON:QUIT",
                                       "QUAND:VICTOIRE", "QUAND:CARTE:TStory01 MAX:150 SINON:QUIT",
                                       "QUAND:PRET MAX:90 SINON:QUIT"])
        self.assertEqual(quands[7:12], ["QUAND:CARTE:TRaynor03 MAX:150 SINON:QUIT", "QUAND:PRET MAX:150 SINON:QUIT",
                                        "QUAND:VICTOIRE", "QUAND:CARTE:TStory01 MAX:150 SINON:QUIT",
                                        "QUAND:PRET MAX:90 SINON:QUIT"])
        self.assertEqual(quands[-1], "QUAND:PRET MAX:90 SINON:QUIT")
        self.assertIn("+30:QUIT", lignes)            # arrêt volontaire au bar après la dernière mission

    def test_marqueur_n_remplace(self):
        lignes, _ = marathon_scenario.generer(marathon_scenario.lire_plan(PLAN_MARSARA), BRIQUES_REELLES)
        self.assertIn("+3:CAPTURE:victoire-1", lignes)
        self.assertIn("+3:CAPTURE:victoire-2", lignes)
        self.assertNotIn("{n}", "\n".join(lignes))

    def test_hubs_consecutifs_refuses(self):
        # deux PRET de suite : le second n'aurait pas de nouveau cycle de chargement à attendre (blocage)
        with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
            marathon_scenario.generer(marathon_scenario.lire_plan("hub test\nhub test\nmission M01\n"), Path("/tmp"))
        self.assertIn("consécutifs", str(ctx.exception))


class TestsHubPlanete(unittest.TestCase):
    """« hub <recette> <x,y> » : le marqueur {planete} de la brique reçoit la coordonnée du plan."""

    def setUp(self):
        self.b = BriquesTmp()

    def tearDown(self):
        self.b.close()

    def test_coordonnee_remplace_le_marqueur(self):
        lignes, _ = marathon_scenario.generer(
            marathon_scenario.lire_plan("hub planete 573,496\nmission M01\n"), self.b.dossier)
        self.assertIn("+5:CLIC:573,496@ui", lignes)
        self.assertNotIn("{planete}", "\n".join(lignes))

    def test_marqueur_sans_coordonnee_refuse(self):
        # clic sur « {planete} » littéral : refusé (le plan doit donner la coordonnée)
        with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
            marathon_scenario.generer(marathon_scenario.lire_plan("hub planete\nmission M01\n"), self.b.dossier)
        self.assertIn("{planete}", str(ctx.exception))
        self.assertIn("coordonnée", str(ctx.exception))

    def test_coordonnee_sans_marqueur_refusee(self):
        # la brique de Mar Sara n'a pas de {planete} : la coordonnée n'atterrirait nulle part
        with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
            marathon_scenario.generer(marathon_scenario.lire_plan("hub test 573,496\nmission M01\n"), self.b.dossier)
        self.assertIn("n'a pas", str(ctx.exception))
        self.assertIn("{planete}", str(ctx.exception))

    def test_coordonnee_mal_ecrite_refusee(self):
        for plan in ("hub planete 573\nmission M01\n", "hub planete 573,496,1\nmission M01\n",
                     "hub planete x,y\nmission M01\n"):
            with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
                marathon_scenario.lire_plan(plan)
            self.assertIn("directive inconnue", str(ctx.exception), plan)


class TestsMissionEtoile(unittest.TestCase):
    """« mission * » : carte inconnue — la mission est repérée à son écran de chargement (CHARGEMENT)."""

    def setUp(self):
        self.b = BriquesTmp()

    def tearDown(self):
        self.b.close()

    def test_segment_chargement_au_lieu_de_carte(self):
        lignes, stats = marathon_scenario.generer(
            marathon_scenario.lire_plan("hub test\nmission *\n"), self.b.dossier)
        self.assertEqual(stats["missions"], 1)
        self.assertNotIn("QUAND:CARTE:*", lignes)
        self.assertIn("QUAND:CHARGEMENT MAX:200 SINON:QUIT", lignes)   # 200 s : la cinématique d'avant-mission
        # la mission reste nommée « * » dans l'en-tête
        self.assertTrue(lignes[0].startswith("# marathon — 1 mission(s) (*), 1 hub(s)"), lignes[0])

    def test_etoile_melangee_aux_cartes_connues(self):
        lignes, _ = marathon_scenario.generer(
            marathon_scenario.lire_plan("hub test\nmission M01\nmission *\nmission M02\n"), self.b.dossier)
        quands = [l for l in lignes if l.startswith("QUAND:")]
        self.assertEqual(quands[2], "QUAND:CARTE:M01 MAX:150 SINON:QUIT")
        self.assertEqual(quands[6], "QUAND:CHARGEMENT MAX:200 SINON:QUIT")   # mission 2, inconnue
        self.assertEqual(quands[10], "QUAND:CARTE:M02 MAX:150 SINON:QUIT")   # mission 3, connue
        self.assertIn("+3:CAPTURE:victoire-2", lignes)                      # {n} suit l'ordre du plan


class TestsRefus(unittest.TestCase):

    def test_limite_evenements_refusee(self):
        # briques à TEXTE long (150 caractères = 150 événements) : 8 missions = 1 291 événements en 34 segments
        # — la limite d'événements tombe AVANT celle de segments.
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            (d / "bar-test.txt").write_text("+1:ESC\n")
            (d / "triche.txt").write_text(f"+40:KEY:0x0D\n+40.5:TEXTE:{'a' * 150}\n+47:KEY:0x0D\n+260:QUIT\n")
            (d / "chargement.txt").write_text("+20:KEY:0x20\n")
            (d / "victoire.txt").write_text("+3:CAPTURE:victoire-{n}\n")
            plan = "hub test\n" + "".join(f"mission M{i:02d}\n" for i in range(8))
            with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
                marathon_scenario.generer(marathon_scenario.lire_plan(plan), d)
            self.assertIn("événements", str(ctx.exception))

    def test_limite_segments_refusee(self):
        b = BriquesTmp()                              # mission = 14 événements, 4 segments : 16 missions = 66 segments
        plan = "hub test\n" + "".join(f"mission M{i:02d}\n" for i in range(16))
        with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
            marathon_scenario.generer(marathon_scenario.lire_plan(plan), b.dossier)
        self.assertIn("segments", str(ctx.exception))
        b.close()

    def test_recette_inconnue_refusee(self):
        # « hyperion » existe depuis le 24/10 : la recette inconnue est testée sur un nom sans brique,
        # et « hub hyperion » SANS coordonnée est refusé aussi (la brique vise {planete})
        with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
            marathon_scenario.generer(marathon_scenario.lire_plan("hub inconnue\nmission TRaynor02\n"), BRIQUES_REELLES)
        self.assertIn("recette inconnue", str(ctx.exception))
        self.assertIn("bar-inconnue.txt", str(ctx.exception))
        with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
            marathon_scenario.generer(marathon_scenario.lire_plan("hub hyperion\nmission TRaynor02\n"), BRIQUES_REELLES)
        self.assertIn("{planete}", str(ctx.exception))

    def test_mission_avant_hub_refusee(self):
        with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
            marathon_scenario.generer(marathon_scenario.lire_plan("mission TRaynor02\nhub marsara\n"), BRIQUES_REELLES)
        self.assertIn("hub", str(ctx.exception))

    def test_plan_vide_ou_sans_mission_refuse(self):
        for plan in ("", "# rien\n", "hub marsara\n"):
            with self.assertRaises(marathon_scenario.ErreurPlan):
                marathon_scenario.generer(marathon_scenario.lire_plan(plan), BRIQUES_REELLES)

    def test_directive_invalide_refusee(self):
        with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
            marathon_scenario.lire_plan("hub\n")      # argument manquant
        self.assertIn("directive inconnue", str(ctx.exception))
        with self.assertRaises(marathon_scenario.ErreurPlan) as ctx:
            marathon_scenario.lire_plan("hub ../chemin\n")   # séparateur de chemin refusé
        self.assertIn("nom invalide", str(ctx.exception))

    def test_main_refus_code_2(self):
        with tempfile.TemporaryDirectory() as t:
            plan = Path(t) / "mauvais.plan"
            plan.write_text("mission TRaynor02\n")
            self.assertEqual(marathon_scenario.main([str(plan)]), 2)


class TestsBriquesReelles(unittest.TestCase):
    """Les briques livrées : comptes tenus par le banc (TEXTE = un événement par caractère)."""

    def test_comptes_des_briques(self):
        for nom, attendu in (("bar-marsara.txt", 23), ("chargement.txt", 21), ("triche.txt", 89), ("victoire.txt", 4)):
            elements = marathon_scenario.charger_brique(BRIQUES_REELLES, nom)
            self.assertEqual(marathon_scenario.compter_evenements(elements), attendu, nom)
            self.assertTrue(all(e.startswith("+") for e in elements), nom)

    def test_aucun_point_virgule_dans_les_commentaires(self):
        for f in BRIQUES_REELLES.glob("*.txt"):       # le point-virgule coupe un élément, même en commentaire
            for ligne in f.read_text().splitlines():
                l = ligne.strip()
                if l.startswith("#"):
                    self.assertNotIn(";", l, f"{f.name} : point-virgule dans un commentaire")

    def test_plan_de_reference_genere(self):
        lignes, stats = marathon_scenario.generer(
            marathon_scenario.lire_plan((BRIQUES_REELLES / "marsara.plan").read_text()), BRIQUES_REELLES)
        self.assertEqual((stats["missions"], stats["segments"]), (2, 12))
        self.assertTrue(all(not l.startswith("+") or "{" not in l for l in lignes))


if __name__ == "__main__":
    unittest.main()
