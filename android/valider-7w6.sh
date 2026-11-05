#!/usr/bin/env bash
# Passe 7w6 — DSP FMOD du jeu (System::createDSP / playDSP / addDSP, rappels read invités) et horloge audio :
# preuve que la cinématique du menu avance. UN run à la fois ; JAMAIS d'ulimit sur les runs ; free -h AVANT/APRÈS consigné.
# LEÇON ENV-EXACT (BUS 04:35Z) : run dir VIERGE, SC2_ROOT + SC2_EXE + SC2_FRAMES_DIR complets ; on juge sur les BILAN.
#   table      : régénère src/shaders/sc2_shaders_spv.h depuis shaders-dumps/{boot,menu,mission,selection} (ulimit -v)
#   x86        : build NEUF recomp-sc2/build-7w6 (cmake + make recomp-sc2, ulimit -v pour le BUILD)
#   defaut<n>  : 600 f, installation de référence ; empreintes attendues be01318cdf69 / d3cd40be732f / 8d1ec057331b
#   menu-<tag> : installation COMPLÈTE (intro cinematic_bettermarine.ogv décodée par le jeu), MENU_FRAMES (défaut 600),
#                1 frame sur PAS (25), SC2_TRACE_DSP=1 ; variables en plus : VARS="SC2_DSP=0" (témoin : rappels coupés)
#                → bilans [DSP] / [AUDIO] / LockRect / [VIDEO-TEX] / textures L8-A8L8, séquence d'empreintes, PNG
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7w6"; O="$B/valider-7w6"; C="$B/captures-7w6"; mkdir -p "$O" "$C"
DEPOT="$HOME/code/reverse/sc2-partout"
INSTALL="$DEPOT/extraction/install"; COMPLET="$DEPOT/extraction/install-complet"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
L="$O/valider${TAG:+-$TAG}.log"; PAS="${PAS:-25}"; FZ="${FRAMES_ZONES:-00100 00200 00300 00400 00500 00600}"
SPV="$R/src/shaders/sc2_shaders_spv.h"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
binaire() { md5sum < "$B/recomp-sc2" 2>/dev/null | cut -c1-12; }
j "=== passe 7w6 ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif., binaire $(binaire)"
run_x86() {   # <nom> <install> <frames> [VAR=val…]
  local nom="$1" d="$O/run-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"
  if pgrep -x recomp-sc2 > /dev/null; then j "  ⚠ un autre recomp-sc2 tourne (PID $(pgrep -x recomp-sc2 | tr '\n' ' ')) : run NON lancé"; return 1; fi
  j "\$ [$nom] binaire $(binaire) ${*:-(aucune variable)} SC2_FRAMES_MAX=$fr — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3000 "$B/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; 1res lectures $(grep -ac '1re lecture' "$d/run.log") — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :|^\[ARRÊT\] \[|^\[BILAN\] \[DSP\]|^\[BILAN\] \[AUDIO\] (mixage|playDSP|System)|^\[BILAN\] \[VIDEO-TEX\]' "$d/run.log" \
    | cut -c1-260 | sed 's/^/  /' | tee -a "$L"
}
for e in "${@:-defaut1}"; do case $e in
table) S="$B/table-src"; rm -rf "$S"; mkdir -p "$S" && cp "$R"/shaders-dumps/{boot,menu,mission,selection}/*.bin "$S"/
  ( ulimit -v 8000000; python3 -B "$W/scripts/d3d9-vers-spirv.py" "$S" "$SPV" > "$O/table.log" 2>&1 ); j "  table rc=$? : $(head -1 "$O/table.log")" ;;
x86)
  ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && make -C "$B" -j3 recomp-sc2 > "$O/x86-make.log" 2>&1 )
  j "  x86 make rc=$? — binaire $(binaire)" ;;
defaut*) run_x86 "$e" "$INSTALL" 600 || continue
  python3 -I "$W/android/outils/ppm-sequence.py" "$O/run-$e/frames" 2>&1 | grep -E '^ +(1|10|100|600) ' | sed 's/^/    /' | tee -a "$L"
  find "$O/run-$e/frames" -name '*.ppm' -delete ;;
menu-*)
  t="${e#menu-}"; d="$O/run-$e"
  # shellcheck disable=SC2086
  run_x86 "$e" "$COMPLET" "${MENU_FRAMES:-600}" SC2_FRAMES_PAS="$PAS" SC2_TRACE_DSP=1 ${VARS:-} || continue
  grep -aE 'cinematic_|\.ogv' "$d/run.log" | grep -a 'MPQ' | cut -c1-160 | sed 's/^/  ~ /' | tee -a "$L"
  grep -aE '^\[DSP\] (createDSP|playDSP|System::addDSP)' "$d/run.log" | head -12 | cut -c1-230 | sed 's/^/  ~ /' | tee -a "$L"
  j "  ~ LockRect (journal D3D9, toutes textures) : $(grep -ac 'IDirect3DTexture9::LockRect' "$d/run.log") lignes ; bilan COM : $(grep -aoE 'LockRect[^,;]*×[0-9]+' "$d/run.log" | head -3 | tr '\n' ' ')"
  grep -aE '^\[BILAN\] \[TEX\]   D3DFMT (50|51) ' "$d/run.log" | cut -c1-160 | sed 's/^/  ~ /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
  for f in $FZ; do
    [ -f "$d/frames/frame_$f.ppm" ] && python3 -I "$W/android/outils/ppm2png.py" "$d/frames/frame_$f.ppm" "$C/menu-$t-f$f.png" > /dev/null 2>&1
  done
  find "$d/frames" -name '*.ppm' -delete; rm -rf "$d/sc2-c" ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
