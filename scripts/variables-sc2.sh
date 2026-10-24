#!/bin/bash
# variables-sc2.sh — liste des variables d'environnement SC2_* lues par le code, et contrôle de leur documentation (22/10, V0.4).
#   scripts/variables-sc2.sh liste       noms triés, un par ligne (littéraux "SC2_…" du code C/ObjC/Java)
#   scripts/variables-sc2.sh ou          nom, puis fichier:ligne de chaque lecture
#   scripts/variables-sc2.sh verifier    rend 1 et nomme toute variable du code absente de docs/VARIABLES.md (verifier.sh) ;
#                                        signale aussi, sans échouer, les variables documentées que le code ne lit plus
#   scripts/variables-sc2.sh squelette   lignes de tableau Markdown des variables non documentées (module, 1er site, commentaire
#                                        voisin), à recopier dans docs/VARIABLES.md puis à relire
# Le code lu : runtime commun, hôte Android (jni + lanceur Java), et les dossiers mac/ et plateforme/ quand ils existent.
# Les variables des seuls scripts (SC2_EXTRACTION, SC2_DISQUE_MIN_GO, SC2_APK_*…) sont documentées en tête de chaque script.
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"
DOC="$R/docs/VARIABLES.md"
cd "$R" || exit 2
DOSSIERS=(recomp-sc2/runtime recomp-sc2/src/main.c android/jni android/src)
for d in mac plateforme; do [ -d "$d" ] && DOSSIERS+=("$d"); done
sites() {  # fichier:ligne:NOM pour chaque littéral "SC2_…" (hors tests hôtes, qui posent leurs propres variables)
    grep -rnoE '"SC2_[A-Z0-9_]+"' --include='*.c' --include='*.h' --include='*.m' --include='*.java' "${DOSSIERS[@]}" \
        | grep -v '/tests/' | tr -d '"'
}
liste() { sites | awk -F: '{print $NF}' | LC_ALL=C sort -u; }
documentees() { [ -f "$DOC" ] && grep -oE '^\| `SC2_[A-Z0-9_]+`' "$DOC" | tr -d '|` ' | LC_ALL=C sort -u; }
case "${1:-liste}" in
liste) liste ;;
ou) sites | awk -F: '{print $3 "\t" $1 ":" $2}' | LC_ALL=C sort ;;
verifier)
    [ -f "$DOC" ] || { echo "variables : $DOC absent"; exit 1; }
    manque=$(LC_ALL=C comm -23 <(liste) <(documentees))
    perime=$(LC_ALL=C comm -13 <(liste) <(documentees))
    [ -n "$perime" ] && echo "variables : documentées mais plus lues par le code : $(echo $perime)"
    if [ -n "$manque" ]; then
        echo "variables : $(echo "$manque" | wc -l | tr -d ' ') variable(s) non documentée(s) dans docs/VARIABLES.md : $(echo $manque)"
        echo "            (scripts/variables-sc2.sh squelette donne les lignes à compléter)"
        exit 1
    fi
    echo "variables : $(liste | wc -l | tr -d ' ') variables SC2_* lues par le code, toutes documentées" ;;
squelette)
    manque=$(LC_ALL=C comm -23 <(liste) <([ -f "$DOC" ] && documentees || true))
    for v in $manque; do
        s=$(sites | grep ":$v\$" | head -1); f=${s%%:*}; l=$(echo "$s" | cut -d: -f2)
        # commentaire voisin : sur la ligne même, sinon dans les 3 lignes au-dessus (/* … */ ou //)
        c=$(sed -n "$((l > 3 ? l - 3 : 1)),${l}p" "$f" | grep -oE '(/\*|//) ?.*' | tail -1 | sed -E 's#^(/\*|//) ?##; s#\*/.*##; s#\|#/#g' | cut -c1-140)
        printf '| `%s` | %s | %s | %s |\n' "$v" "$(basename "$f")" "$f:$l" "${c:-—}"
    done ;;
*) sed -n 2,9p "$0"; exit 64 ;;
esac
