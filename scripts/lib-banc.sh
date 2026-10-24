#!/bin/bash
# lib-banc.sh — fonctions communes des scripts de banc Mac (V0.1, 22/10), SOURCÉES par banc-mac.sh, banc-bar.sh et
# campagne-banc.sh (. "$R/scripts/lib-banc.sh"). Ordre d'appel en tête de script, avant tout rm :
#   banc_garde_disque <dossier> <Gio>   refus (retour 3, message clair) si le volume de <dossier> (ou de son premier
#                                       parent existant) a moins de SC2_DISQUE_MIN_GO (défaut 12) + <Gio> Gio libres ;
#                                       <Gio> = estimation du script (pic de PPM avant compactage, mesuré au banc) ;
#   banc_verrou "$0" "$@"               ré-exécute le script sous le verrou ~/.sc2-verrous/banc (une seule mesure de
#                                       banc à la fois, partagé avec les autres worktrees) : scripts/verrou.sh banc …
#                                       s'il existe (V0.3), sinon /usr/bin/lockf -k. BANC_VERROU_TENU=1 (posé pour le
#                                       fils) : déjà sous le verrou, rien à faire — pas de récursion, pas d'interblocage
#                                       quand un script de banc en appelle un autre.
# Après le run :
#   banc_compacter <dossier> [canon]    frames/*.ppm → <dossier>/empreintes.txt (ppm-sequence.py, AVANT toute
#                                       suppression ; en échec, tous les PPM restent) puis PNG : SANS PERTE ni réduction
#                                       pour les CAPTUREs (tout PPM qui n'est pas frame_<n>.ppm) et, en « canon », pour
#                                       toutes les frames ; sinon vignettes 640 px réservées à la planche, au plus
#                                       BANC_VIGNETTES (défaut 100) réparties sur le run, plus les 10 premières (planche
#                                       de campagne) et la dernière ; les autres frames ne gardent que leur ligne
#                                       d'empreinte. PPM vide ou sans en-tête P6 (run tué ou planté avant le premier
#                                       vidage du tampon) : ligne « illisible » d'empreintes.txt, puis supprimé et
#                                       compté au bilan ; PPM tronqué : ligne « tronqué », puis conversion tentée (un
#                                       échec le garde). Une ligne de bilan sur la sortie (sans « rc= » ni « en N s » : elle
#                                       peut suivre resume.txt sans gêner campagne-rapport.py). Mesure du 22/10 sur
#                                       TRaynor01 : 507 PPM, 1,3 Go → vignettes ≈ 250 Ko, empreintes en 11 s ;
#   banc_planche <dossier> premieres|reparties   <dossier>/planche.png (si ffmpeg) : 10 vignettes 320 px tuilées
#                                       5×2 — les 10 premières (glob, sélection historique de campagne-banc.sh, exige
#                                       frame_00001) ou 10 réparties sur tout le run (bar, mission).
BANC_R="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BANC_VERROU="$HOME/.sc2-verrous/banc"

banc_garde_disque() {
    local p=$1 est=$2 min=${SC2_DISQUE_MIN_GO:-12} libre
    case $min in ''|*[!0-9]*) echo "garde-disque : SC2_DISQUE_MIN_GO=« $min » : nombre entier de Gio attendu" >&2; return 2 ;; esac
    while [ ! -d "$p" ]; do p=$(dirname "$p"); done
    libre=$(df -Pk "$p" 2>/dev/null | awk 'NR == 2 { print int($4 / 1048576) }')
    if [ -z "$libre" ]; then echo "garde-disque : espace libre illisible (df -Pk $p) : run refusé" >&2; return 3; fi
    if [ "$libre" -lt $((min + est)) ]; then
        echo "garde-disque : $libre Gio libres sur le volume de $p, $((min + est)) requis (SC2_DISQUE_MIN_GO=$min" \
             "+ $est estimé(s) pour ce run) : run refusé, rien n'a été écrit" >&2
        return 3
    fi
}

banc_verrou() {
    [ "${BANC_VERROU_TENU:-}" = 1 ] && return 0
    mkdir -p "${BANC_VERROU%/*}"
    export BANC_VERROU_TENU=1
    # sonde sans attente (-t 0) ; -k partout : ôter le fichier sous un détenteur casserait l'exclusion
    /usr/bin/lockf -k -s -t 0 "$BANC_VERROU" true || echo "verrou banc pris, attente… ($BANC_VERROU, $(date +%T))" >&2
    [ -x "$BANC_R/scripts/verrou.sh" ] && exec "$BANC_R/scripts/verrou.sh" banc /bin/bash "$@"
    exec /usr/bin/lockf -k "$BANC_VERROU" /bin/bash "$@"
}

_banc_illisible() {   # <ppm> : vide ou sans « P6 » en tête (read intégré : pas de fork par fichier)
    local e=; IFS= read -r -n 2 e < "$1" 2>/dev/null; [ "$e" != P6 ]
}

_banc_png() {   # <ppm> <png> [largeur] : PNG RGB 8 bits (sans perte si pas de largeur) ; PPM ôté si le PNG est écrit
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -nostdin -loglevel error -y -i "$1" ${3:+-vf scale=$3:-2} -pix_fmt rgb24 "$2"
    else
        sips -s format png ${3:+--resampleWidth $3} "$1" --out "$2" >/dev/null
    fi && [ -s "$2" ] && rm -f -- "$1"
}

banc_compacter() {
    local d=$1 canon=${2:-} f i=0 j=0 n k max=${BANC_VIGNETTES:-100} nppm mo
    local -a num=() cap=() ill=()
    [ -d "$d/frames" ] || { echo "compactage : pas de dossier frames/"; return 0; }
    # illisibles à part (ill) : hors de la sélection des vignettes, la dernière vignette reste la dernière frame lisible
    while IFS= read -r f; do if _banc_illisible "$d/frames/$f"; then ill+=("$f"); else num+=("$f"); fi
        done < <(ls "$d/frames" | grep -E '^frame_[0-9]+\.ppm$' | sort -t_ -k2,2n)
    while IFS= read -r f; do if _banc_illisible "$d/frames/$f"; then ill+=("$f"); else cap+=("$f"); fi
        done < <(ls "$d/frames" | grep '\.ppm$' | grep -vE '^frame_[0-9]+\.ppm$')
    nppm=$(( ${#num[@]} + ${#cap[@]} + ${#ill[@]} ))
    [ "$nppm" -gt 0 ] || { echo "compactage : aucun PPM dans frames/"; return 0; }
    mo=$(du -sm "$d/frames" | cut -f1)
    if ! python3 -I "$BANC_R/android/outils/ppm-sequence.py" "$d/frames" > "$d/empreintes.txt" 2>&1; then
        echo "compactage : ppm-sequence.py en échec (voir $d/empreintes.txt) : $nppm PPM conservés"; return 1
    fi
    for f in ${ill[@]+"${ill[@]}"}; do rm -f -- "$d/frames/$f"; done   # rien à convertir : leur ligne est notée
    n=${#num[@]}; case $max in ''|*[!0-9]*) max=100 ;; esac
    if [ "$max" -gt 0 ]; then k=$(( (n + max - 1) / max )); else k=$((n + 1)); fi; [ "$k" -ge 1 ] || k=1
    # ${t[@]+"${t[@]}"} : tableau vide sous set -u avec le bash 3.2 de macOS (« unbound variable » sinon)
    for f in ${num[@]+"${num[@]}"}; do
        if [ "$canon" ]; then _banc_png "$d/frames/$f" "$d/frames/${f%.ppm}.png" &
        elif [ $i -lt 10 ] || [ $((i % k)) = 0 ] || [ $i = $((n - 1)) ]; then
            _banc_png "$d/frames/$f" "$d/frames/${f%.ppm}.png" 640 &
        else rm -f -- "$d/frames/$f"; i=$((i + 1)); continue; fi
        i=$((i + 1)); j=$((j + 1)); [ $((j % 4)) = 0 ] && wait
    done
    for f in ${cap[@]+"${cap[@]}"}; do
        _banc_png "$d/frames/$f" "$d/frames/${f%.ppm}.png" &
        j=$((j + 1)); [ $((j % 4)) = 0 ] && wait
    done
    wait
    f=$(ls "$d/frames" | grep -c '\.ppm$')
    echo "compactage : $nppm PPM ($mo Mo) → empreintes.txt ($(($(wc -l < "$d/empreintes.txt") - 1)) lignes) +" \
         "$(ls "$d/frames" | grep -c '\.png$') PNG ($(du -sm "$d/frames" | cut -f1) Mo ;" \
         "$([ "$canon" ] && echo "frames sans perte" || echo "vignettes 640 px") ; ${#cap[@]} capture(s) sans perte)$(
         [ ${#ill[@]} -gt 0 ] && echo " ; ${#ill[@]} PPM illisible(s) supprimé(s) (vide ou sans en-tête P6)")$(
         [ "$f" -gt 0 ] && echo " ; $f PPM NON convertis, conservés")"
}

banc_planche() {
    local d=$1 mode=$2
    command -v ffmpeg >/dev/null 2>&1 || return 0
    case $mode in
    premieres)   # numéros non consécutifs (1, 10, 100, puis toutes les 100 ou les k) : motif glob, pas frame_%05d
        [ -f "$d/frames/frame_00001.png" ] || return 0
        ffmpeg -y -loglevel error -pattern_type glob -i "$d/frames/frame_*.png" -vf "scale=320:-2,tile=5x2" \
               -frames:v 1 "$d/planche.png" || true ;;
    reparties)
        ls "$d/frames" 2>/dev/null | grep -E '^frame_[0-9]+\.png$' | sort -t_ -k2,2n \
          | awk -v r="$d/frames" '{ f[NR] = $0 } END { m = NR < 10 ? NR : 10
                for (j = 0; j < m; j++) print "file '\''" r "/" f[m > 1 ? 1 + int(j * (NR - 1) / (m - 1)) : 1] "'\''" }' \
          > "$d/planche.lst"
        [ -s "$d/planche.lst" ] && ffmpeg -y -loglevel error -f concat -safe 0 -i "$d/planche.lst" \
               -vf "scale=320:-2,tile=5x2" -frames:v 1 "$d/planche.png" || true
        rm -f "$d/planche.lst" ;;
    esac
}
