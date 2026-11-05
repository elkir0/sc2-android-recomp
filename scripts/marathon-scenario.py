#!/usr/bin/env python3
"""marathon-scenario.py — génère le scénario d'un marathon de campagne depuis un plan (24/10).

  scripts/marathon-scenario.py <plan> [-o <fichier>]

Le plan (scripts/entrees/marathon/*.plan) enchaîne hub et missions, une directive par ligne (« # » en tête de
ligne commente, lignes vides ignorées) :

  hub <recette>        visite le hub : insère la brique bar-<recette>.txt sous un QUAND:PRET (le bar est chargé),
                       c'est lui qui lance la mission suivante
  hub <recette> <x,y>  de même, la coordonnée remplace le marqueur « {planete} » de la brique (planète à cliquer
                       sur la carte stellaire — refusé dans les deux sens si la brique et le plan ne s'accordent
                       pas sur le marqueur)
  mission <Carte>      enchaîne la carte : chargement (QUAND:CARTE:<Carte> + chargement.txt), code de triche de
                       victoire (QUAND:PRET + triche.txt, garde QUIT), « Continue » (QUAND:VICTOIRE + victoire.txt)
                       puis retour au bar (QUAND:CARTE:TStory01 + chargement.txt)
  mission *            carte inconnue au moment d'écrire le plan : la mission est repérée à son écran de
                       chargement (QUAND:CHARGEMENT MAX:200 + chargement.txt), le reste est identique

Le scénario suit le format 2 du banc d'entrées (recomp-sc2/runtime/sc2_entrees_banc.c) : REF:1280x720, GARDE =
420 s par mission + 120 s de marge, segments armés en chaîne et SINON:QUIT (le bar doit charger, la carte doit
lire, la mission doit devenir jouable), marqueur « {n} » des briques remplacé par le numéro de mission, arrêt
volontaire « +30:QUIT » une fois la dernière mission raccordée au bar. Les événements comptent un TEXTE à raison
d'un par caractère (le banc les développe ainsi) : refus au-delà de 1024 événements ou de 64 segments, recette
inconnue ou mission avant tout hub refusée aussi. Code de retour : 0 = scénario écrit (ou imprimé sur stdout),
2 = plan ou brique en erreur (message en français sur stderr).
"""
import argparse
import re
import sys
from pathlib import Path

BRIQUES = Path(__file__).resolve().parent / "entrees" / "marathon"   # bar-*.txt, chargement.txt…
MAX_EVENEMENTS = 1024     # BANC_MAX (sc2_entrees_banc.c), événements de TOUT le scénario
MAX_SEGMENTS = 64         # BANC_SEG_MAX
GARDE_PAR_MISSION = 420   # secondes de garde par mission (bar → victoire → retour au bar)
GARDE_MARGE = 120         # premier chargement du bar + arrêt final + marge
NOM = re.compile(r"^[A-Za-z0-9_-]+$")   # recette comme carte : pas de séparateur de chemin
PLANETE = re.compile(r"^(\d{1,4}),(\d{1,4})$")   # coordonnée de planète du plan (« hub hyperion 573,496 »)


class ErreurPlan(Exception):
    """Plan ou brique refusé — le message, en français, va sur stderr avec le code 2."""


def lire_plan(texte, source="<plan>"):
    """Texte du plan → [(« hub »|« mission », argument[, planète])] dans l'ordre, directives invalides refusées.

    « hub <recette> <x,y> » porte la coordonnée de la planète à cliquer (marqueur {planete} de la brique) ;
    « mission * » désigne une carte inconnue au moment d'écrire le plan.
    """
    etapes = []
    for num, ligne in enumerate(texte.splitlines(), 1):
        l = ligne.strip()
        if not l or l.startswith("#"):
            continue
        morceaux = l.split()
        if len(morceaux) == 2 and morceaux[0] in ("hub", "mission"):
            if not NOM.match(morceaux[1]) and morceaux[1] != "*":
                raise ErreurPlan(f"{source}:{num} : nom invalide « {morceaux[1]} » "
                                 "(lettres, chiffres, tirets et soulignés seulement)")
            etapes.append((morceaux[0], morceaux[1]))
        elif len(morceaux) == 3 and morceaux[0] == "hub" and PLANETE.match(morceaux[2]):
            if not NOM.match(morceaux[1]):
                raise ErreurPlan(f"{source}:{num} : nom invalide « {morceaux[1]} » "
                                 "(lettres, chiffres, tirets et soulignés seulement)")
            etapes.append(("hub", morceaux[1], morceaux[2]))
        else:
            raise ErreurPlan(f"{source}:{num} : directive inconnue « {l} » "
                             "(attendu : hub <recette> [<x,y>], mission <Carte> ou mission *)")
    return etapes


def charger_brique(dossier, nom):
    """Une brique du dossier → ses lignes d'éléments (commentaires et lignes vides ôtés).

    Une brique ne porte QUE des éléments relatifs « +<s>:<geste> » : un QUAND: y serait un bug (le plan seul
    décide de la structure). Refus si le fichier est absent (recette inconnue).
    """
    chemin = dossier / nom
    if not chemin.is_file():
        raise ErreurPlan(f"recette inconnue : {chemin} est absent (une brique bar-<recette>.txt par hub)")
    elements = []
    for num, ligne in enumerate(chemin.read_text().splitlines(), 1):
        l = ligne.strip()
        if not l or l.startswith("#"):
            continue
        if not l.startswith("+"):
            raise ErreurPlan(f"{chemin.name}:{num} : élément non relatif « {l} » (une brique ne porte que des « +<s>:<geste> »)")
        elements.append(l)
    return elements


def compter_evenements(elements):
    """Éléments → nombre d'événements du banc : un TEXTE vaut un événement PAR CARACTÈRE, les autres un."""
    n = 0
    for e in elements:
        geste = e.split(":", 1)[1] if ":" in e else ""
        n += len(geste[len("TEXTE:"):]) if geste.startswith("TEXTE:") else 1
    return n


def substituer(elements, n):
    """Remplace le marqueur « {n} » par le numéro de mission (captures victoire-1, victoire-2…)."""
    return [e.replace("{n}", str(n)) for e in elements]


def poser_planete(elements, planete, nom_brique):
    """Remplace le marqueur « {planete} » par la coordonnée du plan — ou vérifie l'accord des deux côtés.

    La brique ET le plan doivent s'accorder sur le marqueur : coordonnée donnée sans {planete} dans la
    brique (clic qui n'atterrirait nulle part), ou brique à marqueur sans coordonnée (clic sur « {planete} »
    littéral) — refusés tous les deux.
    """
    avec_marqueur = any("{planete}" in e for e in elements)
    if planete and not avec_marqueur:
        raise ErreurPlan(f"{nom_brique} : coordonnée « {planete} » donnée au plan mais la brique n'a pas "
                         "de marqueur {planete} (le clic n'atterrirait nulle part)")
    if avec_marqueur and not planete:
        raise ErreurPlan(f"{nom_brique} : la brique vise le marqueur {{planete}} mais le plan ne donne pas "
                         "de coordonnée (attendu : hub <recette> <x,y>)")
    return [e.replace("{planete}", planete) for e in elements] if planete else list(elements)


def generer(etapes, dossier=BRIQUES):
    """Étapes du plan → (lignes du scénario, stats) ; refuse ce que le banc refuserait.

    Structure : QUAND:CARTE:TStory01 (le bar charge) puis, par étape — hub : QUAND:PRET + bar-<recette>.txt ;
    mission : QUAND:CARTE:<Carte> (ou QUAND:CHARGEMENT pour « mission * », carte inconnue) + chargement.txt,
    QUAND:PRET + triche.txt, QUAND:VICTOIRE + victoire.txt, QUAND:CARTE:TStory01 + chargement.txt (retour) —
    et pour finir QUAND:PRET + « +30:QUIT » au bar. Le PRET du retour au bar appartient au hub SUIVANT :
    deux segments PRET consécutifs ne verraient jamais un nouveau cycle de chargement.
    """
    if not etapes:
        raise ErreurPlan("le plan est vide (directives : hub <recette> [<x,y>], mission <Carte>)")
    missions = [e[1] for e in etapes if e[0] == "mission"]
    if not missions:
        raise ErreurPlan("le plan ne contient aucune mission")
    premier_hub = next((i for i, e in enumerate(etapes) if e[0] == "hub"), None)
    premiere_mission = next((i for i, e in enumerate(etapes) if e[0] == "mission"), None)
    if premier_hub is None or (premiere_mission is not None and premiere_mission < premier_hub):
        raise ErreurPlan("une mission précède tout hub : le plan doit commencer par « hub <recette> » "
                         "(c'est le bar qui lance la première mission)")
    for i in range(1, len(etapes)):
        if etapes[i][0] == "hub" and etapes[i - 1][0] == "hub":
            raise ErreurPlan(f"deux hub consécutifs (directive {i + 1}) : le second attendrait un nouveau "
                             "chargement qui ne viendra pas — un hub par retour au bar")

    briques = {}   # nom de fichier → éléments, chargées une fois
    def brique(nom):
        if nom not in briques:
            briques[nom] = charger_brique(dossier, nom)
        return briques[nom]

    lignes = [f"# marathon — {len(missions)} mission(s) ({', '.join(missions)}), "
              f"{sum(1 for e in etapes if e[0] == 'hub')} hub(s), briques de {dossier.name}/",
              "REF:1280x720",
              f"GARDE:{GARDE_PAR_MISSION * len(missions) + GARDE_MARGE}"]
    stats = {"missions": len(missions), "hubs": sum(1 for e in etapes if e[0] == "hub"),
             "segments": 0, "evenements": 0, "garde": GARDE_PAR_MISSION * len(missions) + GARDE_MARGE}

    def segment(decl, elements, note):
        stats["segments"] += 1
        stats["evenements"] += compter_evenements(elements)
        lignes.append(f"# {stats['segments']}. {note}")
        lignes.append(decl)
        lignes.extend(elements)

    segment("QUAND:CARTE:TStory01 MAX:60 SINON:QUIT", [],
            "le bar (TStory01) est lu au lancement du jeu, MAX 60 s sans quoi le run s'arrête")
    prochaine = 1                                  # numéro de la mission en cours de lancement
    for etape in etapes:
        type_, arg = etape[0], etape[1]
        planete = etape[2] if len(etape) > 2 else ""
        if type_ == "hub":
            elements = poser_planete(brique(f"bar-{arg}.txt"), planete, f"bar-{arg}.txt")
            segment("QUAND:PRET MAX:90 SINON:QUIT", substituer(elements, prochaine),
                    f"hub {arg}{f' {planete}' if planete else ''} : bar chargé, cinématique passée puis "
                    f"« Mission » et « Launch » (bar-{arg}.txt)")
        elif arg == "*":
            n = prochaine
            prochaine += 1
            segment("QUAND:CHARGEMENT MAX:200 SINON:QUIT", brique("chargement.txt"),
                    f"mission {n} (carte inconnue) : écran de chargement repéré, « Press any key » (chargement.txt)")
            segment("QUAND:PRET MAX:150 SINON:QUIT", substituer(brique("triche.txt"), n),
                    f"mission {n} : jouable, code de triche de victoire en quatre essais (triche.txt, garde QUIT)")
            segment("QUAND:VICTOIRE", substituer(brique("victoire.txt"), n),
                    f"mission {n} : victoire, capture puis « Continue » (victoire.txt)")
            segment("QUAND:CARTE:TStory01 MAX:150 SINON:QUIT", brique("chargement.txt"),
                    f"mission {n} : retour au bar (TStory01 relu), écran « Press any key » (chargement.txt)")
        else:
            n = prochaine
            prochaine += 1
            segment(f"QUAND:CARTE:{arg} MAX:150 SINON:QUIT", brique("chargement.txt"),
                    f"mission {n} ({arg}) : carte en chargement, écran « Press any key » (chargement.txt)")
            segment("QUAND:PRET MAX:150 SINON:QUIT", substituer(brique("triche.txt"), n),
                    f"mission {n} : jouable, code de triche de victoire en quatre essais (triche.txt, garde QUIT)")
            segment("QUAND:VICTOIRE", substituer(brique("victoire.txt"), n),
                    f"mission {n} : victoire, capture puis « Continue » (victoire.txt)")
            segment("QUAND:CARTE:TStory01 MAX:150 SINON:QUIT", brique("chargement.txt"),
                    f"mission {n} : retour au bar (TStory01 relu), écran « Press any key » (chargement.txt)")
    segment("QUAND:PRET MAX:90 SINON:QUIT", ["+30:QUIT"],
            "fin du marathon : de retour au bar, arrêt volontaire du jeu après 30 s")

    for l in lignes:                               # un marqueur non substitué ferait une capture au nom bancal
        if l.startswith("+") and "{" in l:
            raise ErreurPlan(f"marqueur non substitué dans « {l} » ({'{n}'} : numéro de mission, "
                             "{planete} : coordonnée du plan)")
    if stats["segments"] > MAX_SEGMENTS:
        raise ErreurPlan(f"{stats['segments']} segments : le banc n'en arme que {MAX_SEGMENTS}, "
                         "réduisez le plan ou serrez les hub")
    if stats["evenements"] > MAX_EVENEMENTS:
        raise ErreurPlan(f"{stats['evenements']} événements (un TEXTE vaut un événement par caractère) : "
                         f"le banc n'en accepte que {MAX_EVENEMENTS}, réduisez le plan")
    return lignes, stats


def main(argv=None):
    p = argparse.ArgumentParser(description="Génère le scénario d'un marathon de campagne depuis un plan. "
                                            "Code de retour : 0 = scénario prêt, 2 = plan ou brique en erreur.")
    p.add_argument("plan", help="fichier .plan (directives : hub <recette>, mission <Carte>)")
    p.add_argument("-o", "--sortie", metavar="FICHIER", help="écrit le scénario dans FICHIER (défaut : stdout)")
    args = p.parse_args(argv)
    try:
        texte = Path(args.plan).read_text()
    except OSError as e:
        print(f"marathon-scenario : plan illisible : {e}", file=sys.stderr)
        return 2
    try:
        lignes, stats = generer(lire_plan(texte, args.plan))
    except ErreurPlan as e:
        print(f"marathon-scenario : {e}", file=sys.stderr)
        return 2
    scenario = "\n".join(lignes) + "\n"
    if args.sortie:
        Path(args.sortie).write_text(scenario)
        print(f"marathon-scenario : {args.sortie} — {stats['missions']} mission(s), {stats['segments']} segments, "
              f"{stats['evenements']} événements, garde {stats['garde']} s")
    else:
        sys.stdout.write(scenario)
    return 0


if __name__ == "__main__":
    sys.exit(main())
