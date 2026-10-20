#!/usr/bin/env bash
# Passe 7v2 — drapeau runtime « sampler de profondeur » (SC2_OMBRES=1) : preuves. UN run à la fois, JAMAIS d'ulimit sur
# les runs (l'arène réserve ~8 Go d'adresses), free -h consigné. LEÇON ENV-EXACT (BUS 04:35Z) : chaque run part d'un
# dossier VIERGE avec SC2_ROOT + SC2_EXE + SC2_FRAMES_DIR complets ; on juge sur les BILAN, pas sur du texte libre.
#   x86     : build NEUF recomp-sc2/build-7v2 (sous ulimit -v) ;
#   defaut  : 600 f, installation de référence, SANS SC2_OMBRES — ×2 : empreintes be01318c / d3cd40be / 8d1ec057 ;
#   mission : TRaynor01, install-complet, SC2_RUN_SANS_BNET=1, clics « Close » (clic-fermer-aide-7q.txt), SC2_OMBRES=1,
#             MISSION_FRAMES (défaut 1500), une frame sur PAS (100) : bilan [OMBRES] (draws/slots marqués, échantillonnages
#             des cibles de profondeur D32), lignes [OMBRES] (hash PS, slot), PNG dans recomp-sc2/build-7v2/captures.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7v2"; O="$B/valider"; C="$B/captures"; mkdir -p "$O" "$C"
DEPOT="$HOME/code/reverse/sc2-partout"; INST="$DEPOT/extraction/install"; IC="$DEPOT/extraction/install-complet"
EXE="$INST/Versions/Base15405/SC2.exe"; TAG="${TAG:-}"; L="$O/valider${TAG:+-$TAG}.log"; ETAPES="${*:-defaut mission}"
MF="${MISSION_FRAMES:-1500}"; PAS="${PAS:-100}"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
j "=== passe 7v2 ${TAG} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60), arbre $(git -C "$W" status --short | wc -l) modif., binaire $(md5sum < "$B/recomp-sc2" | cut -c1-12)"
run() {   # <nom> <install> <frames> [VAR=val…]
  local d="$O/run${TAG:+-$TAG}-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"; local t0=$SECONDS
  j "--- $(basename "$d") : $(basename "$ins"), $fr f ${*:-(aucune variable)} — free -h AVANT : $(mem)"
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3600 "$B/recomp-sc2" > run.log 2>&1; echo $? > rc)
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; lectures $(grep -ac '1re lecture' "$d/run.log") — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :|^\[BILAN\] \[OMBRES\]|^\[BILAN\] \[INPUT\]' "$d/run.log" | cut -c1-260 | sed 's/^/  /' | tee -a "$L"
}
for e in $ETAPES; do case $e in
x86) ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && make -C "$B" -j3 recomp-sc2 > "$O/x86-make.log" 2>&1 ); j "  x86 make rc=$?" ;;
defaut) for i in 1 2; do run defaut-$i "$INST" 600
          python3 -I "$W/android/outils/ppm-sequence.py" "$O/run${TAG:+-$TAG}-defaut-$i/frames" 2>&1 | grep -E '^ +(1|10|100|600) ' | sed 's/^/    /' | tee -a "$L"; done ;;
mission)
  run mission-ombres "$IC" "$MF" SC2_OMBRES=1 SC2_FRAMES_PAS="$PAS" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 \
      SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_INPUT_SIM="$W/android/outils/clic-fermer-aide-7q.txt"
  d="$O/run${TAG:+-$TAG}-mission-ombres"
  grep -a '^\[OMBRES\]' "$d/run.log" | head -12 | cut -c1-200 | sed 's/^/  /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | grep -E '^ +(1|10|100|500|1000|1200|1500) ' | sed 's/^/    /' | tee -a "$L"
  for f in 01000 01200 $(printf '%05d' "$MF"); do
    [ -f "$d/frames/frame_$f.ppm" ] && python3 -I "$W/android/outils/ppm2png.py" "$d/frames/frame_$f.ppm" "$C/ombres${TAG:+-$TAG}-f$f.png" > /dev/null 2>&1
  done ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
