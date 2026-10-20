#!/bin/bash
# banc-mac.sh — banc natif macOS arm64 (M4, MoltenVK Homebrew épinglé) : run « défaut » ou mission profilée.
#   scripts/banc-mac.sh defaut  <binaire> <dossier>                         600 frames, empreintes PPM
#   scripts/banc-mac.sh mission <binaire> <dossier> [frames] [délai] [durée] TRaynor01, profil `sample` de <durée> s
#                                                                            pris <délai> s après le lancement
# Données du jeu (hors git) : SC2_EXTRACTION, défaut <clone principal>/extraction (copie de la devbox, 19/10).
# Build : cmake -S recomp-sc2 -B recomp-sc2/build-mac-O2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
#           "-DSC2_LIFTED_OPT=-O2;-fno-strict-aliasing;-fwrapv;-g0;-fno-stack-protector" ; ninja -C … recomp-sc2
# Prérequis : brew install vulkan-headers molten-vk (épinglé), ~/tools/StormLib, ~/tools/theora (sources),
#             recomp-sc2/src/lifted et src/shaders/sc2_shaders_spv.h (générés, hors git ; sans la table SPIR-V, le
#             runtime compile sans shaders : écran noir).
set -u
MODE=${1:?mode defaut|mission}; BIN=$(cd "$(dirname "${2:?binaire}")" && pwd)/$(basename "$2"); D=${3:?dossier}
R="$(cd "$(dirname "$0")/.." && pwd)"
X=${SC2_EXTRACTION:-~/sc2-partout/extraction}
[ -f "$X/install/Versions/Base15405/SC2.exe" ] || { echo "données du jeu absentes : $X"; exit 1; }
rm -rf "$D"; mkdir -p "$D"; D=$(cd "$D" && pwd); cd "$D" || exit 1
# Polices Liberation du dépôt (la devbox les prend dans /usr/share/fonts) : sans elles, C:\Windows\Fonts est vide et le
# lanceur plante au 1er accès à une police (NULL + 0xF dans L_0107F9F0, frame ≈ 40).
COMMUN=(SC2_ROOT="$D/sc2-c" SC2_EXE="$X/install/Versions/Base15405/SC2.exe" SC2_INSTALL="$X/install"
        SC2_FRAMES_DIR="$D/frames" SC2_POLICES="${SC2_POLICES:-$R/android/assets/polices}")
t=$SECONDS
case $MODE in
defaut)
    env "${COMMUN[@]}" SC2_FRAMES_MAX=600 "$BIN" > run.log 2>&1
    echo "rc=$? en $((SECONDS - t)) s" > resume.txt
    python3 -I "$R/android/outils/ppm-sequence.py" "$D/frames" 2>&1 | sed -n 1,6p >> resume.txt ;;
mission)
    F=${4:-2500}; DL=${5:-60}; DUR=${6:-60}
    env "${COMMUN[@]}" SC2_FRAMES_MAX="$F" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 SC2_SANS_VIDEOS=1 \
        SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" "$BIN" > run.log 2>&1 &
    P=$!
    sleep "$DL"
    date +%s > echant.debut; sample "$P" "$DUR" 1 -mayDie -file "$D/sample.txt" > /dev/null 2>&1; date +%s > echant.fin
    wait "$P"; echo "rc=$? en $((SECONDS - t)) s" > resume.txt
    python3 -I "$R/scripts/profil/banc-mac-profil.py" "$D" >> resume.txt 2>&1 ;;
*) echo "mode inconnu : $MODE"; exit 1 ;;
esac
grep -a "SIGSEGV\|SIGBUS\|ARRÊT\] faute\|DEVICE_LOST" run.log | head -3 >> resume.txt
rm -rf "$D/sc2-c"
cat resume.txt
