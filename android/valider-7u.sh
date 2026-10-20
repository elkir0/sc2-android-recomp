#!/usr/bin/env bash
# Passe 7u — scénario ÉVÉNEMENTIEL : les clics partent quand l'état attendu est OBSERVÉ (runtime/sc2_input_sim.c,
# « quand:<déclencheur> max:<tours> »). UN run à la fois, JAMAIS d'ulimit sur les runs, free -h consigné.
#   x86     : build NEUF recomp-sc2/build-7u (sous ulimit -v) ;
#   defaut  : 600 f, installation de référence, SANS script — ×2 : empreintes (base be01318c / d3cd40be / 8d1ec057) ;
#   ancien  : 600 f avec un script SANS condition (android/outils/clic-fermer-aide-7q.txt) : chargé/appliqué comme avant ;
#   mission : android/outils/scenario-7u.sim, deux runs à entrée en partie DIFFÉRENTE : A = install-complet (chargement
#             long), B = vue « base » (VUE_B, chargement court) ; FRAMES (défaut 2600), une frame sur PAS (20) ;
#             journal des déclencheurs, sons du héros après l'ordre, zones du HUD (zones-hud-7w.py) en fin de run.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7u"; O="$B/valider"; C="$B/captures"; mkdir -p "$O" "$C"
DEPOT="$HOME/code/reverse/sc2-partout"; INST="$DEPOT/extraction/install"; IC="$DEPOT/extraction/install-complet"
VUE_B="${VUE_B:-$DEPOT/.claude/worktrees/sc2-passe4/recomp-sc2/build-7q-valider/vue-base}"
EXE="$INST/Versions/Base15405/SC2.exe"; TAG="${TAG:-}"; L="$O/valider${TAG:+-$TAG}.log"; ETAPES="${*:-defaut ancien mission}"
FR="${FRAMES:-2600}"; PAS="${PAS:-20}"; SIM="$W/android/outils/scenario-7u.sim"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
j "=== passe 7u ${TAG} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60), arbre $(git -C "$W" status --short | wc -l) modif."
run() {   # <nom> <install> <frames> [VAR=val…]
  local d="$O/run${TAG:+-$TAG}-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"; local t0=$SECONDS
  j "--- $(basename "$d") : $(basename "$ins"), $fr f ${*:-} — free -h AVANT : $(mem)"
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3600 "$B/recomp-sc2" > run.log 2>&1; echo $? > rc)
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; lectures $(grep -ac '1re lecture' "$d/run.log") — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] \[INPUT\]|^\[BILAN\] draws :|SC2_INPUT_SIM=.*chargé' "$d/run.log" | cut -c1-230 | sed 's/^/  /' | tee -a "$L"
}
for e in $ETAPES; do case $e in
x86) ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && make -C "$B" -j3 recomp-sc2 > "$O/x86-make.log" 2>&1 ); j "  x86 make rc=$?" ;;
defaut) for i in 1 2; do run defaut-$i "$INST" 600; python3 -I "$W/android/outils/ppm-sequence.py" "$O/run${TAG:+-$TAG}-defaut-$i/frames" 2>&1 | grep -E '^ +(1|10|100|600) ' | sed 's/^/    /' | tee -a "$L"; done ;;
ancien) run ancien "$INST" 600 SC2_INPUT_SIM="$W/android/outils/clic-fermer-aide-7q.txt"
        grep -ac '^\[INPUT\] frame [0-9]* : souris' "$O/run${TAG:+-$TAG}-ancien/run.log" | sed 's/^/  événements souris appliqués : /' | tee -a "$L" ;;
mission)
  for v in A B; do
    ins="$IC"; [ $v = B ] && ins="$VUE_B"
    run mission-$v "$ins" "$FR" SC2_FRAMES_PAS="$PAS" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 \
        SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_INPUT_SIM="$SIM"
    d="$O/run${TAG:+-$TAG}-mission-$v"
    grep -aE '^\[INPUT\] frame [0-9]+ : déclencheur|^\[INPUT\] observation .*VRAI' "$d/run.log" | cut -c1-230 | sed 's/^/  /' | tee -a "$L"
    j "  sons du héros : $(grep -ao 'playSound « [^»]*CampaignUnits\\Raynor\\[^»]*»' "$d/run.log" | sed 's/.*Raynor\\\\//;s/ »//' | tr '\n' ' ')"
    last=$(ls "$d/frames" | grep '^frame_' | tail -1)
    python3 -I "$W/android/outils/zones-hud-7w.py" "$d/frames/$last" | sed 's/^/    /' | tee -a "$L"
    python3 -I "$W/android/outils/ppm2png.py" "$d/frames/$last" "$C/mission-$v${TAG:+-$TAG}-${last%.ppm}.png" > /dev/null 2>&1
  done ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
