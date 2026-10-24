#!/bin/bash
# Runner canonique des tests Java hors Android du lanceur (passe 10f, V5 : OptionsTest) :
# compile Catalogue + EtatInstall + Options + Prereglages + Progression + les trois tests dans un temporaire,
# lance EtatInstallTest puis CatalogueTest puis OptionsTest avec les bons arguments, une ligne récapitulative
# par test ; exit 0 seulement si les trois finissent à 0 échec.
#   bash android/test-javac.sh            (depuis n'importe où)
# EtatInstallTest est lancé SANS install réelle (sondes factices seules) : la vérification 9 dépend de données
# non versionnées (extraction/install) dont l'état varie selon la machine — cf. android/TRACE-PASSE10F.md.
# OptionsTest reçoit la racine du dépôt : il relit scripts/prereglages/*.txt pour comparer les constantes de
# Prereglages octet à octet.
set -eu
RACINE=$(cd "$(dirname "$0")/.." && pwd)
SRC=$RACINE/android/src
T=$(mktemp -d)
trap 'chmod -R u+rwx "$T" 2>/dev/null; rm -rf "$T"' EXIT

javac -encoding UTF-8 -Xlint:all -d "$T/classes" \
    "$SRC/fr/sc2partout/Catalogue.java" "$SRC/fr/sc2partout/EtatInstall.java" \
    "$SRC/fr/sc2partout/Options.java" "$SRC/fr/sc2partout/Prereglages.java" \
    "$SRC/fr/sc2partout/Progression.java" \
    "$SRC/test/EtatInstallTest.java" "$SRC/test/CatalogueTest.java" "$SRC/test/OptionsTest.java"

lancer() {   # lancer <nom> <args…> : sortie dans $T/<nom>.out, ligne récapitulative, code retourné
    local nom=$1 rc=0; shift
    java -cp "$T/classes" "fr.sc2partout.$nom" "$@" > "$T/$nom.out" 2>&1 || rc=$?
    printf '%-16s rc=%s  %s\n' "$nom" "$rc" "$(grep '^== ' "$T/$nom.out" | tail -1)"
    [ "$rc" = 0 ] || { grep -E '^ÉCHEC|Exception|^usage' "$T/$nom.out" | sed 's/^/    /' || true; }
    return "$rc"
}

ok=0
lancer EtatInstallTest || ok=1
mkdir "$T/travail"
lancer CatalogueTest "$SRC/test/reference" "$T/travail" || ok=1
mkdir "$T/options"
lancer OptionsTest "$RACINE" "$T/options" || ok=1
[ "$ok" = 0 ] && echo "test-javac : VERT" || echo "test-javac : ROUGE"
exit "$ok"
