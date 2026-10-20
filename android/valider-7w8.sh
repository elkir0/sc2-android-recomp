#!/usr/bin/env bash
# Passe 7w8 — crash Android en chargement de mission (SIGSEGV, RIP hôte 0x760ADC8BBC) : débordement de PILE HÔTE d'un
# code lifté PÉRIMÉ (sans trampoline SC2_SAUT, compilé -O0). Reproduction x86 A/B et santé exposée par le runtime :
#   [LIFT] (racines 7j/7q présentes ?), [BILAN] [PILE] (profondeur max de la pile hôte, sauts SC2_SAUT),
#   [ARRÊT] DÉBORDEMENT DE PILE HÔTE / image hôte + base + RIP − base (symbolisable), [ARÈNE] bilan.
# UN run à la fois (pgrep -x recomp-sc2 vide avant chaque run) ; JAMAIS d'ulimit sur les runs ; free -h AVANT/APRÈS.
# ENV-EXACT : run dir VIERGE, SC2_ROOT + SC2_EXE + SC2_FRAMES_DIR complets.
#   x86       : build NEUF build-7w8, lift COURANT (src/lifted = liens durs vers LIFT_COURANT, défaut sc2-passe4)
#   perime    : build NEUF build-7w8-perime, MÊME runtime, lift PÉRIMÉ (LIFT_PERIME, défaut : src/lifted du checkout
#               principal, celui que compile l'APK) ; src/lifted est rétabli sur le lift courant ensuite
#   defaut<n> : 600 f, installation de référence (build-7w8) ; attendu be01318cdf69 / d3cd40be732f / 8d1ec057331b
#   mission-a : TRaynor01 install-complet, MISSION_FRAMES (900), clics « Close », build-7w8 (lift courant)
#   mission-p : idem, build-7w8-perime (lift périmé) → attendu : DÉBORDEMENT DE PILE HÔTE en chargement
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; O="$R/build-7w8/valider"; mkdir -p "$O"
DEPOT="$HOME/code/reverse/sc2-partout"
INSTALL="$DEPOT/extraction/install"; COMPLET="$DEPOT/extraction/install-complet"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
LIFT_COURANT="${LIFT_COURANT:-$DEPOT/.claude/worktrees/sc2-passe4/recomp-sc2/src/lifted}"
LIFT_PERIME="${LIFT_PERIME:-$DEPOT/recomp-sc2/src/lifted}"
L="$O/valider${TAG:+-$TAG}.log"; MF="${MISSION_FRAMES:-900}"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
bin() { md5sum < "$1/recomp-sc2" 2>/dev/null | cut -c1-12; }
lier() { find "$R/src/lifted" -maxdepth 1 -type f \( -name 'recomp_*.c' -o -name '*.h' -o -name 'LIFT-REPORT.json' \) \
           ! -name 'README.md' -delete 2>/dev/null; cp -al "$1"/. "$R/src/lifted/" 2>/dev/null;
         git -C "$W" checkout -- recomp-sc2/src/lifted 2>/dev/null; }   # fichiers suivis par git : ceux du dépôt
j "=== passe 7w8 ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
run_x86() {   # <nom> <build> <install> <frames> [VAR=val…]
  local nom="$1" B="$2" d="$O/run-$1" ins="$3" fr="$4"; shift 4; rm -rf "$d"; mkdir -p "$d"
  if pgrep -x recomp-sc2 > /dev/null; then j "  ⚠ un autre recomp-sc2 tourne (PID $(pgrep -x recomp-sc2 | tr '\n' ' ')) : run NON lancé"; return 1; fi
  j "\$ [$nom] $(basename "$B") binaire $(bin "$B") ${*:-(aucune variable)} SC2_FRAMES_MAX=$fr — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3600 "$B/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; lectures $(grep -ac '1re lecture' "$d/run.log") — free -h APRÈS : $(mem)"
  grep -aE '^\[LIFT\]|^\[ARRÊT\]|^\[BILAN\] \[PILE\]|^\[BILAN\] draws :|\[ARÈNE\] (bilan|arène principale)|^\[BILAN\] \[MANQUE\] [0-9]' "$d/run.log" \
    | cut -c1-260 | sed 's/^/  /' | tee -a "$L"
}
for e in "${@:-defaut1}"; do case $e in
x86)
  lier "$LIFT_COURANT"
  ( ulimit -v 8000000; cmake -S "$R" -B "$R/build-7w8" > "$O/cmake.log" 2>&1 && make -C "$R/build-7w8" -j"${MAKE_J:-2}" recomp-sc2 > "$O/make.log" 2>&1 )
  j "  x86 (lift courant) make rc=$? — binaire $(bin "$R/build-7w8")" ;;
perime)
  lier "$LIFT_PERIME"
  ( ulimit -v 8000000; cmake -S "$R" -B "$R/build-7w8-perime" > "$O/cmake-perime.log" 2>&1 && \
    make -C "$R/build-7w8-perime" -j"${MAKE_J:-2}" recomp-sc2 > "$O/make-perime.log" 2>&1 )
  j "  x86 (lift PÉRIMÉ $LIFT_PERIME) make rc=$? — binaire $(bin "$R/build-7w8-perime")"
  lier "$LIFT_COURANT" ;;
defaut*) run_x86 "$e" "$R/build-7w8" "$INSTALL" 600 || continue
  python3 -I "$W/android/outils/ppm-sequence.py" "$O/run-$e/frames" 2>&1 | grep -E '^ +(1|10|100|600) ' | sed 's/^/    /' | tee -a "$L"
  find "$O/run-$e/frames" -name '*.ppm' -delete; rm -rf "$O/run-$e/sc2-c" ;;
mission-a|mission-p)
  B="$R/build-7w8"; [ "$e" = mission-p ] && B="$R/build-7w8-perime"
  run_x86 "$e" "$B" "$COMPLET" "$MF" SC2_FRAMES_PAS=100 SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 \
      SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_INPUT_SIM="$W/android/outils/clic-fermer-aide-7q.txt" || continue
  d="$O/run-$e"
  grep -aA6 'pile d.appels invitée' "$d/run.log" | head -7 | cut -c1-120 | sed 's/^/  ~ /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | grep -E '^ +(1|100|300|500|600|900) ' | sed 's/^/    /' | tee -a "$L"
  find "$d/frames" -name '*.ppm' -delete; rm -rf "$d/sc2-c" ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
