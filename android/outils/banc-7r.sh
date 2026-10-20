#!/usr/bin/env bash
# Passe 7r — banc ÉMULATEUR complet (AVD sc2partout du Mac mini M4) ou appareil (Fold) : APK neuf, données,
# run « menu » (titre → menu de campagne) et run « mission » (-run TRaynor01 : titre → chargement), captures
# aux phases, FPS PAR PHASE (baseline du Fold), [BILAN] du runtime, rapport. À lancer CÔTÉ MAC (adb ; le
# devbox n'en a pas). Compatible bash 3.2 (macOS) et Linux.
#
#   android/outils/banc-7r.sh <apk> [sortie=./banc-7r-<date>]
#
# Variables (toutes facultatives) :
#   MODES="menu mission"     runs à faire, dans l'ordre (menu : SC2_RUN_SANS_BNET=1 ; mission : + SC2_ARGS=-run …)
#   DUREE_menu=300 DUREE_mission=480   durée max d'un run (s) ; le run s'arrête aussi si le processus meurt
#   CARTE=Maps/Campaign/TRaynor01.SC2Map
#   CAPT_APRES="12 45"       captures N s après chaque passage de phase ([PHASE] de l'APK, sc2_phases.h)
#   CAPT_PERIODE=60          + une capture toutes les N s (0 : aucune)
#   DONNEES=~/code/sc2-install-emu    installation à synchroniser vers /sdcard/SC2Partout/install (taille par
#                            fichier comparée, seuls les fichiers absents ou différents sont poussés) ; NON=0 : rien
#   SC2EXE=<chemin>          SC2.exe à poser dans Versions/Base15405/ s'il manque (défaut : celui de DONNEES)
#   AVD=sc2partout EMU_GPU=swiftshader_indirect   démarrage à froid de l'émulateur si aucun appareil adb
#   ANDROID_SDK_ROOT         défaut ~/Library/Android/sdk (Mac) ou ~/Android/Sdk
#   SC2_ENV_EXTRA="K=V\nK=V" lignes ajoutées au sc2.env des deux runs (ex. SC2_PRESENT=mailbox)
#   DISCORD_WEBHOOK=<url>    (opt-in) poste le rapport + jusqu'à 10 captures (curl multipart). Sans : rien n'est posté.
#
# Le sc2.env de l'appareil est sauvegardé puis restauré ; rien d'autre n'est modifié hors de l'APK et de
# /sdcard/SC2Partout/install (synchronisation). Les captures sont des images du jeu : ne JAMAIS les verser
# dans le dépôt public.
set -uo pipefail
APK="${1:?usage : $0 <apk> [sortie]}"; OUT="${2:-./banc-7r-$(date +%Y%m%d-%H%M%S)}"
PKG=fr.sc2partout; ACT="$PKG/android.app.NativeActivity"; SD=/sdcard/SC2Partout; ENV=$SD/sc2.env
MODES="${MODES:-menu mission}"; CARTE="${CARTE:-Maps/Campaign/TRaynor01.SC2Map}"
CAPT_APRES="${CAPT_APRES:-12 45}"; CAPT_PERIODE="${CAPT_PERIODE:-60}"
DONNEES="${DONNEES:-$HOME/code/sc2-install-emu}"; AVD="${AVD:-sc2partout}"; EMU_GPU="${EMU_GPU:-swiftshader_indirect}"
if [ -z "${ANDROID_SDK_ROOT:-}" ]; then
  for s in "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do [ -d "$s" ] && ANDROID_SDK_ROOT="$s" && break; done
fi
command -v adb > /dev/null || PATH="${ANDROID_SDK_ROOT:-/nonexistent}/platform-tools:$PATH"
command -v adb > /dev/null || { echo "adb introuvable (ANDROID_SDK_ROOT ?)"; exit 2; }
mkdir -p "$OUT"; L="$OUT/rapport.txt"; : > "$L"
j() { echo "$*" | tee -a "$L"; }
if [ "$(uname)" = Darwin ]; then STAT_FMT=(-f '%z %N'); else STAT_FMT=(-c '%s %n'); fi   # GNU stat -f = système de fichiers
taille() { stat "${STAT_FMT[@]}" "$1" | cut -d' ' -f1; }
sha() { if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi; }
maintenant() { date +%s; }
PASSAGE='\[PHASE\] [^ ]+ à [0-9.]+ s'                     # « [PHASE] menu à 41.2 s (…) » (pas « [PHASE] règle : »)

j "=== passe 7r — banc Android $(date '+%F %T %Z')"
j "APK : $APK — $(taille "$APK") o, sha256 $(sha "$APK")"

# --- 0. appareil : émulateur démarré à froid si rien n'est branché -------------------------------
if [ "$(adb get-state 2> /dev/null)" != device ]; then
  EMU="${ANDROID_SDK_ROOT:-}/emulator/emulator"          # le binaire, pas le dossier (piège du jalon 26)
  [ -x "$EMU" ] || { j "ÉCHEC : aucun appareil adb et $EMU absent"; exit 2; }
  j "aucun appareil : démarrage à froid de l'AVD $AVD (-no-window -gpu $EMU_GPU -no-snapshot-load)"
  nohup "$EMU" -avd "$AVD" -no-window -gpu "$EMU_GPU" -no-snapshot-load -no-boot-anim > "$OUT/emulateur.log" 2>&1 &
  adb wait-for-device
  t0=$(maintenant)
  until [ "$(adb shell getprop sys.boot_completed 2> /dev/null | tr -d '\r')" = 1 ]; do
    [ $(( $(maintenant) - t0 )) -gt 400 ] && { j "ÉCHEC : l'émulateur n'a pas fini de démarrer en 400 s"; exit 2; }
    sleep 3
  done
  j "émulateur prêt en $(( $(maintenant) - t0 )) s"
fi
j "appareil : $(adb shell getprop ro.product.model | tr -d '\r') — Android $(adb shell getprop ro.build.version.release | tr -d '\r'), $(adb shell getprop ro.product.cpu.abi | tr -d '\r') ; $(adb shell wm size | tr -d '\r')"

# --- 1. données : synchronisation par taille ----------------------------------------------------
if [ "$DONNEES" != 0 ] && [ -d "$DONNEES" ]; then
  ( cd "$DONNEES" && find . -type f -exec stat "${STAT_FMT[@]}" {} + ) \
    | sed 's#^\([0-9]*\) \./#\1 #' | LC_ALL=C sort > "$OUT/donnees-local.txt"
  adb shell "mkdir -p $SD/install; cd $SD/install && find . -type f -exec stat -c '%s %n' {} +" 2> /dev/null | tr -d '\r' \
    | sed 's#^\([0-9]*\) \./#\1 #' | LC_ALL=C sort > "$OUT/donnees-appareil.txt"
  LC_ALL=C comm -23 "$OUT/donnees-local.txt" "$OUT/donnees-appareil.txt" > "$OUT/donnees-a-pousser.txt"
  n=$(wc -l < "$OUT/donnees-a-pousser.txt" | tr -d ' ')
  j "données : $(wc -l < "$OUT/donnees-local.txt" | tr -d ' ') fichiers locaux ($DONNEES), $(wc -l < "$OUT/donnees-appareil.txt" | tr -d ' ') sur l'appareil, $n à pousser"
  if [ "$n" -gt 0 ]; then
    t0=$(maintenant)
    while read -r _ f; do
      d=$(dirname "$f"); adb shell "mkdir -p '$SD/install/$d'" && adb push "$DONNEES/$f" "$SD/install/$f" > /dev/null \
        || j "  ÉCHEC push $f"
    done < "$OUT/donnees-a-pousser.txt"
    j "  poussés en $(( $(maintenant) - t0 )) s"
  fi
else j "données : synchronisation sautée (DONNEES=$DONNEES)"; fi
EXE_APP="$SD/install/Versions/Base15405/SC2.exe"
if [ -z "$(adb shell "ls $EXE_APP 2>/dev/null" | tr -d '\r')" ]; then
  X="${SC2EXE:-$DONNEES/Versions/Base15405/SC2.exe}"
  [ -f "$X" ] || { j "ÉCHEC : $EXE_APP absent de l'appareil et SC2EXE=$X introuvable"; exit 1; }
  adb shell "mkdir -p $SD/install/Versions/Base15405" && adb push "$X" "$EXE_APP" > /dev/null && j "SC2.exe posé ($X)"
fi
j "SC2.exe sur l'appareil : $(adb shell "stat -c %s $EXE_APP" | tr -d '\r') o"

# --- 2. APK + accès « tous les fichiers » (à reposer après chaque démarrage à froid) -------------
adb install -r "$APK" > "$OUT/install.log" 2>&1 && j "APK installé" || { j "ÉCHEC install"; cat "$OUT/install.log"; exit 1; }
adb shell appops set "$PKG" MANAGE_EXTERNAL_STORAGE allow
j "appops MANAGE_EXTERNAL_STORAGE : $(adb shell appops get "$PKG" MANAGE_EXTERNAL_STORAGE | tr -d '\r')"
adb shell "cp $ENV $ENV.avant-7r 2>/dev/null; true"
restaurer() { adb shell "if [ -f $ENV.avant-7r ]; then mv $ENV.avant-7r $ENV; else rm -f $ENV; fi" > /dev/null 2>&1; }
trap 'restaurer' EXIT

capturer() {   # <dossier> <nom>
  adb exec-out screencap -p > "$1/$2.png" 2> /dev/null
  local t; t=$(taille "$1/$2.png"); echo "$2.png $t" >> "$1/captures.txt"
  j "  capture $2.png : $t o$( [ "$t" -lt 40000 ] && echo ' (< 40 Ko : écran uni ou presque)')"
}

# --- 3. runs ---------------------------------------------------------------------------------------
for MODE in $MODES; do
  M="$OUT/$MODE"; mkdir -p "$M"; : > "$M/captures.txt"
  eval "DUREE=\${DUREE_$MODE:-}"; [ -n "$DUREE" ] || { [ "$MODE" = mission ] && DUREE=480 || DUREE=300; }
  { adb shell "cat $ENV.avant-7r 2>/dev/null" | tr -d '\r' | grep -v -E '^(SC2_RUN_SANS_BNET|SC2_ARGS|SC2_FPS_PERIODE)='
    echo "SC2_FPS_PERIODE=5"; echo "SC2_RUN_SANS_BNET=1"
    [ "$MODE" = mission ] && echo "SC2_ARGS=-run $CARTE"
    [ -n "${SC2_ENV_EXTRA:-}" ] && printf '%b\n' "$SC2_ENV_EXTRA"; } > "$M/sc2.env"
  adb push "$M/sc2.env" "$ENV" > /dev/null
  j ""; j "=== run « $MODE » (durée max $DUREE s) — sc2.env : $(grep -v '^#' "$M/sc2.env" | tr '\n' ' ')"
  adb shell am force-stop "$PKG"; adb logcat -c
  adb logcat -v time -s SC2Partout:V DEBUG:V AndroidRuntime:V libc:V > "$M/logcat.txt" 2>&1 &
  LC=$!
  adb shell am start -n "$ACT" > /dev/null
  t0=$(maintenant); vues=0; prochaines=""; periodique=$(( t0 + CAPT_PERIODE )); mort=""
  while [ $(( $(maintenant) - t0 )) -lt "$DUREE" ]; do
    sleep 2; now=$(maintenant)
    # passages de phase → captures différées
    n=$(grep -c -E "$PASSAGE" "$M/logcat.txt" 2> /dev/null); n=${n:-0}
    while [ "$vues" -lt "$n" ]; do
      vues=$(( vues + 1 ))
      ph=$(grep -E "$PASSAGE" "$M/logcat.txt" | sed -n "${vues}p" | sed 's/.*\[PHASE\] \([^ ]*\) à.*/\1/')
      j "  [$(( now - t0 )) s] $(grep -E "$PASSAGE" "$M/logcat.txt" | sed -n "${vues}p" | sed 's/.*\(\[PHASE\]\)/\1/')"
      prochaines=""                       # captures en attente d'une phase dépassée : annulées (étiquette juste)
      for d in $CAPT_APRES; do prochaines="$prochaines $(( now + d )):$ph-$d""s"; done
    done
    reste=""
    for c in $prochaines; do
      if [ "${c%%:*}" -le "$now" ]; then capturer "$M" "$(( now - t0 ))s-${c#*:}"; else reste="$reste $c"; fi
    done
    prochaines="$reste"
    if [ "$CAPT_PERIODE" -gt 0 ] && [ "$now" -ge "$periodique" ]; then
      capturer "$M" "$(( now - t0 ))s-periodique"; periodique=$(( now + CAPT_PERIODE ))
    fi
    if [ -z "$(adb shell pidof "$PKG" 2> /dev/null | tr -d '\r')" ]; then mort="processus terminé à $(( now - t0 )) s"; break; fi
  done
  if [ -z "$mort" ]; then
    capturer "$M" "$(( $(maintenant) - t0 ))s-fin"
    # sortie PROPRE (appui long sur retour, sc2_android_main.c) → [BILAN] complet du runtime + [BILAN] [FPS]
    adb shell input keyevent --longpress KEYCODE_BACK
    for _ in $(seq 1 30); do sleep 1; [ -z "$(adb shell pidof "$PKG" 2> /dev/null | tr -d '\r')" ] && break; done
    [ -n "$(adb shell pidof "$PKG" 2> /dev/null | tr -d '\r')" ] && { j "  sortie propre non aboutie en 30 s : force-stop"; adb shell am force-stop "$PKG"; }
    mort="arrêt demandé à $DUREE s (appui long retour)"
  fi
  sleep 2; kill "$LC" 2> /dev/null; wait "$LC" 2> /dev/null
  j "  fin : $mort ; logcat $(wc -l < "$M/logcat.txt" | tr -d ' ') lignes"
  # --- rapport du run ---
  j "  --- appareil graphique / présentation ---"
  grep -E '\[VK\] backend|\[VKP\] mode de présentation|fenêtre [0-9]+x[0-9]+ → tampon|\[AUDIO\] Android : flux' "$M/logcat.txt" \
    | head -4 | sed 's/^.*SC2Partout([ 0-9]*): /    /' | cut -c1-220 | tee -a "$L"
  j "  --- phases ---"
  grep -E '\[PHASE\]' "$M/logcat.txt" | sed 's/^.*SC2Partout([ 0-9]*): /    /' | tee -a "$L"
  j "  --- FPS par phase (lignes [FPS] toutes les 5 s : moyenne / min / max des fenêtres) ---"
  grep -E '\[FPS\] [0-9]' "$M/logcat.txt" | sed 's/^.*\[FPS\] \([0-9.]*\) images.*phase \(.*\)$/\2 \1/' \
    | awk '{ n[$1]++; s[$1]+=$2; if (!($1 in mi) || $2<mi[$1]) mi[$1]=$2; if ($2>ma[$1]) ma[$1]=$2; if (!($1 in o)) { o[$1]=++k; nm[k]=$1 } }
           END { for (i=1;i<=k;i++) { p=nm[i]; printf "    %-11s %3d fenêtres : moyenne %5.1f, min %5.1f, max %5.1f images/s\n", p, n[p], s[p]/n[p], mi[p], ma[p] } }' \
    | tee -a "$L"
  grep -E '\[BILAN\] \[FPS\]' "$M/logcat.txt" | sed 's/^.*SC2Partout([ 0-9]*): /    /' | tee -a "$L"
  j "  --- [BILAN] du runtime (extrait) ---"
  grep -E '\[BILAN\] (frames présentées|draws :|textures D3DX|\[AUDIO\] Android)|[0-9]+ / +[0-9]+ fichiers lus' "$M/logcat.txt" \
    | sed 's/^.*SC2Partout([ 0-9]*): /    /' | cut -c1-200 | head -12 | tee -a "$L"
  grep -c '1re lecture' "$M/logcat.txt" | sed 's/^/    1res lectures MPQ : /' | tee -a "$L"
  grep -E 'Maps\\Campaign' "$M/logcat.txt" | grep -c '1re lecture' | sed 's/^/    dont fichiers de carte : /' | tee -a "$L"
  grep -E '\[ARRÊT\]|SIGSEGV|MANQUE|FATAL|Fatal signal' "$M/logcat.txt" | head -4 | sed 's/^.*: /  ! /' | cut -c1-200 | tee -a "$L"
done

# --- 4. publication (opt-in) -------------------------------------------------------------------------
if [ -n "${DISCORD_WEBHOOK:-}" ]; then
  args=(); i=0
  for f in $(for MODE in $MODES; do sed "s#^#$MODE/#" "$OUT/$MODE/captures.txt"; done | awk '$2 >= 40000 {print $1}' | head -10); do
    args+=(-F "files[$i]=@$OUT/$f;filename=$(echo "$f" | tr '/' '-')"); i=$((i + 1))
  done
  msg=$(printf 'SC2 Partout — passe 7r, banc Android (%s)\n```\n%s\n```' "$(adb shell getprop ro.product.model | tr -d '\r')" \
        "$(grep -E '\[PHASE\]|fenêtres :|\[BILAN\] \[FPS\]|^=== run' "$L" | cut -c1-150 | head -24)")
  pj=$(printf '%s' "$msg" | python3 -c 'import json,sys; print(json.dumps({"content": sys.stdin.read()[:1990]}))')
  curl -sS -o "$OUT/discord.json" -w '%{http_code}' --form-string "payload_json=$pj" ${args[@]+"${args[@]}"} "$DISCORD_WEBHOOK" > "$OUT/discord.code"
  j "Discord : HTTP $(cat "$OUT/discord.code"), $i image(s)"
fi
j ""; j "rapport : $L ; captures et logcats : $OUT/<mode>/"
