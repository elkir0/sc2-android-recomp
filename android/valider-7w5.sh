#!/usr/bin/env bash
# Passe 7w5 — diagnostic du re-téléversement des plans vidéo (D3DFMT 50 L8 / 51 A8L8) de la voie MOTEUR.
# AUCUNE ligne du runtime modifiée : variables déjà présentes (SC2_TEX_DUMP, SC2_FRAMES_PAS) + outil neuf
# android/outils/video-7w5-plans.py. UN run à la fois ; JAMAIS d'ulimit sur les runs ; free -h AVANT/APRÈS consigné.
#   x86        : make incrémental de recomp-sc2/build-7w4 (main 3bbc152 fusionnée ; ulimit -v pour le BUILD seulement)
#   defaut1    : 600 f, installation de référence, run dir VIERGE, SC2_ROOT + SC2_EXE + SC2_FRAMES_DIR complets
#                (leçon ENV-EXACT) ; empreintes attendues f1 be01318cdf69, f10 d3cd40be732f, f100/f600 8d1ec057331b
#   menu       : installation COMPLÈTE (enUS : cinematic_bettermarine.ogv décodée par le jeu), MENU_FRAMES (défaut 600),
#                1 frame sur PAS (25), SC2_TEX_DUMP : chaque téléversement Vulkan vidé avec sa clé et sa génération
#                (O_GEN = nombre de Lock/Update reçus) → video-7w5-plans.py (générations et contenu des plans L8/A8L8)
# Sorties hors git : recomp-sc2/build-7w4/valider-7w5/ (journaux) et build-7w4/captures-7w5/ (PNG).
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7w4"; O="$B/valider-7w5"; C="$B/captures-7w5"; mkdir -p "$O" "$C"
DEPOT="$HOME/code/reverse/sc2-partout"
INSTALL="$DEPOT/extraction/install"; COMPLET="$DEPOT/extraction/install-complet"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
L="$O/valider${TAG:+-$TAG}.log"; PAS="${PAS:-25}"
SPV="$R/src/shaders/sc2_shaders_spv.h"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
binaire() { md5sum < "$B/recomp-sc2" 2>/dev/null | cut -c1-12; }
j "=== passe 7w5 ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif., binaire $(binaire), table $(grep -c '^    { 0x' "$SPV") ($(sha256sum < "$SPV" | cut -c1-16))"
run_x86() {   # <nom> <install> <frames> [VAR=val…]
  local nom="$1" d="$O/run-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"
  j "\$ [$nom] binaire $(binaire) ${*:-(aucune variable)} SC2_FRAMES_MAX=$fr — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3000 "$B/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :|^\[ARRÊT\] \[' "$d/run.log" | cut -c1-230 | sed 's/^/  /' | tee -a "$L"
}
for e in "${@:-defaut1}"; do case $e in
x86)
  ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && make -C "$B" -j"${MAKE_J:-3}" recomp-sc2 > "$O/x86-make.log" 2>&1 )
  j "  x86 make rc=$? — binaire $(binaire)" ;;
defaut*) run_x86 "$e" "$INSTALL" 600; python3 -I "$W/android/outils/ppm-sequence.py" "$O/run-$e/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
  find "$O/run-$e/frames" -name '*.ppm' -delete; rm -rf "$O/run-$e/sc2-c" ;;
menu)
  d="$O/run-menu"
  run_x86 menu "$COMPLET" "${MENU_FRAMES:-600}" SC2_FRAMES_PAS="$PAS" SC2_TEX_DUMP="$d/tex"
  grep -aE '\.ogv' "$d/run.log" | grep -a 'MPQ' | cut -c1-160 | sed 's/^/  ~ /' | tee -a "$L"
  grep -aE '^\[BILAN\] \[TEX\]   D3DFMT (50|51) |3DTexture9::LockRect×' "$d/run.log" | cut -c1-200 | sed 's/^/  ~ /' | tee -a "$L"
  python3 -I "$W/android/outils/video-7w5-plans.py" "$d/tex" "$d/run.log" --png "$C" 2>&1 | sed 's/^/    /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | tail -8 | sed 's/^/    /' | tee -a "$L"
  find "$d/frames" "$d/tex" -name '*.p[pg]m' -delete; rm -rf "$d/sc2-c" ;;   # place disque : run dir temporaire nettoyé
esac; done
j "=== fin $(date -u '+%T') UTC"
true
