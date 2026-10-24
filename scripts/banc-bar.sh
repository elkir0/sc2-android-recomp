#!/bin/bash
# banc-bar.sh <binaire> <dossier> [progression] — entrée de campagne par le bar au banc Mac, sans joueur (21/10).
#   SC2_RUN_SANS_BNET=1 SANS SC2_ARGS : le lanceur simulé prend la carte d'histoire par défaut (TStory01) ;
#   SC2_DOCUMENTS = COPIE de la progression (défaut ~/code/sc2-apk/progression/apres-mission1, victoire en mission 1) ;
#   scénario scripts/entrees/bar-lancer-mission.txt (cinématique passée, « Mission », « Launch », QUIT à 150 s) ;
#   une image toutes les 50 frames. Verdict : cartes lues (TStory01 puis TRaynor02), dessins « lumière », plantages.
# V1.1 (22/10) : BANC_BAR_SCENARIO=<fichier> remplace le scénario (défaut inchangé : bar-lancer-mission.txt) — par exemple
#   scripts/entrees/bar-lancer-mission-evt.txt, piloté par événements (QUAND:CARTE, QUAND:PRET, REF:1280x720 : marche
#   aussi en SC2_ECRAN=1280x1024, que l'environnement transmet) ; resume.txt reprend le bilan du banc d'entrées (rejets,
#   segments déclenchés) : un scénario lu par un binaire trop ancien y montre des « rejeté(s) ».
# V0.1 (22/10, scripts/lib-banc.sh) : garde-disque (SC2_DISQUE_MIN_GO + 2 Gio : une image toutes les 50 frames, pic
#   mesuré de 875 Mo de PPM à mi-vitesse, banc chargé), verrou ~/.sc2-verrous/banc, SC2_FRAMES=1 explicite ; après le
#   run, empreintes.txt, vignettes PNG 640 px et planche.png réparties sur tout le run (23 Mo), plus aucun PPM.
set -u
BIN=$(cd "$(dirname "${1:?binaire}")" && pwd)/$(basename "$1"); D=${2:?dossier}
PROG=${3:-$HOME/code/sc2-apk/progression/apres-mission1}
R="$(cd "$(dirname "$0")/.." && pwd)"
SCEN=${BANC_BAR_SCENARIO:-$R/scripts/entrees/bar-lancer-mission.txt}
[ -f "$SCEN" ] || { echo "scénario absent : $SCEN"; exit 1; }
SCEN=$(cd "$(dirname "$SCEN")" && pwd)/$(basename "$SCEN")
. "$R/scripts/lib-banc.sh"
banc_garde_disque "$D" 2 || exit 3
banc_verrou "$0" "$@"
X=${SC2_EXTRACTION:-~/sc2-partout/extraction}
[ -f "$X/install/Versions/Base15405/SC2.exe" ] || { echo "données du jeu absentes : $X"; exit 1; }
[ -d "$PROG/Banks" ] || { echo "progression absente : $PROG"; exit 1; }
rm -rf "${D:?}"; mkdir -p "$D"; D=$(cd "$D" && pwd)
cp -R "$PROG" "$D/docs"                                     # copie jetable : le run écrit dans la progression
cd "$D" || exit 1
t=$SECONDS
env -u SC2_ARGS SC2_ROOT="$D/sc2-c" SC2_EXE="$X/install/Versions/Base15405/SC2.exe" SC2_INSTALL="$X/install" \
    SC2_FRAMES=1 SC2_FRAMES_DIR="$D/frames" SC2_FRAMES_PAS=50 SC2_POLICES="${SC2_POLICES:-$R/android/assets/polices}" \
    SC2_DOCUMENTS="$D/docs" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 \
    SC2_BANC_ENTREES="@$SCEN" "$BIN" > run.log 2>&1
echo "rc=$? en $((SECONDS - t)) s" > resume.txt
grep -a '\[RUN\]' run.log | head -3 >> resume.txt
for c in TStory01 TRaynor02; do
    echo "$c : $(grep -ac "$c\.SC2Map" run.log) ligne(s) du journal" >> resume.txt; done
grep -a '\[BILAN\] draws' run.log | tail -1 >> resume.txt
grep -a "SIGSEGV\|SIGBUS\|ARRÊT\] faute\|DEVICE_LOST" run.log | head -3 >> resume.txt
grep -a '^\[BILAN\] \[INPUT\] banc' run.log >> resume.txt
banc_compacter "$D" >> resume.txt; banc_planche "$D" reparties
rm -rf "$D/sc2-c"
cat resume.txt
