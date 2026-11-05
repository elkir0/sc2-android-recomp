#!/usr/bin/env python3
"""Tests unittest de scripts/garde-adaptateur.sh (V2.5a, 23/10) — run.log et Variables.txt synthétiques, jamais de run.

Les fixtures reprennent les lignes exactes du runtime (recomp-sc2/runtime/sc2_iat_p4.c) : refus « [DDRAW]
DirectDrawCreateEx depuis 0x00A010AA → DDERR_NODIRECTDRAWHW (pas de DirectDraw) » du profil historique, et lignes de
l'objet factice « … → IDirectDraw7 factice 0x… » / « IDirectDraw7::GetAvailableVidMem(…) » quand DirectDraw est servi ;
Variables.txt en fins de ligne CRLF, comme l'écrit le jeu.
"""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "garde-adaptateur.sh"

REFUS = "[DDRAW] DirectDrawCreateEx depuis 0x00A010AA → DDERR_NODIRECTDRAWHW (pas de DirectDraw)\n"
SERVI = ("[DDRAW] DirectDrawCreateEx depuis 0x00A010AA → IDirectDraw7 factice 0x3801F2A0 (DeviceId 0x05E3, VRAM 1024 Mo "
         "locale, 0 non locale ; vtable 0x3801F220 : n° 2 Release et n° 23 GetAvailableVidMem servis, autres → arrêt "
         "0xF2DD0000 + 16 × n°)\n"
         "[DDRAW] IDirectDraw7::GetAvailableVidMem(caps 0x10004000) → 1024 Mo (total = libre) depuis 0x00A010C3\n")
GPU = "[GPU] profil historique : « SC2 Partout D3D9 factice (passe 4) », VendorId 0x10DE DeviceId 0x0402\n"
VARS_LOW = "aolight=0\r\nGraphicsOptionTextureQuality[2]=0\r\nlastDeviceId=1026\r\n"
VARS_ULTRA = "aolight=0\r\nGraphicsOptionTextureQuality[2]=3\r\nlastDeviceId=1507\r\ntexQualityLevel=3\r\n"


class GardeAdaptateur(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def lancer(self, log, variables=None, env=None, sans_variables_arg=False):
        (self.d / "run.log").write_text(GPU + log, encoding="utf-8")
        args = [str(SCRIPT), str(self.d / "run.log")]
        if not sans_variables_arg:
            v = self.d / "Variables-jeu.txt"
            if variables is not None:
                v.write_bytes(variables.encode("utf-8"))
            args.append(str(v))
        e = {k: v for k, v in os.environ.items() if k not in ("SC2_GPU_PROFIL", "SC2_DDRAW")}
        e.update(env or {})
        p = subprocess.run(["bash"] + args, capture_output=True, text=True, env=e, timeout=30)
        return p.returncode, p.stdout.strip()

    def test_canon_ok(self):
        rc, s = self.lancer(REFUS, VARS_LOW)
        self.assertEqual(rc, 0, s)
        self.assertTrue(s.startswith("adaptateur : OK"), s)
        self.assertIn("GraphicsOptionTextureQuality[2]=0, lastDeviceId=1026", s)

    def test_canon_historique_explicite_ok(self):
        rc, s = self.lancer(REFUS, VARS_LOW, env={"SC2_GPU_PROFIL": "historique"})
        self.assertEqual(rc, 0, s)

    def test_canon_sans_variables_ok(self):        # canon épinglé : Variables.txt semé, pas jugé
        rc, s = self.lancer(REFUS, sans_variables_arg=True)
        self.assertEqual(rc, 0, s)
        self.assertNotIn("Variables", s)

    def test_directdraw_servi_en_historique(self):  # mutant M1 de la revue : servi, textures Ultra suggérées
        rc, s = self.lancer(SERVI, VARS_ULTRA)
        self.assertEqual(rc, 5, s)
        self.assertTrue(s.startswith("adaptateur : ÉCHEC"), s)
        self.assertIn("aucun refus", s)
        self.assertIn("IDirectDraw7", s)
        self.assertIn("GraphicsOptionTextureQuality[2]=3 (attendu 0)", s)
        self.assertIn("lastDeviceId=1507 (attendu 1026)", s)

    def test_refus_et_objet(self):                  # refus vu, mais un objet aussi : échec
        rc, s = self.lancer(REFUS + SERVI, VARS_LOW)
        self.assertEqual(rc, 5, s)

    def test_textures_seules(self):
        rc, s = self.lancer(REFUS, VARS_LOW.replace("TextureQuality[2]=0", "TextureQuality[2]=1"))
        self.assertEqual(rc, 5, s)
        self.assertIn("GraphicsOptionTextureQuality[2]=1 (attendu 0)", s)

    def test_variables_absent(self):
        rc, s = self.lancer(REFUS, None)
        self.assertEqual(rc, 5, s)
        self.assertIn("absent", s)

    def test_cle_absente(self):
        rc, s = self.lancer(REFUS, "aolight=0\r\nlastDeviceId=1026\r\n")
        self.assertEqual(rc, 5, s)
        self.assertIn("GraphicsOptionTextureQuality[2]=absente", s)

    def test_dx10_non_controle(self):
        rc, s = self.lancer(SERVI, VARS_ULTRA, env={"SC2_GPU_PROFIL": "dx10"})
        self.assertEqual(rc, 0, s)
        self.assertTrue(s.startswith("adaptateur : non contrôlé"), s)

    def test_forcage_non_controle(self):
        for v in ("0", "1"):
            rc, s = self.lancer(SERVI, VARS_ULTRA, env={"SC2_DDRAW": v})
            self.assertEqual(rc, 0, s)
            self.assertIn("non contrôlé", s)

    def test_usage(self):
        p = subprocess.run(["bash", str(SCRIPT)], capture_output=True, text=True, timeout=30)
        self.assertEqual(p.returncode, 2)


if __name__ == "__main__":
    unittest.main()
