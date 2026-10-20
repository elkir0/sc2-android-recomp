#!/usr/bin/env bash
# Passe 7v (audio) — la bande son du jeu : musique du menu, ambiance et sons de mission, mixés par le FMOD
# logiciel du runtime et écrits en WAV (SC2_AUDIO_DUMP). UN run à la fois, timeout 300 s, free -h consigné.
#   menu    : x86, vue extraction/install-base, MENU_FRAMES frames (défaut 400) : musique du menu
#   mission : x86, vue extraction/install-complet (voix enUS), SC2_RUN_SANS_BNET=1 -run TRaynor01, clics « Close »
#   preuve  : android/outils/audio-preuve-7v.c sur les WAV : activité par 10 s, et corrélation au frame près
#             avec les musiques EXTRAITES des MPQ (tools/mpqget, décodées hors runtime)
# Sorties hors git : recomp-sc2/build-7v-valider/ (run-*/run.log, mix.wav, ref/*.ogg — données du jeu).
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; O="$R/build-7v-valider"; mkdir -p "$O/ref"
DEPOT="$HOME/code/reverse/sc2-partout"; EXE="$DEPOT/extraction/install/Versions/Base15405/SC2.exe"
BIN="${BIN:-$R/build-7v-x86/recomp-sc2}"; L="$O/valider${TAG:+-$TAG}.log"; ETAPES="${*:-menu mission preuve}"
OUTIL="$O/audio-preuve"; LIBERTY="$DEPOT/extraction/install/Mods/Liberty.SC2Mod/base.SC2Assets"
j() { echo "$*" | tee -a "$L"; }
j "=== passe 7v audio ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60), arbre : $(git -C "$W" status --short | wc -l) modif., binaire $(md5sum < "$BIN" | cut -c1-12)"
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
run() {   # <nom> <install> <frames> [VAR=val…]
  local nom="$1" ins="$2" fr="$3" d="$O/run${TAG:+-$TAG}-$1"; shift 3; rm -rf "$d"; mkdir -p "$d"; local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" \
     SC2_FRAMES_DIR="$d/frames" SC2_AUDIO_DUMP="$d/mix.wav" SC2_AUDIO_SORTIE=0 SC2_AUDIO_COMPTEUR=1 "$@" \
     timeout -s INT 300 "$BIN" > run.log 2>&1; echo $? > rc)
  j "--- $nom : $(basename "$ins") ${*:-} — rc=$(cat "$d/rc") ($((SECONDS - t0)) s), $(grep -c 'FRAME n°' "$d/run.log") frames"
  grep -aE '^\[BILAN\] \[AUDIO\] (mixage|System|consommateur|WAV)' "$d/run.log" | cut -c1-330 | sed 's/^/  | /' | tee -a "$L"
  grep -aE '^\[AUDIO\] (System maître|setSoftwareFormat|createSound «|playSound «|setDelay|rappel open|createSound .*non géré)' "$d/run.log" \
    | cut -c1-230 | head -40 | sed 's/^/  | /' | tee -a "$L"
  grep -a '^\[AUDIO-T\]' "$d/run.log" | cut -c1-260 | sed 's/^/  | /' | tee -a "$L"
}
for e in $ETAPES; do case $e in
menu)    run menu "$DEPOT/extraction/install-base" "${MENU_FRAMES:-400}" ;;
mission) run mission "$DEPOT/extraction/install-complet" "${MISSION_FRAMES:-600}" SC2_RUN_SANS_BNET=1 \
           SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_INPUT_SIM="$W/android/outils/clic-fermer-aide-7q.txt" ;;
preuve)
  cc -O2 -w -I "$R/runtime" "$W/android/outils/audio-preuve-7v.c" -lm -o "$OUTIL" || { j "  compilation de l'outil : ÉCHEC"; continue; }
  # références : noms internes MPQ (séparateur /) lus dans Liberty.SC2Mod/base.SC2Assets
  for m in ${PREUVE_SONS:-Assets/Music/MainTitleSC1-01.ogg Assets/Sounds/Uni/Ambience/Gluescreens/Ambience_MainMenu.ogg}; do
    [ -f "$O/ref/$(basename "$m")" ] || "$DEPOT/tools/mpqget" "$LIBERTY" "${m//\//\\}" "$O/ref/$(basename "$m")" > /dev/null
  done
  for d in "$O"/run${TAG:+-$TAG}-*/; do
    [ -f "$d/mix.wav" ] || continue
    j "\$ audio-preuve $(basename "$d")/mix.wav $(cd "$O/ref" && ls *.ogg | tr '\n' ' ')"
    "$OUTIL" "$d/mix.wav" "$O"/ref/*.ogg 2>&1 | sed 's/^/  /' | tee -a "$L"
  done ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
true
