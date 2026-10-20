#!/usr/bin/env bash
# Passe 8h — entrées souris + clavier (Bluetooth) : preuves sur le banc x86.
#   x86       : recomp-sc2 x86 du worktree (cmake + make dans recomp-sc2/build-8h, ulimit -v sur le BUILD seulement)
#   test      : test hôte du pont Android (android/outils/test-entrees-8h.c, 18 cas)
#   mission   : TRaynor01 + android/outils/scenario-8h.sim (sélection, ordre droit, survol, molette, Maj, raccourci A),
#               MISSION_FRAMES (défaut 1400), captures 1 frame sur PAS (défaut 10), TMO (défaut 3000 s)
#   bilan     : messages livrés à la wndproc par type, frames des étapes, captures autour de la molette
#   defaut<n> : non-régression ENV-EXACT (run dir VIERGE, SC2_ROOT + SC2_EXE + SC2_FRAMES_DIR), 600 f ;
#               attendu f1 be01318cdf69, f10 d3cd40be732f, f100/f600 8d1ec057331b
# UN run à la fois (garde pgrep -x recomp-sc2), JAMAIS d'ulimit sur les runs, free -h AVANT/APRÈS consigné.
# Sorties hors git : recomp-sc2/build-8h/runs/.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-8h"; O="$B/runs"; mkdir -p "$O"
DEPOT="$HOME/code/reverse/sc2-partout"; EXE="$DEPOT/extraction/install/Versions/Base15405/SC2.exe"
INSTALL="$DEPOT/extraction/install"; IC="$DEPOT/extraction/install-complet"; BIN="$B/recomp-sc2"
L="$O/valider${TAG:+-$TAG}.log"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
libre() { local p; p=$(pgrep -x recomp-sc2); [ -z "$p" ] || { j "  ⚠ run concurrent ($(echo $p)) : étape sautée"; return 1; }; }
j "=== passe 8h ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60)"
for e in ${*:-test mission bilan}; do case $e in
x86)
  ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && nice -n 10 make -C "$B" -j"${MAKE_J:-2}" recomp-sc2 > "$O/x86-make.log" 2>&1 )
  j "  x86 make rc=$? — binaire $(md5sum < "$BIN" | cut -c1-12)" ;;
test)
  cc -O1 -Wall -Wextra -I "$W/android/jni" "$W/android/outils/test-entrees-8h.c" -o "$O/test-entrees-8h" && "$O/test-entrees-8h" | tail -1 | sed 's/^/  test hôte : /' | tee -a "$L" ;;
mission)
  libre || continue
  D="$O/run${TAG:+-$TAG}-mission"; rm -rf "$D"; mkdir -p "$D"; t0=$SECONDS
  j "--- mission (binaire $(md5sum < "$BIN" | cut -c1-12)) — free -h AVANT : $(mem)"
  (cd "$D" && env SC2_ROOT="$D/sc2-c" SC2_INSTALL="$IC" SC2_EXE="$EXE" SC2_FRAMES_MAX="${MISSION_FRAMES:-1400}" \
     SC2_FRAMES_DIR="$D/frames" SC2_FRAMES_PAS="${PAS:-10}" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 \
     SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_INPUT_SIM="$W/android/outils/scenario-8h.sim" \
     timeout -s INT "${TMO:-3000}" "$BIN" > run.log 2>&1; echo $? > rc)
  j "  rc=$(cat "$D/rc") ($((SECONDS - t0)) s), $(ls "$D/frames" 2>/dev/null | wc -l) captures — free -h APRÈS : $(mem)"
  rm -rf "$D/sc2-c" ;;
bilan)
  D="$O/run${TAG:+-$TAG}-mission"; [ -f "$D/run.log" ] || { j "  pas de run mission"; continue; }
  g() { grep -a "$@" "$D/run.log"; }
  j "  $(g '^\[BILAN\] \[INPUT\]' | head -2 | tr '\n' ' ')"
  for m in WM_LBUTTONDOWN WM_RBUTTONDOWN WM_MOUSEMOVE WM_MOUSEWHEEL WM_KEYDOWN WM_KEYUP WM_CHAR; do
    j "  $m : $(g -c "^\[INPUT\] frame [0-9]* : $m ") livrés à la wndproc"; done
  g '^\[INPUT\] frame [0-9]* : \(déclencheur\|molette\|touche\)\|^\[INPUT\] frame [0-9]* : WM_\(MOUSEWHEEL\|KEYDOWN\|RBUTTONDOWN\|CHAR\)' \
    | cut -c1-200 | sed 's/^/  | /' | tee -a "$L" ;;
defaut*)
  libre || continue
  D="$O/run-$e"; rm -rf "$D"; mkdir -p "$D"; t0=$SECONDS
  j "--- $e (binaire $(md5sum < "$BIN" | cut -c1-12)) — free -h AVANT : $(mem)"
  (cd "$D" && env SC2_ROOT="$D/sc2-c" SC2_INSTALL="$INSTALL" SC2_EXE="$EXE" SC2_FRAMES_MAX=600 SC2_FRAMES_DIR="$D/frames" \
     timeout -s KILL 3000 "$BIN" > run.log 2>&1; echo $? > rc)
  j "  rc=$(cat "$D/rc") ($((SECONDS - t0)) s) — free -h APRÈS : $(mem)"
  python3 -I "$W/android/outils/ppm-sequence.py" "$D/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
  rm -rf "$D/sc2-c" "$D/frames" ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
