#!/usr/bin/env bash
# Passe 7w4 — shaders vidéo de la voie MOTEUR (VS 5eb9b9a29064f4f6 + PS d6d3e544aca485e2) : table 1 639 → 1 641,
# cinématique du menu (cinematic_bettermarine.ogv, décodée par le jeu lui-même) avant/après, défaut inchangé.
# UN run à la fois ; JAMAIS d'ulimit sur les runs ; free -h AVANT/APRÈS consigné.
#   x86          : build NEUF recomp-sc2/build-7w4 (cmake + make recomp-sc2, ulimit -v pour le BUILD)
#   table:<f>    : pose la table <f> (fichier hors git, lien dur) dans src/shaders/ puis make incrémental (ulimit -v)
#   defaut<n>    : 600 f, installation de référence, run dir VIERGE, SC2_ROOT + SC2_EXE + SC2_FRAMES_DIR complets
#                  (leçon ENV-EXACT) ; empreintes attendues f1 be01318cdf69, f10 d3cd40be732f, f100/f600 8d1ec057331b
#   menu-<tag>   : installation COMPLÈTE (enUS : le jeu joue l'intro cinematic_bettermarine.ogv), MENU_FRAMES (défaut 600),
#                  1 frame sur PAS (défaut 25) ; draws « lumière » par shader, textures vidéo, séquence d'empreintes ;
#                  PNG des frames FZ dans build-7w4/captures-7w4/, PPM gardés dans garde-<tag>/ pour la comparaison
# Sorties hors git : recomp-sc2/build-7w4/valider-7w4/ et build-7w4/captures-7w4/.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7w4"; O="$B/valider-7w4"; C="$B/captures-7w4"; mkdir -p "$O" "$C"
DEPOT="$HOME/code/reverse/sc2-partout"
INSTALL="$DEPOT/extraction/install"; COMPLET="$DEPOT/extraction/install-complet"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
L="$O/valider${TAG:+-$TAG}.log"; PAS="${PAS:-25}"
FZ="${FRAMES_ZONES:-00100 00200 00300 00400 00500 00600}"
SPV="$R/src/shaders/sc2_shaders_spv.h"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
binaire() { md5sum < "$B/recomp-sc2" 2>/dev/null | cut -c1-12; }
j "=== passe 7w4 ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif., binaire $(binaire), table $(grep -c '^    { 0x' "$SPV") ($(sha256sum < "$SPV" | cut -c1-16))"
run_x86() {   # <nom> <install> <frames> [VAR=val…]
  local nom="$1" d="$O/run-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"
  j "\$ [$nom] binaire $(binaire) ${*:-(aucune variable)} SC2_FRAMES_MAX=$fr — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3000 "$B/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; 1res lectures $(grep -ac '1re lecture' "$d/run.log") — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :|^\[ARRÊT\] \[' "$d/run.log" | cut -c1-230 | sed 's/^/  /' | tee -a "$L"
}
for e in "${@:-defaut1}"; do case $e in
x86)
  ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && make -C "$B" -j3 recomp-sc2 > "$O/x86-make.log" 2>&1 )
  j "  x86 make rc=$? — binaire $(binaire)" ;;
table:*)
  f="${e#table:}"; rm -f "$SPV"; ln "$f" "$SPV"; touch "$SPV"   # mtime : sinon make garde l'ancien objet
  ( ulimit -v 8000000; make -C "$B" -j3 recomp-sc2 > "$O/make-table.log" 2>&1 )
  j "  table posée : $(grep -c '^    { 0x' "$SPV") entrées, sha256 $(sha256sum < "$SPV" | cut -c1-16) ; make rc=$? — binaire $(binaire)" ;;
defaut*) run_x86 "$e" "$INSTALL" 600; python3 -I "$W/android/outils/ppm-sequence.py" "$O/run-$e/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
  find "$O/run-$e/frames" -name '*.ppm' -delete ;;
menu-*)
  t="${e#menu-}"; d="$O/run-$e"
  run_x86 "$e" "$COMPLET" "${MENU_FRAMES:-600}" SC2_FRAMES_PAS="$PAS"
  grep -aE 'cinematic_|\.ogv' "$d/run.log" | grep -a 'MPQ' | cut -c1-160 | sed 's/^/  ~ /' | tee -a "$L"
  grep -aE '^\[BILAN\]   (VS|PS) |^\[BILAN\] \[TEX\]   D3DFMT (50|51|21) ' "$d/run.log" | cut -c1-160 | sed 's/^/  ~ /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
  mkdir -p "$O/garde-$t"
  for f in $FZ; do
    [ -f "$d/frames/frame_$f.ppm" ] || continue
    cp "$d/frames/frame_$f.ppm" "$O/garde-$t/"
    python3 -I "$W/android/outils/ppm2png.py" "$d/frames/frame_$f.ppm" "$C/menu-$t-f$f.png" > /dev/null 2>&1
  done
  find "$d/frames" -name '*.ppm' -delete; rm -rf "$d/sc2-c" ;;   # place disque : run dir temporaire nettoyé
esac; done
j "=== fin $(date -u '+%T') UTC"
true
