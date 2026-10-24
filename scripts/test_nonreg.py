#!/usr/bin/env python3
"""Tests unittest de scripts/nonreg.py (V1.4, 22/10) — dossiers de banc synthétiques, jamais de run réel.

Formats repris du runtime : « [INPUT] banc t=…s tour N : … » (sc2_entrees_banc.c), « [BILAN] frames présentées
(Present) : N » et « [BILAN] draws : … « lumière » … table SPIR-V embarquée : K shaders », « [BILAN] samplers des draws
réels : … », « [BILAN]   repli : texture … » (sc2_d3d9.c), « [SPV] pack SPIR-V externe ouvert/absent » (sc2_spv_pack.c),
empreintes.txt de android/outils/ppm-sequence.py, resume.txt « rc=N en S s » des scripts de banc.
Aussi : contrôle des tolérances par nonreg.sh (avant tout banc : binaire inexistant, aucun run) et
scripts/comparer-captures.sh sur de petites images PPM synthétiques (magick, puis le repli ffmpeg, s'ils existent).
"""
import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "nonreg.py"
NONREG_SH = SCRIPT.with_name("nonreg.sh")
COMPARER = SCRIPT.with_name("comparer-captures.sh")
spec = importlib.util.spec_from_file_location("nonreg", SCRIPT)
nonreg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nonreg)


def bilan(frames=30000, lumiere=0, sans_donnees=0, cube=0, sans_texture=0, table=2856, pack=True):
    l = [f"[BILAN] frames présentées (Present) : {frames}",
         f"[BILAN] draws : 7000000 RÉELS (shaders traduits), {lumiere} « lumière » (sans SPIR-V {lumiere}, sans "
         f"déclaration 0, sans données 0) ; table SPIR-V embarquée : {table} shaders",
         f"[BILAN] samplers des draws réels : 1000 servis (texture ou cible), replis sur texture factice : {sans_texture} sans "
         f"texture liée, {cube} cube, {sans_donnees} texture sans données, 0 format non géré"]
    if sans_donnees:
        l.append(f"[BILAN]   repli : texture 2048×2048 D3DFMT 0x50 (genre 3, usage 0x2, pool 0) — sans données ×{sans_donnees}")
    l.insert(0, "[SPV] pack SPIR-V externe ouvert : /nulle/part/sc2-spirv.pak — 75797 entrées, 1537.1 Mo (mmap lecture seule)"
             if pack else "[SPV] pack SPIR-V externe absent : /nulle/part/absent.pak : No such file or directory")
    return "\n".join(l) + "\n"


def journal_mission(fps=100.0, victoire=True, **k):
    """Échantillons toutes les 5 s de t=40 à 65, victoire à 67 s, QUIT à 187 s, à fps tours/s."""
    l, tour = [], 2764
    for t in (40.0, 45.0, 50.0, 55.0, 60.0, 65.0):
        tour = 2764 + int((t - 40.0) * fps)
        l.append(f"[INPUT] banc t={t:.2f}s tour {tour} : CLIC (953, 588)")
    if victoire:
        l.append(f"[INPUT] banc t=67.00s tour {2764 + int(27 * fps)} : VICTOIRE détectée (Liberation Day Victory.SC2Save)")
    l.append(f"[INPUT] banc t=187.00s tour {2764 + int(147 * fps)} : QUIT — arrêt volontaire du run")
    l.append(f"[ARRÊT] [thread 1] SC2_BANC_ENTREES : QUIT à t=187.0s")
    l.append(f"[BILAN] [INPUT] banc (SC2_BANC_ENTREES) : 26/93 événement(s) appliqué(s), 0 rejeté(s), "
             f"{2764 + int(147 * fps)} tours de pompe, 187.0 s")
    return "\n".join(l) + "\n" + bilan(**k)


EMPREINTES = """\
 frame       R       G       B  non noirs   changés  empreinte
     1    0.00    0.00    0.00     0.00%         —  be01318cdf69
    10   29.65   36.88   47.19    99.17%    99.17%  {f10}
   100    1.00    0.00    1.00   100.00%   100.00%  ae1ff947a8df
   200    1.00    0.00    1.00   100.00%     0.00%  ae1ff947a8df
   300    1.00    0.00    1.00   100.00%     0.00%  {f300}
"""


class Sortie:
    """Sortie synthétique de nonreg.sh : canon/, bar/, missions/<carte>/."""

    def __init__(self, fps=100.0, f10="470ebae2b462", f300="ae1ff947a8df", canon_frames=600, victoire=True, **k):
        self.tmp = tempfile.TemporaryDirectory()
        self.racine = Path(self.tmp.name)
        self.s = self.racine / "sortie"
        c = self.s / "canon"
        c.mkdir(parents=True)
        (c / "run.log").write_text("[ARRÊT] [thread 9] SC2_FRAMES_MAX=600 : 600 frames présentées par la boucle de "
                                   "rendu — arrêt volontaire du run\n" + bilan(frames=canon_frames, **k))
        (c / "resume.txt").write_text("rc=0 en 22 s\n")
        (c / "empreintes.txt").write_text(EMPREINTES.format(f10=f10, f300=f300))
        b = self.s / "bar"
        b.mkdir()
        (b / "run.log").write_text("[MPQ] … Maps\\Campaign\\TStory01.SC2Map\\DocumentHeader — 1re lecture\n"
                                   "[MPQ] … Maps\\Campaign\\TRaynor02.SC2Map\\DocumentHeader — 1re lecture\n"
                                   f"[BILAN] [INPUT] banc (SC2_BANC_ENTREES) : 13/13 événement(s) appliqué(s), 0 "
                                   f"rejeté(s), {int(170 * fps * 0.7)} tours de pompe, 170.0 s\n" + bilan(frames=12000, **k))
        (b / "resume.txt").write_text("rc=0 en 170 s\n")
        self.missions = ["TRaynor01", "THanson03A"]
        for carte in self.missions:
            d = self.s / "missions" / carte
            d.mkdir(parents=True)
            (d / "run.log").write_text(journal_mission(fps=fps, victoire=victoire, **k))
            (d / "resume.txt").write_text("rc=0 en 188 s\n")
            (d / "empreintes.txt").write_text(EMPREINTES.format(f10="0b1fab61e9b1", f300="15fba75bdb71"))
        (self.s / "missions" / "rapport.md").write_text("| carte |\n")
        self.ref = self.racine / "ref"

    def analyser(self, ref=True, seul=False, env=()):
        a = ["analyser", str(self.s), "--mode", "fumee", "--controles", ",".join(["canon", "bar"] + self.missions),
             "--binaire", str(SCRIPT), "--duree", "800"] + (["--ref", str(self.ref)] if ref else [])
        a += ["--seul"] if seul else []
        for e in env:
            a += ["--env", e]
        rc = nonreg.main(a)
        return rc, json.loads((self.s / "nonreg.json").read_text())

    def close(self):
        self.tmp.cleanup()


def criteres(rapport, nom):
    c = next(x for x in rapport["controles"] if x["nom"] == nom)
    return {x["critere"]: x for x in c["criteres"]}


def silence(f, *a):
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return f(*a)


class TestsCadence(unittest.TestCase):

    def test_plateau_et_fenetres(self):
        e = [(40.0, 1000), (42.0, 1200), (45.0, 1500), (50.0, 2000), (55.0, 2400), (57.0, 2600)]
        p = nonreg.cadence(e, 40, 57)
        self.assertEqual((p["images"], p["s"], p["images_s"]), (1600, 17.0, 94.12))
        self.assertEqual(p["fenetres"], 3)                        # 40→45, 45→50, 50→55 ; 55→57 trop court
        self.assertEqual((p["mediane"], p["min"], p["max"]), (100.0, 80.0, 100.0))

    def test_bornes_et_plateau_trop_court(self):
        e = [(10.0, 0), (40.0, 1000), (43.0, 1300), (70.0, 4000)]
        self.assertIsNone(nonreg.cadence(e, 40, 69))              # 40 → 43 : 3 s < 5 s
        self.assertIsNone(nonreg.cadence([(40.0, 1)], None, None))
        self.assertEqual(nonreg.cadence(e)["images_s"], round(4000 / 60, 2))


class TestsAnalyse(unittest.TestCase):

    def test_premier_run_sans_reference_puis_maj_ref(self):
        s = Sortie()
        rc, r = silence(s.analyser)
        self.assertEqual((rc, r["global"]["verdict"]), (0, "PASS"))
        self.assertIsNone(r["ref"])
        self.assertEqual(criteres(r, "TRaynor01")["images/s"]["note"], "sans réf.")
        self.assertEqual(r["pack"]["etat"], "ouvert")
        self.assertIsNone(r["pack"]["sha256"])                    # /nulle/part : illisible, pas d'erreur
        self.assertEqual(r["controles"][2]["images_s"], 100.0)    # (5464 − 2764) / 27 s
        self.assertEqual(silence(nonreg.main, ["maj-ref", str(s.s), "--ref", str(s.ref)]), 0)
        self.assertTrue((s.ref / "fumee.json").is_file())
        poses = sorted(str(p.relative_to(s.ref)) for p in s.ref.rglob("*") if p.is_file())
        self.assertIn("fumee/TRaynor01/empreintes.txt", poses)
        self.assertIn("fumee/rapport-campagne.md", poses)
        self.assertFalse([p for p in poses if p.endswith((".png", ".ppm", "run.log"))])
        s.close()

    def _avec_ref(self, **k):
        """Référence posée par un run nominal, puis second run (paramètres k) analysé contre elle."""
        s0 = Sortie()
        silence(s0.analyser)
        silence(nonreg.main, ["maj-ref", str(s0.s), "--ref", str(s0.ref)])
        s1 = Sortie(**k)
        s1.ref = s0.ref
        rc, r = silence(s1.analyser)
        return s0, s1, rc, r

    def test_a_contre_a_pass(self):
        s0, s1, rc, r = self._avec_ref(fps=101.0)
        self.assertEqual((rc, r["global"]["verdict"]), (0, "PASS"))
        x = criteres(r, "TRaynor01")["images/s"]
        self.assertEqual((x["verdict"], x["ecart_pct"]), ("PASS", 1.0))
        self.assertAlmostEqual(r["global"]["images_s_missions"]["images_s"] /
                               r["global"]["ref_images_s_missions"]["images_s"], 1.01, places=2)
        s0.close(), s1.close()

    def test_images_s_en_baisse_regression(self):
        s0, s1, rc, r = self._avec_ref(fps=52.0)                  # SC2_ACTIVATION=0 : Sleep(10) par image
        self.assertEqual((rc, r["global"]["verdict"]), (1, "RÉGRESSION"))
        x = criteres(r, "TRaynor01")["images/s"]
        self.assertEqual(x["verdict"], "RÉGRESSION")
        self.assertTrue(x["perf"])
        self.assertEqual(criteres(r, "bar")["images/s"]["verdict"], "RÉGRESSION")
        self.assertEqual(criteres(r, "canon")["images"]["verdict"], "PASS")   # le canon ne juge pas images/s
        self.assertNotIn("images/s", criteres(r, "canon"))
        # maj-ref accepte (seul images/s régresse) … sauf les images des runs minutés, à 52 % : au-dessus de 50 %
        self.assertEqual(silence(nonreg.main, ["maj-ref", str(s1.s), "--ref", str(s1.racine / "ref2")]), 0)
        s0.close(), s1.close()

    def test_sans_table_spirv_regression_lumiere(self):
        s0, s1, rc, r = self._avec_ref(lumiere=480000, table=0, pack=False)
        self.assertEqual(rc, 1)
        for nom in ("canon", "bar", "TRaynor01"):
            x = criteres(r, nom)["lumière"]
            self.assertEqual((x["verdict"], x["valeur"], x["ref"]), ("RÉGRESSION", 480000, 0))
            self.assertIn("table SPIR-V embarquée : 0 shaders", x["note"])
        self.assertEqual(r["pack"]["etat"], "absent")
        out = (s1.s / "nonreg.md").read_text()
        self.assertIn("TRaynor01 : lumière = 480000 (réf. 0)", out)
        self.assertEqual(silence(nonreg.main, ["maj-ref", str(s1.s), "--ref", str(s1.racine / "ref2")]), 4)
        self.assertFalse((s1.racine / "ref2" / "fumee.json").exists())
        self.assertEqual(silence(nonreg.main, ["maj-ref", str(s1.s), "--ref", str(s1.racine / "ref2"), "--forcer"]), 0)
        self.assertTrue(json.loads((s1.racine / "ref2" / "fumee.json").read_text())["ref_posee"]["forcee"])
        s0.close(), s1.close()

    def test_sans_donnees_zero_exige_meme_sans_reference(self):
        s = Sortie(sans_donnees=12)
        rc, r = silence(s.analyser, False)
        self.assertEqual(rc, 1)
        x = criteres(r, "THanson03A")["replis"]
        self.assertEqual((x["verdict"], x["valeur"]), ("RÉGRESSION", 12))
        self.assertIn("« sans données » 12 (0 exigé)", x["note"])
        s.close()

    def test_nouvelle_cause_de_repli(self):
        s0, s1, rc, r = self._avec_ref(cube=3)
        self.assertEqual(criteres(r, "TRaynor01")["replis"]["verdict"], "RÉGRESSION")
        self.assertIn("nouveau(x) : cube 3", criteres(r, "TRaynor01")["replis"]["note"])
        s0.close(), s1.close()

    def test_autre_cause_sans_reference_notee_puis_jugee(self):
        # 22/10 : TZeratul04 de main, 4926 « sans texture liée » au 1er run : la pose de la référence ne doit pas
        # être refusée ; ensuite la cause fait partie de la référence (même cause : PASS), une autre reste jugée
        s0 = Sortie(sans_texture=4926)
        rc, r = silence(s0.analyser)
        self.assertEqual((rc, r["global"]["verdict"]), (0, "PASS"))
        x = criteres(r, "TRaynor01")["replis"]
        self.assertEqual(x["verdict"], "PASS")
        self.assertIn("sans réf. (non jugé) : sans_texture 4926", x["note"])
        self.assertEqual(silence(nonreg.main, ["maj-ref", str(s0.s), "--ref", str(s0.ref)]), 0)
        s1 = Sortie(sans_texture=5100)
        s1.ref = s0.ref
        rc, r = silence(s1.analyser)
        self.assertEqual((rc, criteres(r, "TRaynor01")["replis"]["verdict"]), (0, "PASS"))
        s2 = Sortie(sans_texture=10, cube=2)
        s2.ref = s0.ref
        rc, r = silence(s2.analyser)
        self.assertEqual(rc, 1)
        self.assertIn("nouveau(x) : cube 2", criteres(r, "TRaynor01")["replis"]["note"])
        s0.close(), s1.close(), s2.close()

    def test_empreintes_du_canon(self):
        s0, s1, rc, r = self._avec_ref(f10="b5f7db71833c")         # variante tolérée de la frame 10
        self.assertEqual(criteres(r, "canon")["empreintes"]["verdict"], "PASS")
        s0.close(), s1.close()
        s0, s1, rc, r = self._avec_ref(f300="0123456789ab")
        x = criteres(r, "canon")["empreintes"]
        self.assertEqual((x["verdict"], x["note"]), ("RÉGRESSION", "frame(s) 300"))
        s0.close(), s1.close()

    def test_canon_images_exactes(self):
        s0, s1, rc, r = self._avec_ref(canon_frames=599)
        self.assertEqual(criteres(r, "canon")["images"]["verdict"], "RÉGRESSION")
        s0.close(), s1.close()

    def test_victoire_perdue_et_controle_absent(self):
        s = Sortie(victoire=False)
        import shutil
        shutil.rmtree(s.s / "missions" / "THanson03A")
        rc, r = silence(s.analyser, False)
        self.assertEqual(rc, 1)
        self.assertEqual(criteres(r, "TRaynor01")["victoire"]["verdict"], "RÉGRESSION")
        self.assertIsNone(next(c for c in r["controles"] if c["nom"] == "TRaynor01")["images_s"])
        self.assertEqual(criteres(r, "THanson03A")["présent"]["verdict"], "RÉGRESSION")
        s.close()

    def test_run_partiel_refuse_en_maj_ref(self):
        s = Sortie()
        rc, r = silence(s.analyser, False, True)
        self.assertTrue(r["seul"])
        self.assertEqual(silence(nonreg.main, ["maj-ref", str(s.s), "--ref", str(s.ref)]), 4)
        s.close()


class TestsAB(unittest.TestCase):

    def test_paires_entrelacees(self):
        with tempfile.TemporaryDirectory() as t:
            s = Path(t)
            lignes = []
            for i, (fa, fb) in enumerate(((100.0, 102.0), (98.0, 99.0), (101.0, 103.0)), 1):
                for etiq, fps in (("A", fa), ("B", fb)):
                    run = f"{etiq}{i}"
                    (s / run).mkdir()
                    ech = "".join(f"[INPUT] banc t={t_:.2f}s tour {int(t_ * fps)} : CLIC (1, 1)\n"
                                  for t_ in range(55, 131, 5))
                    (s / run / "run.log").write_text(ech + bilan(frames=13000))
                    (s / run / "resume.txt").write_text("rc=0 en 140 s\n")
                    lignes.append(f"{run} {etiq} {i} /x/recomp-sc2 {'a' * 64}")
            (s / "ab.txt").write_text("\n".join(lignes) + "\n")
            self.assertEqual(silence(nonreg.main, ["ab", str(s), "--debut", "65"]), 0)
            r = json.loads((s / "banc-ab.json").read_text())
            self.assertEqual([x["ecart_pct"] for x in r["paires"]], [2.0, 1.02, 1.98])
            self.assertEqual(r["ecart_median_paires_pct"], 1.98)
            self.assertEqual(r["mediane"], {"A": 100.0, "B": 102.0})
            self.assertEqual(len(r["triplets"]), 2)                # A1 B1 A2, A2 B2 A3
            self.assertEqual(r["runs"][0]["plateau"]["debut"], 65.0)

    def test_paire_zero_de_chauffe_exclue(self):
        with tempfile.TemporaryDirectory() as t:
            s = Path(t)
            lignes = []
            for i, (fa, fb) in enumerate(((60.0, 100.0), (100.0, 101.0), (99.0, 100.0)), 0):   # A0 froid
                for etiq, fps in (("A", fa), ("B", fb)):
                    run = f"{etiq}{i}"
                    (s / run).mkdir()
                    ech = "".join(f"[INPUT] banc t={t_:.2f}s tour {int(t_ * fps)} : CLIC (1, 1)\n"
                                  for t_ in range(55, 131, 5))
                    (s / run / "run.log").write_text(ech + "[BILAN] [PIPECACHE] passe 10s (cache de pipelines) : actif, "
                                                     "x ; 0 octets chargés au démarrage ; 351 pipelines créés en 9580.1 ms"
                                                     " de vkCreateGraphicsPipelines (max 390.8 ms par appel) ; 1 sauvegarde\n")
                    (s / run / "resume.txt").write_text("rc=0 en 140 s\n")
                    lignes.append(f"{run} {etiq} {i} /x/recomp-sc2 {'a' * 64}")
            (s / "ab.txt").write_text("\n".join(lignes) + "\n")
            self.assertEqual(silence(nonreg.main, ["ab", str(s)]), 0)
            r = json.loads((s / "banc-ab.json").read_text())
            self.assertEqual([x["paire"] for x in r["paires"]], [1, 2])
            self.assertEqual(r["mediane"], {"A": 99.5, "B": 100.5})
            self.assertEqual(r["runs"][0]["pipelines_ms"], 9580.1)
            self.assertIn("| A0 (chauffe) |", (s / "banc-ab.md").read_text())


class TestsTolerances(unittest.TestCase):

    def test_fraction(self):
        self.assertEqual((nonreg.fraction("0.05"), nonreg.fraction("0"), nonreg.fraction(".5")), (0.05, 0.0, 0.5))
        for v in ("5", "50", "1", "-0.1", "5%", "nan", "inf", ""):
            with self.assertRaises(argparse.ArgumentTypeError, msg=v):
                nonreg.fraction(v)

    def test_analyser_refuse_une_tolerance_en_pourcent(self):
        s = Sortie()
        a = ["analyser", str(s.s), "--mode", "fumee", "--controles", "canon", "--binaire", str(SCRIPT)]
        for opt in ("--tol-fps", "--tol-images"):
            with self.assertRaises(SystemExit) as cm:
                silence(nonreg.main, a + [opt, "5"])
            self.assertEqual(cm.exception.code, 2)
        self.assertFalse((s.s / "nonreg.json").exists())
        s.close()

    def test_nonreg_sh_refuse_avant_le_banc(self):
        """Binaire inexistant : une tolérance valide passe le contrôle et bute sur le binaire ; aucun banc lancé."""
        with tempfile.TemporaryDirectory() as d:
            sortie = Path(d) / "sortie"
            for var, val, attendu in (("NONREG_TOL_FPS", "5", "NONREG_TOL_FPS « 5 »"),
                                      ("NONREG_TOL_IMAGES", "50", "NONREG_TOL_IMAGES « 50 »"),
                                      ("NONREG_TOL_FPS", "5%", "NONREG_TOL_FPS « 5% »"),
                                      ("NONREG_TOL_FPS", "0.03", "binaire « /nulle/part/recomp-sc2 »")):
                env = {k: v for k, v in os.environ.items() if not k.startswith("NONREG_")}
                env[var] = val
                r = subprocess.run(["/bin/bash", str(NONREG_SH), "/nulle/part/recomp-sc2", str(sortie)], env=env,
                                   capture_output=True, text=True, timeout=30)
                self.assertEqual(r.returncode, 2, (var, val, r.stderr))
                self.assertIn(attendu, r.stderr)
                self.assertFalse(sortie.exists())


def ppm(chemin, l=16, h=12, decale=0):
    """Petite image P6 synthétique (dégradé) ; decale > 0 change franchement les pixels de la 1re ligne."""
    px = bytearray()
    for y in range(h):
        for x in range(l):
            px += bytes(((x * 16) % 256, (y * 20) % 256, 128))
    for i in range(min(decale, l)):
        px[3 * i:3 * i + 3] = bytes((255 - px[3 * i], 255 - px[3 * i + 1], 0))
    chemin.write_bytes(f"P6 {l} {h} 255\n".encode() + bytes(px))


def outils_comparer():
    o = []
    if shutil.which("magick"):
        o.append(("magick", {}))
    if shutil.which("ffmpeg") and shutil.which("ffprobe"):
        o.append(("ffmpeg", {"COMPARER_SANS_MAGICK": "1"}))
    return o


@unittest.skipUnless(outils_comparer(), "ni magick ni ffmpeg")
class TestsComparerCaptures(unittest.TestCase):

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.d = Path(self._tmp.name)
        for n in ("a", "b", "diff", "ca", "cb"):
            (self.d / n).mkdir()
        ppm(self.d / "a" / "frame_00001.ppm")
        ppm(self.d / "b" / "frame_00001.ppm")
        ppm(self.d / "diff" / "frame_00001.ppm", decale=10)
        for n in ("ca", "cb"):
            (self.d / n / "frame_00001.png").write_bytes(b"pas une image")

    def tearDown(self):
        self._tmp.cleanup()

    def comparer(self, env, a, b, *opts, sortie=None):
        sortie = sortie or Path(tempfile.mkdtemp(dir=self.d))
        r = subprocess.run(["/bin/bash", str(COMPARER), str(self.d / a), str(self.d / b), "--sortie", str(sortie), *opts],
                           env={**os.environ, **env}, capture_output=True, text=True, timeout=60)
        return r.returncode, (r.stdout.strip().splitlines() or [""])[-1], r.stderr

    def test_identiques_ae_0(self):
        for nom, env in outils_comparer():
            rc, fin, err = self.comparer(env, "a", "b", "--max-ae", "0")
            self.assertEqual(rc, 0, (nom, err))
            self.assertIn("1/1 paire(s) comparée(s), 0 différente(s), AE max 0 px ; 0 en échec", fin, nom)
            self.assertIn("; planche ", fin, nom)

    def test_seuil_franchi(self):
        for nom, env in outils_comparer():
            rc, fin, err = self.comparer(env, "a", "diff", "--max-ae", "0")
            self.assertEqual(rc, 1, (nom, err))
            self.assertIn("1 différente(s), AE max 10 px ; 0 en échec", fin, nom)

    def test_paire_illisible_en_echec(self):
        for nom, env in outils_comparer():
            rc, fin, err = self.comparer(env, "ca", "cb", "--max-ae", "0")
            self.assertEqual(rc, 2, (nom, err))
            self.assertIn("0/1 paire(s) comparée(s), 0 différente(s), AE max 0 px ; 1 en échec", fin, nom)
            self.assertIn("pas de planche", fin, nom)
            self.assertIn("ÉCHEC", err, nom)

    def test_tmp_de_la_sortie_preserve(self):
        for nom, env in outils_comparer():
            s = Path(tempfile.mkdtemp(dir=self.d))
            (s / "tmp").mkdir()
            (s / "tmp" / "a-garder.txt").write_text("garder\n")
            rc, fin, err = self.comparer(env, "a", "b", sortie=s)
            self.assertEqual(rc, 0, (nom, err))
            self.assertEqual((s / "tmp" / "a-garder.txt").read_text(), "garder\n", nom)
            self.assertEqual(sorted(p.name for p in s.iterdir()), ["comparaison.tsv", "diff", "planche.png", "tmp"], nom)

    def test_seuil_illisible_refuse(self):
        for nom, env in outils_comparer():
            for opt in ("--max-ae", "--min-ae"):
                rc, fin, err = self.comparer(env, "a", "b", opt, "5x")
                self.assertEqual(rc, 2, (nom, opt))
                self.assertIn("seuil « 5x » : entier attendu", err, nom)


if __name__ == "__main__":
    unittest.main()
