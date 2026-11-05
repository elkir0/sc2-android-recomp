#!/usr/bin/env python3
"""Tests unittest de scripts/progression-oracle.py — banques et StoryModeDebug synthétiques, jamais de fichiers réels.

Les fixtures reprennent le format exact des .SC2Bank (racine <Bank>, <Section name>, <Key name> à valeurs
Value/StateValue int/string/fixed/flag/text, sections vides admises) et du journal libre
UserLogs/StoryModeDebug.txt (« Not in story mode » / « in story mode »). Le module est chargé par chemin
(nom à tirets, non importable)."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "progression-oracle.py"
spec = importlib.util.spec_from_file_location("progression_oracle", SCRIPT)
progression_oracle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(progression_oracle)

# Banque « avant » : mission 1 terminée, achats vides, StoryStage minimal.
BANQUE_AVANT = """<?xml version="1.0" encoding="utf-8"?>
<Bank version="1">
    <Section name="MissionCompletion">
        <Key name="1-Normal">
            <Value flag="1"/>
        </Key>
    </Section>
    <Section name="MissionCompletedAge">
        <Key name="1">
            <Value int="1"/>
        </Key>
    </Section>
    <Section name="Hotspots"/>
    <Section name="StoryStage">
        <Key name="4">
            <Value int="1"/>
        </Key>
    </Section>
</Bank>
"""

# Banque « après » : mission 2 terminée, un achat en Research, StoryStage qui avance.
BANQUE_APRES = """<?xml version="1.0" encoding="utf-8"?>
<Bank version="1">
    <Section name="MissionCompletion">
        <Key name="1-Normal">
            <Value flag="1"/>
        </Key>
        <Key name="2-Normal">
            <Value flag="1"/>
        </Key>
    </Section>
    <Section name="MissionCompletedAge">
        <Key name="1">
            <Value int="1"/>
        </Key>
        <Key name="2">
            <Value int="2"/>
        </Key>
    </Section>
    <Section name="Hotspots"/>
    <Section name="StoryStage">
        <Key name="4">
            <Value int="2"/>
        </Key>
    </Section>
    <Section name="Research">
        <Key name="4">
            <Value int="1"/>
        </Key>
    </Section>
</Bank>
"""

BANQUE_IDENTIQUE = BANQUE_AVANT

BANQUE_MALFORMEE = "<Bank version=\"1\"><Section name=\"MissionCompletion\">"    # ni Key ni fermeture


class InstantaneTmp:
    """Instantané synthétique sous un répertoire temporaire (Banks/ + UserLogs facultatif)."""

    def __init__(self, banques=None, story=None):
        self.tmp = tempfile.TemporaryDirectory()
        self.racine = Path(self.tmp.name)
        if banques:
            banks = self.racine / "Banks"
            banks.mkdir()
            for nom, contenu in banques.items():
                (banks / nom).write_text(contenu)
        if story is not None:
            logs = self.racine / "UserLogs"
            logs.mkdir()
            (logs / "StoryModeDebug.txt").write_text(story)

    def close(self):
        self.tmp.cleanup()


STORY_MODE_AVANT = "Scene loading\nNot in story mode\n"
STORY_MODE_APRES = "Scene loading\nin story mode\n"


class TestsProgressionOracle(unittest.TestCase):

    def test_mission_ajoutee(self):
        a = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_AVANT})
        b = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_APRES})
        r = progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"])
        self.assertEqual(r["banques"][0]["missions_terminees"], ["2-Normal", "2"])
        self.assertTrue(r["differences"])
        out = progression_oracle.en_texte(r)
        self.assertIn("missions terminées : +2 (2-Normal, 2)", out)         # MissionCompletion puis l'âge
        self.assertIn("    + 2-Normal : Value flag=1", out)
        a.close(), b.close()

    def test_valeur_modifiee(self):
        a = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_AVANT})
        b = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_APRES})
        ba = progression_oracle.charger_banque(a.racine / "Banks" / "TCampaign.SC2Bank")
        self.assertEqual(ba["StoryStage"]["4"], [("Value", "int", "1")])
        d = progression_oracle.comparer(ba, progression_oracle.charger_banque(b.racine / "Banks" / "TCampaign.SC2Bank"))
        self.assertEqual(d["StoryStage"]["modifiees"]["4"]["ancien"], [("Value", "int", "1")])
        self.assertEqual(d["StoryStage"]["modifiees"]["4"]["nouveau"], [("Value", "int", "2")])
        out = progression_oracle.en_texte(progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"]))
        self.assertIn("~ 4 : Value int=1 → Value int=2", out)
        self.assertIn("StoryStage : Value int=1 → Value int=2", out)
        a.close(), b.close()

    def test_section_ajoutee_et_vides(self):
        a = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_AVANT})
        b = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_APRES})
        d = progression_oracle.comparer(progression_oracle.charger_banque(a.racine / "Banks" / "TCampaign.SC2Bank"),
                                        progression_oracle.charger_banque(b.racine / "Banks" / "TCampaign.SC2Bank"))
        self.assertIn("Research", d)                                        # section nouvelle côté après
        self.assertEqual(d["Research"]["ajoutes"], {"4": [("Value", "int", "1")]})
        self.assertNotIn("Hotspots", d)                                     # vide des deux côtés : rien à dire
        out = progression_oracle.en_texte(progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"]))
        self.assertIn("Section Research : ajoutée (1 clé(s))", out)
        self.assertIn("achats : +1", out)
        a.close(), b.close()

    def test_sections_generees_entete(self):
        banque_a = '<Bank version="1"><Section name="Zebra"><Key name="z"><Value int="1"/></Key></Section></Bank>'
        banque_b = '<Bank version="1"><Section name="Zebra"><Key name="z"><Value int="2"/></Key></Section></Bank>'
        a = InstantaneTmp({"TCampaign.SC2Bank": banque_a})
        b = InstantaneTmp({"TCampaign.SC2Bank": banque_b})
        ba = progression_oracle.charger_banque(a.racine / "Banks" / "TCampaign.SC2Bank")
        self.assertEqual(ba["Zebra"]["z"], [("Value", "int", "1")])         # section inconnue lue génériquement
        out = progression_oracle.en_texte(progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"]))
        self.assertIn("Section Zebra", out)
        a.close(), b.close()

    def test_banque_absente_d_un_cote(self):
        a = InstantaneTmp()                                                 # Banks/ même pas créé
        b = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_AVANT})
        r = progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"])
        self.assertEqual(r["banques"][0]["etat"], "absente-avant")
        self.assertTrue(r["differences"])
        self.assertIn("Banque TCampaign.SC2Bank : absente côté avant → apparue",
                      progression_oracle.en_texte(r))
        a.close(), b.close()

    def test_xml_malformee_exit_2(self):
        a = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_AVANT})
        b = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_MALFORMEE})
        with self.assertRaises(progression_oracle.ErreurBanque):
            progression_oracle.charger_banque(b.racine / "Banks" / "TCampaign.SC2Bank")
        rc = progression_oracle.main([str(a.racine), str(b.racine)])
        self.assertEqual(rc, 2)                                             # pas de crash, code 2
        a.close(), b.close()

    def test_instantanes_identiques_exit_1(self):
        a = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_IDENTIQUE})
        b = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_IDENTIQUE})
        r = progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"])
        self.assertFalse(r["differences"])
        self.assertIn("Aucune différence", progression_oracle.en_texte(r))
        self.assertEqual(progression_oracle.main([str(a.racine), str(b.racine)]), 1)
        a.close(), b.close()

    def test_sortie_json(self):
        a = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_AVANT}, story=STORY_MODE_AVANT)
        b = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_APRES}, story=STORY_MODE_APRES)
        r = progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"])
        self.assertEqual(list(r), ["avant", "apres", "banques", "story_mode_debug", "differences"])
        blob = json.dumps(r, ensure_ascii=False)
        recharge = json.loads(blob)
        self.assertEqual(recharge["banques"][0]["missions_terminees"], ["2-Normal", "2"])
        self.assertEqual(recharge["story_mode_debug"]["avant"], "non")
        a.close(), b.close()

    def test_verdicts_story_mode(self):
        self.assertEqual(progression_oracle.verdict_story_mode(["Not in story mode"]), "non")
        self.assertEqual(progression_oracle.verdict_story_mode(["Now in story mode"]), "oui")
        self.assertEqual(progression_oracle.verdict_story_mode(["rien de pertinent"]), "inconnu")
        a = InstantaneTmp(story=STORY_MODE_AVANT)
        self.assertEqual(progression_oracle.lire_debogue(a.racine)[0], "non")
        b = InstantaneTmp(story=STORY_MODE_APRES)
        r = progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"])
        self.assertEqual(r["story_mode_debug"]["avant"], "non")
        self.assertEqual(r["story_mode_debug"]["apres"], "oui")
        self.assertIn("story mode avant/après : non → oui", progression_oracle.en_texte(r))
        self.assertIn("+ in story mode", progression_oracle.en_texte(r))    # la ligne différente du journal
        a.close(), b.close()

    def test_journal_story_absent_d_un_cote(self):
        a = InstantaneTmp(story=STORY_MODE_AVANT)
        b = InstantaneTmp()                                                 # pas de UserLogs/
        r = progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"])
        self.assertEqual((r["story_mode_debug"]["avant"], r["story_mode_debug"]["apres"]), ("non", "inconnu"))
        self.assertEqual(r["story_mode_debug"]["retirees"], ["Scene loading", "Not in story mode"])  # tout le journal part
        a.close(), b.close()

    def test_banque_all_et_plusieurs_banques(self):
        a = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_AVANT})
        b = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_APRES, "TUI.SC2Bank": BANQUE_AVANT})
        r = progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank", "TUI.SC2Bank"])
        self.assertEqual([e["nom"] for e in r["banques"]], ["TCampaign.SC2Bank", "TUI.SC2Bank"])
        self.assertEqual(r["banques"][1]["etat"], "absente-avant")
        out = progression_oracle.en_texte(r)
        self.assertIn("Banque TUI.SC2Bank : absente côté avant → apparue", out)
        self.assertIn("missions terminées : +2 (2-Normal, 2)", out)         # résumé global, toutes banques
        a.close(), b.close()

    def test_instantane_introuvable_exit_2(self):
        with tempfile.TemporaryDirectory() as t:
            b = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_AVANT})
            self.assertEqual(progression_oracle.main([str(Path(t) / "nulle-part"), str(b.racine)]), 2)
            b.close()

    def test_mission_retiree(self):
        a = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_APRES})
        b = InstantaneTmp({"TCampaign.SC2Bank": BANQUE_AVANT})
        out = progression_oracle.en_texte(progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"]))
        self.assertIn("- 2-Normal : Value flag=1", out)                     # sens inverse : retrait
        self.assertIn("missions terminées : +0", out)
        self.assertIn("StoryStage : Value int=2 → Value int=1", out)
        a.close(), b.close()

    def test_cles_multiples_valeurs(self):
        banque_a = ('<Bank version="1"><Section name="Purchases"><Key name="p1">'
                    '<Value int="1"/><Value string="Merc"/></Key></Section></Bank>')
        banque_b = ('<Bank version="1"><Section name="Purchases"><Key name="p1">'
                    '<Value int="2"/><Value string="Merc"/></Key></Section></Bank>')
        a = InstantaneTmp({"TCampaign.SC2Bank": banque_a})
        b = InstantaneTmp({"TCampaign.SC2Bank": banque_b})
        ba = progression_oracle.charger_banque(a.racine / "Banks" / "TCampaign.SC2Bank")
        self.assertEqual(ba["Purchases"]["p1"], [("Value", "int", "1"), ("Value", "string", "Merc")])
        d = progression_oracle.comparer(ba, progression_oracle.charger_banque(b.racine / "Banks" / "TCampaign.SC2Bank"))
        self.assertIn("p1", d["Purchases"]["modifiees"])                    # seule la 1re valeur a changé
        a.close(), b.close()

    def test_clé_section_sans_valeur(self):
        banque_a = '<Bank version="1"><Section name="StoryMisc"><Key name="0"/></Section></Bank>'
        banque_b = '<Bank version="1"><Section name="StoryMisc"><Key name="0"><Value int="2"/></Key></Section></Bank>'
        a = InstantaneTmp({"TCampaign.SC2Bank": banque_a})
        b = InstantaneTmp({"TCampaign.SC2Bank": banque_b})
        ba = progression_oracle.charger_banque(a.racine / "Banks" / "TCampaign.SC2Bank")
        self.assertEqual(ba["StoryMisc"]["0"], [])                          # clé sans valeur : liste vide
        d = progression_oracle.comparer(ba, progression_oracle.charger_banque(b.racine / "Banks" / "TCampaign.SC2Bank"))
        self.assertIn("0", d["StoryMisc"]["modifiees"])
        out = progression_oracle.en_texte(progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"]))
        self.assertIn("~ 0 : sans valeur → Value int=2", out)
        a.close(), b.close()

    def test_sections_retiree_et_cles_retirees(self):
        banque_a = ('<Bank version="1"><Section name="Armory"><Key name="a1"><Value int="1"/></Key>'
                    '<Key name="a2"><Value int="1"/></Key></Section>'
                    '<Section name="Vieille"><Key name="v"><Value int="9"/></Key></Section></Bank>')
        banque_b = '<Bank version="1"><Section name="Armory"><Key name="a1"><Value int="1"/></Key></Section></Bank>'
        a = InstantaneTmp({"TCampaign.SC2Bank": banque_a})
        b = InstantaneTmp({"TCampaign.SC2Bank": banque_b})
        d = progression_oracle.comparer(progression_oracle.charger_banque(a.racine / "Banks" / "TCampaign.SC2Bank"),
                                        progression_oracle.charger_banque(b.racine / "Banks" / "TCampaign.SC2Bank"))
        self.assertEqual(d["Vieille"]["retires"], {"v": [("Value", "int", "9")]})   # section entière partie
        self.assertEqual(d["Armory"]["retires"], {"a2": [("Value", "int", "1")]})
        out = progression_oracle.en_texte(progression_oracle.analyser(a.racine, b.racine, ["TCampaign.SC2Bank"]))
        self.assertIn("Section Vieille : retirée (1 clé(s))", out)
        self.assertIn("- v : Value int=9", out)
        self.assertIn("- a2 : Value int=1", out)
        a.close(), b.close()


if __name__ == "__main__":
    unittest.main()
