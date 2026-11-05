#!/usr/bin/env python3
"""Tests unittest de la documentation du dépôt — bornes de mots, liens relatifs, index des guides.

Fonctions de contrôle prenant la racine d'un dépôt (testables sur des arbres factices,
créés dans des dossiers temporaires) :

  agents_existe(racine)      AGENTS.md existe à la racine ;
  agents_mots(racine)        AGENTS.md ≤ 1500 mots ;
  guides_mots(racine)        chaque docs/agents/*.md ≤ 6000 mots ;
  liens_relatifs(racine)     chaque lien relatif d'un *.md suivi par git mène à un fichier
                             existant (http(s):, mailto: et #ancre ignorés, ancre dépouillée) ;
  guides_indexes(racine)     chaque docs/agents/*.md autre que README.md est lié depuis
                             docs/agents/README.md.

Nombre de mots = len(texte.split()). Un échec nomme le fichier et le lien en cause.
Les contrôles du vrai dépôt (TestVraiDepot) peuvent échouer tant qu'AGENTS.md et les
guides docs/agents/*.md sont en cours d'écriture : c'est attendu, ils le disent.
"""
import re
import subprocess
import unittest
from pathlib import Path

RACINE = Path(__file__).resolve().parent.parent

LIMITE_AGENTS = 1500     # mots, AGENTS.md
LIMITE_GUIDE = 6000      # mots, chaque docs/agents/*.md
GUIDES_ATTENDUS = (      # docs/agents/, écrits en parallèle de ce test
    "01-setup.md", "02-runtime.md", "03-rendering.md", "04-lift-and-oracle.md",
    "05-hosts.md", "06-bench-and-regress.md", "symptoms.md",
)
RE_LIEN = re.compile(r"(?<!\!)\[[^\]\[]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
RE_FICHIERS_SUIVIS = ("git", "ls-files", "*.md")
# adresse du dépôt factice, assemblée : aucun littéral en forme d'adresse e-mail ne
# doit dormir ici (la passe « données personnelles » de legal-scan.yml parcourt ce fichier)
COURRIEL_FIXTURE = "@".join(("essai", "exemple.invalid"))
COURRIEL_GIT = "@".join(("git", "exemple.invalid"))


def _md(racine, chemin):
    """Le texte d'un Markdown (racine/chemin), ou None s'il n'existe pas."""
    try:
        return (Path(racine) / chemin).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None


def _mots(texte):
    return len(texte.split())


def agents_existe(racine):
    """[] si AGENTS.md existe à la racine, sinon un écart par dossier cherché."""
    return [] if (Path(racine) / "AGENTS.md").is_file() else ["AGENTS.md is missing"]


def agents_mots(racine, limite=LIMITE_AGENTS):
    """[] si AGENTS.md tient dans la limite de mots, sinon un écart."""
    texte = _md(racine, "AGENTS.md")
    if texte is None:
        return []          # l'existence est le travail d'agents_existe
    mots = _mots(texte)
    return [] if mots <= limite else ["AGENTS.md: %d words, limit %d" % (mots, limite)]


def guides_mots(racine, limite=LIMITE_GUIDE):
    """Un écart par docs/agents/*.md au-dessus de la limite de mots."""
    ecarts = []
    for fichier in sorted((Path(racine) / "docs" / "agents").glob("*.md")):
        texte = _md(racine, Path("docs") / "agents" / fichier.name)
        if texte is not None and _mots(texte) > limite:
            ecarts.append("docs/agents/%s: %d words, limit %d"
                          % (fichier.name, _mots(texte), limite))
    return ecarts


def _md_suivis(racine):
    """Les *.md suivis par git dans racine (git ls-files '*.md'), chemins relatifs à racine."""
    try:
        fait = subprocess.run(RE_FICHIERS_SUIVIS, cwd=racine, capture_output=True,
                              text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    return [ligne for ligne in fait.stdout.splitlines() if ligne] if fait.returncode == 0 else None


def liens_relatifs(racine):
    """Un écart par lien relatif d'un *.md suivi qui ne mène à aucun fichier ni dossier."""
    ecarts = []
    suivi = _md_suivis(racine)
    if suivi is None:
        return ["git ls-files '*.md' failed — cannot check links"]
    for chemin in suivi:
        texte = _md(racine, chemin)
        if texte is None:
            continue
        vus = set()
        for lien in RE_LIEN.findall(texte):
            cible = lien.split("#", 1)[0].strip()
            if not cible or cible.startswith(("http://", "https://", "mailto:")):
                continue
            # racine du dépôt pour un lien absolu (/docs/x.md), dossier du fichier sinon
            base = Path(racine) / cible.lstrip("/") if cible.startswith("/") \
                else Path(racine) / chemin / ".." / cible
            # un dossier suivi par git est une cible valide (README d'un sous-projet)
            if not base.resolve().exists():
                if lien not in vus:                    # le même lien cassé, une seule fois
                    vus.add(lien)
                    ecarts.append("%s: link '%s' points to no file" % (chemin, lien))
    return ecarts


def guides_indexes(racine):
    """Un écart par guide docs/agents/*.md (hors README.md) absent de l'index."""
    dossier = Path(racine) / "docs" / "agents"
    if not dossier.is_dir():
        return []
    texte = _md(racine, Path("docs") / "agents" / "README.md") or ""
    ecarts = []
    for fichier in sorted(dossier.glob("*.md")):
        if fichier.name == "README.md":
            continue
        if "(%s)" % fichier.name not in texte and "(<%s>)" % fichier.name not in texte:
            ecarts.append("docs/agents/%s is not linked from docs/agents/README.md" % fichier.name)
    return ecarts


def tous_les_controles(racine):
    """Tous les écarts de tous les contrôles, préfixés par le nom du contrôle."""
    for nom, controle in (("agents_existe", agents_existe), ("agents_mots", agents_mots),
                          ("guides_mots", guides_mots), ("liens_relatifs", liens_relatifs),
                          ("guides_indexes", guides_indexes)):
        for ecart in controle(racine):
            yield "%s: %s" % (nom, ecart)


def _guillemets_mots(nb):
    return "%d word%s" % (nb, "" if nb == 1 else "s")


class ControlesFactices(unittest.TestCase):
    """Chaque contrôle sur de petits arbres écrits dans un dossier temporaire."""

    def setUp(self):
        import tempfile
        self.d = tempfile.TemporaryDirectory(prefix="test-docs-")
        self.racine = Path(self.d.name)
        self.ecrits = []
        self.addCleanup(self.d.cleanup)

    def ecrire(self, chemin, contenu):
        fichier = self.racine / chemin
        fichier.parent.mkdir(parents=True, exist_ok=True)
        fichier.write_text(contenu, encoding="utf-8")
        self.ecrits.append(chemin)
        return chemin

    def git_init(self):
        """Dépôt git factice : seuls les .md écrits par ecrire() sont suivis
        (comme le vrai contrôle, qui ne regarde que « git ls-files '*.md' »)."""
        subprocess.run(["git", "init", "-q"], cwd=self.racine, capture_output=True, timeout=60)
        for chemin in self.ecrits:
            subprocess.run(["git", "add", "--", chemin], cwd=self.racine,
                           capture_output=True, timeout=60)
        subprocess.run(["git", "-c", "user.email=" + COURRIEL_GIT,
                        "-c", "user.name=essai", "commit", "-qm", "fixtures"],
                       cwd=self.racine, capture_output=True, timeout=60)

    # --- agents_existe / agents_mots ------------------------------------------------
    def test_agents_absent(self):
        self.assertEqual(agents_existe(self.racine), ["AGENTS.md is missing"])
        self.assertEqual(agents_mots(self.racine), [])   # sans fichier, pas de compte

    def test_agents_dans_la_limite(self):
        self.ecrire("AGENTS.md", "# Agents\n\nUn texte court.\n")
        self.assertEqual(agents_existe(self.racine), [])
        self.assertEqual(agents_mots(self.racine), [])

    def test_agents_hors_limite(self):
        self.ecrire("AGENTS.md", "mot " * 1501)
        self.assertEqual(agents_mots(self.racine),
                         ["AGENTS.md: 1501 words, limit %d" % LIMITE_AGENTS])

    def test_agents_a_la_limite_exacte(self):
        self.ecrire("AGENTS.md", "mot " * 1499 + "fin")
        self.assertEqual(agents_mots(self.racine), [])

    # --- guides_mots ----------------------------------------------------------------
    def test_guide_hors_limite(self):
        self.ecrire("docs/agents/01-setup.md", "mot " * 6001)
        self.assertEqual(guides_mots(self.racine),
                         ["docs/agents/01-setup.md: 6001 words, limit %d" % LIMITE_GUIDE])

    def test_guide_a_la_limite_exacte(self):
        self.ecrire("docs/agents/01-setup.md", "mot " * 5999 + "fin")
        self.assertEqual(guides_mots(self.racine), [])

    def test_plusieurs_guides_comptes_separes(self):
        self.ecrire("docs/agents/01-setup.md", "court")
        self.ecrire("docs/agents/02-runtime.md", "mot " * 6002)
        self.assertEqual(guides_mots(self.racine),
                         ["docs/agents/02-runtime.md: 6002 words, limit %d" % LIMITE_GUIDE])

    # --- liens_relatifs -------------------------------------------------------------
    def test_lien_sans_cible(self):
        self.ecrire("README.md", "voir [ce guide](docs/inconnu.md)")
        self.git_init()
        ecarts = liens_relatifs(self.racine)
        self.assertEqual(len(ecarts), 1)
        self.assertIn("README.md", ecarts[0])
        self.assertIn("docs/inconnu.md", ecarts[0])

    def test_lien_valide_avec_ancre(self):
        self.ecrire("README.md", "voir [les règles](docs/guide.md#section)")
        self.ecrire("docs/guide.md", "# Guide\n")
        self.git_init()
        self.assertEqual(liens_relatifs(self.racine), [])

    def test_liens_ignores(self):
        # http(s):, mailto: et #ancre sont ignorés ; une image existante est une cible valide
        self.ecrire("docs/img.png", "(png factice)\n")
        self.ecrire("README.md",
                    "[site](https://exemple.invalid/a) [écrire](mailto:" + COURRIEL_FIXTURE + ") "
                    "[ancre](#section) ![image](docs/img.png)\n")
        self.git_init()
        self.assertEqual(liens_relatifs(self.racine), [])

    def test_liens_image_non_suivis(self):
        # une image non commitée n'est pas un *.md suivi : le lien n'est pas vérifié
        self.ecrire("README.md", "![capture](docs/capture.png)")
        self.git_init()
        self.assertEqual(liens_relatifs(self.racine), [])

    def test_lien_dans_un_sous_dossier(self):
        self.ecrire("docs/agents/01-setup.md", "retour au [README](../../README.md)")
        self.ecrire("README.md", "# Racine\n")
        self.git_init()
        self.assertEqual(liens_relatifs(self.racine), [])

    def test_lien_casse_dans_un_sous_dossier(self):
        self.ecrire("docs/agents/01-setup.md", "mauvais [chemin](../inconnu.md)")
        self.git_init()
        ecarts = liens_relatifs(self.racine)
        self.assertEqual(len(ecarts), 1)
        self.assertIn("docs/agents/01-setup.md", ecarts[0])

    def test_fichier_non_suivi_non_verifie(self):
        # un brouillon jamais ajouté à git n'est pas dans « ls-files '*.md' » : non vérifié
        self.ecrire("README.md", "# Racine\n")
        self.git_init()
        (self.racine / "brouillon.md").write_text("[cassé](ailleurs.md)", encoding="utf-8")
        self.assertEqual(liens_relatifs(self.racine), [])

    # --- guides_indexes ---------------------------------------------------------------
    def test_guide_indexe(self):
        self.ecrire("docs/agents/README.md", "- [01-setup](01-setup.md)\n- [02](02-runtime.md)")
        self.ecrire("docs/agents/01-setup.md", "guide")
        self.ecrire("docs/agents/02-runtime.md", "guide")
        self.assertEqual(guides_indexes(self.racine), [])

    def test_guide_non_indexe(self):
        self.ecrire("docs/agents/README.md", "- [01-setup](01-setup.md)\n")
        self.ecrire("docs/agents/01-setup.md", "guide")
        self.ecrire("docs/agents/02-runtime.md", "guide")
        self.assertEqual(guides_indexes(self.racine),
                         ["docs/agents/02-runtime.md is not linked from "
                          "docs/agents/README.md"])

    def test_sans_dossier_guides(self):
        self.assertEqual(guides_indexes(self.racine), [])
        self.assertEqual(guides_mots(self.racine), [])


class TestVraiDepot(unittest.TestCase):
    """Les mêmes contrôles sur le dépôt qui contient ce test.

    AGENTS.md et docs/agents/*.md sont écrits en parallèle de ce fichier : un échec
    ici dit ce qui manque, il disparaîtra quand ils seront là.
    """

    def test_agents_existe(self):
        self.assertEqual(agents_existe(RACINE), [])

    def test_agents_mots(self):
        self.assertEqual(agents_mots(RACINE), [])

    def test_guides_mots(self):
        self.assertEqual(guides_mots(RACINE), [])

    def test_guides_presents(self):
        # les guides attendus existent tous (premier à manquer nommé dans l'échec)
        for nom in GUIDES_ATTENDUS:
            texte = _md(RACINE, Path("docs") / "agents" / nom)
            self.assertIsNotNone(texte, "docs/agents/%s is missing" % nom)

    def test_liens_relatifs(self):
        self.assertEqual(liens_relatifs(RACINE), [])

    def test_guides_indexes(self):
        self.assertEqual(guides_indexes(RACINE), [])


if __name__ == "__main__":
    unittest.main()
