#!/usr/bin/env bash
# Passe 7c — validation des draws réels SANS VK_KHR_push_descriptor (descripteurs classiques) :
# builds NEUFS x86-64 + ARM64 + ctest, runs x86 600 f (poussés / classiques), banc android/sim
# 300 f poussés et 300 f classiques (SC2_VK_SANS_PUSH_DESCRIPTOR=1), teinte 20 f, APK normal + vk.
# Builds sous ulimit -v 8000000 ; runs du banc SANS ulimit mais avec MALLOC_ARENA_MAX=2 (leçon 7b).
#
#   android/valider-7c.sh [étape…]   étapes : x86 arm64 run-x86 sim run-sim apk fps (défaut : toutes)
# Journal : android/build-7c/valider.log
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"
R="$W/recomp-sc2"
O="$W/android/build-7c"; mkdir -p "$O"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"
JOBS=3
SIMVK=(-L "$HOME/tools/arm64-vk-root" -E LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu
       -E VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json -E MALLOC_ARENA_MAX=2)
ETAPES="${*:-x86 arm64 run-x86 sim run-sim apk fps}"
L="$O/valider.log"
chrono() { local f="$1"; shift; local t0=$SECONDS; "$@"; local rc=$?; echo "$((SECONDS - t0)) s" > "$f"; return $rc; }
borne() { ( ulimit -v 8000000; "$@" ); }          # builds / données sous la borne mémoire
j() { echo "$*" | tee -a "$L"; }
cmd() { j "\$ $*"; }
j "=== passe 7c — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70)"
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"

run_x86() {   # <dossier> [VAR=val…]
  local d="$1"; shift; rm -rf "$d"; mkdir -p "$d"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_EXE="$INSTALL/Versions/Base15405/SC2.exe" \
     SC2_FRAMES_MAX=600 SC2_FRAMES_DIR="$d/frames" "$@" timeout 1800 "$R/build-7c-x86/recomp-sc2" > "$d/run.log" 2>&1)
  j "  rc=$? ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons ; $(grep -E 'frames présentées' "$d/run.log" | tail -1)"
  grep -E '^\[VK\] descripteurs|^\[BILAN\] (draws RÉELS|descripteurs)|^\[ARRÊT\] \[' "$d/run.log" | cut -c1-260 | sed 's/^/  /' | tee -a "$L"
  grep -E '\[VK\] Present frame n°(10|100|600) ' "$d/run.log" | sed "s|$d/||" | cut -c1-200 | sed 's/^/  /' | tee -a "$L"
}
run_sim() {   # <dossier> <SIM_SORTIE> [VAR=val…]
  local d="$1" n="$2"; shift 2; rm -rf "$d"; mkdir -p "$d"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_FRAMES_DIR="$d/frames" SIM_SORTIE="$n" \
     SIM_PPM="$d/sim" "$@" timeout 3600 qemu-aarch64 "${SIMVK[@]}" "$W/android/build-sim/sc2-sim" > "$d/run.log" 2>&1)
  j "  rc=$? ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons"
  grep -E '^\[VK\] descripteurs|^\[SIM\] (zone|sortie|cadence)|^\[BILAN\] (draws RÉELS|descripteurs)|^\[ARRÊT\] \[|DIAGNOSTIC' "$d/run.log" \
    | sed "s|$HOME|~|g" | cut -c1-300 | sed 's/^/  /' | tee -a "$L"
}

for e in $ETAPES; do case $e in
x86)
  cmd "cmake -S recomp-sc2 -B recomp-sc2/build-7c-x86 && make -j$JOBS && ctest"
  rm -rf "$R/build-7c-x86"
  borne cmake -S "$R" -B "$R/build-7c-x86" > "$O/x86-cmake.log" 2>&1
  borne chrono "$O/x86-temps" make -C "$R/build-7c-x86" -j$JOBS > "$O/x86-make.log" 2>&1; j "  make rc=$? ($(cat "$O/x86-temps"))"
  (cd "$R/build-7c-x86" && borne ctest > "$O/x86-ctest.log" 2>&1); j "  ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/x86-ctest.log")"
  ;;
arm64)
  cmd "cmake -S recomp-sc2 -B recomp-sc2/build-7c-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake -DCMAKE_BUILD_TYPE=Debug && make -j$JOBS && ctest"
  rm -rf "$R/build-7c-arm64"
  borne cmake -S "$R" -B "$R/build-7c-arm64" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" -DCMAKE_BUILD_TYPE=Debug > "$O/arm64-cmake.log" 2>&1
  borne chrono "$O/arm64-temps" make -C "$R/build-7c-arm64" -j$JOBS > "$O/arm64-make.log" 2>&1; j "  make rc=$? ($(cat "$O/arm64-temps"))"
  (cd "$R/build-7c-arm64" && borne ctest > "$O/arm64-ctest.log" 2>&1); j "  ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/arm64-ctest.log")"
  ;;
run-x86)
  cmd "SC2_FRAMES_MAX=600 recomp-sc2/build-7c-x86/recomp-sc2   (descripteurs poussés, Lavapipe x86)"
  run_x86 "$O/run-x86-push"
  cmd "SC2_VK_SANS_PUSH_DESCRIPTOR=1 SC2_FRAMES_MAX=600 recomp-sc2/build-7c-x86/recomp-sc2   (descripteurs classiques)"
  run_x86 "$O/run-x86-classique" SC2_VK_SANS_PUSH_DESCRIPTOR=1
  ;;
sim)
  cmd "cmake -S android/sim -B android/build-sim -DCMAKE_TOOLCHAIN_FILE=recomp-sc2/cmake/toolchain-aarch64.cmake && make -j$JOBS sc2-sim"
  rm -rf "$W/android/build-sim"
  borne cmake -S "$W/android/sim" -B "$W/android/build-sim" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" > "$O/sim-cmake.log" 2>&1
  grep -E "delta-propre" "$O/sim-cmake.log" | sed 's/^/  /' | tee -a "$L"
  borne chrono "$O/sim-temps" make -C "$W/android/build-sim" -j$JOBS sc2-sim > "$O/sim-make.log" 2>&1; j "  make rc=$? ($(cat "$O/sim-temps"))"
  ;;
run-sim)
  cmd "SIM_SORTIE=300 qemu-aarch64 (arm64-vk-root, MALLOC_ARENA_MAX=2) android/build-sim/sc2-sim   (poussés)"
  run_sim "$O/run-sim-push" 300
  cmd "SC2_VK_SANS_PUSH_DESCRIPTOR=1 SIM_SORTIE=300 … sc2-sim   (classiques — LA preuve : ≥ 299 non noires)"
  run_sim "$O/run-sim-classique" 300 SC2_VK_SANS_PUSH_DESCRIPTOR=1
  cmd "SC2_VK_TEINTE=FF204060 SC2_VK_SANS_PUSH_DESCRIPTOR=1 SIM_SORTIE=20 … sc2-sim   (teinte, non discriminant depuis 5d)"
  run_sim "$O/run-sim-teinte" 20 SC2_VK_TEINTE=FF204060 SC2_VK_SANS_PUSH_DESCRIPTOR=1
  ;;
fps)
  # mesure APPARIÉE (runs enchaînés, charge machine notée) : la cadence du banc dépend fortement de
  # la charge du serveur Linux (sessions parallèles) ; deux runs de 300 f espacés de 15 min ne se comparent pas.
  for m in poussés classiques; do
    cmd "SIM_SORTIE=100 … sc2-sim   ($m ; charge avant : $(cut -d' ' -f1-3 /proc/loadavg))"
    if [ $m = poussés ]; then run_sim "$O/fps-push" 100; else run_sim "$O/fps-classique" 100 SC2_VK_SANS_PUSH_DESCRIPTOR=1; fi
  done
  ;;
apk)
  for v in 0 1; do
    cmd "SC2_APK_SWAPCHAIN=$v scripts/build-apk.sh"
    rm -rf "$W/android/build$([ $v = 1 ] && echo -vk)"
    SC2_APK_SWAPCHAIN=$v JOBS=$JOBS borne "$W/scripts/build-apk.sh" > "$O/apk-$v.log" 2>&1; j "  rc=$?"
    grep -E "delta-propre|SWAPCHAIN=ON|table SPIR-V|native-code|zipalign :|Mo$|\.apk$" "$O/apk-$v.log" | sed "s|$W/||g" | sed 's/^/  /' | tee -a "$L"
  done
  ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
