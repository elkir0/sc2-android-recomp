#!/usr/bin/env bash
# Passe 7e — validation de l'audio Android (FMOD logiciel du runtime lié + sortie AAudio par anneau) :
# banc android/sim reconstruit : 300 f (non-régression du rendu, lignes [AUDIO], 0 FMOD_ERR_OUTPUT_INIT),
# 150 f SC2_AUDIO_TEST (DIAGNOSTIC étiqueté : 2 vrais sons du jeu joués par le RUNTIME → échantillons non
# nuls à travers l'anneau), 60 f SC2_AUDIO_ANDROID=0 ; builds neufs x86 + arm64 + ctest ; contrôle des
# fichiers interdits ; APK normal + vk construits DEUX fois de zéro.
# Builds sous ulimit -v 8000000 ; runs SANS ulimit, banc avec MALLOC_ARENA_MAX=2.
#
#   android/valider-7e.sh [étape…]   étapes : sim run-sim ctest interdits apk adb (défaut : toutes)
# Journal : android/build-7e/valider.log
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"
R="$W/recomp-sc2"
O="$W/android/build-7e"; mkdir -p "$O"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"
A="$INSTALL/Mods/Liberty.SC2Mod/base.SC2Assets"
TEST="$A|Assets\\Music\\UI_Stinger_Complete01.ogg;$A|Assets\\Sounds\\Protoss\\Archon\\Archon_AttackImpact0.wav_"
JOBS=3
SIMVK=(-L "$HOME/tools/arm64-vk-root" -E LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu
       -E VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json -E MALLOC_ARENA_MAX=2)
ETAPES="${*:-sim run-sim ctest interdits apk adb}"
L="$O/valider.log"
chrono() { local f="$1"; shift; local t0=$SECONDS; "$@"; local rc=$?; echo "$((SECONDS - t0)) s" > "$f"; return $rc; }
borne() { ( ulimit -v 8000000; "$@" ); }
j() { echo "$*" | tee -a "$L"; }
cmd() { j "\$ $*"; }
j "=== passe 7e — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"

run_sim() {   # <dossier> <SIM_SORTIE> [VAR=val…]
  local d="$1" n="$2"; shift 2; rm -rf "$d"; mkdir -p "$d"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_FRAMES_DIR="$d/frames" SIM_SORTIE="$n" \
     SIM_PPM="$d/sim" "$@" timeout 3600 qemu-aarch64 "${SIMVK[@]}" "$W/android/build-sim/sc2-sim" > "$d/run.log" 2>&1)
  j "  rc=$? ($(cat "$d/temps")) ; $(grep -c '^\s*\[x\]' "$d/run.log") jalons ; FMOD_ERR_OUTPUT_INIT : $(grep -c OUTPUT_INIT "$d/run.log") ; lignes [AUDIO] : $(grep -c '^\[AUDIO\]' "$d/run.log")"
  grep -E '^\[AUDIO\] (FMOD_System_Create →|Android|sortie temps réel|DIAGNOSTIC)|^\[SIM\] sortie|^\[BILAN\] \[AUDIO\] (mixage|Android)|^\[BILAN\] draws RÉELS|^\[ARRÊT\] \[' \
    "$d/run.log" | sed "s|$HOME|~|g" | cut -c1-330 | sed 's/^/  /' | tee -a "$L"
}

for e in $ETAPES; do case $e in
sim)
  cmd "cmake -S android/sim -B android/build-sim -DCMAKE_TOOLCHAIN_FILE=recomp-sc2/cmake/toolchain-aarch64.cmake && make -j$JOBS sc2-sim"
  rm -rf "$W/android/build-sim"
  borne cmake -S "$W/android/sim" -B "$W/android/build-sim" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" > "$O/sim-cmake.log" 2>&1
  grep -E "delta-propre" "$O/sim-cmake.log" | sed 's/^/  /' | tee -a "$L"
  borne chrono "$O/sim-temps" make -C "$W/android/build-sim" -j$JOBS sc2-sim > "$O/sim-make.log" 2>&1; j "  make rc=$? ($(cat "$O/sim-temps"))"
  ;;
run-sim)
  cmd "SIM_SORTIE=300 … sc2-sim   (non-régression rendu : 7d = 300/301 non noires ; audio lié)"
  run_sim "$O/run-sim" 300
  cmd "SC2_AUDIO_TEST='<base.SC2Assets>|UI_Stinger_Complete01.ogg;…|Archon_AttackImpact0.wav_' SIM_SORTIE=150 … sc2-sim   (DIAGNOSTIC étiqueté : sons joués par le RUNTIME)"
  run_sim "$O/run-sim-test" 150 SC2_AUDIO_TEST="$TEST"
  cmd "SC2_AUDIO_ANDROID=0 SIM_SORTIE=60 … sc2-sim   (sortie Android coupée : FMOD logiciel seul)"
  run_sim "$O/run-sim-coupe" 60 SC2_AUDIO_ANDROID=0
  ;;
ctest)
  for a in x86 arm64; do
    b="$R/build-7e-$a"; rm -rf "$b"
    if [ $a = x86 ]; then cmd "cmake -S recomp-sc2 -B recomp-sc2/build-7e-x86 && make -j$JOBS && ctest"
         borne cmake -S "$R" -B "$b" > "$O/$a-cmake.log" 2>&1
    else cmd "cmake -S recomp-sc2 -B recomp-sc2/build-7e-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake -DCMAKE_BUILD_TYPE=Debug && make -j$JOBS && ctest"
         borne cmake -S "$R" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" -DCMAKE_BUILD_TYPE=Debug > "$O/$a-cmake.log" 2>&1; fi
    borne chrono "$O/$a-temps" make -C "$b" -j$JOBS > "$O/$a-make.log" 2>&1; j "  $a make rc=$? ($(cat "$O/$a-temps"))"
    (cd "$b" && ctest > "$O/$a-ctest.log" 2>&1); j "  $a ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/$a-ctest.log")"
  done
  ;;
interdits)
  cmd "git diff --stat main -- recomp-sc2/runtime recomp-sc2/src scripts/d3d9* (fichiers interdits : attendu vide)"
  n=$(git -C "$W" diff --stat main -- recomp-sc2/runtime recomp-sc2/src 'scripts/d3d9*' | wc -l)
  j "  $n ligne(s) de diff ; fichiers touchés depuis main : $(git -C "$W" diff --name-only main | tr '\n' ' ')"
  ;;
apk)
  for v in 0 1; do
    for passe in 1 2; do
      b="$W/android/build$([ $v = 1 ] && echo -vk)"
      cmd "SC2_APK_SWAPCHAIN=$v scripts/build-apk.sh   (construction $passe/2, dossier vidé)"
      rm -rf "$b"
      SC2_APK_SWAPCHAIN=$v JOBS=$JOBS borne "$W/scripts/build-apk.sh" > "$O/apk-$v-$passe.log" 2>&1; j "  rc=$?"
      grep -E "delta-propre|SWAPCHAIN=ON|table SPIR-V|ATTENTION|native-code|zipalign :|Mo$|\.apk$|libaaudio" "$O/apk-$v-$passe.log" | sed "s|$W/||g" | sed 's/^/  /' | tee -a "$L"
    done
    a1=$(grep -E '\.apk$' "$O/apk-$v-1.log" | tail -1 | cut -c1-64); a2=$(grep -E '\.apk$' "$O/apk-$v-2.log" | tail -1 | cut -c1-64)
    j "  reproductibilité SC2_APK_SWAPCHAIN=$v : $([ -n "$a1" ] && [ "$a1" = "$a2" ] && echo "IDENTIQUES ($a1)" || echo "DIFFÉRENTS ($a1 / $a2)")"
  done
  ;;
adb)
  cmd "adb devices"
  if command -v adb > /dev/null; then adb devices 2>&1 | sed 's/^/  /' | tee -a "$L"
  else j "  adb absent du devbox : test émulateur (logcat [AUDIO] FMOD logiciel + flux AAudio) par la supervision"; fi
  ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
