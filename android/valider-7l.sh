#!/usr/bin/env bash
# Passe 7l — validation LÉGÈRE (3e slot : aucun build massif, un run à la fois, free -h avant/après).
#   relink  : android/outils/relink-runtime.sh (runtime seul, objets liftés figés de la 7i) ;
#   tests   : test_d3d9v (backend Vulkan seul : décodeur BC logiciel, BC natif vs logiciel, mips, cubes), ×2 (BC natif,
#             SC2_VK_SANS_BC=1) + test_win32_compat + cpu_selftest (petits, mêmes drapeaux que le CMake) ;
#   defaut  : x86, installation de référence, ${FRAMES:-150} frames (écran-titre f10, dialogue Battle.net f100) ;
#   mission : x86, vue « base », SC2_RUN_SANS_BNET=1 -run TRaynor01, ${FRAMES} frames (écran de chargement) ;
#   *-sansbc: les mêmes avec SC2_VK_SANS_BC=1 (repli logiciel BC1/2/3 → RGBA8 : le cas Adreno / émulateur).
# BIN=<exe> remplace le binaire relié (mesure AVANT) ; TAG=<suffixe> sépare les dossiers ;
# REF=<tag> compare chaque frame à celle du run run-<tag>-<même nom> (REFNOM=<nom> : autre nom de run).
# Mesures par frame : android/outils/ppm-blocs.py (moyennes locales de blocs 16×16, blocs distincts).
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; O="$R/build-7l-valider"; mkdir -p "$O"
INSTALL="$HOME/code/reverse/sc2-partout/extraction/install"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
TAG="${TAG:-}"; L="$O/valider${TAG:+-$TAG}.log"; ETAPES="${*:-relink tests defaut mission}"
BIN="${BIN:-$R/build-7l/recomp-sc2}"; FRAMES="${FRAMES:-150}"; TMO="${TMO:-600}"
MISSION=(SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_RUN_SANS_BNET=1)
chrono() { local f="$1"; shift; local t0=$SECONDS; "$@"; local rc=$?; echo "$((SECONDS - t0)) s" > "$f"; return $rc; }
j() { echo "$*" | tee -a "$L"; }
j "=== passe 7l ${TAG} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
run_x86() {   # <nom> <install> [VAR=val…]
  local nom="$1" d="$O/run${TAG:+-$TAG}-$1" ins="$2"; shift 2; rm -rf "$d"; mkdir -p "$d"
  j "\$ ${*:-(aucune variable)} SC2_INSTALL=$(basename "$ins") SC2_FRAMES_MAX=$FRAMES timeout $TMO $(basename "$BIN")"
  (cd "$d" && chrono "$d/temps" env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$FRAMES" \
     SC2_FRAMES_DIR="$d/frames" "$@" timeout -s KILL "$TMO" "$BIN" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($(cat "$d/temps")) ; frames écrites : $(ls "$d/frames" 2>/dev/null | wc -l)"
  grep -E '^\[BILAN\] (draws RÉELS|textures D3DX)|^\[BILAN\] \[TEX\] (backend|LockRect|  +(DXT|D3DFMT))|^\[POLICE\]|^\[TEXTE\]|^\[ARRÊT\] \[' "$d/run.log" | cut -c1-200 | sed 's/^/  | /' | tee -a "$L"
  local ref=""; [ -n "${REF:-}" ] && [ -d "$O/run-$REF-${REFNOM:-$nom}/frames" ] && ref="--ref $O/run-$REF-${REFNOM:-$nom}/frames"
  [ -n "$ref" ] && j "    (comparé à run-$REF-${REFNOM:-$nom})"
  python3 -I "$W/android/outils/ppm-blocs.py" "$d/frames" $ref 2>&1 | sed 's/^/    /' | tee -a "$L"
}
for e in $ETAPES; do case $e in
relink) "$W/android/outils/relink-runtime.sh" > "$O/relink.log" 2>&1; j "  relink rc=$? ($(tail -1 "$O/relink.log"))" ;;
tests)
  cc -Wall -Wextra -O1 -I"$R/runtime" -I"$R/build-7l/vk-entetes" "$R/tests/test_d3d9v.c" "$R/runtime/sc2_d3d9v.c" -o "$O/test_d3d9v" -lpthread -ldl \
     > "$O/tests-cc.log" 2>&1; j "  test_d3d9v cc rc=$?"
  (cd "$O" && ./test_d3d9v > "$O/test_d3d9v.log" 2>&1); j "  test_d3d9v rc=$? : $(tail -1 "$O/test_d3d9v.log")"
  (cd "$O" && SC2_VK_SANS_BC=1 ./test_d3d9v > "$O/test_d3d9v-sansbc.log" 2>&1); j "  test_d3d9v SC2_VK_SANS_BC=1 rc=$? : $(tail -1 "$O/test_d3d9v-sansbc.log")"
  cc -Wall -Wextra -I"$R/runtime" "$R/tests/test_win32_compat.c" "$R/runtime/win32_compat.c" "$R/runtime/stubs_sc2.c" -o "$O/test_win32_compat" -lpthread \
     && (cd "$O" && ./test_win32_compat > "$O/test_win32_compat.log" 2>&1); j "  test_win32_compat rc=$? : $(tail -1 "$O/test_win32_compat.log")"
  cc -fno-pie -no-pie -std=gnu17 -I"$R/runtime" "$R/runtime/cpu_selftest.c" -o "$O/cpu_selftest" -lm \
     && (cd "$O" && ./cpu_selftest > "$O/cpu_selftest.log" 2>&1); j "  cpu_selftest rc=$? : $(tail -1 "$O/cpu_selftest.log")" ;;
defaut) run_x86 defaut "$INSTALL" ;;
defaut-sansbc) run_x86 defaut-sansbc "$INSTALL" SC2_VK_SANS_BC=1 ;;
mission|mission-sansbc)
  "$W/android/outils/vue-install.sh" "$O/vue-base" base > /dev/null
  if [ $e = mission ]; then run_x86 mission "$O/vue-base" "${MISSION[@]}"; else run_x86 mission-sansbc "$O/vue-base" "${MISSION[@]}" SC2_VK_SANS_BC=1; fi ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
true
