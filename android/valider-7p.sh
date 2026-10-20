#!/usr/bin/env bash
# Passe 7p — menu navigable ? (banc x86, vue extraction/install-base, binaire relié de build-7l avec le
# correctif movhps 7n). Un run à la fois ; free -h consigné.
#   menu    : 320 frames, clics et touches injectés (SC2_INPUT_SIM, android/outils/menu-sonde.sim) : messages
#             livrés à la wndproc, réaction de l'interface (draws de sprites, textes mis en page, frames)
#   temoin  : le même run SANS entrée (comparaison)
#   mission : court-circuit du clic « Jouer » (lanceur simulé 7h : SC2_RUN_SANS_BNET=1 -run TRaynor01) : le
#             chargement démarre-t-il ? (fichiers de la carte lus, barre de chargement)
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; O="$R/build-7p-valider"; mkdir -p "$O"
I="$HOME/code/reverse/sc2-partout/extraction/install-base"; EXE="$I/Versions/Base15405/SC2.exe"
BIN="${BIN:-$R/build-7l/recomp-sc2}"; L="$O/valider${TAG:+-$TAG}.log"; ETAPES="${*:-menu temoin mission}"
j() { echo "$*" | tee -a "$L"; }
j "=== passe 7p ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60), binaire $(md5sum < "$BIN" | cut -c1-12)"
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
run() {   # <nom> <frames> [VAR=val…]
  local nom="$1" fr="$2" d="$O/run${TAG:+-$TAG}-$1"; shift 2; rm -rf "$d"; mkdir -p "$d"; local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$I" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_PAS=50 SC2_TEXTE_MAX=200 \
     SC2_FRAMES_DIR="$d/frames" "$@" timeout -s KILL 900 "$BIN" > run.log 2>&1; echo $? > rc)
  j "--- $nom : ${*:-(sans variable)} — rc=$(cat "$d/rc") ($((SECONDS - t0)) s)"
  grep -E '^\[ARRÊT\] \[|^\[BILAN\] \[INPUT\]' "$d/run.log" | cut -c1-220 | sed 's/^/  | /' | tee -a "$L"
  j "  draws de sprites d'interface (VS 30da5441) : $(grep -oE 'VS 30da5441a796d9ca v3.0 +[0-9]+ o : [0-9]+ draws' "$d/run.log" | grep -oE '[0-9]+ draws')"
  j "  textes mis en page : $(grep -c '^\[TEXTE\]' "$d/run.log") ; fichiers TRaynor01 lus : $(grep 'TRaynor01.SC2Map\\' "$d/run.log" | grep -c '1re lecture')"
  python3 -I "$W/android/outils/ppm-blocs.py" "$d/frames" ${REF:+--ref "$O/run${TAG:+-$TAG}-$REF/frames"} 2>&1 | sed 's/^/    /' | tee -a "$L"
}
for e in $ETAPES; do case $e in
temoin)  run temoin 320 ;;
menu)    REF=temoin run menu 320 SC2_INPUT_SIM="$W/android/outils/menu-sonde.sim" ;;
mission) run mission 400 SC2_RUN_SANS_BNET=1 SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
