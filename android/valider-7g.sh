#!/usr/bin/env bash
# Passe 7g — validation : builds neufs x86 + arm64 + ctest ; recomp-sc2 x86 natif :
#   défaut 600 f (non-régression : empreintes PPM de la 7f), SC2_ARGS="-run Campaign/TRaynor01.SC2Map",
#   clic avec survol sur le bouton du dialogue (fermeture), SC2_BNET_DUMP=1 (table de rappels Battle.net).
# Builds sous ulimit -v 8000000 ; runs sans.   android/valider-7g.sh [étape…]  (x86 arm64 runs)
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; O="$W/android/build-7g"; mkdir -p "$O"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"; JOBS=3; L="$O/valider.log"
ETAPES="${*:-x86 arm64 runs}"
chrono() { local f="$1"; shift; local t0=$SECONDS; "$@"; local rc=$?; echo "$((SECONDS - t0)) s" > "$f"; return $rc; }
borne() { ( ulimit -v 8000000; "$@" ); }
j() { echo "$*" | tee -a "$L"; }
cmd() { j "\$ $*"; }
j "=== passe 7g — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
construire() {
  local a=$1 b="$R/build-7g-$1"; rm -rf "$b"
  if [ $a = x86 ]; then cmd "cmake -S recomp-sc2 -B recomp-sc2/build-7g-x86 && make -j$JOBS && ctest"; borne cmake -S "$R" -B "$b" > "$O/$a-cmake.log" 2>&1
  else cmd "cmake … -B recomp-sc2/build-7g-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake -DCMAKE_BUILD_TYPE=Debug && make && ctest"
       borne cmake -S "$R" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" -DCMAKE_BUILD_TYPE=Debug > "$O/$a-cmake.log" 2>&1; fi
  borne chrono "$O/$a-temps" make -C "$b" -j$JOBS > "$O/$a-make.log" 2>&1; j "  $a make rc=$? ($(cat "$O/$a-temps"))"
  (cd "$b" && ctest > "$O/$a-ctest.log" 2>&1); j "  $a ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/$a-ctest.log")"
}
run() {   # <nom> <frames> [VAR=val…]
  local d="$O/run-$1" n="$2"; shift 2; rm -rf "$d"; mkdir -p "$d"
  cmd "$* SC2_FRAMES_MAX=$n recomp-sc2/build-7g-x86/recomp-sc2"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_EXE="$INSTALL/Versions/Base15405/SC2.exe" \
     SC2_FRAMES_MAX="$n" SC2_FRAMES_DIR="$d/frames" "$@" timeout 1800 "$R/build-7g-x86/recomp-sc2" > "$d/run.log" 2>&1)
  j "  rc=$? ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons ; 1res lectures MPQ $(grep -c '1re lecture' "$d/run.log") ; lectures Battle.net.MPQ / .gfx : $(grep -ciE 'Battle\.net\.MPQ|\.gfx' "$d/run.log") ; occurrences « TRaynor » hors [ARGS] : $(grep -i traynor "$d/run.log" | grep -vc '^\[ARGS\]')"
  grep -E '^\[ARGS\]|^\[BNET\] (rappels|données)|^\[INPUT\] frame [0-9]+ : WM_LBUTTON|^\[ARRÊT\] \[' "$d/run.log" | cut -c1-260 | sed 's/^/  /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
}
for e in $ETAPES; do case $e in
x86) construire x86 ;;
arm64) construire arm64 ;;
runs)
  run defaut 600
  run args 600 SC2_ARGS="-run Campaign/TRaynor01.SC2Map"
  printf 'frame 140 souris 767 301\nframe 150 souris 767 301 bouton g down\nframe 152 souris 767 301 bouton g up\n' > "$O/clic-survol.txt"
  run fermeture 600 SC2_INPUT_SIM="$O/clic-survol.txt"
  run dump 30 SC2_BNET_DUMP=1 ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
