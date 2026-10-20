#!/usr/bin/env bash
# Passe 7w7 — UnlockRect/UnlockBox → génération (patch préventif 7w5) + 2e DSP du jeu (« MovieRecordingDSP ») +
# marqueurs de réussite 7w5 sur un run menu AVEC DSP (7w6). UN run à la fois (pgrep vérifié) ; JAMAIS d'ulimit sur les
# runs ; free -h AVANT/APRÈS consigné. ENV-EXACT : run dir VIERGE, SC2_ROOT + SC2_EXE + SC2_FRAMES_DIR complets.
#   x86        : build NEUF recomp-sc2/build-7w7 (ulimit -v pour le BUILD seulement)
#   defaut<n>  : 600 f, installation de référence ; attendu be01318cdf69 / d3cd40be732f / 8d1ec057331b
#   menu       : install-complet, MENU_FRAMES (600), PAS (25), SC2_TEX_DUMP → outils/video-7w5-plans.py (générations et
#                contenu des plans Y L8 1280×720 / CbCr A8L8 640×360), séquence d'empreintes, PNG
#   film       : menu + SC2_DSP_TEST=film (le runtime lance l'enregistrement vidéo du jeu 0x009E7340) + SC2_TRACE_DSP=1 :
#                MovieRecordingDSP créé, posé par addDSP, rappel read 0x009E7260 appelé, échantillons capturés
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7w7"; O="$B/valider-7w7"; C="$B/captures-7w7"; mkdir -p "$O" "$C"
DEPOT="$HOME/code/reverse/sc2-partout"
INSTALL="$DEPOT/extraction/install"; COMPLET="$DEPOT/extraction/install-complet"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
L="$O/valider${TAG:+-$TAG}.log"; PAS="${PAS:-25}"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
binaire() { md5sum < "$B/recomp-sc2" 2>/dev/null | cut -c1-12; }
j "=== passe 7w7 ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif., binaire $(binaire)"
run_x86() {   # <nom> <install> <frames> [VAR=val…]
  local nom="$1" d="$O/run-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"
  if pgrep -x recomp-sc2 > /dev/null; then j "  ⚠ un autre recomp-sc2 tourne (PID $(pgrep -x recomp-sc2 | tr '\n' ' ')) : run NON lancé"; return 1; fi
  j "\$ [$nom] binaire $(binaire) ${*:-(aucune variable)} SC2_FRAMES_MAX=$fr — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3000 "$B/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :|^\[ARRÊT\] \[|^\[BILAN\] \[DSP\]|^\[BILAN\] \[AUDIO\] (mixage|playDSP)' "$d/run.log" | cut -c1-240 | sed 's/^/  /' | tee -a "$L"
}
for e in "${@:-defaut1}"; do case $e in
x86)
  ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && make -C "$B" -j"${MAKE_J:-3}" recomp-sc2 > "$O/x86-make.log" 2>&1 )
  j "  x86 make rc=$? — binaire $(binaire)" ;;
defaut*) run_x86 "$e" "$INSTALL" 600 || continue
  python3 -I "$W/android/outils/ppm-sequence.py" "$O/run-$e/frames" 2>&1 | grep -E '^ +(1|10|100|600) ' | sed 's/^/    /' | tee -a "$L"
  find "$O/run-$e/frames" -name '*.ppm' -delete; rm -rf "$O/run-$e/sc2-c" ;;
menu)
  d="$O/run-menu"
  run_x86 menu "$COMPLET" "${MENU_FRAMES:-600}" SC2_FRAMES_PAS="$PAS" SC2_TEX_DUMP="$d/tex" || continue
  grep -aE '^\[BILAN\] \[TEX\]   D3DFMT (50|51) |3DTexture9::LockRect×' "$d/run.log" | cut -c1-200 | sed 's/^/  ~ /' | tee -a "$L"
  grep -aoE 'IDirect3DTexture9::LockRect[^,]*×[0-9]+' "$d/run.log" | head -2 | sed 's/^/  ~ /' | tee -a "$L"
  python3 -I "$W/android/outils/video-7w5-plans.py" "$d/tex" "$d/run.log" --png "$C" 2>&1 | sed 's/^/    /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
  for f in 00300 00500 00600; do [ -f "$d/frames/frame_$f.ppm" ] && python3 -I "$W/android/outils/ppm2png.py" "$d/frames/frame_$f.ppm" "$C/menu-f$f.png" > /dev/null 2>&1; done
  find "$d/frames" "$d/tex" -name '*.p[pg]m' -delete; rm -rf "$d/sc2-c" ;;
film)
  d="$O/run-film"
  run_x86 film "$COMPLET" "${MENU_FRAMES:-600}" SC2_FRAMES_PAS="$PAS" SC2_DSP_TEST=film SC2_TRACE_DSP=1 || continue
  grep -aE '^\[DSP\] (DIAGNOSTIC|createDSP n°[0-9]+ « Movie|System::addDSP|read n°[0-9]+ « Movie|DSP n°[0-9]+ setActive)' "$d/run.log" | head -12 | cut -c1-260 | sed 's/^/  ~ /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | grep -E '^ +(100|300|500|600) ' | sed 's/^/    /' | tee -a "$L"
  find "$d/frames" -name '*.ppm' -delete; rm -rf "$d/sc2-c" ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
