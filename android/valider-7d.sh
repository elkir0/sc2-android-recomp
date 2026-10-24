#!/usr/bin/env bash
# Passe 7d — validation des entrées tactiles par l'API runtime 5f (sc2_input) :
# banc android/sim reconstruit, 300 f (non-régression des frames vs 7c, touchers → wndproc),
# 100 f avec Échap (touche retour), 100 f contre-épreuve SIM_ACTIVER_TARD=1, APK normal + vk
# construits DEUX fois de zéro (reproductibilité), appareil adb s'il y en a un.
# Builds sous ulimit -v 8000000 ; runs du banc sans ulimit, MALLOC_ARENA_MAX=2 (leçon 7b).
#
#   android/valider-7d.sh [étape…]   étapes : sim run-sim apk adb (défaut : toutes)
# Journal : android/build-7d/valider.log
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"
R="$W/recomp-sc2"
O="$W/android/build-7d"; mkdir -p "$O"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"
JOBS=3
SIMVK=(-L "$HOME/tools/arm64-vk-root" -E LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu
       -E VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json -E MALLOC_ARENA_MAX=2)
ETAPES="${*:-sim run-sim apk adb}"
L="$O/valider.log"
chrono() { local f="$1"; shift; local t0=$SECONDS; "$@"; local rc=$?; echo "$((SECONDS - t0)) s" > "$f"; return $rc; }
borne() { ( ulimit -v 8000000; "$@" ); }
j() { echo "$*" | tee -a "$L"; }
cmd() { j "\$ $*"; }
j "=== passe 7d — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"

run_sim() {   # <dossier> <SIM_SORTIE> [VAR=val…]
  local d="$1" n="$2"; shift 2; rm -rf "$d"; mkdir -p "$d"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_FRAMES_DIR="$d/frames" SIM_SORTIE="$n" \
     SIM_PPM="$d/sim" "$@" timeout 3600 qemu-aarch64 "${SIMVK[@]}" "$W/android/build-sim/sc2-sim" > "$d/run.log" 2>&1)
  j "  rc=$? ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons ; lignes [INPUT] → wndproc : $(grep -c '^\[INPUT\] frame .* → wndproc' "$d/run.log")"
  grep -E '^\[SIM\] (sortie|SIM_ACTIVER|retour|toucher)|^\[INPUT\]|^\[ARRÊT\] \[' "$d/run.log" | sed "s|$HOME|~|g" | cut -c1-230 | sed 's/^/  /' | tee -a "$L"
}

for e in $ETAPES; do case $e in
sim)
  cmd "cmake -S android/sim -B android/build-sim -DCMAKE_TOOLCHAIN_FILE=recomp-sc2/cmake/toolchain-aarch64.cmake && make -j$JOBS sc2-sim"
  rm -rf "$W/android/build-sim"
  borne cmake -S "$W/android/sim" -B "$W/android/build-sim" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" > "$O/sim-cmake.log" 2>&1
  grep -E "delta-propre" "$O/sim-cmake.log" | sed 's/^/  /' | tee -a "$L"
  borne chrono "$O/sim-temps" make -C "$W/android/build-sim" -j$JOBS sc2-sim > "$O/sim-make.log" 2>&1; j "  make rc=$? ($(cat "$O/sim-temps"))"
  j "  --wrap au lien : $(grep -o -- '--wrap=[a-z0-9_]*' "$W/android/build-sim/CMakeFiles/sc2-sim.dir/link.txt" | sort -u | tr '\n' ' ')"
  ;;
run-sim)
  cmd "SIM_SORTIE=300 qemu-aarch64 (arm64-vk-root, MALLOC_ARENA_MAX=2) android/build-sim/sc2-sim   (non-régression : 299/300 non noires en 7c)"
  run_sim "$O/run-sim" 300
  cmd "SIM_ECHAP=60 SIM_SORTIE=100 … sc2-sim   (touche retour de l'APK = VK_ESCAPE → WM_KEYDOWN/WM_KEYUP 0x1B)"
  run_sim "$O/run-sim-echap" 100 SIM_ECHAP=60
  cmd "SIM_ACTIVER_TARD=1 SIM_SORTIE=100 … sc2-sim   (contre-épreuve : activation après le boot → aucune ligne [INPUT] attendue)"
  run_sim "$O/run-sim-tard" 100 SIM_ACTIVER_TARD=1
  ;;
apk)
  for v in 0 1; do
    for passe in 1 2; do
      b="$W/android/build$([ $v = 1 ] && echo -vk)"
      cmd "SC2_APK_SWAPCHAIN=$v scripts/build-apk.sh   (construction $passe/2, dossier vidé)"
      rm -rf "$b"
      SC2_APK_SWAPCHAIN=$v JOBS=$JOBS borne "$W/scripts/build-apk.sh" > "$O/apk-$v-$passe.log" 2>&1; j "  rc=$?"
      grep -E "delta-propre|SWAPCHAIN=ON|table SPIR-V|ATTENTION|native-code|zipalign :|Mo$|\.apk$" "$O/apk-$v-$passe.log" | sed "s|$W/||g" | sed 's/^/  /' | tee -a "$L"
      j "  --wrap au lien : $(grep -o -- '--wrap=[a-z0-9_]*' "$b/CMakeFiles/SC2Partout.dir/link.txt" | sort -u | tr '\n' ' ')"
    done
    a1=$(grep -E '\.apk$' "$O/apk-$v-1.log" | tail -1 | cut -c1-64); a2=$(grep -E '\.apk$' "$O/apk-$v-2.log" | tail -1 | cut -c1-64)
    j "  reproductibilité SC2_APK_SWAPCHAIN=$v : $([ -n "$a1" ] && [ "$a1" = "$a2" ] && echo "IDENTIQUES ($a1)" || echo "DIFFÉRENTS ($a1 / $a2)")"
  done
  ;;
adb)
  cmd "adb devices"
  if command -v adb > /dev/null; then adb devices 2>&1 | sed 's/^/  /' | tee -a "$L"
  else j "  adb absent du serveur Linux : test émulateur hors de ce banc (protocole docs/PIPELINE-ANDROID.md §7)"; fi
  ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
