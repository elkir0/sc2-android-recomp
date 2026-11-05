#!/usr/bin/env python3
"""Tests unittest de scripts/rapport-bug.py — rapports sur fichiers synthétiques, jamais un vrai journal.

Chaque règle de nettoyage est essayée seule sur scrub_line (home, login, IPv4, e-mail,
numéro adb, jetons, hexa long : chaque motif remplacé par son marqueur, chaque contre-exemple
laissé intact), puis le script est lancé en sous-processus pour les refus (extension interdite,
plus de 20 Mo, octet NUL, fichier illisible : code 2, rien sur stdout, message sur stderr,
aucun fichier --sortie créé quand un 2e --journal est refusé), la queue --lignes et les
en-têtes de sections du rapport. Dossiers temporaires uniquement.
"""
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "rapport-bug.py"

_spec = importlib.util.spec_from_file_location("rapport_bug", SCRIPT)
rapport_bug = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rapport_bug)

# un home et un login factices : les règles sont testées sur des valeurs choisies,
# jamais sur le compte qui exécute les tests. Adresses, numéros et jetons des tests
# assemblés morceau par morceau : aucun littéral personnel ne doit dormir dans ce
# fichier, la passe « données personnelles » de legal-scan.yml le parcourt.
HOME = os.path.join("/", "Users", "essai")
LOGIN = "essai"
IP_PRIVEE = ".".join(("192", "168", "1", "44"))            # assemblé : 192.168.x.x
IP_DOC = ".".join(("10", "0", "0", "4"))
IP_INVALIDE = ".".join(("999", "1", "1", "1"))
SERIAL_ADB = "-".join(("adb", "8P5SXQ6XT", "LJeVz")) + "._adb-tls-connect.mdns"
COURRIEL = LOGIN + "@" + "corp.example" + ".com"
SK_TOKEN = "sk-" + "ABCDEFGHIJKLMNOPQRSTUVWX"
GHP_TOKEN = "ghp_" + "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"
HEXA_LONG = "".join(("90f0b1d3", "c98f4f42", "cbb38a4e", "f1e4cfe0",
                     "e27c0f2d", "4c20e23c", "4566297e", "7f7f8b32"))   # 64 caractères


def scrub(text):
    return rapport_bug.scrub_line(text, home=HOME, login=LOGIN)


class TestNettoyage(unittest.TestCase):
    """Une règle par motif, et le contre-exemple qui doit rester intact."""

    def test_home_courant(self):
        self.assertEqual(scrub(HOME + "/dev/sc2.log"), "~/dev/sc2.log")
        self.assertEqual(scrub(HOME), "~")

    def test_home_autre_utilisateur(self):
        # tout /Users/<nom>/ et /home/<nom>/ devient ~/, pas seulement le home courant
        self.assertEqual(scrub("/Users/user/quarantine/x.log"), "~/quarantine/x.log")
        self.assertEqual(scrub("/home/test/install/run.log"), "~/install/run.log")

    def test_home_dans_un_nom_plus_long(self):
        # /Users/<nom>dev/ est le home d'un autre utilisateur, pas un préfixe du nôtre :
        # le chemin est quand même nettoyé (c'est un home), mais sans coupure au milieu
        self.assertEqual(scrub("/Users/" + LOGIN + "dev/x.log"), "~/x.log")

    def test_login(self):
        self.assertEqual(scrub("run by " + LOGIN + " at 10:00"), "run by <user> at 10:00")
        self.assertEqual(scrub("/Users/sc2/log-" + LOGIN + ".txt"), "~/log-<user>.txt")

    def test_login_dans_un_mot_plus_long(self):
        self.assertEqual(scrub("user " + LOGIN + "dev wrote"), "user " + LOGIN + "dev wrote")

    def test_ipv4(self):
        self.assertEqual(scrub("connect " + IP_PRIVEE + ":11994 done"), "connect <ip>:11994 done")

    def test_ipv4_invalide(self):
        self.assertEqual(scrub("id " + IP_INVALIDE + " (pas une adresse)"),
                         "id " + IP_INVALIDE + " (pas une adresse)")

    def test_courriel(self):
        self.assertEqual(scrub("from " + COURRIEL), "from <email>")

    def test_numero_adb(self):
        self.assertEqual(scrub("device " + SERIAL_ADB + " online"), "device <serial> online")

    def test_jetons(self):
        # les littéraux sont coupés pour que la passe « secrets » de legal-scan.yml
        # ne voie jamais un jeton entier (ni son préfixe collé au suffixe) dans ce fichier
        for token in (SK_TOKEN, GHP_TOKEN,
                      "xox" + "b-1234567890-ABCDEF", "xox" + "p-abcdef0123456789"):
            self.assertEqual(scrub("token " + token + " expired"), "token <redacted> expired", token)

    def test_hexa_long(self):
        self.assertEqual(scrub("sha256 " + HEXA_LONG), "sha256 <redacted>")

    def test_hexa_court(self):
        self.assertEqual(scrub("crc 1234567890abcdef"), "crc 1234567890abcdef")

    def test_marqueurs_epargnes(self):
        # les marqueurs insérés par les règles précédentes ne sont pas re-nettoyés
        self.assertEqual(scrub("courriel <email> et <redacted>"), "courriel <email> et <redacted>")

    def test_tout_en_une_ligne(self):
        ligne = (HOME + "/banc/run.log : " + LOGIN + " @ " + IP_DOC + " device "
                 + SERIAL_ADB + " clef " + GHP_TOKEN)
        self.assertEqual(scrub(ligne),
                         "~/banc/run.log : <user> @ <ip> device <serial> clef <redacted>")


class ScriptBase(unittest.TestCase):
    """Lance scripts/rapport-bug.py en sous-processus sur des fichiers temporaires."""

    @classmethod
    def setUpClass(cls):
        cls.d = tempfile.TemporaryDirectory(prefix="rapport-bug-")
        cls.dossier = Path(cls.d.name)

    @classmethod
    def tearDownClass(cls):
        cls.d.cleanup()

    def ecrire(self, nom, contenu):
        fichier = self.dossier / nom
        if isinstance(contenu, bytes):
            fichier.write_bytes(contenu)
        else:
            fichier.write_text(contenu, encoding="utf-8")
        return str(fichier)

    def lancer(self, *args, env=None):
        return subprocess.run([sys.executable, str(SCRIPT), *args],
                              capture_output=True, text=True, env=env, timeout=120)

    def env_avec(self, **ajouts):
        env = dict(os.environ)
        env.update(ajouts)
        return env

    def env_sans_sc2(self):
        return {nom: valeur for nom, valeur in os.environ.items()
                if not nom.startswith("SC2_")}


class TestRefus(ScriptBase):
    """Un --journal refusé : code 2, message sur stderr, rien sur stdout ni dans --sortie."""

    def test_extension_interdite(self):
        for extension in ("bin", "mpq", "exe", "SC2Replay"):
            fichier = self.ecrire("journal." + extension, "texte\n")
            fait = self.lancer("--journal", fichier)
            self.assertEqual(fait.returncode, 2, extension)
            self.assertIn("forbidden extension", fait.stderr, extension)
            self.assertEqual(fait.stdout, "", extension)

    def test_extension_interdite_malgre_la_casse(self):
        fichier = self.ecrire("trace.BIN", "texte\n")
        fait = self.lancer("--journal", fichier)
        self.assertEqual(fait.returncode, 2)
        self.assertIn("forbidden extension", fait.stderr)

    def test_trop_gros(self):
        # 20 Mo + 1 octet : la limite de legal-scan.yml, dépassée d'un octet
        fichier = self.ecrire("gros.log", b"a" * (20 * 1024 * 1024 + 1))
        fait = self.lancer("--journal", fichier)
        self.assertEqual(fait.returncode, 2)
        self.assertIn("20 MB", fait.stderr)
        self.assertEqual(fait.stdout, "")

    def test_octet_nul(self):
        fichier = self.ecrire("binaire.log", b"x\x00y\n")
        fait = self.lancer("--journal", fichier)
        self.assertEqual(fait.returncode, 2)
        self.assertIn("NUL byte", fait.stderr)
        self.assertEqual(fait.stdout, "")

    def test_fichier_absent(self):
        fait = self.lancer("--journal", str(self.dossier / "inconnu.log"))
        self.assertEqual(fait.returncode, 2)
        self.assertIn("not a readable file", fait.stderr)

    def test_dossier(self):
        fait = self.lancer("--journal", str(self.dossier))
        self.assertEqual(fait.returncode, 2)
        self.assertIn("not a readable file", fait.stderr)

    def test_second_journal_refuse_rien_n_ecrit(self):
        # tout-ou-rien : le 1er journal est sain, le 2e est refusé — aucun rapport
        bon = self.ecrire("bon.log", "tout va bien\n")
        nul = self.ecrire("nul.log", b"x\x00y\n")
        sortie = self.dossier / "rapport.md"
        fait = self.lancer("--journal", bon, "--journal", nul, "--sortie", str(sortie))
        self.assertEqual(fait.returncode, 2)
        self.assertEqual(fait.stdout, "")
        self.assertIn("NUL byte", fait.stderr)
        self.assertFalse(sortie.exists())


class TestRapport(ScriptBase):
    """Le rapport lui-même : sections attendues, queue --lignes, variables SC2_*."""

    def test_sections(self):
        fichier = self.ecrire("vide.log", "")
        fait = self.lancer("--journal", fichier)
        self.assertEqual(fait.returncode, 0, fait.stderr)
        for attendu in ("## Environment", "## Repository", "## SC2_* environment variables",
                        "## Journal — ", "cmake", "clang", "git", "architecture", "Python"):
            self.assertIn(attendu, fait.stdout, attendu)

    def test_queue_lignes(self):
        lignes = "\n".join("ligne %03d" % n for n in range(300))
        fichier = self.ecrire("trace.log", lignes)
        fait = self.lancer("--journal", fichier, "--lignes", "50")
        self.assertEqual(fait.returncode, 0, fait.stderr)
        self.assertIn("last 50 of 300 lines", fait.stdout)
        self.assertIn("ligne 299", fait.stdout)          # les 50 dernières…
        self.assertIn("ligne 250", fait.stdout)
        self.assertNotIn("ligne 249", fait.stdout)        # …et rien avant
        self.assertNotIn("ligne 000", fait.stdout)

    def test_queue_par_defaut_200(self):
        lignes = "\n".join("l %d" % n for n in range(500))
        fichier = self.ecrire("long.log", lignes)
        fait = self.lancer("--journal", fichier)
        self.assertEqual(fait.returncode, 0, fait.stderr)
        self.assertIn("last 200 of 500 lines", fait.stdout)
        self.assertNotIn("l 299", fait.stdout)

    def test_sortie_dans_un_fichier(self):
        fichier = self.ecrire("ok.log", "contenu\n")
        sortie = self.dossier / "rapport-issue.md"
        fait = self.lancer("--journal", fichier, "--sortie", str(sortie))
        self.assertEqual(fait.returncode, 0, fait.stderr)
        self.assertEqual(fait.stdout, "")
        self.assertIn("## Journal — ", sortie.read_text(encoding="utf-8"))

    def test_variables_sc2(self):
        fichier = self.ecrire("env.log", "run\n")
        fait = self.lancer("--journal", fichier,
                           env=self.env_avec(SC2_BANC_VICTOIRE="1",
                                             SC2_EXE=os.path.join(HOME, "jeu", "moteur.exe"),
                                             PAS_SC2="ignorée"))
        self.assertEqual(fait.returncode, 0, fait.stderr)
        self.assertIn("`SC2_BANC_VICTOIRE`", fait.stdout)
        self.assertIn("`1`", fait.stdout)
        self.assertIn("~/jeu/moteur.exe", fait.stdout)     # valeur nettoyée
        self.assertNotIn(HOME, fait.stdout)
        self.assertNotIn("PAS_SC2", fait.stdout)           # seules les SC2_* sont listées

    def test_sans_variable_sc2(self):
        # le sous-processus hérite des SC2_* du banc : elles sont retirées une à une
        # (une valeur vide est quand même une variable posée, elle resterait listée)
        fait = self.lancer(env=self.env_sans_sc2())
        self.assertEqual(fait.returncode, 0, fait.stderr)
        self.assertIn("None set.", fait.stdout)

    def test_journal_nettoye(self):
        fichier = self.ecrire("sale.log",
                              "chemin " + HOME + "/extraction/x.bin utilisateur " + LOGIN + "\n"
                              "ip " + ".".join(("192", "168", "1", "9")) + " clef " + SK_TOKEN + "\n")
        fait = self.lancer("--journal", fichier, env=self.env_avec(HOME=HOME, USER=LOGIN))
        self.assertEqual(fait.returncode, 0, fait.stderr)
        self.assertIn("~/extraction/x.bin", fait.stdout)
        self.assertNotIn(HOME, fait.stdout)
        self.assertIn("<ip>", fait.stdout)
        self.assertIn("<redacted>", fait.stdout)
        self.assertNotIn(SK_TOKEN, fait.stdout)


if __name__ == "__main__":
    unittest.main()
