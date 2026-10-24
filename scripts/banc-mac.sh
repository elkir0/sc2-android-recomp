#!/bin/bash
# banc-mac.sh — banc natif macOS arm64 (M4, MoltenVK Homebrew épinglé) : run « défaut » ou mission profilée.
#   scripts/banc-mac.sh defaut  <binaire> <dossier>                         600 frames, empreintes PPM
#   scripts/banc-mac.sh mission <binaire> <dossier> [frames] [délai] [durée] TRaynor01, profil `sample` de <durée> s
#                                                                            pris <délai> s après le lancement
# Données du jeu (hors git) : SC2_EXTRACTION, défaut <clone principal>/extraction (copie du serveur Linux, 19/10).
# Build : cmake -S recomp-sc2 -B recomp-sc2/build-mac-O2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
#           "-DSC2_LIFTED_OPT=-O2;-fno-strict-aliasing;-fwrapv;-g0;-fno-stack-protector" ; ninja -C … recomp-sc2
# Prérequis : brew install vulkan-headers molten-vk (épinglé), ~/tools/StormLib, ~/tools/theora (sources),
#             recomp-sc2/src/lifted et src/shaders/sc2_shaders_spv.h (générés, hors git ; sans la table SPIR-V, le
#             runtime compile sans shaders : écran noir).
# V0.1 (22/10, scripts/lib-banc.sh) : garde-disque (SC2_DISQUE_MIN_GO + 1 Gio), verrou ~/.sc2-verrous/banc,
#   SC2_FRAMES=1 explicite, puis compactage : <dossier>/empreintes.txt (ppm-sequence.py, AVANT la suppression des
#   PPM ; le canon y lit image 1 = be01318…, images 100-300 = ae1ff947…), frames du canon en PNG sans perte ;
#   en mission, vignettes 640 px et planche.png, APRÈS banc-mac-profil.py (qui date les PPM).
# V1.5 (22/10) : BANC_VARIABLES=<fichier> sème ce Variables.txt FIGÉ (SC2_VARIABLES + SC2_VARIABLES_FIGE=1) et recopie
#   à la fin celui du jeu dans <dossier>/Variables-relu.txt ; absente : rien ne change (défaut inchangé). Le canon
#   épinglé (scripts/banc-options.sh --canon) passe par là avec scripts/prereglages/canon.txt et SC2_GPU_PROFIL=historique.
# V2.5a (23/10) : en « defaut », garde de l'adaptateur historique (scripts/garde-adaptateur.sh, SC2_GPU_PROFIL absente
#   ou « historique », SC2_DDRAW absente) : DirectDraw refusé dans run.log et, sans BANC_VARIABLES, Variables.txt du jeu
#   (gardé : <dossier>/Variables-jeu.txt) en textures Low sous la 8600 GT. Ligne « adaptateur : … » dans resume.txt ;
#   code de sortie 5 si la garde échoue (les empreintes ne voient pas un DirectDraw servi en historique), 0 sinon.
set -u
MODE=${1:?mode defaut|mission}; BIN=$(cd "$(dirname "${2:?binaire}")" && pwd)/$(basename "$2"); D=${3:?dossier}
R="$(cd "$(dirname "$0")/.." && pwd)"
. "$R/scripts/lib-banc.sh"
banc_garde_disque "$D" 1 || exit 3
banc_verrou "$0" "$@"
X=${SC2_EXTRACTION:-~/sc2-partout/extraction}
[ -f "$X/install/Versions/Base15405/SC2.exe" ] || { echo "données du jeu absentes : $X"; exit 1; }
rm -rf "$D"; mkdir -p "$D"; D=$(cd "$D" && pwd); cd "$D" || exit 1
# Polices Liberation du dépôt (le serveur Linux les prend dans /usr/share/fonts) : sans elles, C:\Windows\Fonts est vide et le
# lanceur plante au 1er accès à une police (NULL + 0xF dans L_0107F9F0, frame ≈ 40).
# SC2_FRAMES=1 explicite : la vague 0 bis fait de SC2_FRAMES=0 (aucune capture) le défaut des lanceurs.
COMMUN=(SC2_ROOT="$D/sc2-c" SC2_EXE="$X/install/Versions/Base15405/SC2.exe" SC2_INSTALL="$X/install"
        SC2_FRAMES=1 SC2_FRAMES_DIR="$D/frames" SC2_POLICES="${SC2_POLICES:-$R/android/assets/polices}")
if [ -n "${BANC_VARIABLES:-}" ]; then
    [ -f "$BANC_VARIABLES" ] || { echo "BANC_VARIABLES=$BANC_VARIABLES : fichier absent"; exit 1; }
    COMMUN+=(SC2_VARIABLES="$(cd "$(dirname "$BANC_VARIABLES")" && pwd)/$(basename "$BANC_VARIABLES")" SC2_VARIABLES_FIGE=1)
fi
t=$SECONDS
case $MODE in
defaut)
    env "${COMMUN[@]}" SC2_FRAMES_MAX=600 "$BIN" > run.log 2>&1
    echo "rc=$? en $((SECONDS - t)) s" > resume.txt
    C=$(banc_compacter "$D" canon)
    sed -n 1,6p empreintes.txt >> resume.txt 2>/dev/null; echo "$C" >> resume.txt
    # V2.5a : garde de l'adaptateur historique (scripts/garde-adaptateur.sh) ; Variables.txt du jeu gardé sans semis
    VJ=()
    if [ -z "${BANC_VARIABLES:-}" ]; then
        cp "$D/sc2-c/Users/sc2/Documents/StarCraft II/Variables.txt" "$D/Variables-jeu.txt" 2>/dev/null
        VJ=("$D/Variables-jeu.txt")
    fi
    "$R/scripts/garde-adaptateur.sh" "$D/run.log" ${VJ[@]+"${VJ[@]}"} >> resume.txt; GARDE=$? ;;
mission)
    F=${4:-2500}; DL=${5:-60}; DUR=${6:-60}
    env "${COMMUN[@]}" SC2_FRAMES_MAX="$F" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 SC2_SANS_VIDEOS=1 \
        SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" "$BIN" > run.log 2>&1 &
    P=$!
    sleep "$DL"
    date +%s > echant.debut; sample "$P" "$DUR" 1 -mayDie -file "$D/sample.txt" > /dev/null 2>&1; date +%s > echant.fin
    wait "$P"; echo "rc=$? en $((SECONDS - t)) s" > resume.txt
    python3 -I "$R/scripts/profil/banc-mac-profil.py" "$D" >> resume.txt 2>&1
    banc_compacter "$D" >> resume.txt; banc_planche "$D" reparties ;;
*) echo "mode inconnu : $MODE"; exit 1 ;;
esac
grep -a "SIGSEGV\|SIGBUS\|ARRÊT\] faute\|DEVICE_LOST" run.log | head -3 >> resume.txt
if [ -n "${BANC_VARIABLES:-}" ]; then
    cp "$D/sc2-c/Users/sc2/Documents/StarCraft II/Variables.txt" "$D/Variables-relu.txt" 2>/dev/null
    echo "Variables.txt : semé figé depuis $BANC_VARIABLES ; relu → Variables-relu.txt$(cmp -s "$BANC_VARIABLES" \
         "$D/Variables-relu.txt" 2>/dev/null && echo ' (identique au semé)')" >> resume.txt
fi
rm -rf "$D/sc2-c"
cat resume.txt
exit "${GARDE:-0}"
