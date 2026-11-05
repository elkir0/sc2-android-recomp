#!/usr/bin/env bash
# Passe 7b — validation des correctifs amont (cpu.h atomiques GPTR, sc2_iat.c noms NULL) :
# builds NEUFS x86-64 + ARM64, ctest, run x86 600 frames, run ARM64+Vulkan 50 frames, banc
# android/sim, APK. Chaque binaire lancé vient d'être reconstruit ici (dossiers build-7b-*).
#
#   android/valider-7b.sh [étape…]     étapes : x86 arm64 run-x86 run-arm64 sim apk (défaut : toutes)
# Journal : android/build-7b/valider.log (les sorties complètes à côté).
ulimit -v 8000000
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"
R="$W/recomp-sc2"
O="$W/android/build-7b"; mkdir -p "$O"
DEPOT="$HOME/code/reverse/sc2-partout"
INSTALL="$DEPOT/extraction/install"
JOBS=3
VK=(-L "$HOME/tools/arm64-vk-root" -E LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu
    -E VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json)
# Banc sous ulimit -v 8000000 : zone invitée (4 Go réservés) + faux tas ART + cache TCG de qemu
# dépassent la limite (VmPeak mesuré 8,53 Go) si la glibc invitée ouvre une arène malloc par fil ;
# MALLOC_ARENA_MAX=2 → 7,03 Go. Réglage du banc seulement (bionic/scudo sur le téléphone).
SIMVK=("${VK[@]}" -E MALLOC_ARENA_MAX=2)
ETAPES="${*:-x86 arm64 run-x86 run-arm64 sim apk}"

# chrono <fichier> cmd… : exécute, écrit la durée (s) dans <fichier>, rend le code de retour.
chrono() { local f="$1"; shift; local t0=$SECONDS; "$@"; local rc=$?; echo "$((SECONDS - t0)) s" > "$f"; return $rc; }
j() { echo "$*" | tee -a "$O/valider.log"; }
cmd() { j "\$ $*"; }
j "=== passe 7b — validation $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60)"
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"

for e in $ETAPES; do case $e in
x86)
  cmd "cmake -S recomp-sc2 -B recomp-sc2/build-7b-x86 && make -C recomp-sc2/build-7b-x86 -j$JOBS && ctest"
  rm -rf "$R/build-7b-x86"
  cmake -S "$R" -B "$R/build-7b-x86" > "$O/x86-cmake.log" 2>&1
  chrono "$O/x86-temps" make -C "$R/build-7b-x86" -j$JOBS > "$O/x86-make.log" 2>&1; j "  make rc=$? ($(cat "$O/x86-temps"))"
  (cd "$R/build-7b-x86" && ctest > "$O/x86-ctest.log" 2>&1); j "  ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/x86-ctest.log")"
  ;;
arm64)
  cmd "cmake -S recomp-sc2 -B recomp-sc2/build-7b-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake -DCMAKE_BUILD_TYPE=Debug && make -j$JOBS && ctest"
  rm -rf "$R/build-7b-arm64"
  cmake -S "$R" -B "$R/build-7b-arm64" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" -DCMAKE_BUILD_TYPE=Debug > "$O/arm64-cmake.log" 2>&1
  chrono "$O/arm64-temps" make -C "$R/build-7b-arm64" -j$JOBS > "$O/arm64-make.log" 2>&1; j "  make rc=$? ($(cat "$O/arm64-temps"))"
  (cd "$R/build-7b-arm64" && ctest > "$O/arm64-ctest.log" 2>&1); j "  ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/arm64-ctest.log")"
  ;;
run-x86)
  cmd "SC2_ROOT=<vide> SC2_INSTALL=extraction/install SC2_EXE=…/SC2.exe SC2_FRAMES_MAX=600 recomp-sc2/build-7b-x86/recomp-sc2"
  d="$O/run-x86"; rm -rf "$d"; mkdir -p "$d"
  (cd "$d" && SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_EXE="$INSTALL/Versions/Base15405/SC2.exe" SC2_FRAMES_MAX=600 \
     SC2_FRAMES_DIR="$d/frames" chrono "$d/temps" timeout 1800 "$R/build-7b-x86/recomp-sc2" > "$d/run.log" 2>&1)
  j "  rc=$? ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons ; $(grep -E 'frames présentées' "$d/run.log" | tail -1)"
  j "  $(grep -E '^\[ARRÊT\] \[' "$d/run.log" | head -1)"
  j "  $(grep -E 'BILAN\] Vulkan' "$d/run.log" | cut -c1-200)"
  ;;
run-arm64)
  cmd "qemu-aarch64 -L ~/tools/arm64-vk-root -E LD_LIBRARY_PATH=… -E VK_ICD_FILENAMES=lvp_icd.aarch64.json recomp-sc2/build-7b-arm64/recomp-sc2  (SC2_FRAMES_MAX=50)"
  d="$O/run-arm64"; rm -rf "$d"; mkdir -p "$d"
  (cd "$d" && SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_EXE="$INSTALL/Versions/Base15405/SC2.exe" SC2_FRAMES_MAX=50 \
     SC2_FRAMES_DIR="$d/frames" chrono "$d/temps" timeout 1800 qemu-aarch64 "${VK[@]}" "$R/build-7b-arm64/recomp-sc2" > "$d/run.log" 2>&1)
  j "  rc=$? ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons ; $(grep -E 'frames présentées' "$d/run.log" | tail -1)"
  j "  $(grep -E '^\[ARRÊT\] \[' "$d/run.log" | head -1)"
  j "  $(grep -E 'BILAN\] Vulkan' "$d/run.log" | cut -c1-200)"
  ;;
sim)
  cmd "cmake -S android/sim -B android/build-sim -DCMAKE_TOOLCHAIN_FILE=recomp-sc2/cmake/toolchain-aarch64.cmake && make -j$JOBS sc2-sim"
  rm -rf "$W/android/build-sim"
  cmake -S "$W/android/sim" -B "$W/android/build-sim" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" > "$O/sim-cmake.log" 2>&1
  j "  $(grep 'cpu.h delta-propre' "$O/sim-cmake.log")"
  chrono "$O/sim-temps" make -C "$W/android/build-sim" -j$JOBS sc2-sim > "$O/sim-make.log" 2>&1; j "  make rc=$? ($(cat "$O/sim-temps"))"
  cmd "SIM_SORTIE=150 qemu-aarch64 (arm64-vk-root) android/build-sim/sc2-sim"
  d="$O/run-sim"; rm -rf "$d"; mkdir -p "$d"
  (cd "$d" && SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_FRAMES_DIR="$d/frames" SIM_SORTIE=150 SIM_PPM="$d/sim" \
     chrono "$d/temps" timeout 2400 qemu-aarch64 "${SIMVK[@]}" "$W/android/build-sim/sc2-sim" > "$d/run.log" 2>&1)
  j "  rc=$? ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons"
  grep -E '^\[SIM\] (zone|sortie|faux)|^\[ANDROID\]|^\[ARRÊT\] \[' "$d/run.log" | sed "s|$HOME|~|g" | sed 's/^/  /' | tee -a "$O/valider.log"
  j "  $(grep -E 'BILAN\] Vulkan' "$d/run.log" | cut -c1-200)"
  ;;
apk)
  cmd "scripts/build-apk.sh"
  rm -rf "$W/android/build"
  JOBS=$JOBS "$W/scripts/build-apk.sh" > "$O/apk.log" 2>&1; j "  rc=$?"
  grep -E "delta-propre|table SPIR-V|native-code|zipalign :|Mo$|sc2-partout.apk$" "$O/apk.log" "$W/android/build/make.log" 2>/dev/null | sed "s|$W/||g;s|^[^:]*log:||" | sed 's/^/  /' | tee -a "$O/valider.log"
  grep -h "cpu.h delta-propre" "$O/apk.log" 2>/dev/null | head -1 | sed 's/^/  /' | tee -a "$O/valider.log"
  ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
