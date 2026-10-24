#!/bin/bash
# campagne-banc.sh — parcours automatique de la campagne Wings of Liberty au banc Mac (20/10).
#   scripts/campagne-banc.sh <binaire> <dossier de sortie> [carte …]
# Sans carte : les 29 missions dans l'ordre (TRaynor01 … TZeratul04). Chaque mission est gagnée sans joueur :
# le scénario scripts/entrees/victoire-generique.txt ferme l'aide, tape le code « whatisbestinlife » dans le
# chat, puis le module d'entrées DÉTECTE la victoire (sauvegarde …Victory.SC2Save écrite pendant le run),
# annule les éléments absolus restants et joue la suite en relatif (clics « Continue », puis +QUIT) — fini les
# frappes de secours qui gênent le « Continue ». Garde-fous : 300:QUIT sans victoire, kill murale après 8 min.
# Sortie par mission : <sortie>/<carte>/{run.log,resume.txt,empreintes.txt,frames/,shaders/} (+ planche.png si
# ffmpeg), puis le tableau Markdown de campagne-rapport.py (aussi recopié dans <sortie>/rapport.md).
# V0.1 (22/10, scripts/lib-banc.sh) : garde-disque en tête (SC2_DISQUE_MIN_GO + 5 Gio pour 29 cartes, au prorata,
#   2 au moins : pic d'une mission non compactée, 1,3 Go de PPM pour TRaynor01) puis avant chaque mission (+ 2 Gio :
#   arrêt propre, rapport quand même, code 3) ; verrou ~/.sc2-verrous/banc ; SC2_FRAMES=1 explicite ; chaque mission
#   est compactée dès sa fin (empreintes.txt avant toute suppression, vignettes PNG 640 px, plus aucun PPM :
#   ≈ 30 Mo au lieu de 1,3 Go pour TRaynor01) ; planche inchangée (10 premières frames, 320 px, 5×2), tirée des PNG.
set -u
BIN=$(cd "$(dirname "${1:?binaire}")" && pwd)/$(basename "$1"); S=${2:?dossier de sortie}
R="$(cd "$(dirname "$0")/.." && pwd)"
. "$R/scripts/lib-banc.sh"
NB=$(( $# > 2 ? $# - 2 : 29 )); EST=$(( (5 * NB + 28) / 29 )); [ "$EST" -ge 2 ] || EST=2
banc_garde_disque "$S" "$EST" || exit 3
banc_verrou "$0" "$@"
shift 2
X=${SC2_EXTRACTION:-~/sc2-partout/extraction}
[ -f "$X/install/Versions/Base15405/SC2.exe" ] || { echo "données du jeu absentes : $X"; exit 1; }
if [ $# -gt 0 ]; then CARTES="$*"; else
    CARTES="TRaynor01 TRaynor02 TRaynor03 THanson01 THanson02 THanson03A THanson03B TTosh01 TTosh02 TTosh03A TTosh03B \
TTychus01 TTychus02 TTychus03 TTychus04 TTychus05 THorner01 THorner02 THorner03 THorner04 THorner05S TValerian01 \
TValerian02A TValerian02B TValerian03 TZeratul01 TZeratul02 TZeratul03 TZeratul04"
fi
mkdir -p "$S"; S=$(cd "$S" && pwd)
GARDE_FOU=480                                   # kill murale après 8 min par mission
ARRET=0
for CARTE in $CARTES; do
    D="$S/$CARTE"; t=$SECONDS
    banc_garde_disque "$S" 2 || { echo "campagne arrêtée avant $CARTE (garde-disque)" >&2; ARRET=3; break; }
    echo "=== $CARTE ($(date +%H:%M:%S)) ==="
    rm -rf "$D"; mkdir -p "$D/frames"
    # mêmes variables COMMUN que banc-mac.sh ; SC2_SPIRV_PACK / SC2_DOCUMENTS transmis si présents
    COMMUN=(SC2_ROOT="$D/sc2-c" SC2_EXE="$X/install/Versions/Base15405/SC2.exe" SC2_INSTALL="$X/install"
            SC2_FRAMES=1 SC2_FRAMES_DIR="$D/frames" SC2_POLICES="${SC2_POLICES:-$R/android/assets/polices}")
    [ "${SC2_SPIRV_PACK:-}" ] && COMMUN[${#COMMUN[*]}]="SC2_SPIRV_PACK=$SC2_SPIRV_PACK"
    [ "${SC2_DOCUMENTS:-}" ] && COMMUN[${#COMMUN[*]}]="SC2_DOCUMENTS=$SC2_DOCUMENTS"
    env "${COMMUN[@]}" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 SC2_SANS_VIDEOS=1 \
        SC2_ARGS="-run Maps/Campaign/$CARTE.SC2Map" \
        SC2_BANC_ENTREES="@$R/scripts/entrees/victoire-generique.txt" \
        SC2_FRAMES_PAS=300 SC2_SHADERS_DUMP="$D/shaders" "$BIN" > "$D/run.log" 2>&1 &
    P=$!
    n=0
    while kill -0 "$P" 2>/dev/null && [ "$n" -lt "$GARDE_FOU" ]; do sleep 1; n=$((n + 1)); done
    if kill -0 "$P" 2>/dev/null; then
        echo "garde-fou murale : $CARTE arrêtée après ${GARDE_FOU} s" >&2
        kill -TERM "$P" 2>/dev/null; sleep 2; kill -KILL "$P" 2>/dev/null
    fi
    wait "$P"; RC=$?
    echo "rc=$RC en $((SECONDS - t)) s" > "$D/resume.txt"
    # compactage dès la fin de la mission (le disque ne porte qu'une mission non compactée à la fois) ; sa ligne de
    # bilan suit « rc=… en N s » sans le motif « en N s » (campagne-rapport.py lit la 1re occurrence)
    banc_compacter "$D" >> "$D/resume.txt"
    rm -rf -- "${D:?}/sc2-c"
    # planche d'images par mission (10 frames tuilées 5×2) si ffmpeg est là — purement indicative
    banc_planche "$D" premieres
done
python3 "$R/scripts/campagne-rapport.py" "$S" | tee "$S/rapport.md"
exit "$ARRET"
