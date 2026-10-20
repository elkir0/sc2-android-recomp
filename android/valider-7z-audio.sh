#!/usr/bin/env bash
# Passe 7z (audio suite) — corrélation de la bande son de MISSION avec les sons du jeu extraits des MPQ.
# Étapes : mission (x86, install-complet, -run TRaynor01, clics « Close », SC2_MANQUE_CONTINUE=1, SC2_AUDIO_DUMP,
#          timeout TMO s, défaut 600, MISSION_FRAMES défaut 1200) ; menu (non-régression, 200 f) ;
#          preuve (android/outils/audio-preuve-7v.c, références de mission extraites par tools/mpqget) ;
#          ordres (7z suite : mission + scénario ÉVÉNEMENTIEL 7u — aide fermée, Raynor sélectionné, ordres par la
#          minicarte, attaque — 2 600 f, TMO conseillé 3000 ; la sim 7w2 à frames fixes s'est jouée sur l'écran de
#          chargement d'un run plus rapide en frames — pour les
#          bruitages : lectures .wav_, sites createSound, SC2_TRACE_VA=0x009EA2D0 = méthode +0x60 « ouvrir » de la
#          ressource son) ; piste (android/outils/audio-piste-7z.c : reconstruction du mix de chaque run depuis
#          SC2_AUDIO_PISTE, références = TOUS les sons joués, extraits de l'archive notée par le journal [MPQ]).
# 7z suite (après merge main 7w6/7w7) : TMO défaut 1500 (600 s ne suffit pas sous charge : R2 r2z tué au délai).
# UN run à la fois, JAMAIS d'ulimit sur les runs, free -h consigné. Sorties hors git : recomp-sc2/build-7z-valider/.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; O="$R/build-7z-valider"; mkdir -p "$O/ref"
DEPOT="$HOME/code/reverse/sc2-partout"; EXE="$DEPOT/extraction/install/Versions/Base15405/SC2.exe"
BIN="${BIN:-$R/build-7v-x86/recomp-sc2}"; L="$O/valider${TAG:+-$TAG}.log"; ETAPES="${*:-mission preuve piste}"; TMO="${TMO:-1500}"
OUTIL="$O/audio-preuve"; IC="$DEPOT/extraction/install-complet"
# référence = « archive|chemin interne » (archives de l'installation complète)
REFS=( "Mods/Liberty.SC2Mod/base.SC2Assets|Assets/Sounds/Uni/Ambience/Tilesets/Ambience_MarSara.ogg"
       "Mods/Liberty.SC2Mod/base.SC2Assets|Assets/Music/MissionMusic/LibertyMusicStingerIntro_A.ogg"
       "Mods/Liberty.SC2Mod/base.SC2Assets|Assets/Music/MissionMusic/Music_ClustersPitchedDown.ogg"
       "Mods/Liberty.SC2Mod/base.SC2Assets|Assets/Music/MissionMusic/TempMusic.ogg"
       "Campaigns/Liberty.SC2Campaign/enUS.SC2Assets|LocalizedData/Sounds/TerranVO/CampaignVO/TRaynor01Raynor00028.ogg"
       "Mods/Liberty.SC2Mod/base.SC2Assets|Assets/Music/MissionMusic/Mission_MusicCue001.ogg"
       "Campaigns/Liberty.SC2Campaign/enUS.SC2Assets|LocalizedData/Sounds/TerranVO/CampaignVO/TRaynor01Adjutant00019.ogg"
       "Campaigns/Liberty.SC2Campaign/enUS.SC2Assets|LocalizedData/Sounds/TerranVO/CampaignVO/TRaynor01Raynor00030.ogg" )
# 7z : Mission_MusicCue001 et les voix 00019/00030 = sons RÉELLEMENT lancés après 80 s (journal [AUDIO] playSound du run)
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
j "=== passe 7z audio ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60), binaire $(md5sum < "$BIN" | cut -c1-12)"
run() {   # <nom> <install> <frames> [VAR=val…]
  local nom="$1" ins="$2" fr="$3" d="$O/run${TAG:+-$TAG}-$1"; shift 3; rm -rf "$d"; mkdir -p "$d"; local t0=$SECONDS
  j "--- $nom — free -h AVANT : $(mem)"
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" \
     SC2_FRAMES_DIR="$d/frames" SC2_AUDIO_DUMP="$d/mix.wav" SC2_AUDIO_PISTE="$d/piste.txt" SC2_AUDIO_SORTIE=0 SC2_AUDIO_COMPTEUR=1 "$@" \
     timeout -s INT "$TMO" "$BIN" > run.log 2>&1; echo $? > rc)
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s), $(grep -ac 'FRAME n°' "$d/run.log") frames — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] \[AUDIO\] (mixage|System)' "$d/run.log" | cut -c1-260 | sed 's/^/  | /' | tee -a "$L"
  grep -aE '^\[AUDIO\] (playSound «|setDelay|setLoopPoints|rappel open)' "$d/run.log" | cut -c1-200 | head -30 | sed 's/^/  | /' | tee -a "$L"
  grep -a '^\[AUDIO-T\]' "$d/run.log" | cut -c1-260 | sed 's/^/  | /' | tee -a "$L"
}
for e in $ETAPES; do case $e in
mission) run mission "$IC" "${MISSION_FRAMES:-1200}" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 \
           SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_INPUT_SIM="$W/android/outils/clic-fermer-aide-7q.txt" ;;
ordres)  run ordres "$IC" "${ORDRES_FRAMES:-2600}" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 SC2_FRAMES_PAS="${PAS:-50}" \
           SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_INPUT_SIM="${SIM:-$W/android/outils/scenario-7u.sim}" \
           SC2_TRACE_VA="${TRACE_VA:-9EA2D0}" SC2_TRACE_N="${TRACE_N:-60}"
         d="$O/run${TAG:+-$TAG}-ordres"
         j "  bruitages : lectures MPQ .wav_ $(grep -aci '\.wav_' "$d/run.log") ; sites createSound :" \
           "$(grep -a 'FMOD::System::createSound' "$d/run.log" | grep -o '← 0x[0-9A-F]*' | sort | uniq -c | tr '\n' ' ')"
         j "  méthode +0x60 de la ressource son (0x009EA2D0) : $(grep -ac '^\[TRACE\] *0x009EA2D0' "$d/run.log") entrée(s) tracée(s)"
         grep -a '^\[INPUT\]\|^\[BILAN\] \[INPUT\]' "$d/run.log" | tail -3 | cut -c1-200 | sed 's/^/  | /' | tee -a "$L" ;;
piste)
  cc -O2 -w -I "$R/runtime" "$W/android/outils/audio-piste-7z.c" -lm -o "$O/audio-piste" || { j "  compilation audio-piste : ÉCHEC"; continue; }
  for d in "$O"/run${TAG:+-$TAG}-*/; do [ -s "$d/piste.txt" ] && [ -f "$d/mix.wav" ] || continue
    # références : chaque son de la piste, extrait de l'archive que le journal [MPQ] du run a lue
    grep '^S ' "$d/piste.txt" | cut -d' ' -f10- | sort -u | while IFS= read -r m; do f="$O/ref/${m##*\\}"
      [ -s "$f" ] && continue
      a=$(grep -a -m1 -F "→ $m (" "$d/run.log" | sed -E 's/^\[MPQ\] (.*) → .*/\1/' | tr '\\' /)
      [ -n "$a" ] && "$DEPOT/tools/mpqget" "$IC/$a" "$m" "$f" > /dev/null 2>&1
      [ -s "$f" ] || for x in $(cd "$IC" && find . -name '*.SC2Assets' | sort); do   # repli : lu hors du journal [MPQ]
        "$DEPOT/tools/mpqget" "$IC/${x#./}" "$m" "$f" > /dev/null 2>&1 && { a="${x#./} (repli)"; break; }; done
      [ -s "$f" ] || j "  référence ABSENTE : $m (archive « ${a:-?} »)"
    done
    j "\$ audio-piste $(basename "$d")/mix.wav piste.txt ref/"
    "$O/audio-piste" "$d/mix.wav" "$d/piste.txt" "$O/ref" 2>&1 | sed 's/^/  /' | tee -a "$L"
  done ;;
menu)    run menu "$DEPOT/extraction/install-base" "${MENU_FRAMES:-200}" ;;
preuve)
  cc -O2 -w -I "$R/runtime" "$W/android/outils/audio-preuve-7v.c" -lm -o "$OUTIL" || { j "  compilation : ÉCHEC"; continue; }
  for r in "${REFS[@]}"; do a="${r%%|*}"; m="${r#*|}"; f="$O/ref/$(basename "$m")"
    [ -s "$f" ] || "$DEPOT/tools/mpqget" "$IC/$a" "${m//\//\\}" "$f" > /dev/null 2>&1
    j "  référence $(basename "$m") : $( [ -s "$f" ] && stat -c %s "$f" || echo ABSENTE) o"
  done
  d="$O/run${TAG:+-$TAG}-mission"
  [ -f "$d/mix.wav" ] && { j "\$ audio-preuve mission/mix.wav $(cd "$O/ref" && ls *.ogg | tr '\n' ' ')"
    "$OUTIL" "$d/mix.wav" "$O"/ref/*.ogg 2>&1 | sed 's/^/  /' | tee -a "$L"; } ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
