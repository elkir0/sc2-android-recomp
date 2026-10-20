#!/usr/bin/env bash
# Passe 7q2 — le terrain 3D de TRaynor01 suit la partie (unités qui se déplacent, caméra, cinématique) sur main.
#   x86     : build NEUF recomp-sc2/build-7q2 (sous ulimit -v) ;
#   mission : install-complet (texte anglais), SC2_RUN_SANS_BNET=1, -run TRaynor01, scénario android/outils/scenario-7q2.sim
#             (aide fermée, Raynor sélectionné, ordres par la minicarte, attaque-déplacement — repris de la 7w2),
#             FRAMES (défaut 1750), une frame sur PAS (défaut 20) ; mesure android/outils/terrain-7q2.py (zone monde),
#             PNG dans recomp-sc2/build-7q2/captures. UN run, JAMAIS d'ulimit sur le run, free -h consigné.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7q2"; O="$B/valider"; C="$B/captures"; mkdir -p "$O" "$C"
DEPOT="$HOME/code/reverse/sc2-partout"; IC="$DEPOT/extraction/install-complet"; EXE="$IC/Versions/Base15405/SC2.exe"
TAG="${TAG:-}"; L="$O/valider${TAG:+-$TAG}.log"; ETAPES="${*:-mission}"; FR="${FRAMES:-1750}"; PAS="${PAS:-20}"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
j "=== passe 7q2 ${TAG} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60), arbre $(git -C "$W" status --short | wc -l) modif."
for e in $ETAPES; do case $e in
x86) ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && make -C "$B" -j3 recomp-sc2 > "$O/x86-make.log" 2>&1 ); j "  x86 make rc=$?" ;;
mission)
  d="$O/run${TAG:+-$TAG}-mission"; rm -rf "$d"; mkdir -p "$d"; j "--- mission $FR f (1 sur $PAS) — free -h AVANT : $(mem)"; t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$IC" SC2_EXE="$EXE" SC2_FRAMES_MAX="$FR" SC2_FRAMES_PAS="$PAS" \
     SC2_FRAMES_DIR="$d/frames" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" \
     SC2_INPUT_SIM="$W/android/outils/scenario-7q2.sim" timeout -s KILL 3000 "$B/recomp-sc2" > run.log 2>&1; echo $? > rc)
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; lectures $(grep -ac '1re lecture' "$d/run.log") ; messages livrés $(grep -ac 'wndproc du jeu' "$d/run.log") — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :' "$d/run.log" | cut -c1-200 | sed 's/^/  /' | tee -a "$L"
  python3 -I "$W/android/outils/terrain-7q2.py" "$d/frames" | sed 's/^/  /' | tee -a "$L"
  for f in 01000 01140 01300 01440 01600 $(printf '%05d' "$FR"); do
    [ -f "$d/frames/frame_$f.ppm" ] && python3 -I "$W/android/outils/ppm2png.py" "$d/frames/frame_$f.ppm" "$C/mission${TAG:+-$TAG}-f$f.png" > /dev/null 2>&1
  done ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
