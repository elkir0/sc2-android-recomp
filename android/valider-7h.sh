#!/usr/bin/env bash
# Passe 7h — validation : lanceur simulé (SC2_RUN_SANS_BNET=1) vers la 1re mission. UN run à la fois (la machine
# partagée et les runs parallèles augmentent la course connue des fils du jeu).
#   x86 / arm64 : builds NEUFS + ctest ;
#   defaut      : recomp-sc2 x86, 600 f, installation de référence (6a), SANS les variables 7h — ×2, empreintes ;
#   mission     : x86, vue « base » (6a + Liberty.SC2Mod/Base.SC2Data + LibertyMulti), SC2_ARGS="-run
#                 Maps/Campaign/TRaynor01.SC2Map" SC2_RUN_SANS_BNET=1, 600 f — ×2 ;
#   sim         : banc android/sim (qemu ARM64) reconstruit, même mission, SIM_SORTIE=250.
# Builds sous ulimit -v 8000000 ; runs sans (banc : MALLOC_ARENA_MAX=2).
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; O="$W/android/build-7h"; mkdir -p "$O"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"; EXE="$INSTALL/Versions/Base15405/SC2.exe"; JOBS=3
L="$O/valider.log"; ETAPES="${*:-x86 arm64 defaut mission sim}"
SIMVK=(-L "$HOME/tools/arm64-vk-root" -E LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu
       -E VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json -E MALLOC_ARENA_MAX=2)
MISSION=(SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_RUN_SANS_BNET=1)
chrono() { local f="$1"; shift; local t0=$SECONDS; "$@"; local rc=$?; echo "$((SECONDS - t0)) s" > "$f"; return $rc; }
borne() { ( ulimit -v 8000000; "$@" ); }
j() { echo "$*" | tee -a "$L"; }
cmd() { j "\$ $*"; }
j "=== passe 7h — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
construire() {
  local a=$1 b="$R/build-7h-$1"; rm -rf "$b"
  if [ $a = x86 ]; then cmd "cmake -S recomp-sc2 -B recomp-sc2/build-7h-x86 && make -j$JOBS && ctest"; borne cmake -S "$R" -B "$b" > "$O/$a-cmake.log" 2>&1
  else cmd "cmake … -B recomp-sc2/build-7h-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake -DCMAKE_BUILD_TYPE=Debug && make && ctest"
       borne cmake -S "$R" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" -DCMAKE_BUILD_TYPE=Debug > "$O/$a-cmake.log" 2>&1; fi
  borne chrono "$O/$a-temps" make -C "$b" -j$JOBS > "$O/$a-make.log" 2>&1; j "  $a make rc=$? ($(cat "$O/$a-temps"))"
  (cd "$b" && ctest > "$O/$a-ctest.log" 2>&1); j "  $a ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/$a-ctest.log")"
}
resume() {   # <dossier>
  local d="$1"
  j "  rc=$(cat "$d/rc") ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons ; 1res lectures MPQ $(grep -c '1re lecture' "$d/run.log") ; fichiers de TRaynor01 lus : $(grep 'TRaynor01.SC2Map\\' "$d/run.log" | grep -c '1re lecture') ; débordement d'arène : $(grep -c 'DÉBORDEMENT' "$d/run.log")"
  grep -E '^\[RUN\]|^\[ARÈNE\]|^\[RÉSEAU\] WSAIoctl|^\[BNET\] client::méthode8|^\[ARRÊT\] \[' "$d/run.log" | head -8 | sed "s|$HOME|~|g" | cut -c1-230 | sed 's/^/  /' | tee -a "$L"
  j "  fichiers de la carte lus : $(grep 'TRaynor01.SC2Map\\' "$d/run.log" | grep '1re lecture' | sed 's/.*TRaynor01.SC2Map\\//;s/ (.*//' | tr '\n' ' ')"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
}
run_x86() {   # <nom> <install> [VAR=val…]
  local d="$O/run-$1" ins="$2"; shift 2; rm -rf "$d"; mkdir -p "$d"
  cmd "${*:-(aucune variable 7h)} SC2_INSTALL=$(basename "$ins") SC2_FRAMES_MAX=600 recomp-sc2/build-7h-x86/recomp-sc2"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX=600 \
     SC2_FRAMES_DIR="$d/frames" "$@" timeout 3000 "$R/build-7h-x86/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  resume "$d"
}
for e in $ETAPES; do case $e in
x86) construire x86 ;;
arm64) construire arm64 ;;
defaut) run_x86 defaut-1 "$INSTALL"; run_x86 defaut-2 "$INSTALL" ;;
mission)
  cmd "android/outils/vue-install.sh android/build-7h/vue-base base"; "$W/android/outils/vue-install.sh" "$O/vue-base" base | tee -a "$L"
  run_x86 mission-1 "$O/vue-base" "${MISSION[@]}"; run_x86 mission-2 "$O/vue-base" "${MISSION[@]}" ;;
sim)
  cmd "cmake -S android/sim -B android/build-sim … && make -j$JOBS sc2-sim"
  rm -rf "$W/android/build-sim"
  borne cmake -S "$W/android/sim" -B "$W/android/build-sim" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" > "$O/sim-cmake.log" 2>&1
  borne chrono "$O/sim-temps" make -C "$W/android/build-sim" -j$JOBS sc2-sim > "$O/sim-make.log" 2>&1; j "  make rc=$? ($(cat "$O/sim-temps"))"
  d="$O/run-sim-mission"; rm -rf "$d"; mkdir -p "$d"
  cmd "${MISSION[*]} SIM_SORTIE=250 qemu-aarch64 (arm64-vk-root) android/build-sim/sc2-sim   (vue « base »)"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$O/vue-base" SC2_FRAMES_DIR="$d/frames" SIM_SORTIE=250 \
     SIM_PPM="$d/sim" "${MISSION[@]}" timeout 3600 qemu-aarch64 "${SIMVK[@]}" "$W/android/build-sim/sc2-sim" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  resume "$d" ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
