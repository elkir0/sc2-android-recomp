#!/usr/bin/env python3
"""appels-directs.py — remplace les dispatch constants du lift SC2 par des appels directs.

    python3 scripts/appels-directs.py <src/lifted> <sortie> [--exclure FICHIER] [--verifier]

Dans chaque recomp_*.c, `dispatch(c, 0xXXXXXXXXu)` à cible constante liftée (présente dans
recomp_sc2_list.h, hors --exclure) devient `SC2_APPEL(c, L_XXXXXXXX, 0xXXXXXXXXu)` : seul
l'appel change, le reste de la ligne (push32(...) ;, return;, commentaire asm) est conservé.
Les prototypes de TOUTES les cibles référencées (C99 : une cible définie plus bas dans le même
fichier doit aussi être déclarée avant l'appel) sont insérés juste après le dernier
#include d'en-tête, sous un bloc marqué ; pas de bloc si le fichier n'en a pas besoin.
Adresses non liftées, appels calculés (dispatch(c, _ct)), SC2_SAUT et dispatch_jmp : intacts.

--verifier : retire la transformation de <sortie> (SC2_APPEL redevient dispatch, bloc de
prototypes supprimé) et exige de retrouver chaque fichier d'entrée octet pour octet ;
échec = code de retour 1.
"""
import argparse, re, shutil, sys
from pathlib import Path

MARQUEUR = "/* appels directs (scripts/appels-directs.py) */"
RX_CONST = re.compile(r"dispatch\(c, (0x([0-9A-Fa-f]{8})u)\)")   # cible constante à 8 chiffres
RX_CONST_TOUTES = re.compile(r"dispatch\(c, 0x[0-9A-Fa-f]+u\)")  # constantes de longueur libre (stats)
RX_DEFINITION = re.compile(r"^void L_([0-9A-Fa-f]{8})\(CPU \*c\)[ \t]*$", re.MULTILINE)
RX_PROTO = re.compile(r"void L_[0-9A-F]{8}\(CPU \*c\);")
RX_APPEL_DIRECT = re.compile(r"SC2_APPEL\(c, L_[0-9A-F]{8}, (0x[0-9A-Fa-f]{8}u)\)")
RX_ENTREE_LISTE = re.compile(r"\bX\(([0-9A-Fa-f]{8})\)")


def erreur(message):
    print(f"erreur : {message}", file=sys.stderr)
    raise SystemExit(1)


def est_recomp(chemin):
    return chemin.name.startswith("recomp_") and chemin.name.endswith(".c")


def lire_texte(chemin):
    try:
        with open(chemin, "r", encoding="utf-8", newline="") as f:
            return f.read()
    except UnicodeDecodeError as e:
        erreur(f"{chemin} : contenu non décodable en UTF-8 ({e})")


def fichiers_identiques(a, b):
    if a.stat().st_size != b.stat().st_size:
        return False
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(1 << 16), fb.read(1 << 16)
            if x != y:
                return False
            if not x:
                return True


def ecrire_si_different(chemin, contenu):
    """N'écrit que si le contenu change (évite les recompilations inutiles) ; True si écrit."""
    donnees = contenu.encode("utf-8")
    if chemin.is_file() and open(chemin, "rb").read() == donnees:
        return False
    chemin.write_bytes(donnees)
    return True


def copier_si_different(source, destination):
    if destination.is_file() and fichiers_identiques(source, destination):
        return False
    shutil.copyfile(source, destination)
    return True


def charger_liste(chemin):
    """Adresses liftées de recomp_sc2_list.h (8 chiffres hex, majuscules)."""
    adresses = {m.group(1).upper() for m in RX_ENTREE_LISTE.finditer(lire_texte(chemin))}
    if not adresses:
        erreur(f"{chemin} : aucune entrée X(XXXXXXXX) trouvée")
    return adresses


def charger_exclusions(chemin):
    """Adresses à ne jamais transformer : une par ligne, 0x optionnel, # pour un commentaire."""
    exclus = set()
    for numero, ligne in enumerate(lire_texte(chemin).splitlines(), 1):
        adresse = ligne.split("#", 1)[0].strip()
        if not adresse:
            continue
        if adresse[:2].upper() == "0X":
            adresse = adresse[2:]
        if not re.fullmatch(r"[0-9A-Fa-f]{8}", adresse):
            erreur(f"{chemin}:{numero} : adresse invalide « {ligne.strip()} »")
        exclus.add(adresse.upper())
    return exclus


def transformer_fichier(texte, adresses, exclus):
    """Substitue les dispatch constants liftés ; retourne (texte, cibles référencées,
    fonctions définies sur place, compteurs du fichier)."""
    compteurs = {"transformes": 0, "non_liftes": 0, "exclus": 0, "calcules": 0}
    definies = {m.group(1).upper() for m in RX_DEFINITION.finditer(texte)}
    references = set()

    def remplacer(m):
        cible = m.group(2).upper()
        if cible in exclus:
            compteurs["exclus"] += 1
            return m.group(0)
        if cible not in adresses:
            compteurs["non_liftes"] += 1
            return m.group(0)
        compteurs["transformes"] += 1
        references.add(cible)
        return f"SC2_APPEL(c, L_{cible}, {m.group(1)})"  # littéral d'origine conservé tel quel

    nouveau = RX_CONST.sub(remplacer, texte)
    n_const = len(RX_CONST_TOUTES.findall(texte))
    n_huit = compteurs["transformes"] + compteurs["non_liftes"] + compteurs["exclus"]
    compteurs["non_liftes"] += n_const - n_huit          # constantes hors forme à 8 chiffres
    compteurs["calcules"] = texte.count("dispatch(c, ") - n_const
    return nouveau, references, definies, compteurs


def inserer_prototypes(texte, references, definies, chemin):
    """Bloc de prototypes de toutes les cibles référencées, juste après le dernier #include d'en-tête : une cible
    définie plus bas dans le même fichier doit aussi être déclarée avant l'appel (pas de déclaration implicite en C99)."""
    externes = sorted(references)
    if not externes:
        return texte
    lignes = texte.split("\n")
    derniere_include = None
    for i, ligne in enumerate(lignes):
        if ligne.startswith("void "):                    # premières définitions : fin de l'en-tête
            break
        if ligne.startswith("#include"):
            derniere_include = i
    if derniere_include is None:
        erreur(f"{chemin} : aucun #include en tête de fichier pour insérer les prototypes")
    bloc = [MARQUEUR] + [f"void L_{a}(CPU *c);" for a in externes]
    lignes[derniere_include + 1:derniere_include + 1] = bloc
    return "\n".join(lignes)


def retirer_transformation(texte):
    """Inverse exact de la passe (pour --verifier) : appels directs redevenant dispatch,
    puis suppression du bloc marqué et de ses lignes de prototypes."""
    texte = RX_APPEL_DIRECT.sub(r"dispatch(c, \1)", texte)
    if MARQUEUR not in texte:
        return texte
    lignes, resultat = texte.split("\n"), []
    i = 0
    while i < len(lignes):
        if lignes[i] == MARQUEUR:
            i += 1
            while i < len(lignes) and RX_PROTO.fullmatch(lignes[i]):
                i += 1
        else:
            resultat.append(lignes[i])
            i += 1
    return "\n".join(resultat)


def verifier(source, sortie):
    """Chaque fichier de <sortie>, transformation retirée, doit redevenir son entrée."""
    if not sortie.is_dir():
        erreur(f"dossier de sortie introuvable : {sortie}")
    ecarts, verifies = [], 0
    for chemin in sorted(p for p in sortie.rglob("*") if p.is_file()):
        relatif = chemin.relative_to(sortie)
        origine = source / relatif
        if not origine.is_file():
            ecarts.append(f"{relatif} : aucun fichier d'entrée correspondant")
        elif est_recomp(chemin):
            if retirer_transformation(lire_texte(chemin)) == lire_texte(origine):
                verifies += 1
            else:
                ecarts.append(f"{relatif} : ne redevient pas le fichier d'origine sans la passe")
        elif fichiers_identiques(chemin, origine):
            verifies += 1
        else:
            ecarts.append(f"{relatif} : recopié tel quel mais différent de l'entrée")
    if ecarts:
        for e in ecarts:
            print(f"écart : {e}", file=sys.stderr)
        print(f"échec : {len(ecarts)} fichier(s) non conformes sur {verifies + len(ecarts)}", file=sys.stderr)
        return 1
    print(f"vérification : {verifies} fichier(s) conformes (retrait de la passe = entrée, octet pour octet)")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Remplace les dispatch constants du lift par des appels directs SC2_APPEL.")
    ap.add_argument("source", help="dossier d'entrée (src/lifted : recomp_*.c et recomp_sc2_list.h)")
    ap.add_argument("sortie", help="dossier de sortie (créé si besoin)")
    ap.add_argument("--exclure", metavar="FICHIER", help="adresses à laisser en sc2_dispatch, une par ligne")
    ap.add_argument("--verifier", action="store_true", help="contrôle que <sortie> redevient <source> sans la passe")
    args = ap.parse_args()

    source, sortie = Path(args.source), Path(args.sortie)
    if not source.is_dir():
        erreur(f"entrée introuvable ou pas un dossier : {source}")
    liste = source / "recomp_sc2_list.h"
    if not liste.is_file():
        erreur(f"{liste} manquant : le dossier d'entrée doit contenir recomp_sc2_list.h")
    fichiers = sorted(p for p in source.rglob("*") if p.is_file())
    if not any(est_recomp(p) for p in fichiers):
        erreur(f"aucun recomp_*.c dans {source}")
    if args.exclure and not Path(args.exclure).is_file():
        erreur(f"fichier d'exclusion introuvable : {args.exclure}")

    if args.verifier:
        return verifier(source, sortie)

    adresses = charger_liste(liste)
    exclus = charger_exclusions(Path(args.exclure)) if args.exclure else set()
    sortie.mkdir(parents=True, exist_ok=True)
    stats = {"recomps": 0, "autres": 0, "ecrits": 0, "transformes": 0, "non_liftes": 0, "exclus": 0, "calcules": 0}
    for chemin in fichiers:                              # fichier par fichier : jamais tout chargé d'un coup
        destination = sortie / chemin.relative_to(source)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if est_recomp(chemin):
            texte = lire_texte(chemin)
            nouveau, references, definies, compteurs = transformer_fichier(texte, adresses, exclus)
            nouveau = inserer_prototypes(nouveau, references, definies, chemin)
            stats["recomps"] += 1
            for cle in ("transformes", "non_liftes", "exclus", "calcules"):
                stats[cle] += compteurs[cle]
            ecrit = ecrire_si_different(destination, nouveau)
        else:
            stats["autres"] += 1
            ecrit = copier_si_different(chemin, destination)
        if ecrit:
            stats["ecrits"] += 1

    print(f"{stats['recomps']} fichier(s) recomp_*.c traité(s), {stats['autres']} autre(s) recopié(s), "
          f"{stats['ecrits']} réécrit(s)")
    print(f"sites transformés : {stats['transformes']}")
    print(f"sites constants non liftés laissés : {stats['non_liftes']}")
    print(f"sites exclus : {stats['exclus']}")
    print(f"appels calculés laissés : {stats['calcules']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
