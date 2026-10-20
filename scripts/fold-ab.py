#!/usr/bin/env python3
"""fold-ab.py — dépouillement A/B des runs « jeu actif » du Fold (logcat + simpleperf).

    python3 scripts/fold-ab.py RUN [RUN ...] [--apres S] [--duree S] [--json]

RUN est un dossier de run tourné sur le téléphone (au moins logcat.txt ; en
plus selon la capture : resume.txt, deroule.txt, rapport-threads.txt,
rapport-jeu-self.txt) ou le chemin direct d'un fichier logcat ; la forme
« etiquette=chemin » fixe l'étiquette affichée. Sans --json, imprime en
français des tableaux Markdown :

  1. plateau de jeu actif — début = 1er clic WM_LBUTTONDOWN + --apres s,
     fin = min(--duree éventuelle, début du profil simpleperf, dernière ligne
     [FPS]) ; lignes [FPS] dont la fenêtre [t − durée, t] tombe entière dans
     le plateau (à 0,5 s près) : FPS = frames/durée, FPS médian/min/max,
     ms/frame jeu et backend pondérés par les frames, part des lignes à
     backend > 5 ms (GPU limitant), thermique avant→après (resume.txt),
     plantages, Present du [BILAN]. Sans clic : 12 dernières lignes hors
     fenêtre profilée, run marqué « sans clic » ;
  2. si le run a été profilé (deroule.txt + rapport-threads.txt +
     rapport-jeu-self.txt) : FPS de la fenêtre profilée, CPU du fil sc2-jeu en
     ms/frame et sa répartition par catégorie (lifté, dispatch, dessin,
     pilote GPU, libc, noyau, runtime, autres — première catégorie qui
     correspond), avec la couverture du rapport simpleperf (tronqué < 100 %) ;
  3. à partir de 2 runs : écarts en % au premier run.

--json imprime les mêmes valeurs brutes (points décimaux), un objet par run.
Bibliothèque standard uniquement ; aucun accès réseau, aucun adb.
"""

import argparse
import json
import re
import statistics
import sys
from pathlib import Path

APRES_DEFAUT = 10.0    # secondes après le 1er clic avant le plateau
MARGE_FENETRE = 0.5    # tolérance d'inclusion des fenêtres [FPS] (s)
LIGNES_SANS_CLIC = 12  # taille du plateau de repli sans clic
SEUIL_GPU = 5.0        # backend ms au-delà duquel le GPU limite
CATEGORIES = ["lifte", "dispatch", "dessin", "pilote", "libc", "noyau", "runtime", "autres"]
AFFICHAGE_CATEGORIES = {"lifte": "lifté"}  # en-têtes du tableau Markdown

RE_HORODATAGE = re.compile(r"^\d{2}-\d{2} (\d{2}):(\d{2}):(\d{2})\.(\d{3}) ")
RE_HMS = re.compile(r"^(\d{2}):(\d{2}):(\d{2})")
RE_FPS = re.compile(
    r"\[FPS\]\s+(?P<fps>\d+(?:\.\d+)?)\s+images/s\s+sur\s+(?P<duree>\d+(?:\.\d+)?)\s+s"
    r"\s+\(frames\s+(?P<debut>\d+)→(?P<fin>\d+),"
    r".*?ms/frame\s*:\s*jeu\s+(?P<jeu>\d+(?:\.\d+)?),\s*backend\s+(?P<backend>\d+(?:\.\d+)?)"
    r"(?:,\s*présentation\s+GPU\s+(?P<gpu>\d+(?:\.\d+)?))?"
)
RE_PRESENT = re.compile(r"\[BILAN\] frames présentées \(Present\) : (\d+)")
# un événement par plantage : ligne libc « Fatal signal », arrêt du jeu, ou en-tête « [ARRÊT] [thread N] » du runtime —
# sauf la sortie normale (appui long sur Retour : « sortie demandée par la plate-forme »)
RE_PLANTAGE = re.compile(r"Fatal signal|FATAL-JEU|\[ARRÊT\] \[thread")
SORTIE_NORMALE = "sortie demandée"
RE_THERMIQUE = re.compile(r"Thermal Status:\s*(\d+).*?mValue=([\d.]+)")
RE_LIGNE_THREADS = re.compile(r"^([\d.]+)%\s+(\d+)\s+(\S.*?)\s*$")
RE_LIGNE_SELF = re.compile(r"^([\d.]+)%\s+(\d+)\s+(\S+)\s+(\S.*?)\s*$")


class ErreurRun(Exception):
    """RUN inutilisable (chemin inexistant, pas de logcat, pas de ligne [FPS])."""


# ---- lecture des entrées ----

def horodatage(ligne):
    """Secondes depuis minuit (flottant) du préfixe logcat, None si absent."""
    m = RE_HORODATAGE.match(ligne)
    if not m:
        return None
    h, minutes, s, ms = (int(g) for g in m.groups())
    return h * 3600 + minutes * 60 + s + ms / 1000.0


def hms_vers_secondes(texte):
    """Secondes depuis minuit d'un « HH:MM:SS » (deroule.txt, resume.txt)."""
    m = RE_HMS.match(texte)
    if not m:
        return None
    h, minutes, s = (int(g) for g in m.groups())
    return h * 3600 + minutes * 60 + s


def lire_fps(ligne):
    """Champs d'une ligne [FPS] (dict) ; None si la ligne n'en est pas une.

    Les lignes « [BILAN] [FPS] phase … : N images en S s » ne portent pas
    « images/s sur » et ne correspondent pas.
    """
    m = RE_FPS.search(ligne)
    if not m:
        return None
    return {
        "fps": float(m["fps"]),
        "duree": float(m["duree"]),
        "debut": int(m["debut"]),
        "fin": int(m["fin"]),
        "frames": int(m["fin"]) - int(m["debut"]),
        "jeu": float(m["jeu"]),
        "backend": float(m["backend"]),
        "gpu": float(m["gpu"]) if m["gpu"] is not None else None,
    }


def lire_logcat(chemin):
    """Dépouille un logcat : lignes [FPS], 1er clic, bilan, plantages."""
    lignes_fps, premier_clic = [], None
    present, bilan_vol, bilan_gpu, plantages = None, [], [], 0
    with open(chemin, encoding="utf-8", errors="replace") as f:
        for ligne in f:
            t = horodatage(ligne)
            fps = lire_fps(ligne)
            if fps is not None and t is not None:
                fps["t"] = t
                lignes_fps.append(fps)
            if premier_clic is None and "WM_LBUTTONDOWN" in ligne:
                premier_clic = t
            if RE_PLANTAGE.search(ligne) and SORTIE_NORMALE not in ligne:
                plantages += 1
            m = RE_PRESENT.search(ligne)
            if m:
                present = int(m[1])
            i = ligne.find("[BILAN] ")
            if i >= 0:
                texte = ligne[i + len("[BILAN] "):].rstrip("\n")
                if texte.startswith("[VOL]"):
                    bilan_vol.append(texte)
                elif texte.startswith("[GPU]"):
                    bilan_gpu.append(texte)
    return {
        "lignes_fps": lignes_fps,
        "premier_clic": premier_clic,
        "present": present,
        "bilan_vol": bilan_vol,
        "bilan_gpu": bilan_gpu,
        "plantages": plantages,
    }


def lire_thermique(chemin):
    """Statut thermique et température AP avant/après (None par côté manquant)."""
    avant = apres = None
    with open(chemin, encoding="utf-8", errors="replace") as f:
        for ligne in f:
            if "température avant" not in ligne and "température après" not in ligne:
                continue
            m = RE_THERMIQUE.search(ligne)
            if not m:
                continue
            cote = {"statut": int(m[1]), "temperature": float(m[2])}
            if "température avant" in ligne:
                avant = cote
            else:
                apres = cote
    return avant, apres


def lire_fenetre_profil(chemin):
    """Fenêtre profilée (début, fin) en secondes d'après deroule.txt ; None si incomplète."""
    debut = fin = None
    with open(chemin, encoding="utf-8", errors="replace") as f:
        for ligne in f:
            m = re.match(r"^(\d{2}:\d{2}:\d{2}) simpleperf\b", ligne)
            if m:
                debut = hms_vers_secondes(m[1])
            m = re.match(r"^(\d{2}:\d{2}:\d{2}) profil fini\b", ligne)
            if m:
                fin = hms_vers_secondes(m[1])
    if debut is not None and fin is not None:
        return debut, fin
    return None


def lire_rapport_threads(chemin):
    """Période d'échantillonnage (ns) et échantillons du fil sc2-jeu."""
    total = evenement = jeu = None
    with open(chemin, encoding="utf-8", errors="replace") as f:
        for ligne in f:
            m = re.match(r"^Samples:\s*(\d+)", ligne)
            if m:
                total = int(m[1])
            m = re.match(r"^Event count:\s*(\d+)", ligne)
            if m:
                evenement = int(m[1])
            m = RE_LIGNE_THREADS.match(ligne)
            if m and m[3] == "sc2-jeu":
                jeu = int(m[2])
    periode = evenement / total if total and evenement else None
    return {"periode_ns": periode, "echantillons_jeu": jeu}


def lire_rapport_jeu_self(chemin):
    """Lignes (échantillons, dso, symbole) du rapport simpleperf du fil du jeu.

    Le dso est le premier champ après les échantillons, le symbole tout le
    reste de la ligne (il peut contenir des espaces).
    """
    lignes = []
    with open(chemin, encoding="utf-8", errors="replace") as f:
        for ligne in f:
            m = RE_LIGNE_SELF.match(ligne)
            if m:
                lignes.append({
                    "echantillons": int(m[2]),
                    "dso": m[3],
                    "symbole": m[4].strip(),
                })
    return lignes


# ---- sélection du plateau et du profil ----

def fenetre_dans(ligne, debut, fin):
    """La fenêtre [t − durée, t] de la ligne est-elle entière dans [début, fin] à 0,5 s près ?"""
    return ligne["t"] - ligne["duree"] >= debut - MARGE_FENETRE and ligne["t"] <= fin + MARGE_FENETRE


def chevauche_profil(ligne, debut, fin):
    return ligne["t"] >= debut and ligne["t"] - ligne["duree"] <= fin


def mesures(lignes):
    """Agrégat d'un groupe de lignes [FPS] (FPS, ms/frame pondérés par les frames)."""
    n = len(lignes)
    frames = sum(l["frames"] for l in lignes)
    duree = sum(l["duree"] for l in lignes)
    valeurs = [l["fps"] for l in lignes]
    return {
        "lignes": n,
        "frames": frames,
        "duree": duree,
        "fps": frames / duree if duree > 0 else None,
        "fps_median": statistics.median(valeurs) if valeurs else None,
        "fps_min": min(valeurs) if valeurs else None,
        "fps_max": max(valeurs) if valeurs else None,
        "jeu_ms": sum(l["jeu"] * l["frames"] for l in lignes) / frames if frames else None,
        "backend_ms": sum(l["backend"] * l["frames"] for l in lignes) / frames if frames else None,
        "gpu_limitant": sum(1 for l in lignes if l["backend"] > SEUIL_GPU) / n if n else None,
    }


def categorie(dso, symbole):
    """Catégorie d'une ligne du rapport self — la PREMIÈRE qui correspond."""
    if re.fullmatch(r"L_[0-9A-Fa-f]+", symbole):
        return "lifte"
    if symbole in ("sc2_dispatch", "dispatch_un") or symbole.startswith("dispatch_"):
        return "dispatch"
    if ("libSC2Partout.so" in dso
            and (symbole in ("dessin_reel", "pipeline5")
                 or symbole.startswith(("v_", "d3dv_", "sc2_vk", "vk_")))):
        return "dessin"
    if any(morceau in dso for morceau in ("vulkan.", "libllvm-qgl", "libgsl", "adreno")):
        return "pilote"
    if dso.endswith("libc.so"):
        return "libc"
    if "[kernel.kallsyms]" in dso:
        return "noyau"
    if "libSC2Partout.so" in dso:
        return "runtime"
    return "autres"


def depouille_profil(dossier, fenetre, lignes_fps):
    """Dépouille le profil simpleperf (None si incomplet ou fenêtre vide)."""
    f_threads = dossier / "rapport-threads.txt"
    f_self = dossier / "rapport-jeu-self.txt"
    if fenetre is None or not (f_threads.is_file() and f_self.is_file()):
        return None, None, []
    threads = lire_rapport_threads(f_threads)
    if not threads["periode_ns"] or not threads["echantillons_jeu"]:
        return None, None, []
    lignes_profil = [l for l in lignes_fps if fenetre_dans(l, fenetre[0], fenetre[1])]
    compte = mesures(lignes_profil)
    if not compte["frames"]:
        return None, None, []
    periode_ms = threads["periode_ns"] / 1e6
    frames = compte["frames"]
    par_categorie = {c: 0 for c in CATEGORIES}
    agreges = {}
    for entree in lire_rapport_jeu_self(f_self):
        par_categorie[categorie(entree["dso"], entree["symbole"])] += entree["echantillons"]
        cle = (entree["dso"], entree["symbole"])
        agreges[cle] = agreges.get(cle, 0) + entree["echantillons"]
    profil = {
        "debut": fenetre[0],
        "fin": fenetre[1],
        "lignes": compte["lignes"],
        "frames": frames,
        "duree": compte["duree"],
        "fps": compte["fps"],
        "cpu_jeu_ms": threads["echantillons_jeu"] * periode_ms / frames,
        "categories": {c: par_categorie[c] * periode_ms / frames for c in CATEGORIES},
    }
    couverture = sum(par_categorie.values()) / threads["echantillons_jeu"]
    top_symboles = [
        {"symbole": symbole, "dso": dso, "echantillons": echantillons,
         "ms_frame": echantillons * periode_ms / frames}
        for (dso, symbole), echantillons in sorted(agreges.items(), key=lambda kv: -kv[1])[:10]
    ]
    return profil, couverture, top_symboles


def depouille_run(etiquette, chemin, apres=APRES_DEFAUT, duree=None):
    """Dépouille un run (dossier ou fichier logcat) en un objet complet."""
    chemin = Path(chemin)
    if chemin.is_dir():
        dossier = chemin
        fichier = chemin / "logcat.txt"
    else:
        dossier, fichier = chemin.parent, chemin
    if not fichier.is_file():
        raise ErreurRun(f"{etiquette} : aucun logcat.txt dans {chemin}")
    log = lire_logcat(fichier)
    if not log["lignes_fps"]:
        raise ErreurRun(f"{etiquette} : aucune ligne [FPS] dans {fichier}")

    f_deroule = dossier / "deroule.txt"
    fenetre = lire_fenetre_profil(f_deroule) if f_deroule.is_file() else None

    lignes_fps = log["lignes_fps"]
    if log["premier_clic"] is not None:
        debut = log["premier_clic"] + apres
        bornes = [debut + duree] if duree is not None else []
        if fenetre is not None:
            bornes.append(fenetre[0])
        bornes.append(lignes_fps[-1]["t"])
        lignes_plateau = [l for l in lignes_fps if fenetre_dans(l, debut, min(bornes))]
        sans_clic = False
    else:
        hors_profil = [l for l in lignes_fps
                       if fenetre is None or not chevauche_profil(l, *fenetre)]
        lignes_plateau = hors_profil[-LIGNES_SANS_CLIC:]
        sans_clic = True

    f_resume = dossier / "resume.txt"
    thermique_avant, thermique_apres = (
        lire_thermique(f_resume) if f_resume.is_file() else (None, None))
    profil, couverture, top_symboles = depouille_profil(dossier, fenetre, lignes_fps)

    run = {"run": etiquette, "sans_clic": sans_clic}
    run.update({k: v for k, v in mesures(lignes_plateau).items()})
    run.update({
        "plantages": log["plantages"],
        "present": log["present"],
        "bilan_vol": log["bilan_vol"],
        "bilan_gpu": log["bilan_gpu"],
        "thermique_avant": thermique_avant,
        "thermique_apres": thermique_apres,
        "profil": profil,
        "couverture": couverture,
        "top_symboles": top_symboles,
    })
    return run


# ---- rendu Markdown ----

def fr(x, dec=1):
    """Nombre en notation française (virgule décimale, moins U+2212)."""
    if x is None:
        return "?"
    return f"{x:.{dec}f}".replace(".", ",").replace("-", "−")


def texte_ecart(v):
    """Écart en % signé (« +12,3 % » / « −4,0 % ») ; « — » si indisponible."""
    if v is None:
        return "—"
    return ("+" if v >= 0 else "−") + fr(abs(v)) + " %"


def ecart_pct(reference, valeur):
    if reference is None or valeur is None or reference == 0:
        return None
    return (valeur - reference) / reference * 100.0


def thermique_texte(avant, apres):
    def cote(d):
        return f"{d['statut']} ({fr(d['temperature'])} °C)" if d else "?"
    if avant is None and apres is None:
        return "?"
    return cote(avant) + " → " + cote(apres)


def tableau_markdown(entetes, lignes):
    """Rendu d'un tableau Markdown avec colonnes alignées."""
    cellules = [entetes] + lignes
    larg = [max(len(l[i]) for l in cellules) for i in range(len(entetes))]
    rendu = []
    for i, ligne in enumerate(cellules):
        rendu.append("| " + " | ".join(c.ljust(larg[j]) for j, c in enumerate(ligne)) + " |")
        if i == 0:
            rendu.append("|" + "|".join("-" * (larg[j] + 2) for j in range(len(entetes))) + "|")
    return "\n".join(rendu)


def ligne_tableau_plateau(run):
    nom = run["run"] + (" (sans clic)" if run["sans_clic"] else "")
    if run["fps"] is None:
        fps = mediane = "?"
    else:
        fps = f"{fr(run['fps'])} ({run['frames']} frames / {fr(run['duree'])} s)"
        mediane = f"{fr(run['fps_median'])} [{fr(run['fps_min'])}-{fr(run['fps_max'])}]"
    gpu = fr(run["gpu_limitant"] * 100) + " %" if run["gpu_limitant"] is not None else "?"
    return [
        nom, str(run["lignes"]), fps, mediane, fr(run["jeu_ms"]), fr(run["backend_ms"]),
        gpu, thermique_texte(run["thermique_avant"], run["thermique_apres"]),
        str(run["plantages"]), str(run["present"]) if run["present"] is not None else "?",
    ]


def ligne_tableau_profil(run):
    profil = run["profil"]
    if profil is None:
        return [run["run"]] + ["—"] * (2 + len(CATEGORIES) + 1)
    return (
        [run["run"], fr(profil["fps"]), fr(profil["cpu_jeu_ms"], 2)]
        + [fr(profil["categories"][c], 2) for c in CATEGORIES]
        + [fr(run["couverture"] * 100) + " %"]
    )


def ligne_tableau_ecarts(premier, run):
    cellules = [run["run"]]
    if run is premier:
        return cellules + ["—"] * 4
    for cle in ("fps", "jeu_ms"):
        cellules.append(texte_ecart(ecart_pct(premier[cle], run[cle])))
    for cle in ("cpu_jeu_ms",):  # profil : CPU du fil du jeu puis lifté
        if premier["profil"] and run["profil"]:
            cellules.append(texte_ecart(ecart_pct(premier["profil"][cle], run["profil"][cle])))
            cellules.append(texte_ecart(ecart_pct(
                premier["profil"]["categories"]["lifte"], run["profil"]["categories"]["lifte"])))
        else:
            cellules += ["—", "—"]
    return cellules


def imprimer_markdown(runs):
    entetes_plateau = ["run", "lignes", "FPS (frames/durée)", "FPS médian [min-max]",
                       "jeu ms", "backend ms", "GPU limitant", "thermique avant → après",
                       "plantages", "Present"]
    print(tableau_markdown(entetes_plateau, [ligne_tableau_plateau(r) for r in runs]))
    if any(r["profil"] for r in runs):
        print()
        entetes_profil = (["run", "FPS profilé", "CPU fil du jeu"]
                          + [AFFICHAGE_CATEGORIES.get(c, c) for c in CATEGORIES]
                          + ["couverture"])
        print(tableau_markdown(entetes_profil, [ligne_tableau_profil(r) for r in runs]))
    if len(runs) >= 2:
        print()
        entetes_ecarts = ["run", "FPS plateau", "jeu ms", "CPU fil du jeu", "lifté"]
        print(tableau_markdown(entetes_ecarts, [ligne_tableau_ecarts(runs[0], r) for r in runs]))


# ---- CLI ----

def resoudre_run(spec):
    """Sépare « etiquette=chemin » ; étiquette par défaut = nom du dossier ou du fichier."""
    if "=" in spec:
        etiquette, chemin = spec.split("=", 1)
        etiquette = etiquette.strip() or None
    else:
        etiquette, chemin = None, spec
    p = Path(chemin)
    if not p.exists():
        raise ErreurRun(f"{spec} : chemin inexistant ({chemin})")
    if p.is_dir() and not (p / "logcat.txt").is_file():
        raise ErreurRun(f"{spec} : aucun logcat.txt dans {chemin}")
    if etiquette is None:
        etiquette = p.name if p.is_dir() else p.stem
    return etiquette, p


def main(argv=None):
    analyse = argparse.ArgumentParser(
        prog="fold-ab.py",
        description="Dépouillement A/B des runs « jeu actif » du Fold (logcat + simpleperf).")
    analyse.add_argument("runs", nargs="+", metavar="RUN",
                         help="dossier de run (logcat.txt…) ou fichier logcat ; etiquette=chemin")
    analyse.add_argument("--apres", type=float, default=APRES_DEFAUT, metavar="S",
                         help="secondes après le 1er clic avant le plateau (défaut %(default)s)")
    analyse.add_argument("--duree", type=float, default=None, metavar="S",
                         help="durée maximale du plateau en secondes")
    analyse.add_argument("--json", action="store_true",
                         help="sortie JSON brute (un objet par run)")
    args = analyse.parse_args(argv)

    runs = []
    try:
        for spec in args.runs:
            etiquette, chemin = resoudre_run(spec)
            runs.append(depouille_run(etiquette, chemin, args.apres, args.duree))
    except ErreurRun as e:
        print(f"fold-ab : {e}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(runs, ensure_ascii=False, indent=2))
    else:
        imprimer_markdown(runs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
