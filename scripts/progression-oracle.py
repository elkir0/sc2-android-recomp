#!/usr/bin/env python3
"""progression-oracle.py — compare deux instantanés de progression de campagne StarCraft II (Wings of Liberty).

  scripts/progression-oracle.py <avant/> <apres/> [--json] [--banque NOM]

Un instantané est un dossier Banks/ (banques *.SC2Bank, XML) et, facultativement, UserLogs/StoryModeDebug.txt.
La banque de campagne TCampaign.SC2Bank est comparée par défaut ; --banque all compare toutes les *.SC2Bank
présentes d'un côté comme de l'autre. Sortie texte : par banque puis par section, les clés ajoutées (+),
retirées (-) et modifiées (~ ancienne → nouvelle valeur), puis un résumé — missions terminées, StoryStage,
achats, verdict story mode (grep « Not in story mode » / « in story mode » dans StoryModeDebug.txt).
Tolérant : banque ou section absent d'un côté → signalé, jamais d'erreur ; sections inconnues traitées
génériquement. Codes de retour : 0 = des différences, 1 = aucune différence, 2 = erreur d'usage ou entrée
illisible (instantané introuvable, XML malformé — nom du fichier sur stderr).
"""
import argparse
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

BANQUE_DEFAUT = "TCampaign.SC2Bank"
SECTIONS_MISSIONS = ("MissionCompletion", "MissionCompletedAge")    # clés ajoutées → missions terminées
SECTIONS_ACHATS = ("Research", "Mercenary", "Armory", "Purchases")  # clés ajoutées → achats


class ErreurBanque(Exception):
    """XML malformé dans une banque — le message porte le nom du fichier."""


def charger_banque(chemin):
    """Analyse un fichier .SC2Bank → {section: {clé: [(balise, attr, valeur), …]}}.

    Chaque clé garde la liste de ses éléments de valeur dans l'ordre du fichier (Value, StateValue ou
    autre, avec l'attribut porté — int, string, fixed, flag, text…). Sections et clés vides admises.
    Lève ErreurBanque (nom du fichier en message) si le XML est malformé.
    """
    try:
        racine = ET.parse(chemin).getroot()
    except ET.ParseError as e:
        raise ErreurBanque(f"{chemin} : {e}") from None
    banque = {}
    for section in racine.findall("Section"):
        cles = {}
        for cle in section.findall("Key"):
            cles[cle.get("name", "")] = [(v.tag, attr, val) for v in cle for attr, val in v.attrib.items()]
        banque[section.get("name", "")] = cles
    return banque


def comparer(a, b):
    """Diff de deux banques chargées → {section: {"ajoutes": {clé: valeurs}, "retires": …, "modifiees": …}}.

    Sections inchangées omises ; une section absente d'un côté compte comme vide (ses clés basculent en
    ajoutées/retirées). Clés triées ; « modifiees » vaut {clé: {"ancien": valeurs, "nouveau": valeurs}}.
    """
    diff = {}
    for section in sorted(set(a) | set(b)):
        sa, sb = a.get(section, {}), b.get(section, {})
        d = {"ajoutes": {c: sb[c] for c in sorted(set(sb) - set(sa))},
             "retires": {c: sa[c] for c in sorted(set(sa) - set(sb))},
             "modifiees": {c: {"ancien": sa[c], "nouveau": sb[c]}
                           for c in sorted(set(sa) & set(sb)) if sa[c] != sb[c]}}
        if d["ajoutes"] or d["retires"] or d["modifiees"]:
            diff[section] = d
    return diff


def decrire_valeurs(valeurs):
    """[(balise, attr, valeur), …] → « Value int=1 + Value fixed=0.5 » (« sans valeur » si vide)."""
    return " + ".join(f"{b} {a}={v}" for b, a, v in valeurs) if valeurs else "sans valeur"


def signer_section(section):
    """Empreinte lisible d'une section : « clé=Value int=1, … » (clés triées) ; « — » si vide.

    Section à clé unique : la valeur seule (« Value int=1 »), plus lisible dans le résumé StoryStage.
    """
    if not section:
        return "—"
    if len(section) == 1:
        return decrire_valeurs(next(iter(section.values())))
    return ", ".join(f"{cle}={decrire_valeurs(v)}" for cle, v in sorted(section.items()))


def verdict_story_mode(lignes):
    """« oui » / « non » / « inconnu » — « Not in story mode » l'emporte sur « in story mode » (grep)."""
    texte = "\n".join(lignes).lower()
    if "not in story mode" in texte:
        return "non"
    if "in story mode" in texte:
        return "oui"
    return "inconnu"


def lire_debogue(instantane):
    """(verdict, lignes) du UserLogs/StoryModeDebug.txt d'un instantané ; (« inconnu », []) s'il est absent."""
    chemin = instantane / "UserLogs" / "StoryModeDebug.txt"
    if not chemin.is_file():
        return "inconnu", []
    lignes = [l.rstrip() for l in chemin.read_text(errors="replace").splitlines()]
    return verdict_story_mode(lignes), lignes


def comparer_lignes(a, b):
    """Lignes ajoutées puis retirées entre deux journaux (ordre du fichier, doublons écrasés)."""
    da, db = dict.fromkeys(a), dict.fromkeys(b)
    return [l for l in db if l not in da], [l for l in da if l not in db]


def analyser(avant, apres, noms):
    """Tout le rapport en un dict (la forme du --json) : banques, résumé, StoryModeDebug."""
    rapport = {"avant": str(avant), "apres": str(apres), "banques": [],
               "story_mode_debug": {}, "differences": False}
    for nom in noms:
        ca, cb = avant / "Banks" / nom, apres / "Banks" / nom
        entree = {"nom": nom, "etat": "les-deux", "sections": [], "missions_terminees": [], "achats": []}
        if not ca.exists() and not cb.exists():
            entree["etat"] = "absente-des-deux"
        elif not ca.exists():
            entree["etat"] = "absente-avant"                 # banque apparue côté après
            rapport["differences"] = True
        elif not cb.exists():
            entree["etat"] = "absente-apres"                 # banque disparue côté après
            rapport["differences"] = True
        else:
            ba, bb = charger_banque(ca), charger_banque(cb)
            for section, d in comparer(ba, bb).items():
                cote = "les-deux" if section in ba and section in bb else ("apres" if section in bb else "avant")
                entree["sections"].append({"nom": section, "cote": cote,
                                           "ajoutes": d["ajoutes"], "retires": d["retires"],
                                           "modifiees": d["modifiees"]})
            par_nom = {s["nom"]: s for s in entree["sections"]}
            for s in SECTIONS_MISSIONS:                      # MissionCompletion d'abord, puis l'âge
                entree["missions_terminees"].extend(par_nom.get(s, {}).get("ajoutes", {}))
            for s in SECTIONS_ACHATS:
                entree["achats"].extend(par_nom.get(s, {}).get("ajoutes", {}))
            if "StoryStage" in ba or "StoryStage" in bb:
                entree["story_stage"] = {"avant": signer_section(ba.get("StoryStage", {})),
                                         "apres": signer_section(bb.get("StoryStage", {}))}
            if entree["sections"]:
                rapport["differences"] = True
        rapport["banques"].append(entree)
    va, la = lire_debogue(avant)
    vb, lb = lire_debogue(apres)
    ajoutees, retirees = comparer_lignes(la, lb)
    rapport["story_mode_debug"] = {"avant": va, "apres": vb, "ajoutees": ajoutees, "retirees": retirees}
    if ajoutees or retirees:
        rapport["differences"] = True
    return rapport


def en_texte(rapport):
    """Le rapport en texte lisible (sortie par défaut) : détail par banque/section puis résumé."""
    l = [f"Comparaison : {rapport['avant']} → {rapport['apres']}"]
    if not rapport["banques"]:
        l.append("Aucune banque *.SC2Bank trouvée.")
    for e in rapport["banques"]:
        if e["etat"] == "absente-des-deux":
            l.append(f"Banque {e['nom']} : absente des deux côtés")
        elif e["etat"] == "absente-avant":
            l.append(f"Banque {e['nom']} : absente côté avant → apparue")
        elif e["etat"] == "absente-apres":
            l.append(f"Banque {e['nom']} : absente côté après → disparue")
        elif not e["sections"]:
            l.append(f"Banque {e['nom']} : aucune différence")
        else:
            l.append(f"Banque {e['nom']}")
            for s in e["sections"]:
                entete = f"  Section {s['nom']}"
                if s["cote"] == "apres":
                    entete += f" : ajoutée ({len(s['ajoutes'])} clé(s))"
                elif s["cote"] == "avant":
                    entete += f" : retirée ({len(s['retires'])} clé(s))"
                l.append(entete)
                for cle, v in s["ajoutes"].items():
                    l.append(f"    + {cle} : {decrire_valeurs(v)}")
                for cle, v in s["retires"].items():
                    l.append(f"    - {cle} : {decrire_valeurs(v)}")
                for cle, m in s["modifiees"].items():
                    l.append(f"    ~ {cle} : {decrire_valeurs(m['ancien'])} → {decrire_valeurs(m['nouveau'])}")
    deb = rapport["story_mode_debug"]
    if deb["ajoutees"] or deb["retirees"]:
        l.append(f"UserLogs/StoryModeDebug.txt (avant : {deb['avant']}, après : {deb['apres']})")
        for ligne in deb["ajoutees"]:
            l.append(f"  + {ligne}")
        for ligne in deb["retirees"]:
            l.append(f"  - {ligne}")
    missions = [c for e in rapport["banques"] for c in e["missions_terminees"]]
    achats = [c for e in rapport["banques"] for c in e["achats"]]
    stages = [e["story_stage"] for e in rapport["banques"] if "story_stage" in e]
    l.append("")
    l.append("Résumé :")
    l.append(f"missions terminées : +{len(missions)}" + (f" ({', '.join(missions)})" if missions else ""))
    if not stages:
        l.append("StoryStage : section absente des deux côtés")
    else:
        st = stages[0]
        l.append(f"StoryStage : inchangé ({st['apres']})" if st["avant"] == st["apres"]
                 else f"StoryStage : {st['avant']} → {st['apres']}")
    l.append(f"achats : +{len(achats)}")
    l.append(f"story mode avant/après : {deb['avant']} → {deb['apres']}")
    if not rapport["differences"]:
        l.append("Aucune différence.")
    return "\n".join(l) + "\n"


def construire_analyseur():
    p = argparse.ArgumentParser(
        description="Compare deux instantanés de progression de campagne (Banks/*.SC2Bank + StoryModeDebug.txt). "
                    "Codes de retour : 0 = différences, 1 = aucune, 2 = erreur d'usage ou entrée illisible.")
    p.add_argument("avant", help="instantané de départ (dossier contenant Banks/)")
    p.add_argument("apres", help="instantané d'arrivée (dossier contenant Banks/)")
    p.add_argument("--json", action="store_true", help="sortie du même contenu en un objet JSON sur stdout")
    p.add_argument("--banque", metavar="NOM", default=BANQUE_DEFAUT,
                   help="fichier banque à comparer (défaut : %(default)s ; « all » → toutes les *.SC2Bank)")
    return p


def main(argv=None):
    args = construire_analyseur().parse_args(argv)
    avant, apres = Path(args.avant), Path(args.apres)
    for cote in (avant, apres):
        if not cote.is_dir():
            print(f"progression-oracle : instantané introuvable ou pas un dossier : {cote}", file=sys.stderr)
            return 2
    if args.banque == "all":
        noms = set()
        for cote in (avant, apres):
            d = cote / "Banks"
            if d.is_dir():
                noms |= {f.name for f in d.glob("*.SC2Bank")}
        noms = sorted(noms)
    else:
        noms = [args.banque]
    try:
        rapport = analyser(avant, apres, noms)
    except ErreurBanque as e:
        print(f"progression-oracle : XML malformé : {e}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(rapport, ensure_ascii=False, indent=2))
    else:
        print(en_texte(rapport), end="")
    return 0 if rapport["differences"] else 1


if __name__ == "__main__":
    sys.exit(main())
