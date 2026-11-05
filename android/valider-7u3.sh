#!/usr/bin/env bash
# Passe 7u-étape3 — R2 Linux des patchs runtime MoltenVK/macOS (SPEC 7u-moltenvk §4 étape 3).
# Linux doit rester inchangé : seule la ligne « [BILAN] draws D3DPT_TRIANGLEFAN » est neuve hors __APPLE__.
# UN run à la fois (pgrep vérifié) ; JAMAIS d'ulimit sur les runs ; free -h AVANT/APRÈS consigné.
#   base       : build NEUF recomp-sc2/build-7u3 sur les 3 fichiers de main (c471528) — ulimit -v pour le BUILD seul
#   patch      : mêmes fichiers rendus à HEAD, make incrémental (2 sources C) + ctest
#   defaut<n>  : 600 f, installation de référence ; attendu be01318cdf69 / d3cd40be732f / 8d1ec057331b
# Lift et table SPIR-V : liens DURS vers le checkout principal (lecture seule, aucun octet copié).
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7u3"; O="$B/valider-7u3"; mkdir -p "$O"
DEPOT="$HOME/code/reverse/sc2-partout"; BASE="${BASE:-c471528}"
INSTALL="$DEPOT/extraction/install"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
FICHIERS="recomp-sc2/runtime/sc2_runtime.c recomp-sc2/runtime/sc2_d3d9v.c recomp-sc2/CMakeLists.txt"
L="$O/valider${TAG:+-$TAG}.log"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
binaire() { md5sum < "$B/recomp-sc2" 2>/dev/null | cut -c1-12; }
lourd() { pgrep -x recomp-sc2 > /dev/null || pgrep -f "sc2-si[m]" > /dev/null; }
poser_donnees() {
  [ -e "$R/src/lifted/recomp_00_0.c" ] || ln "$DEPOT"/recomp-sc2/src/lifted/recomp_*.c "$DEPOT/recomp-sc2/src/lifted/recomp_sc2_list.h" "$R/src/lifted/"
  [ -e "$R/src/shaders/sc2_shaders_spv.h" ] || ln "$DEPOT/recomp-sc2/src/shaders/sc2_shaders_spv.h" "$R/src/shaders/sc2_shaders_spv.h"
}
construire() {   # <étiquette>
  j "\$ [$1] build — free -h AVANT : $(mem)"
  local t0=$SECONDS
  ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/$1-cmake.log" 2>&1 && nice make -C "$B" -j"${MAKE_J:-3}" > "$O/$1-make.log" 2>&1 )
  j "  [$1] make rc=$? ($((SECONDS - t0)) s) — binaire $(binaire), $(ls -l --time-style=+%T "$B/recomp-sc2" | awk '{print $6}') — free -h APRÈS : $(mem)"
}
run_x86() {   # <nom> <frames> [VAR=val…]
  local nom="$1" d="$O/run-$1" fr="$2"; shift 2; rm -rf "$d"; mkdir -p "$d"
  if lourd; then j "  ⚠ un run lourd tourne : run $nom NON lancé"; return 1; fi
  j "\$ [$nom] binaire $(binaire) ${*:-(aucune variable)} SC2_FRAMES_MAX=$fr — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3000 "$B/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :|^\[BILAN\] draws D3DPT_TRIANGLEFAN|^\[ARRÊT\] \[|^\[VK\] (backend|bibli|portab)' "$d/run.log" | cut -c1-240 | sed 's/^/  /' | tee -a "$L"
}
j "=== passe 7u3 ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
for e in "${@:-base defaut1 defaut2 patch defaut3 defaut4}"; do case $e in
base)
  if lourd; then j "  ⚠ un run lourd tourne : build NON lancé"; continue; fi
  poser_donnees
  git -C "$W" checkout "$BASE" -- $FICHIERS && j "  sources $FICHIERS ← $BASE (main)"
  construire base
  git -C "$W" checkout HEAD -- $FICHIERS && j "  sources rendues à HEAD ($(git -C "$W" status --short | wc -l) modif. hors données)" ;;
patch)
  if lourd; then j "  ⚠ un run lourd tourne : build NON lancé"; continue; fi
  touch "$R/runtime/sc2_runtime.c" "$R/runtime/sc2_d3d9v.c"
  construire patch
  ( cd "$B" && ctest --output-on-failure > "$O/ctest.log" 2>&1 ); j "  ctest rc=$? : $(grep -E 'tests passed|tests failed' "$O/ctest.log")" ;;
defaut*) run_x86 "$e" 600 || continue
  python3 -I "$W/android/outils/ppm-sequence.py" "$O/run-$e/frames" 2>&1 | grep -E '^ +(1|10|100|600) ' | sed 's/^/    /' | tee -a "$L"
  find "$O/run-$e/frames" -name '*.ppm' -delete; rm -rf "$O/run-$e/sc2-c" ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
