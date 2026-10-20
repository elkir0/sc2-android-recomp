#!/usr/bin/env bash
# Passe 7v3 — traducteur « ombres » (variante Dref des PS sous profondeur_k) : preuves. UN run à la fois, JAMAIS d'ulimit
# sur les runs (l'arène réserve ~8 Go d'adresses), free -h consigné. LEÇON ENV-EXACT (BUS 04:35Z) : chaque run part d'un
# dossier VIERGE avec SC2_ROOT + SC2_EXE + SC2_FRAMES_DIR complets ; on juge sur les BILAN, pas sur du texte libre.
#   table   : régénère src/shaders/sc2_shaders_spv.h depuis shaders-dumps/{boot,menu,mission,selection} avec
#             --ombres android/outils/profondeur-ps-7v2.txt (sous ulimit -v) ;
#   x86     : build NEUF recomp-sc2/build-7v3 (sous ulimit -v) ;
#   defaut  : 600 f, installation de référence, SANS SC2_OMBRES — ×2 : empreintes be01318c / d3cd40be / 8d1ec057 ;
#   mission : TRaynor01, install-complet, SC2_RUN_SANS_BNET=1, clics « Close » (clic-fermer-aide-7q.txt), SC2_OMBRES=1 ;
#   temoin  : la même SANS SC2_OMBRES (A de l'A/B) ; ab : outils/ombres-ab-7v3.py sur les frames communes ;
#   menu    : menu de campagne install-complet (cinématique .ogv du jeu), bilan [VIDEO-TEX] (demande croisée 7w3).
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7v3"; O="$B/valider"; C="$B/captures"; mkdir -p "$O" "$C"
DEPOT="$HOME/code/reverse/sc2-partout"; INST="$DEPOT/extraction/install"; IC="$DEPOT/extraction/install-complet"
EXE="$INST/Versions/Base15405/SC2.exe"; TAG="${TAG:-}"; L="$O/valider${TAG:+-$TAG}.log"; ETAPES="${*:-defaut mission temoin ab}"
MF="${MISSION_FRAMES:-1500}"; PAS="${PAS:-100}"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
j "=== passe 7v3 ${TAG} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60), arbre $(git -C "$W" status --short | wc -l) modif., binaire $(md5sum < "$B/recomp-sc2" | cut -c1-12)"
run() {   # <nom> <install> <frames> [VAR=val…]
  local d="$O/run${TAG:+-$TAG}-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"; local t0=$SECONDS
  j "--- $(basename "$d") : $(basename "$ins"), $fr f ${*:-(aucune variable)} — free -h AVANT : $(mem)"
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3600 "$B/recomp-sc2" > run.log 2>&1; echo $? > rc)
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; lectures $(grep -ac '1re lecture' "$d/run.log") — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :|^\[BILAN\] \[OMBRES\]|^\[BILAN\] \[INPUT\]|^\[BILAN\] \[VIDEO-TEX\]' "$d/run.log" | cut -c1-260 | sed 's/^/  /' | tee -a "$L"
}
for e in $ETAPES; do case $e in
table) S="$B/table-src"; rm -rf "$S"; mkdir -p "$S" && cp "$R"/shaders-dumps/{boot,menu,mission,selection}/*.bin "$S"/
       ( ulimit -v 8000000; python3 -B "$W/scripts/d3d9-vers-spirv.py" "$S" "$R/src/shaders/sc2_shaders_spv.h" --glsl "$B/glsl" \
           --ombres "$W/android/outils/profondeur-ps-7v2.txt" > "$O/table.log" 2>&1 ); j "  table rc=$? : $(grep -a 'traduits\|variantes' "$O/table.log" | tr '\n' ' ')" ;;
x86) ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && make -C "$B" -j3 recomp-sc2 > "$O/x86-make.log" 2>&1 ); j "  x86 make rc=$?" ;;
defaut) for i in 1 2; do run defaut-$i "$INST" 600
          python3 -I "$W/android/outils/ppm-sequence.py" "$O/run${TAG:+-$TAG}-defaut-$i/frames" 2>&1 | grep -E '^ +(1|10|100|600) ' | sed 's/^/    /' | tee -a "$L"; done ;;
mission|temoin)
  if [ "$e" = mission ]; then nom=mission-ombres; ov=SC2_OMBRES=1; else nom=mission-temoin; ov=SC2_OMBRES=0; fi
  run "$nom" "$IC" "$MF" "$ov" SC2_FRAMES_PAS="$PAS" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 \
      SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_INPUT_SIM="$W/android/outils/clic-fermer-aide-7q.txt"
  d="$O/run${TAG:+-$TAG}-$nom"
  grep -a '^\[OMBRES\]' "$d/run.log" | head -6 | cut -c1-200 | sed 's/^/  /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | grep -E '^ +(1|10|100|500|1000|1200|1500) ' | sed 's/^/    /' | tee -a "$L"
  for f in 01000 01200 $(printf '%05d' "$MF"); do
    [ -f "$d/frames/frame_$f.ppm" ] && python3 -I "$W/android/outils/ppm2png.py" "$d/frames/frame_$f.ppm" "$C/$nom${TAG:+-$TAG}-f$f.png" > /dev/null 2>&1
  done ;;
ab)
  a="$O/run${TAG:+-$TAG}-mission-temoin/frames"; b="$O/run${TAG:+-$TAG}-mission-ombres/frames"
  for f in $(ls "$a" | grep -E 'frame_0(0[6-9]|1[0-5])00\.ppm' | sort); do
    [ -f "$b/$f" ] || continue
    j "  A/B $f : $(python3 -I "$W/android/outils/ombres-ab-7v3.py" "$a/$f" "$b/$f" "$C/ab${TAG:+-$TAG}-${f%.ppm}.png" | tr '\n' ' ')"
  done ;;
menu)
  run menu-video "$IC" "${MENU_FRAMES:-700}" SC2_FRAMES_PAS="$PAS" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1
  d="$O/run${TAG:+-$TAG}-menu-video"
  grep -aE '^\[BILAN\] \[VIDEO-TEX\]' "$d/run.log" | sed 's/^/  /' | tee -a "$L"
  grep -a '\[VIDEO-TEX\]' "$d/run.log" | grep -v BILAN | head -8 | cut -c1-200 | sed 's/^/  /' | tee -a "$L" ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
