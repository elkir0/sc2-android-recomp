#!/usr/bin/env bash
# Passe 7s — sélection d'unités → panneaux bas, ordre par la carte de commande, diagnostic des ombres.
# UN run à la fois ; JAMAIS d'ulimit sur les runs ; free -h AVANT/APRÈS consigné.
#   defaut                 : 600 f, installation de référence, sans variable (empreintes attendues be01318c/d3cd40be/8d1ec05)
#   mission:<nom>:<sim>    : vue « base » (comme 7w), SC2_RUN_SANS_BNET=1 -run TRaynor01, SC2_RTT_LOG=1, SC2_INPUT_SIM=<sim>
#                            (chemin relatif à android/outils), MISSION_FRAMES (défaut 1500), frames toutes les PAS (défaut 25) ;
#                            zones du HUD (zones-hud-7w.py) sur FRAMES_ZONES, PNG dans recomp-sc2/build-7w/captures-7s/
# Binaire : recomp-sc2/build-7w/recomp-sc2 (make incrémental sur la branche 7s). Sorties hors git : build-7w/valider-7s/.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7w"; O="$B/valider-7s"; C="$B/captures-7s"; mkdir -p "$O" "$C"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
L="$O/valider${TAG:+-$TAG}.log"; MF="${MISSION_FRAMES:-1500}"; VUE="${VUE:-$B/valider/vue-base}"; PAS="${PAS:-25}"
FZ="${FRAMES_ZONES:-01000 01100 01125 01150 01200 01300 01400 01500}"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
j "=== passe 7s ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif., binaire $(md5sum < "$B/recomp-sc2" | cut -c1-12)"
run_x86() {   # <nom> <install> <frames> [VAR=val…]
  local d="$O/run${TAG:+-$TAG}-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"
  j "\$ ${*:-(aucune variable)} SC2_FRAMES_MAX=$fr — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3000 "$B/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; 1res lectures $(grep -ac '1re lecture' "$d/run.log") — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :|^\[BILAN\] \[INPUT\]|^\[ARRÊT\] \[' "$d/run.log" | cut -c1-230 | sed 's/^/  /' | tee -a "$L"
}
for e in "${@:-defaut}"; do case $e in
defaut) run_x86 defaut "$INSTALL" 600; python3 -I "$W/android/outils/ppm-sequence.py" "$O/run${TAG:+-$TAG}-defaut/frames" 2>&1 | sed 's/^/    /' | tee -a "$L" ;;
mission:*)
  IFS=: read -r _ nom sim <<< "$e"; d="$O/run${TAG:+-$TAG}-$nom"
  [ -d "$VUE" ] || "$W/android/outils/vue-install.sh" "$VUE" base > /dev/null
  run_x86 "$nom" "$VUE" "$MF" SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_RUN_SANS_BNET=1 SC2_RTT_LOG=1 SC2_FRAMES_PAS="$PAS" \
     SC2_INPUT_SIM="$W/android/outils/$sim"
  grep -aE '^\[INPUT\]' "$d/run.log" | cut -c1-160 | head -40 | sed 's/^/  | /' | tee -a "$L"
  for f in $FZ; do
    [ -f "$d/frames/frame_$f.ppm" ] || continue
    python3 -I "$W/android/outils/zones-hud-7w.py" "$d/frames/frame_$f.ppm" | sed 's/^/    /' | tee -a "$L"
    python3 -I "$W/android/outils/ppm2png.py" "$d/frames/frame_$f.ppm" "$C/$nom${TAG:+-$TAG}-f$f.png" > /dev/null 2>&1
  done
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | tail -30 | sed 's/^/    /' | tee -a "$L"
  find "$d/frames" -name '*.ppm' -delete ;;   # place disque : PNG gardés pour FRAMES_ZONES seulement
esac; done
j "=== fin $(date -u '+%T') UTC"
true
