#!/usr/bin/env bash
# Passe 7w — minicarte et cibles de rendu secondaires (RTT). UN run à la fois ; JAMAIS d'ulimit -v sur les runs.
#   x86     : build NEUF recomp-sc2/build-7w (sous ulimit -v) + ctest ;
#   defaut  : 600 f, installation de référence, sans variable — ×2, empreintes ;
#   mission : vue « base », SC2_RUN_SANS_BNET=1, -run TRaynor01, clics « Close » (android/outils/clic-fermer-aide-7q.txt,
#             LU seulement), SC2_RTT_LOG=1, MISSION_FRAMES (défaut 1500) ×MISSION_N (défaut 2) ; bilan [RTT] + cibles,
#             zones du HUD (android/outils/zones-hud-7w.py), PNG f1200/f1500 dans recomp-sc2/build-7w/captures.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7w"; O="$B/valider"; C="$B/captures"; mkdir -p "$O" "$C"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
TAG="${TAG:-}"; L="$O/valider${TAG:+-$TAG}.log"; ETAPES="${*:-x86 defaut mission}"
N="${MISSION_N:-2}"; MF="${MISSION_FRAMES:-1500}"; VUE="${VUE:-$O/vue-base}"
MISSION=(SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_RUN_SANS_BNET=1 SC2_RTT_LOG=1
         SC2_INPUT_SIM="$W/android/outils/clic-fermer-aide-7q.txt")
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
j "=== passe 7w ${TAG} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
run_x86() {   # <nom> <install> <frames> [VAR=val…]
  local d="$O/run${TAG:+-$TAG}-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"
  j "\$ ${*:-(aucune variable)} SC2_FRAMES_MAX=$fr recomp-sc2   — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3000 "$B/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; 1res lectures $(grep -c '1re lecture' "$d/run.log") — free -h APRÈS : $(mem)"
  grep -E '^\[BILAN\] draws :|^\[BILAN\] \[RTT\]' "$d/run.log" | cut -c1-230 | sed 's/^/  /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
}
for e in $ETAPES; do case $e in
x86)
  rm -rf "$B/CMakeFiles" "$B/CMakeCache.txt"
  ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && make -C "$B" -j3 > "$O/x86-make.log" 2>&1 ); j "  x86 make rc=$?"
  (cd "$B" && ctest > "$O/x86-ctest.log" 2>&1); j "  x86 ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/x86-ctest.log")" ;;
defaut) run_x86 defaut-1 "$INSTALL" 600; run_x86 defaut-2 "$INSTALL" 600 ;;
mission)
  [ -d "$VUE" ] || "$W/android/outils/vue-install.sh" "$VUE" base > /dev/null
  for i in $(seq 1 "$N"); do
    run_x86 "mission-$i" "$VUE" "$MF" "${MISSION[@]}"
    d="$O/run${TAG:+-$TAG}-mission-$i"
    grep -E '^\[BILAN\]   cible n°' "$d/run.log" | cut -c1-200 | sed 's/^/  /' | tee -a "$L"
    for f in 01200 $(printf '%05d' "$MF"); do
      [ -f "$d/frames/frame_$f.ppm" ] || continue
      python3 -I "$W/android/outils/zones-hud-7w.py" "$d/frames/frame_$f.ppm" | sed 's/^/    /' | tee -a "$L"
      python3 -I "$W/android/outils/ppm2png.py" "$d/frames/frame_$f.ppm" "$C/mission${TAG:+-$TAG}-$i-f$f.png" > /dev/null 2>&1
    done
  done ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
