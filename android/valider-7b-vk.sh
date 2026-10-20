#!/usr/bin/env bash
# Passe 7b — validation de la présentation GPU par swapchain (SC2_SWAPCHAIN) sans téléphone :
# banc android/sim construit avec -DSC2_SWAPCHAIN=ON (même sc2_vk_presente.c que l'APK « vk »,
# fenêtre X 2316x1920 sur un Xvfb lancé ici, VK_KHR_xcb_surface au lieu de VK_KHR_android_surface), image de swapchain
# relue pour contrôle (SC2_SWAPCHAIN_CONTROLE=1) ; puis APK « vk ».
#
#   android/valider-7b-vk.sh [étape…]    étapes : sim-vk run-vk run-vk-teinte apk-vk (défaut : toutes)
# Journal : android/build-7b/valider-vk.log
ulimit -v 8000000
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"
R="$W/recomp-sc2"
O="$W/android/build-7b"; mkdir -p "$O"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"
JOBS=3
VK=(-L "$HOME/tools/arm64-vk-root" -E LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu
    -E VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json)
# Banc sous ulimit -v 8000000 : zone invitée (4 Go réservés) + faux tas ART + cache TCG de qemu
# dépassent la limite (VmPeak mesuré 8,53 Go) si la glibc invitée ouvre une arène malloc par fil ;
# MALLOC_ARENA_MAX=2 → 7,03 Go. Réglage du banc seulement (bionic/scudo sur le téléphone).
SIMVK=("${VK[@]}" -E MALLOC_ARENA_MAX=2)
ETAPES="${*:-sim-vk run-vk run-vk-teinte apk-vk}"
L="$O/valider-vk.log"
chrono() { local f="$1"; shift; local t0=$SECONDS; "$@"; local rc=$?; echo "$((SECONDS - t0)) s" > "$f"; return $rc; }
j() { echo "$*" | tee -a "$L"; }
cmd() { j "\$ $*"; }
j "=== passe 7b swapchain — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60)"
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"

# Serveur X virtuel du banc (créé et arrêté ici) : la fenêtre 2316x1920 du présentateur y vit.
XVFB=""
xvfb() {
  [ -n "$XVFB" ] && return
  Xvfb :97 -screen 0 2400x2000x24 -nolisten tcp > "$O/xvfb.log" 2>&1 & XVFB=$!
  sleep 2; kill -0 "$XVFB" 2>/dev/null || { j "  Xvfb :97 n'a pas démarré (cf. $O/xvfb.log)"; XVFB=""; return; }
  export DISPLAY=:97; j "  Xvfb :97 (pid $XVFB) 2400x2000x24"
}
trap '[ -n "$XVFB" ] && kill "$XVFB" 2>/dev/null' EXIT

lancer() {   # <dossier> <SIM_SORTIE> [VAR=val…]
  xvfb
  local d="$1" n="$2"; shift 2
  rm -rf "$d"; mkdir -p "$d"
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_FRAMES_DIR="$d/frames" SIM_SORTIE="$n" SIM_PPM="$d/sim" \
     SC2_SWAPCHAIN=1 SC2_SWAPCHAIN_CONTROLE=1 "$@" \
     bash -c 'ulimit -v 8000000; exec "$@"' _ timeout 2400 qemu-aarch64 "${SIMVK[@]}" "$W/android/build-sim-vk/sc2-sim" > "$d/run.log" 2>&1)
  local rc=$?
  j "  rc=$rc ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons"
  grep -E '^\[VKP\]|^\[SIM\] (zone|sortie|swapchain)|^\[ANDROID\]|^\[ARRÊT\] \[' "$d/run.log" | sed "s|$HOME|~|g" | sed 's/^/  /' | tee -a "$L"
}

for e in $ETAPES; do case $e in
sim-vk)
  cmd "cmake -S android/sim -B android/build-sim-vk -DCMAKE_TOOLCHAIN_FILE=recomp-sc2/cmake/toolchain-aarch64.cmake -DSC2_SWAPCHAIN=ON && make -j$JOBS sc2-sim"
  rm -rf "$W/android/build-sim-vk"
  cmake -S "$W/android/sim" -B "$W/android/build-sim-vk" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" -DSC2_SWAPCHAIN=ON > "$O/sim-vk-cmake.log" 2>&1
  grep -E "delta-propre|SWAPCHAIN" "$O/sim-vk-cmake.log" | sed 's/^/  /' | tee -a "$L"
  chrono "$O/sim-vk-temps" make -C "$W/android/build-sim-vk" -j$JOBS sc2-sim > "$O/sim-vk-make.log" 2>&1; j "  make rc=$? ($(cat "$O/sim-vk-temps"))"
  ;;
run-vk)
  cmd "SC2_SWAPCHAIN=1 SC2_SWAPCHAIN_CONTROLE=1 SIM_SORTIE=150 qemu-aarch64 (arm64-vk-root) android/build-sim-vk/sc2-sim"
  lancer "$O/run-sim-vk" 150
  j "  PPM du backend (mêmes frames, à comparer aux moyennes [VKP] ci-dessus) :"
  grep -E "\[VK\] Present frame" "$O/run-sim-vk/run.log" | sed "s|$O/||" | sed 's/^/    /' | tee -a "$L"
  ;;
run-vk-teinte)
  cmd "SC2_VK_TEINTE=FF204060 SC2_SWAPCHAIN=1 SC2_SWAPCHAIN_CONTROLE=1 SIM_SORTIE=20 … sc2-sim   (frame 1 attendue : R G B = 32 64 96, bandes 0)"
  lancer "$O/run-sim-vk-teinte" 20 SC2_VK_TEINTE=FF204060
  ;;
apk-vk)
  cmd "SC2_APK_SWAPCHAIN=1 scripts/build-apk.sh"
  rm -rf "$W/android/build-vk"
  SC2_APK_SWAPCHAIN=1 JOBS=$JOBS "$W/scripts/build-apk.sh" > "$O/apk-vk.log" 2>&1; j "  rc=$?"
  grep -E "delta-propre|SWAPCHAIN|table SPIR-V|native-code|zipalign :|Mo$|\.apk$" "$O/apk-vk.log" | sed "s|$W/||g" | sed 's/^/  /' | tee -a "$L"
  ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
