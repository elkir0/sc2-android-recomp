#!/usr/bin/env bash
# Passe 7w3 — vidéos .ogv : décodeur Theora du runtime (runtime/sc2_video.c), voie écran, défaut inchangé.
# UN run à la fois ; JAMAIS d'ulimit sur les runs ; free -h AVANT/APRÈS consigné.
#   x86                : build de recomp-sc2/build-7w3 (cmake + make recomp-sc2 banc-video-7w3, ulimit -v pour le BUILD)
#   banc               : banc-video-7w3 sur VIDEO (toute la vidéo, 1 frame sur PAS_BANC=30 présentée) :
#                        décodeur → texture Vulkan A8R8G8B8 → quad plein écran → Present (PPM), comparés pixel à pixel
#   defaut<n>          : 600 f, installation de référence, run dir VIERGE avec SC2_ROOT + SC2_EXE + SC2_FRAMES_DIR
#                        complets (leçon ENV-EXACT du BUS 04:35Z), sans variable ; empreintes attendues
#                        f1 be01318cdf69, f10 d3cd40be732f, f100/f600 8d1ec057331b
#   mission            : vue « base », -run TRaynor01, MISSION_FRAMES (défaut 900) + SC2_VIDEO=<archive>|<vidéo>
#                        (fil de décodage du runtime pendant le run, 1 frame sur SC2_VIDEO_PAS=30 écrite en PPM)
# Sorties hors git : recomp-sc2/build-7w3/valider-7w3/ (journaux, runs) et build-7w3/captures-7w3/ (PNG).
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7w3"; O="$B/valider-7w3"; C="$B/captures-7w3"; mkdir -p "$O" "$C"
DEPOT="$HOME/code/reverse/sc2-partout"
INSTALL="$DEPOT/extraction/install"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
ARC="${VIDEO_ARCHIVE:-$(readlink -f "$DEPOT/extraction/install-complet/Campaigns/Liberty.SC2Campaign/base.SC2Assets")}"
VID="${VIDEO:-Assets\\Textures\\briefing_video_traynor01_00.ogv}"
L="$O/valider${TAG:+-$TAG}.log"; MF="${MISSION_FRAMES:-900}"; VUE="${VUE:-$B/vue-base}"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
j "=== passe 7w3 ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif., binaire $(md5sum < "$B/recomp-sc2" 2>/dev/null | cut -c1-12)"
run_x86() {   # <nom> <install> <frames> [VAR=val…]
  local d="$O/run-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"
  j "\$ ${*:-(aucune variable)} SC2_FRAMES_MAX=$fr — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3000 "$B/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; 1res lectures $(grep -ac '1re lecture' "$d/run.log") — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :|^\[BILAN\] \[VIDEO\]|^\[ARRÊT\] \[' "$d/run.log" | cut -c1-260 | sed 's/^/  /' | tee -a "$L"
}
for e in "${@:-defaut1}"; do case $e in
x86)
  ( ulimit -v 8000000; cmake -S "$R" -B "$B" > "$O/x86-cmake.log" 2>&1 && make -C "$B" -j3 recomp-sc2 banc-video-7w3 > "$O/x86-make.log" 2>&1 )
  j "  x86 make rc=$? — $(grep -a 'passe 7w3' "$O/x86-cmake.log" | sed 's/^-- //') ; table $(sha256sum < "$R/src/shaders/sc2_shaders_spv.h" | cut -c1-16)" ;;
banc)
  d="$O/banc"; rm -rf "$d"; mkdir -p "$d"
  j "\$ banc-video-7w3 $VID (1 sur ${PAS_BANC:-30}) — free -h AVANT : $(mem)"; t0=$SECONDS
  "$B/banc-video-7w3" "$ARC" "$VID" 100000 "${PAS_BANC:-30}" "$d" > "$d/banc.log" 2>&1; rc=$?
  j "  rc=$rc ($((SECONDS - t0)) s) — free -h APRÈS : $(mem)"
  grep -aE '^\[VIDEO\] |^\[BILAN\] \[VIDEO-BANC\]' "$d/banc.log" | cut -c1-330 | sed 's/^/  /' | tee -a "$L"
  for f in "$d"/ecran_*.ppm; do
    n=$(basename "$f" .ppm); case $n in ecran_00001|ecran_00301|ecran_00601|ecran_00901)
      python3 -I "$W/android/outils/ppm2png.py" "$f" "$C/banc-$n.png" > /dev/null 2>&1 ;; esac
  done
  find "$d" -name '*.ppm' -delete ;;
defaut*) run_x86 "$e" "$INSTALL" 600; python3 -I "$W/android/outils/ppm-sequence.py" "$O/run-$e/frames" 2>&1 | sed 's/^/    /' | tee -a "$L" ;;
mission)
  [ -d "$VUE" ] || "$W/android/outils/vue-install.sh" "$VUE" base > /dev/null
  d="$O/run-mission"
  run_x86 mission "$VUE" "$MF" SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_RUN_SANS_BNET=1 SC2_FRAMES_PAS=100 \
     SC2_VIDEO="$ARC|$VID" SC2_VIDEO_PAS="${SC2_VIDEO_PAS:-30}"
  grep -aE '^\[VIDEO\]' "$d/run.log" | cut -c1-230 | head -45 | sed 's/^/  | /' | tee -a "$L"
  for f in "$d"/frames/video_*.ppm; do
    n=$(basename "$f" .ppm); case $n in video_00001|video_00301|video_00601|video_00901)
      python3 -I "$W/android/outils/ppm2png.py" "$f" "$C/run-$n.png" > /dev/null 2>&1 ;; esac
  done
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | grep -v video_ | tail -12 | sed 's/^/    /' | tee -a "$L"
  find "$d/frames" -name '*.ppm' -delete ;;   # place disque : PNG gardés pour 4 frames vidéo
menu)   # voie MOTEUR : install complète (enUS), le jeu lit cinematic_bettermarine.ogv et la décode LUI-MÊME (libTheora
        # liée dans SC2.exe, fil 0x00EE0490) ; shaders du quad vidéo dumpés pour la traduction (hors git)
  d="$O/run-menu"
  run_x86 menu "$DEPOT/extraction/install-complet" "${MENU_FRAMES:-600}" SC2_SHADERS_DUMP="$O/shaders-menu"
  grep -aE 'ogv|^\[BILAN\]   (VS|PS) .*module -2|^\[BILAN\] \[TEX\]   D3DFMT' "$d/run.log" | cut -c1-200 | sort | uniq -c | sort -rn | head -20 | sed 's/^/  ~ /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | tail -8 | sed 's/^/    /' | tee -a "$L"
  find "$d/frames" -name '*.ppm' -delete ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
