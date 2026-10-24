#!/usr/bin/env python3
"""Tests unittest de scripts/campagne-rapport.py — run.log et resume.txt synthétiques, jamais de run réel.

Les fixtures reprennent les formats exacts du runtime : ligne « [INPUT] banc t=…s tour … : VICTOIRE détectée »
(sc2_entrees_banc.c), « [BILAN] frames présentées (Present) : N » et « [BILAN] draws : N RÉELS …, M « lumière » »
(sc2_d3d9.c), « [BILAN] [MANQUE] 0x…  1re frame …  occurrences …  appelant 0x… » (sc2_runtime.c, bilan
SC2_MANQUE_CONTINUE) et « [ARRÊT] [thread N] <raison> » (sc2_stop). Le module est chargé par chemin (nom à
tirets, non importable). V1.4 (22/10) : replis de texture (« [BILAN] samplers des draws réels : … » et
« [BILAN]   repli : texture … — <cause> ×N », sc2_d3d9.c), échantillons « [INPUT] banc t=…s tour N ».
"""
import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "campagne-rapport.py"
spec = importlib.util.spec_from_file_location("campagne_rapport", SCRIPT)
campagne_rapport = importlib.util.module_from_spec(spec)
spec.loader.exec_module(campagne_rapport)

LOG_VICTOIRE = """\
[INPUT] banc : SC2_BANC_ENTREES = 60 événement(s), 0 rejeté(s) ; horloge = secondes depuis l'activation (f<n> : tours de pompe)
[INPUT] banc t=63.25s tour 2048 : VICTOIRE détectée (The Outlaws Victory.SC2Save)
[INPUT] banc t=63.25s : 9 élément(s) absolu(s) restant(s) annulé(s) — la suite est relative à la victoire
[INPUT] banc t=69.31s tour 2240 : CLIC (998, 674)
[BILAN] frames présentées (Present) : 5210
[BILAN] draws : 31245 RÉELS (shaders traduits), 120 « lumière » (sans SPIR-V K, sans déclaration 0, sans données 0) ; table SPIR-V embarquée : 2856 shaders
[BILAN] [MANQUE] 2 adresse(s) manquante(s) contournée(s) (SC2_MANQUE_CONTINUE) ; ordre d'apparition :
[BILAN] [MANQUE] 0x00A1B2C3  1re frame 412  occurrences 3401  1er dispatch 991234  ret 0x0099A3CC  appelant 0x0081F400
[BILAN] [MANQUE] 0x00B2C3D4  1re frame 500  occurrences 17  1er dispatch 998001  ret 0x0099A3CC  appelant 0x00821000
[MANQUE-CONTINUE] MANQUE : aucune fonction liftée à 0x00A1B2C3 n=1 frame 412 ret 0x0099A3CC appelant 0x0081F400 ecx 00000000 edx 00000000 [ecx] 00000000
[BILAN] [INPUT] banc (SC2_BANC_ENTREES) : 60/60 événement(s) appliqué(s), 0 rejeté(s), 2240 tours de pompe, 185.4 s
[BILAN] [INPUT] banc victoire (QUAND:VICTOIRE) : t=63.25s (The Outlaws Victory.SC2Save) ; 9 élément(s) absolu(s) annulé(s)

[ARRÊT] [thread 456] SC2_BANC_ENTREES : QUIT à t=185.4s
[ARRÊT] pile d'appels invitée (fantôme, la plus récente en haut) :
    #0   0x0099A3CC
"""

LOG_SANS_VICTOIRE = """\
[INPUT] banc : SC2_BANC_ENTREES = 60 événement(s), 0 rejeté(s) ; horloge = secondes depuis l'activation (f<n> : tours de pompe)
[BILAN] frames présentées (Present) : 8902
[BILAN] draws : 5 RÉELS (shaders traduits), 7000 « lumière » (sans SPIR-V K, sans déclaration 0, sans données 0) ; table SPIR-V embarquée : 2856 shaders
[BILAN] [INPUT] banc (SC2_BANC_ENTREES) : 46/60 événement(s) appliqué(s), 0 rejeté(s), 3000 tours de pompe, 301.7 s

[ARRÊT] [thread 456] SC2_FRAMES_MAX=9000 : 9000 frames présentées par la boucle de rendu — arrêt volontaire du run
"""

LOG_ARRET_FAUTE = """\
[INPUT] banc : SC2_BANC_ENTREES = 60 événement(s), 0 rejeté(s)
[MANQUE-CONTINUE] MANQUE : saut hors image vers 0x00C0FFEE n=1 frame 88 ret 0x0099A3CC appelant 0x0081F400 ecx 0 edx 0 [ecx] 0
[MANQUE-CONTINUE] MANQUE : saut hors image vers 0x00C0FFEE n=2 frame 90 ret 0x0099A3CC appelant 0x0081F400 ecx 0 edx 0 [ecx] 0

[ARRÊT] [thread 789] faute dans un accesseur : retour hôte [rbp+8] = 0x0
[ARRÊT] pile d'appels invitée (fantôme, la plus récente en haut) :
    #0   0x0107F9F0
"""


LOG_REPLIS_NULS = LOG_VICTOIRE + """\
[BILAN] samplers des draws réels : 16093723 servis (texture ou cible), replis sur texture factice : 0 sans texture liée, 0 cube, 0 texture sans données, 0 format non géré
"""

LOG_REPLIS_SANS_DONNEES = LOG_VICTOIRE + """\
[BILAN] samplers des draws réels : 4788 servis (texture ou cible), replis sur texture factice : 0 sans texture liée, 5 cube, 340 texture sans données, 0 format non géré
[BILAN]   repli : texture 2048×2048 D3DFMT 0x50 (genre 3, usage 0x2, pool 0) — sans données ×300
[BILAN]   repli : texture 64×64 D3DFMT 0x15 (genre 3, usage 0x0, pool 1) — sans données ×40
[BILAN]   repli : texture 32×32 D3DFMT 0x15 (genre 5, usage 0x0, pool 1) — cube ×5
"""

# bilan tronqué : lignes de détail sans le résumé (ne devrait pas arriver, mais le détail suffit alors)
LOG_REPLIS_DETAIL_SEUL = LOG_SANS_VICTOIRE + """\
[BILAN]   repli : texture 128×128 D3DFMT 0x31545844 (genre 3, usage 0x0, pool 1) — format non géré ×7
"""


class MissionTmp:
    """Dossier de mission synthétique sous un répertoire temporaire."""

    def __init__(self, carte, log=None, resume=None, n_shaders=0):
        self.tmp = tempfile.TemporaryDirectory()
        self.racine = Path(self.tmp.name)
        self.dossier = self.racine / carte
        self.dossier.mkdir()
        if log is not None:
            (self.dossier / "run.log").write_text(log)
        if resume is not None:
            (self.dossier / "resume.txt").write_text(resume)
        if n_shaders:
            sh = self.dossier / "shaders"
            sh.mkdir()
            for k in range(n_shaders):
                (sh / f"ps_{k:016x}.bin").write_bytes(b"\x00")

    def close(self):
        self.tmp.cleanup()


class TestsCampagneRapport(unittest.TestCase):

    def test_victoire_detectee(self):
        m = MissionTmp("TRaynor02", LOG_VICTOIRE, "rc=0 en 186 s")
        r = campagne_rapport.lire_mission(m.dossier)
        self.assertEqual(r["victoire"], 63.25)
        self.assertEqual(r["fichier"], "The Outlaws Victory.SC2Save")
        m.close()

    def test_pas_de_victoire(self):
        m = MissionTmp("TTosh01", LOG_SANS_VICTOIRE, "rc=0 en 302 s")
        r = campagne_rapport.lire_mission(m.dossier)
        self.assertIsNone(r["victoire"])
        out = campagne_rapport.rapport(m.racine)
        self.assertIn("| TTosh01 | 0 | non |", out)
        m.close()

    def test_rc_et_duree_du_resume(self):
        m = MissionTmp("TRaynor01", LOG_VICTOIRE, "rc=0 en 186 s")
        r = campagne_rapport.lire_mission(m.dossier)
        self.assertEqual((r["rc"], r["duree"]), (0, 186))
        m.close()

    def test_duree_de_secours_du_bilan_sans_resume(self):
        m = MissionTmp("THanson01", LOG_SANS_VICTOIRE, None)
        r = campagne_rapport.lire_mission(m.dossier)
        self.assertEqual(r["duree"], 301.7)
        m.close()

    def test_frames_et_draws(self):
        m = MissionTmp("TRaynor03", LOG_VICTOIRE, "rc=0 en 186 s")
        r = campagne_rapport.lire_mission(m.dossier)
        self.assertEqual((r["frames"], r["reels"], r["lumiere"]), (5210, 31245, 120))
        out = campagne_rapport.rapport(m.racine)
        self.assertIn("| 5210 | 31245 / 120 |", out)
        m.close()

    def test_manque_du_bilan(self):
        m = MissionTmp("THorner01", LOG_VICTOIRE, "rc=0 en 186 s")
        r = campagne_rapport.lire_mission(m.dossier)
        self.assertEqual(len(r["manque"]), 2)
        self.assertEqual(r["manque"][0], ("0x00A1B2C3", 3401, "0x0081F400"))
        self.assertEqual(r["manque"][1], ("0x00B2C3D4", 17, "0x00821000"))
        out = campagne_rapport.rapport(m.racine)
        self.assertIn("| 2 (3418 occ.) |", out)                       # distinctes + occurrences totales
        self.assertIn("0x00A1B2C3 (appelant 0x0081F400) : 3401 occurrence(s) — THorner01", out)
        m.close()

    def test_manque_de_secours_sans_bilan(self):
        m = MissionTmp("TValerian01", LOG_ARRET_FAUTE, "rc=3 en 45 s")
        r = campagne_rapport.lire_mission(m.dossier)
        self.assertEqual(r["manque"], [("0x00C0FFEE", 0, "0x0081F400")])   # occurrences inconnues : 0
        out = campagne_rapport.rapport(m.racine)
        self.assertIn("0x00C0FFEE (appelant 0x0081F400) : 0 occurrence(s) — TValerian01", out)
        m.close()

    def test_arrets_hors_volontaires(self):
        for log, attendu in ((LOG_VICTOIRE, []), (LOG_SANS_VICTOIRE, []), (LOG_ARRET_FAUTE, ["faute dans un accesseur : retour hôte [rbp+8] = 0x0"])):
            m = MissionTmp("TTychus01", log, "rc=0 en 1 s")
            self.assertEqual(campagne_rapport.lire_mission(m.dossier)["arrets"], attendu)
            m.close()
        m = MissionTmp("TTychus02", LOG_ARRET_FAUTE, "rc=3 en 1 s")
        out = campagne_rapport.rapport(m.racine)
        self.assertIn("| 1 (faute dans un accesseur : retour hôte [r…", out)   # raison tronquée à 40 caractères
        m.close()

    def test_shaders_comptes_du_dossier(self):
        m = MissionTmp("TZeratul01", LOG_VICTOIRE, "rc=0 en 186 s", n_shaders=3)
        self.assertEqual(campagne_rapport.lire_mission(m.dossier)["shaders"], 3)
        out = campagne_rapport.rapport(m.racine)
        self.assertIn("| 31245 / 120 | 2 (3418 occ.) | 0 | 3 |", out)   # la ligne se termine par les shaders
        m.close()

    def test_mission_vide_et_table_complete(self):
        avec = MissionTmp("TRaynor01", LOG_VICTOIRE, "rc=0 en 186 s", n_shaders=1)
        vide = MissionTmp("TArcade00", None, None)                       # ni run.log ni resume.txt
        vide.dossier.rmdir()                                             # ... ni même les fichiers : dossier vide
        (avec.racine / "TArcade00").mkdir()
        out = campagne_rapport.rapport(avec.racine)
        lignes = [l for l in out.splitlines() if l.startswith("| T")]
        self.assertEqual(len(lignes), 2)                                 # une ligne par mission, ordre des cartes
        self.assertIn("| TArcade00 | — | non | — | — | — / — | 0 | 0 | 0 |", out)
        self.assertIn("2 mission(s), 1 victoire(s) détectée(s).", out)
        avec.close()
        vide.close()


    def test_replis_nuls(self):
        m = MissionTmp("TRaynor01", LOG_REPLIS_NULS, "rc=0 en 186 s")
        r = campagne_rapport.lire_mission(m.dossier)
        self.assertEqual(r["replis"], {"servis": 16093723, "sans_texture": 0, "cube": 0, "sans_donnees": 0, "format": 0})
        out = campagne_rapport.rapport(m.racine)
        self.assertIn("| replis de texture |", out.splitlines()[0])
        self.assertIn("| 2 (3418 occ.) | 0 | 0 | 0 |", out)               # arrêts, shaders, replis
        self.assertNotIn("sans données", out)
        m.close()

    def test_replis_sans_donnees_signales(self):
        m = MissionTmp("THanson03A", LOG_REPLIS_SANS_DONNEES, "rc=0 en 186 s")
        r = campagne_rapport.lire_mission(m.dossier)
        self.assertEqual((r["replis"]["sans_donnees"], r["replis"]["cube"]), (340, 5))
        self.assertEqual([(x["l"], x["cause"], x["n"]) for x in r["replis_detail"]],
                         [(2048, "sans_donnees", 300), (64, "sans_donnees", 40), (32, "cube", 5)])
        out = campagne_rapport.rapport(m.racine)
        self.assertIn("| sans données 340 (2 tex.), cube 5 (1 tex.) |", out)
        self.assertIn("Replis « sans données » : 0 exigé — 1 mission(s) en défaut : THanson03A.", out)
        m.close()

    def test_replis_inconnus_sans_bilan(self):
        m = MissionTmp("TTosh01", LOG_VICTOIRE, "rc=0 en 186 s")                 # pas de ligne « samplers »
        self.assertIsNone(campagne_rapport.lire_mission(m.dossier)["replis"])
        self.assertIn("| 2 (3418 occ.) | 0 | 0 | — |", campagne_rapport.rapport(m.racine))
        m.close()

    def test_replis_du_detail_seul(self):
        m = MissionTmp("TTosh02", LOG_REPLIS_DETAIL_SEUL, "rc=0 en 302 s")
        r = campagne_rapport.lire_mission(m.dossier)
        self.assertEqual((r["replis"]["format"], r["replis"]["sans_donnees"]), (7, 0))
        self.assertIn("| format non géré 7 (1 tex.) |", campagne_rapport.rapport(m.racine))
        m.close()

    def test_echantillons_et_total_du_banc(self):
        m = MissionTmp("TRaynor01", LOG_VICTOIRE, "rc=0 en 186 s")
        r = campagne_rapport.lire_mission(m.dossier)
        # « t=63.25s : 9 élément(s)… » n'a pas de tour : pas un échantillon
        self.assertEqual(r["echantillons"], [(63.25, 2048), (69.31, 2240)])
        self.assertEqual(r["victoire_tour"], 2048)
        self.assertEqual((r["banc_tours"], r["banc_s"]), (2240, 185.4))
        m.close()


if __name__ == "__main__":
    unittest.main()
