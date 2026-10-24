#!/usr/bin/env bash
# Passe 7f — validation : swapchain PERSISTANTE (APK vk) + séquence de l'écran de connexion + non-régression.
#   x86 / arm64 : builds NEUFS + ctest ;
#   longs       : recomp-sc2 x86 natif (hors temps réel), SC2_FRAMES_MAX=3000 sans entrée puis 5000 avec un clic
#                 sur le bouton du dialogue Battle.net (767, 301) à la frame 100 ; séquence des PPM + imports ;
#   sim / run-sim : banc android/sim reconstruit, 300 f (non-régression : 300/301 non noires, max 37.36) ;
#   sim-vk / run-vk : banc -DSC2_SWAPCHAIN=ON sur Xvfb : normal, SUBOPTIMAL permanent (cas émulateur), redimensionnement ;
#   apk         : APK normal + vk, DEUX constructions de zéro chacun.
# Builds sous ulimit -v 8000000 ; runs SANS ulimit, banc avec MALLOC_ARENA_MAX=2.
#   android/valider-7f.sh [étape…]   (défaut : x86 arm64 longs sim run-sim sim-vk run-vk apk adb)
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"
R="$W/recomp-sc2"
O="$W/android/build-7f"; mkdir -p "$O"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"
JOBS=3
VK=(-L "$HOME/tools/arm64-vk-root" -E LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu
    -E VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json -E MALLOC_ARENA_MAX=2)
ETAPES="${*:-x86 arm64 longs sim run-sim sim-vk run-vk apk adb}"
L="$O/valider.log"
chrono() { local f="$1"; shift; local t0=$SECONDS; "$@"; local rc=$?; echo "$((SECONDS - t0)) s" > "$f"; return $rc; }
borne() { ( ulimit -v 8000000; "$@" ); }
j() { echo "$*" | tee -a "$L"; }
cmd() { j "\$ $*"; }
j "=== passe 7f — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"

construire() {   # <x86|arm64>
  local a=$1 b="$R/build-7f-$1"; rm -rf "$b"
  if [ $a = x86 ]; then cmd "cmake -S recomp-sc2 -B recomp-sc2/build-7f-x86 && make -j$JOBS && ctest"; borne cmake -S "$R" -B "$b" > "$O/$a-cmake.log" 2>&1
  else cmd "cmake -S recomp-sc2 -B recomp-sc2/build-7f-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake -DCMAKE_BUILD_TYPE=Debug && make -j$JOBS && ctest"
       borne cmake -S "$R" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" -DCMAKE_BUILD_TYPE=Debug > "$O/$a-cmake.log" 2>&1; fi
  borne chrono "$O/$a-temps" make -C "$b" -j$JOBS > "$O/$a-make.log" 2>&1; j "  $a make rc=$? ($(cat "$O/$a-temps"))"
  (cd "$b" && ctest > "$O/$a-ctest.log" 2>&1); j "  $a ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/$a-ctest.log")"
}
long() {   # <dossier> <frames> [VAR=val…]
  local d="$1" n="$2"; shift 2; rm -rf "$d"; mkdir -p "$d"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_EXE="$INSTALL/Versions/Base15405/SC2.exe" \
     SC2_FRAMES_MAX="$n" SC2_FRAMES_DIR="$d/frames" SC2_TRACE_VA=00BC6CF0 SC2_TRACE_N=3 "$@" timeout 3600 "$R/build-7f-x86/recomp-sc2" > "$d/run.log" 2>&1)
  j "  rc=$? ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons ; $(grep -E 'frames présentées' "$d/run.log" | tail -1)"
  grep -E '^\[BNET\]|^\[INPUT\] frame [0-9]+ : WM_LBUTTON|^\[BILAN\] draws RÉELS|^\[ARRÊT\] \[' "$d/run.log" | cut -c1-230 | sed 's/^/  /' | tee -a "$L"
  j "  BnetClient::méthode9 (Poll) : $(grep -o 'méthode9 (Poll, chaque frame)×[0-9]*' "$d/run.log" | head -1) ; entrées dans le rappel Battle.net 0x00BC6CF0 (SC2_TRACE_VA) : $(grep -c '^\[TRACE\] *0x00BC6CF0' "$d/run.log")"
  j "  imports les plus appelés (bilan) :"
  grep -A3 '^\[BILAN\] imports appelés' "$d/run.log" | tail -3 | cut -c1-230 | sed 's/^/    /' | tee -a "$L"
  j "  séquence des PPM (android/outils/ppm-sequence.py) :"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" --carte "$n" 2>&1 | sed 's/^/    /' | tee -a "$L"
}
run_sim() {   # <binaire> <dossier> <SIM_SORTIE> [VAR=val…]
  local bin="$1" d="$2" n="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_FRAMES_DIR="$d/frames" SIM_SORTIE="$n" \
     SIM_PPM="$d/sim" "$@" timeout 3600 qemu-aarch64 "${VK[@]}" "$bin" > "$d/run.log" 2>&1)
  j "  rc=$? ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons ; lignes « [VKP] swapchain » (créations) : $(grep -c '^\[VKP\] swapchain ' "$d/run.log")"
  grep -E '^\[VKP\]|^\[SIM\] (sortie|swapchain)|^\[ARRÊT\] \[' "$d/run.log" | sed "s|$HOME|~|g" | cut -c1-260 | sed 's/^/  /' | tee -a "$L"
}
construire_sim() {   # <dossier build> [option cmake]
  local b="$1"; shift; rm -rf "$b"
  local jl="$O/$(basename "$b")"                     # journaux dans build-7f/ (hors git), pas à côté du build
  borne cmake -S "$W/android/sim" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" "$@" > "$jl.cmake.log" 2>&1
  grep -E "delta-propre|SWAPCHAIN=ON" "$jl.cmake.log" | sed 's/^/  /' | tee -a "$L"
  borne chrono "$O/t" make -C "$b" -j$JOBS sc2-sim > "$jl.make.log" 2>&1; j "  make rc=$? ($(cat "$O/t"))"
  j "  --wrap au lien : $(grep -o -- '--wrap=[a-z0-9_]*' "$b/CMakeFiles/sc2-sim.dir/link.txt" | sort -u | tr '\n' ' ')"
}
XVFB=""
xvfb() { [ -n "$XVFB" ] && return; Xvfb :97 -screen 0 2400x2000x24 -nolisten tcp > "$O/xvfb.log" 2>&1 & XVFB=$!; sleep 2
         kill -0 "$XVFB" 2>/dev/null && { export DISPLAY=:97; j "  Xvfb :97 (pid $XVFB)"; } || { j "  Xvfb n'a pas démarré"; XVFB=""; }; }
trap '[ -n "$XVFB" ] && kill "$XVFB" 2>/dev/null' EXIT

for e in $ETAPES; do case $e in
x86) construire x86 ;;
arm64) construire arm64 ;;
longs)
  cmd "SC2_FRAMES_MAX=3000 recomp-sc2/build-7f-x86/recomp-sc2   (aucune entrée)"
  long "$O/long-3000" 3000
  printf 'frame 100 souris 767 301 bouton g down\nframe 101 souris 767 301 bouton g up\n' > "$O/clic-dialogue.txt"
  cmd "SC2_INPUT_SIM=clic-dialogue.txt SC2_FRAMES_MAX=5000 recomp-sc2/build-7f-x86/recomp-sc2   (clic (767, 301) aux frames 100-101)"
  long "$O/long-5000-clic" 5000 SC2_INPUT_SIM="$O/clic-dialogue.txt"
  ;;
sim)
  cmd "cmake -S android/sim -B android/build-sim … && make -j$JOBS sc2-sim"; construire_sim "$W/android/build-sim" ;;
run-sim)
  cmd "SIM_SORTIE=300 … android/build-sim/sc2-sim   (non-régression : 300/301 non noires, moyenne max 37.36)"
  run_sim "$W/android/build-sim/sc2-sim" "$O/run-sim" 300 ;;
sim-vk)
  cmd "cmake -S android/sim -B android/build-sim-vk -DSC2_SWAPCHAIN=ON … && make -j$JOBS sc2-sim"; construire_sim "$W/android/build-sim-vk" -DSC2_SWAPCHAIN=ON ;;
run-vk)
  xvfb
  cmd "SC2_SWAPCHAIN=1 SC2_SWAPCHAIN_CONTROLE=1 SIM_SORTIE=100 … build-sim-vk/sc2-sim   (attendu : 1 création)"
  run_sim "$W/android/build-sim-vk/sc2-sim" "$O/run-vk" 100 SC2_SWAPCHAIN=1 SC2_SWAPCHAIN_CONTROLE=1
  cmd "+ SC2_SWAPCHAIN_TEST_SUBOPTIMAL=1   (cas émulateur : SUBOPTIMAL à chaque present → attendu : 1 création, ~99 ignorés)"
  run_sim "$W/android/build-sim-vk/sc2-sim" "$O/run-vk-subopt" 100 SC2_SWAPCHAIN=1 SC2_SWAPCHAIN_CONTROLE=1 SC2_SWAPCHAIN_TEST_SUBOPTIMAL=1
  cmd "+ SC2_SWAPCHAIN_TEST_REDIM=50   (fenêtre X 2316x1920 → 1920x1080 à la frame 50 → attendu : 2 créations)"
  run_sim "$W/android/build-sim-vk/sc2-sim" "$O/run-vk-redim" 100 SC2_SWAPCHAIN=1 SC2_SWAPCHAIN_CONTROLE=1 SC2_SWAPCHAIN_TEST_REDIM=50
  ;;
apk)
  for v in 0 1; do
    for passe in 1 2; do
      b="$W/android/build$([ $v = 1 ] && echo -vk)"
      cmd "SC2_APK_SWAPCHAIN=$v scripts/build-apk.sh   (construction $passe/2, dossier vidé)"
      rm -rf "$b"
      SC2_APK_SWAPCHAIN=$v JOBS=$JOBS borne "$W/scripts/build-apk.sh" > "$O/apk-$v-$passe.log" 2>&1; j "  rc=$?"
      grep -E "delta-propre|SWAPCHAIN=ON|table SPIR-V|ATTENTION|native-code|zipalign :|Mo$|\.apk$|libaaudio" "$O/apk-$v-$passe.log" | sed "s|$W/||g" | sed 's/^/  /' | tee -a "$L"
      j "  --wrap au lien : $(grep -o -- '--wrap=[a-z0-9_]*' "$b/CMakeFiles/SC2Partout.dir/link.txt" | sort -u | tr '\n' ' ')"
    done
    a1=$(grep -E '\.apk$' "$O/apk-$v-1.log" | tail -1 | cut -c1-64); a2=$(grep -E '\.apk$' "$O/apk-$v-2.log" | tail -1 | cut -c1-64)
    j "  reproductibilité SC2_APK_SWAPCHAIN=$v : $([ -n "$a1" ] && [ "$a1" = "$a2" ] && echo "IDENTIQUES ($a1)" || echo "DIFFÉRENTS ($a1 / $a2)")"
  done ;;
adb) cmd "adb devices"; command -v adb > /dev/null && adb devices 2>&1 | sed 's/^/  /' | tee -a "$L" || j "  adb absent du serveur Linux : test émulateur par la supervision" ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
