#!/usr/bin/env bash
# Passe 7q — validation du RENDU 3D de la mission TRaynor01 (terrain, scène d'intro, HUD). UN run à la fois.
#   x86 / arm64 : builds NEUFS (recomp-sc2/build-7q-<arch>) + ctest ;
#   defaut      : x86, 600 f, installation de référence, sans variable — ×2, empreintes (nouvelle base 7k+7n+7j) ;
#   mission     : x86, vue « base » (6a + Liberty/LibertyMulti Base.SC2Data), SC2_RUN_SANS_BNET=1, -run TRaynor01,
#                 MISSION_FRAMES (défaut 1500) ×MISSION_N (défaut 3), clics « Close » (android/outils/clic-fermer-aide-7q.txt) ;
#                 bilan draws RÉELS / « lumière », PNG f300 / f600 / f1000 / dernière dans build-7q-valider/captures.
# Prérequis hors git : src/shaders/sc2_shaders_spv.h régénéré boot+menu+MISSION (src/shaders/README.md, passe 7q) et le
# code lifté régénéré (FPU_CW 7k + movhps 7n + trampoline 7j). Builds sous ulimit -v 8000000 ; runs sans.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; O="$R/build-7q-valider"; C="$O/captures"; mkdir -p "$O" "$C"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"; EXE="$INSTALL/Versions/Base15405/SC2.exe"; JOBS=3
TAG="${TAG:-}"; L="$O/valider${TAG:+-$TAG}.log"; ETAPES="${*:-x86 arm64 defaut mission}"
BIN="${BIN:-$R/build-7q-x86/recomp-sc2}"; N="${MISSION_N:-3}"; MF="${MISSION_FRAMES:-1500}"
VUE="${VUE:-$O/vue-base}"
MISSION=(SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_RUN_SANS_BNET=1 SC2_INPUT_SIM="$W/android/outils/clic-fermer-aide-7q.txt")
chrono() { local f="$1"; shift; local t0=$SECONDS; "$@"; local rc=$?; echo "$((SECONDS - t0)) s" > "$f"; return $rc; }
borne() { ( ulimit -v 8000000; "$@" ); }
j() { echo "$*" | tee -a "$L"; }
j "=== passe 7q ${TAG} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
j "table SPIR-V : $(grep -c '^    { 0x' "$R/src/shaders/sc2_shaders_spv.h" 2>/dev/null || echo ?) entrées ; free -h AVANT : $(free -h | awk '/Mem:/{print $3" / "$7" dispo"}')"
construire() {
  local a=$1 b="$R/build-7q-$1"; rm -rf "$b"
  if [ $a = x86 ]; then borne cmake -S "$R" -B "$b" > "$O/$a-cmake.log" 2>&1
  else borne cmake -S "$R" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" -DCMAKE_BUILD_TYPE=Debug > "$O/$a-cmake.log" 2>&1; fi
  borne chrono "$O/$a-temps" make -C "$b" -j$JOBS > "$O/$a-make.log" 2>&1; j "  $a make rc=$? ($(cat "$O/$a-temps"))"
  (cd "$b" && ctest > "$O/$a-ctest.log" 2>&1); j "  $a ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/$a-ctest.log")"
}
run_x86() {   # <nom> <install> <frames> [VAR=val…]
  local d="$O/run${TAG:+-$TAG}-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"
  j "\$ ${*:-(aucune variable)} SC2_INSTALL=$(basename "$ins") SC2_FRAMES_MAX=$fr $(basename "$BIN")"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" \
     SC2_FRAMES_DIR="$d/frames" "$@" timeout -s KILL 3000 "$BIN" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($(cat "$d/temps")) ; 1res lectures $(grep -c '1re lecture' "$d/run.log") ; fichiers TRaynor01 : $(grep 'TRaynor01.SC2Map\\' "$d/run.log" | grep -c '1re lecture')"
  grep -E '^\[BILAN\] draws :|^\[BILAN\] shaders distincts' "$d/run.log" | head -2 | cut -c1-220 | sed 's/^/  /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
}
for e in $ETAPES; do case $e in
x86) construire x86 ;;
arm64) construire arm64 ;;
defaut) run_x86 defaut-1 "$INSTALL" 600; run_x86 defaut-2 "$INSTALL" 600 ;;
mission)
  [ -d "$VUE" ] || "$W/android/outils/vue-install.sh" "$VUE" base > /dev/null
  for i in $(seq 1 "$N"); do
    run_x86 "mission-$i" "$VUE" "$MF" "${MISSION[@]}"
    d="$O/run${TAG:+-$TAG}-mission-$i"
    for f in 00300 00600 01000 $(printf '%05d' "$MF"); do
      [ -f "$d/frames/frame_$f.ppm" ] && python3 -I "$W/android/outils/ppm2png.py" "$d/frames/frame_$f.ppm" "$C/mission${TAG:+-$TAG}-$i-f$f.png" > /dev/null 2>&1
    done
  done
  j "  BILAN mission ×$N : $(for i in $(seq 1 "$N"); do cat "$O/run${TAG:+-$TAG}-mission-$i/rc"; done | sort | uniq -c | awk '{printf "%s×rc=%s ", $1, $2}')" ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
true
