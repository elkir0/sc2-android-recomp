#!/usr/bin/env bash
# Passe 7m — matrice « ce que le jeu valide » : run défaut (sans -run, donc l'écran-titre puis l'interface)
# sur des vues d'installation qui ajoutent les archives manquantes UNE PAR UNE (android/outils/vue-install.sh
# liste). Pour chaque vue : le dialogue « UI/GameLaunchInvalidFiles » apparaît-il ? quels textes, quelles
# archives montées, quel écran (ppm-blocs) ? Un run à la fois (3e slot, sans build massif).
#   android/valider-7m.sh [scénario…]       (défaut : tous ; FRAMES=300 ; BIN=binaire relié de build-7l)
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; O="$R/build-7m-valider"; mkdir -p "$O"
EXE="$HOME/code/reverse/sc2-partout/extraction/install/Versions/Base15405/SC2.exe"
BIN="${BIN:-$R/build-7l/recomp-sc2}"; FRAMES="${FRAMES:-300}"; L="$O/valider${TAG:+-$TAG}.log"
LB="Mods/Liberty.SC2Mod/Base.SC2Data Mods/LibertyMulti.SC2Mod/Base.SC2Data"
CD="Mods/Core.SC2Mod/enUS.SC2Data"; CA="Mods/Core.SC2Mod/enUS.SC2Assets"
LD="Mods/Liberty.SC2Mod/enUS.SC2Data"; LA="Mods/Liberty.SC2Mod/enUS.SC2Assets"; MD="Mods/LibertyMulti.SC2Mod/enUS.SC2Data"
KD="Campaigns/Liberty.SC2Campaign/enUS.SC2Data"; KS="Campaigns/LibertyStory.SC2Campaign/enUS.SC2Data"
KA="Campaigns/Liberty.SC2Campaign/enUS.SC2Assets"
declare -A V=(
  [ref]=""                                   # installation de référence seule (= défaut des passes 6a-7l)
  [base]="$LB"                               # + dépendances Liberty (vue « base » de la 7h)
  [libdata]="Mods/Liberty.SC2Mod/Base.SC2Data"          # + la seule Base.SC2Data de Liberty
  [multidata]="Mods/LibertyMulti.SC2Mod/Base.SC2Data"   # + la seule Base.SC2Data de LibertyMulti
  [coredata]="$LB $CD"                       # + chaînes Core (GameStrings, Errors…)
  [core]="$LB $CD $CA"                       # + assets Core enUS
  [mods]="$LB $CD $CA $LD $LA $MD"           # + langue des mods Liberty / LibertyMulti
  [sanscine]="$LB $CD $CA $LD $LA $MD $KD $KS"   # toute la langue SAUF Campaigns/Liberty enUS.SC2Assets (cinématique)
  [complet]="$LB $CD $CA $LD $LA $MD $KD $KS $KA"
)
ORDRE="${*:-ref base coredata core mods sanscine complet}"
j() { echo "$*" | tee -a "$L"; }
j "=== passe 7m ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60) ; binaire $(md5sum < "$BIN" | cut -c1-12) ; $FRAMES frames"
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
for s in $ORDRE; do
  d="$O/run${TAG:+-$TAG}-$s"; rm -rf "$d"; mkdir -p "$d"
  AJOUTS="${V[$s]}" "$W/android/outils/vue-install.sh" "$d/vue" liste > /dev/null
  t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$d/vue" SC2_EXE="$EXE" SC2_FRAMES_MAX="$FRAMES" SC2_FRAMES_DIR="$d/frames" \
     timeout -s KILL 900 "$BIN" > run.log 2>&1; echo $? > rc)
  j "--- $s : +[${V[$s]:-rien}]"
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; dialogue InvalidFiles : $(grep -q 'GameLaunchInvalidFiles' "$d/run.log" && echo OUI || echo non) ; archives montées : $(grep -c 'MONTÉE par le moteur' "$d/run.log") ; 1res lectures MPQ : $(grep -c '1re lecture' "$d/run.log")"
  j "  textes : $(grep '^\[TEXTE\]' "$d/run.log" | sed 's/.*« \(.*\) ».*/\1/' | awk '!v[$0]++' | head -8 | paste -sd'|')"
  j "  dernière 1re lecture : $(grep '1re lecture' "$d/run.log" | tail -1 | cut -c1-150)"
  python3 -I "$W/android/outils/ppm-blocs.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
