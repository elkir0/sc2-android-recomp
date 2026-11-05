#!/usr/bin/env python3
"""Tests unittest de scripts/fold-ab.py — fixtures synthétiques calculées à la main.

Les dossiers scripts/fold-ab-fixtures/synth-*/ sont de petits runs fabriqués
pour le test : chaque valeur attendue est recalculée à la main dans les
commentaires (FPS = frames/durée des lignes retenues, jeu/backend pondérés par
les frames). Les fixtures réelles v15-profil et v13h-manuel (lecture seule)
servent au test de bout en bout : valeurs vérifiées sur les fichiers, script
lancé en sous-processus, tables 1/2/3 présentes.
"""
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "fold-ab.py"
FIXTURES = Path(__file__).resolve().parent / "fold-ab-fixtures"
SYNTHESE_PLATEAU = FIXTURES / "synth-plateau"
SYNTHESE_PROFIL = FIXTURES / "synth-profil"
SYNTHESE_SANS_CLIC = FIXTURES / "synth-sans-clic"
V15 = FIXTURES / "v15-profil"
V13H = FIXTURES / "v13h-manuel"

_spec = importlib.util.spec_from_file_location("fold_ab", SCRIPT)
fold_ab = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fold_ab)

LIGNE_FPS = (
    "10-19 12:30:00.591 I/SC2Partout(22915): [FPS] 20.9 images/s sur 5.0 s "
    "(frames 1638→1743, swapchain ; swapchain 2504x2256) ; moyenne depuis la 1re : 17.2 ; "
    "phase chargement ; ms/frame : jeu 46.8, backend 0.8, présentation GPU 0.2"
)
LIGNE_FPS_SANS_GPU = (
    "10-19 12:30:00.591 I/SC2Partout(22915): [FPS] 20.9 images/s sur 5.0 s "
    "(frames 1638→1743, swapchain ; swapchain 2504x2256) ; moyenne depuis la 1re : 17.2 ; "
    "phase chargement ; ms/frame : jeu 46.8, backend 0.8"
)


class TestLectureFPS(unittest.TestCase):
    """Analyse d'une ligne [FPS] isolée."""

    def test_ligne_complete(self):
        # Tous les champs : fps 20.9, durée 5.0, frames 1638→1743 (105 frames),
        # jeu 46.8, backend 0.8, présentation GPU 0.2 ; horodatage 12:30:00.591.
        ligne = fold_ab.lire_fps(LIGNE_FPS)
        self.assertEqual(ligne["fps"], 20.9)
        self.assertEqual(ligne["duree"], 5.0)
        self.assertEqual(ligne["debut"], 1638)
        self.assertEqual(ligne["fin"], 1743)
        self.assertEqual(ligne["frames"], 105)
        self.assertEqual(ligne["jeu"], 46.8)
        self.assertEqual(ligne["backend"], 0.8)
        self.assertEqual(ligne["gpu"], 0.2)
        self.assertEqual(fold_ab.horodatage(LIGNE_FPS), 12 * 3600 + 30 * 60 + 0.591)

    def test_sans_presentation_gpu(self):
        ligne = fold_ab.lire_fps(LIGNE_FPS_SANS_GPU)
        self.assertEqual(ligne["fps"], 20.9)
        self.assertIsNone(ligne["gpu"])

    def test_ligne_bilan_fps_ignoree(self):
        # Les « [BILAN] [FPS] phase … : N images en S s » ne portent pas
        # « images/s sur » : elles ne comptent pas comme lignes [FPS].
        bilan = ("10-19 11:29:00.135 I/SC2Partout( 7004): [BILAN] [FPS] phase chargement :"
                 "   2777 images en   145.5 s =  19.1 images/s (de 1.1 s, en cours)")
        self.assertIsNone(fold_ab.lire_fps(bilan))


class TestPlateau(unittest.TestCase):
    """Plateau de jeu actif sur les fixtures synthétiques."""

    def test_plateau_manuel(self):
        # synth-plateau : clic à 10:00:00 (36000 s), --apres 10 → début 36010 ;
        # pas de profil → fin = dernière ligne [FPS] (36030). La ligne de
        # 10:00:10 sort (fenêtre [36005, 36010] avant début − 0,5 s) ; restent
        # 10:00:15/20/25/30 : frames 100+150+200+50 = 500, durée 20,0 s.
        #   FPS = 500 / 20 = 25,0
        #   médiane de [20, 30, 40, 10] = (20+30)/2 = 25,0 ; min 10, max 40
        #   jeu = (30×100 + 40×150 + 50×200 + 60×50) / 500 = 22000/500 = 44,0
        #   backend = (1×100 + 6×150 + 2×200 + 7×50) / 500 = 1750/500 = 3,5
        #   GPU limitant : lignes à backend 6,0 et 7,0 > 5 → 2/4 = 0,5
        run = fold_ab.depouille_run("synth-plateau", SYNTHESE_PLATEAU)
        self.assertFalse(run["sans_clic"])
        self.assertEqual(run["lignes"], 4)
        self.assertEqual(run["frames"], 500)
        self.assertAlmostEqual(run["duree"], 20.0)
        self.assertAlmostEqual(run["fps"], 25.0)
        self.assertAlmostEqual(run["fps_median"], 25.0)
        self.assertAlmostEqual(run["fps_min"], 10.0)
        self.assertAlmostEqual(run["fps_max"], 40.0)
        self.assertAlmostEqual(run["jeu_ms"], 44.0)
        self.assertAlmostEqual(run["backend_ms"], 3.5)
        self.assertAlmostEqual(run["gpu_limitant"], 0.5)

    def test_fenetre_profilee_exclue_du_plateau(self):
        # synth-profil : profil 10:00:30 → 10:00:40 ; le plateau (début 36010)
        # s'arrête au début du profil (36030) : seules 10:00:15/20/25 comptent.
        #   FPS = (100+150+200) / 15 = 30,0 ; jeu = 19000/450 = 42,22 ms
        run = fold_ab.depouille_run("synth-profil", SYNTHESE_PROFIL)
        self.assertEqual(run["lignes"], 3)
        self.assertEqual(run["frames"], 450)
        self.assertAlmostEqual(run["fps"], 30.0)
        self.assertAlmostEqual(run["jeu_ms"], 19000 / 450)
        # FPS profilé : fenêtres 10:00:35 et 10:00:40 dans le profil →
        # (100+100) frames / 10 s = 20,0 images/s.
        self.assertAlmostEqual(run["profil"]["fps"], 20.0)

    def test_sans_clic(self):
        # synth-sans-clic : 14 lignes [FPS] sans clic → plateau = 12 dernières
        # (fps 3 à 14) : frames 12×100 = 1200, durée 60 s → FPS 20,0 ;
        # médiane de [3..14] = (8+9)/2 = 8,5.
        run = fold_ab.depouille_run("synth-sans-clic", SYNTHESE_SANS_CLIC)
        self.assertTrue(run["sans_clic"])
        self.assertEqual(run["lignes"], 12)
        self.assertEqual(run["frames"], 1200)
        self.assertAlmostEqual(run["fps"], 20.0)
        self.assertAlmostEqual(run["fps_median"], 8.5)
        self.assertAlmostEqual(run["fps_min"], 3.0)
        self.assertAlmostEqual(run["fps_max"], 14.0)
        self.assertAlmostEqual(run["jeu_ms"], 10.0)

    def test_duree(self):
        # --duree 8 sur synth-plateau : fin = 36010+8 = 36018 ; seule la ligne
        # de 10:00:15 (fenêtre [36010, 36015]) reste → 100 frames / 5 s.
        run = fold_ab.depouille_run("synth-plateau", SYNTHESE_PLATEAU, duree=8.0)
        self.assertEqual(run["lignes"], 1)
        self.assertEqual(run["frames"], 100)
        self.assertAlmostEqual(run["fps"], 20.0)
        self.assertAlmostEqual(run["jeu_ms"], 30.0)


class TestProfil(unittest.TestCase):
    """Profil simpleperf sur la fixture synth-profil."""

    def setUp(self):
        self.run = fold_ab.depouille_run("synth-profil", SYNTHESE_PROFIL)
        self.threads = fold_ab.lire_rapport_threads(SYNTHESE_PROFIL / "rapport-threads.txt")

    def test_periode(self):
        # En-tête : Event count 200 000 000 / Samples 800 = 250 000 ns = 0,25 ms.
        self.assertAlmostEqual(self.threads["periode_ns"], 250000.0)
        self.assertEqual(self.threads["echantillons_jeu"], 760)

    def test_cpu_fil_du_jeu(self):
        # 760 échantillons sc2-jeu × 0,25 ms / 200 frames = 0,95 ms/frame.
        self.assertAlmostEqual(self.run["profil"]["cpu_jeu_ms"], 0.95)

    def test_categories(self):
        # ms/frame = échantillons × 0,25 / 200 = échantillons × 0,00125 :
        #   lifte    : L_00A02FF0 (100) + L_0000AB (100)           = 200 → 0,25
        #   dispatch : sc2_dispatch (50) + dispatch_truc (30) + dispatch_un (10) = 90 → 0,1125
        #   dessin   : dessin_reel (40) + vk_present (20) + v_init (20) = 80 → 0,10
        #   pilote   : vulkan.adreno.so pilote_foo (20)             = 20 → 0,025
        #   libc     : memcpy (30)                                   = 30 → 0,0375
        #   noyau    : native_queued… (40)                           = 40 → 0,05
        #   runtime  : runtime_foo (50) + « symbole avec espaces » (60) = 110 → 0,1375
        #   autres   : mystere (20)                                  = 20 → 0,025
        categories = self.run["profil"]["categories"]
        self.assertAlmostEqual(categories["lifte"], 0.25)
        self.assertAlmostEqual(categories["dispatch"], 0.1125)
        self.assertAlmostEqual(categories["dessin"], 0.10)
        self.assertAlmostEqual(categories["pilote"], 0.025)
        self.assertAlmostEqual(categories["libc"], 0.0375)
        self.assertAlmostEqual(categories["noyau"], 0.05)
        self.assertAlmostEqual(categories["runtime"], 0.1375)
        self.assertAlmostEqual(categories["autres"], 0.025)

    def test_couverture(self):
        # Σ échantillons listés = 590 sur 760 du fil → 590/760 ≈ 0,776.
        self.assertAlmostEqual(self.run["couverture"], 590 / 760, places=6)

    def test_premiere_categorie_et_symboles_avec_espaces(self):
        # L_0000AB vit dans vulkan.adreno.so mais matche d'abord ^L_[0-9A-Fa-f]+$
        # → lifte, pas pilote ; vk_present commence par vk_ → dessin.
        # Le symbole « symbole avec espaces » est lu entier (catégorie runtime).
        self.assertEqual(fold_ab.categorie("/vendor/lib64/hw/vulkan.adreno.so", "L_0000AB"), "lifte")
        self.assertEqual(
            fold_ab.categorie("/data/app/lib/arm64/libSC2Partout.so", "vk_present"),
            "dessin")
        self.assertEqual(
            fold_ab.categorie("/data/app/lib/arm64/libSC2Partout.so", "symbole avec espaces"),
            "runtime")
        haut = [t["symbole"] for t in self.run["top_symboles"]]
        self.assertIn("symbole avec espaces", haut)
        self.assertAlmostEqual(self.run["top_symboles"][0]["ms_frame"], 100 * 0.25 / 200)
        self.assertEqual(len(self.run["top_symboles"]), 10)


class TestThermiquePlantagesPresent(unittest.TestCase):
    """Thermique (resume.txt), plantages et Present du bilan."""

    def test_thermique_present(self):
        avant, apres = fold_ab.lire_thermique(SYNTHESE_PROFIL / "resume.txt")
        self.assertEqual(avant, {"statut": 2, "temperature": 40.0})
        self.assertEqual(apres, {"statut": 3, "temperature": 44.5})

    def test_thermique_absent(self):
        run = fold_ab.depouille_run("synth-plateau", SYNTHESE_PLATEAU)
        self.assertIsNone(run["thermique_avant"])
        self.assertIsNone(run["thermique_apres"])

    def test_plantages_et_present(self):
        # Une ligne « Fatal signal 11 (SIGSEGV) » → 1 plantage ; Present 501.
        run = fold_ab.depouille_run("synth-plateau", SYNTHESE_PLATEAU)
        self.assertEqual(run["plantages"], 1)
        self.assertEqual(run["present"], 501)
        self.assertEqual(len(run["bilan_vol"]), 1)
        self.assertTrue(run["bilan_vol"][0].startswith("[VOL]"))
        self.assertEqual(len(run["bilan_gpu"]), 1)


class TestFixturesReelles(unittest.TestCase):
    """Valeurs vérifiées à la main sur les fixtures réelles (lecture seule)."""

    def test_v15_profil(self):
        # 1er clic 12:28:57.062 = 44937,062 s ; profil 12:29:17 → 12:30:20 ;
        # période 50 538 000 000 / 202 152 = 250 000 ns ; sc2-jeu 190 516
        # échantillons ; rapport self tronqué → couverture 126 465/190 516
        # ≈ 66,4 % < 100 %.
        log = fold_ab.lire_logcat(V15 / "logcat.txt")
        self.assertAlmostEqual(log["premier_clic"], 44937.062, places=3)
        self.assertEqual(fold_ab.lire_fenetre_profil(V15 / "deroule.txt"), (44957.0, 45020.0))
        threads = fold_ab.lire_rapport_threads(V15 / "rapport-threads.txt")
        self.assertAlmostEqual(threads["periode_ns"], 250000.0)
        self.assertEqual(threads["echantillons_jeu"], 190516)
        run = fold_ab.depouille_run("v15-profil", V15)
        self.assertLess(run["couverture"], 1.0)
        self.assertGreater(run["couverture"], 0.5)
        self.assertAlmostEqual(run["couverture"], 126465 / 190516, places=6)
        self.assertEqual(run["thermique_avant"], {"statut": 0, "temperature": 37.8})
        self.assertEqual(run["thermique_apres"], {"statut": 1, "temperature": 43.1})

    def test_v13h_manuel(self):
        # Logcat seul : Present 2778, pas de profil, 2 lignes [BILAN] [VOL],
        # 2 lignes [ARRÊT] de la sortie normale (appui long sur Retour) : 0 plantage.
        run = fold_ab.depouille_run("v13h-manuel", V13H)
        self.assertEqual(run["present"], 2778)
        self.assertIsNone(run["profil"])
        self.assertEqual(len(run["bilan_vol"]), 2)
        self.assertEqual(run["plantages"], 0)

    def test_script_sur_les_deux_fixtures(self):
        # Sous-processus : code 0, tableaux 1 (plateau), 2 (profil, car v15 en
        # a un) et 3 (écarts, car 2 runs) présents, thermique au format français.
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), str(V15), str(V13H)],
            capture_output=True, text=True, check=True)
        self.assertIn("FPS (frames/durée)", proc.stdout)
        self.assertIn("couverture", proc.stdout)
        self.assertIn("FPS plateau", proc.stdout)
        self.assertIn("0 (37,8 °C) → 1 (43,1 °C)", proc.stdout)
        self.assertIn("v13h-manuel", proc.stdout)

    def test_json_sur_fixture_reelle(self):
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), "--json", str(V15)],
            capture_output=True, text=True, check=True)
        donnees = json.loads(proc.stdout)
        self.assertIsInstance(donnees, list)
        self.assertEqual(len(donnees), 1)
        self.assertEqual(donnees[0]["run"], "v15-profil")
        self.assertIn("couverture", donnees[0])
        self.assertIn("top_symboles", donnees[0])
        self.assertIn("sans_clic", donnees[0])
        self.assertIn("bilan_vol", donnees[0])
        self.assertIn("profil", donnees[0])


class TestErreurs(unittest.TestCase):
    """RUN inexistant ou sans ligne [FPS] : code de sortie 2."""

    def test_run_inexistant(self):
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), "/nexiste/pas"],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("inexistant", proc.stderr)

    def test_run_sans_ligne_fps(self):
        with tempfile.TemporaryDirectory() as tmp:
            chemin = Path(tmp) / "logcat.txt"
            chemin.write_text(
                "10-20 10:00:00.000 I/SC2Partout( 1000): [INPUT] frame 10 : WM_LBUTTONDOWN\n",
                encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), tmp],
                capture_output=True, text=True)
            self.assertEqual(proc.returncode, 2)
            self.assertIn("[FPS]", proc.stderr)

    def test_etiquette(self):
        # « etiquette=chemin » fixe l'étiquette ; sinon nom du dossier.
        etiquette, chemin = fold_ab.resoudre_run(f"avant={SYNTHESE_PLATEAU}")
        self.assertEqual(etiquette, "avant")
        self.assertEqual(chemin, SYNTHESE_PLATEAU)
        etiquette, _ = fold_ab.resoudre_run(str(SYNTHESE_PLATEAU))
        self.assertEqual(etiquette, "synth-plateau")


if __name__ == "__main__":
    unittest.main()
