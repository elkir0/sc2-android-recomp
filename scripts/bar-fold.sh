#!/bin/bash
# bar-fold.sh <nom> <apk|-> [VAR=valeur ...] (21/10) : entrée de campagne par le bar sur le Fold, SANS joueur.
#   SC2_RUN_SANS_BNET=1 SANS SC2_ARGS (carte d'histoire par défaut TStory01, APK ≥ v22), progression de TEST copiée de
#   ~/code/sc2-apk/progression/apres-mission1 dans /sdcard/SC2Partout/test-docs-bar (SC2_DOCUMENTS), scénario
#   scripts/entrees/bar-lancer-mission-fold.txt (cinématique, survol de Raynor, « Mission », « Launch »), une image
#   toutes les 100 frames (SC2_FRAMES=1 explicite : la vague 0 bis fait de SC2_FRAMES=0 le défaut des lanceurs),
#   SC2_ECRAN=1280x720 (BAR_FOLD_ECRAN=<mode>, ou « defaut » : aucune ligne),
#   deux captures de l'écran RÉEL (adb screencap : bar à 100 s, fin de run). Le sc2.env
#   du mainteneur est sauvegardé (local + /sdcard), complété le temps du run et remis à l'identique à la sortie.
# Sortie : ~/code/sc2-apk/mesures/<jour>/bar-<nom>/ (sc2.env*, logcat.txt, frames/*.png, planche.png, ecran-*.png).
# ADB_FAKE=1 (V0.1, 22/10, sur le modèle de test-matin.sh) : adb est une FONCTION qui rejoue scripts/bar-fold-fixtures/
#   (logcat.txt et sc2.env synthétiques, 3 PPM factices 64x36 générés, progression vide) ; aucun appareil touché,
#   aucune attente ; sortie dans ${TMPDIR:-/tmp}/bar-fold-fake/bar-<nom>/ (refaite à chaque run), sc2.env « du
#   téléphone » contrôlé à la fin. ex. : ADB_FAKE=1 scripts/bar-fold.sh fake -
# Appareil : SC2_ADB_SERIAL, sinon le Fold (modèle SM_F976B) trouvé dans « adb devices -l » (scripts/lib-fold.sh).
set -u
N=${1:?nom}; A=${2:?apk ou -}; shift 2
R="$(cd "$(dirname "$0")/.." && pwd)"; O=$HOME/code/sc2-apk/mesures/$(date +%Y%m%d)/bar-$N
PROG=$HOME/code/sc2-apk/progression/apres-mission1; DUREE=${BAR_FOLD_DUREE:-260}
F=/storage/emulated/0/Android/data/fr.sc2partout/files/frames
if [ "${ADB_FAKE:-0}" = 1 ]; then
  FX=$R/scripts/bar-fold-fixtures; O=${TMPDIR:-/tmp}/bar-fold-fake/bar-$N; PROG=$FX/progression
  rm -rf -- "${O:?}"; ETAT=$O/.telephone; mkdir -p "$ETAT"; cp "$FX/sc2.env" "$ETAT/sc2.env"
  adb() {                                      # mêmes appels que plus bas, réponses tirées des fixtures
    [ "$1" = -s ] && shift 2
    case "$*" in
      "devices -l") printf 'List of devices attached\nfake-fold:5555         device product:q8qxeea model:SM_F976B device:q8q transport_id:1\n' ;;
      get-state) echo device ;;
      "shell dumpsys thermalservice"*) echo "Thermal Status: 0" ;;
      "shell cat /sdcard/SC2Partout/sc2.env") cat "$ETAT/sc2.env" ;;
      "shell cp /sdcard/SC2Partout/sc2.env "*) cp "$ETAT/sc2.env" "$ETAT/${4##*/}" ;;
      "push "*" /sdcard/SC2Partout/sc2.env") cp "$2" "$ETAT/sc2.env" ;;
      "shell date "*) date +%Y%m%d%H%M%S ;;
      "logcat -v time"*) cat "$FX/logcat.txt" ;;
      "shell dumpsys SurfaceFlinger --display-id") printf 'Display 0 (HWC display 0): port=0\nDisplay 1 (HWC display 1): port=1\n' ;;
      "exec-out screencap"*) ffmpeg -nostdin -v error -i "$FX/frames/frame_00100.ppm" -f image2pipe -c:v png - ;;
      "shell find "*) for f in "$FX"/frames/frame_*.ppm; do printf '%s/%s\r\n' "$F" "${f##*/}"; done ;;
      "pull "*) cp "$FX/frames/${2##*/}" "$3" ;;
      *) ;;                                    # install, appops, rm, push (progression, scénario), am, logcat -c, input
    esac
  }
  sleep() { :; }
  echo "== MODE ADB_FAKE : fixtures $FX, sortie $O (aucun appel au vrai adb)"
fi
mkdir -p "$O"
. "$R/scripts/lib-fold.sh"; D=$(fold_serie) || exit 1; echo "Fold : $D"
a() { adb -s "$D" "$@"; }
[ -d "$PROG/Banks" ] || { echo "progression de test absente : $PROG"; exit 1; }
statut() { a shell 'dumpsys thermalservice | grep -m1 "Thermal Status"' | tr -dc '0-9'; }
until [ "$(statut)" = 0 ]; do echo "$(date +%T) statut thermique $(statut) : refroidissement…"; sleep 20; done
a shell cat /sdcard/SC2Partout/sc2.env > "$O/sc2.env.avant"
[ -s "$O/sc2.env.avant" ] || { echo "sc2.env illisible : abandon"; exit 1; }
a shell cp /sdcard/SC2Partout/sc2.env /sdcard/SC2Partout/sc2.env.sauvegarde-bar-$N
remettre() {
  a push "$O/sc2.env.avant" /sdcard/SC2Partout/sc2.env >/dev/null && echo "$(date +%T) sc2.env d'origine remis"
  if [ "${ADB_FAKE:-0}" = 1 ]; then
    cmp -s "$FX/sc2.env" "$ETAT/sc2.env" && echo "(fake) sc2.env du téléphone identique à la fixture" \
      || echo "(fake) ÉCHEC : sc2.env du téléphone différent de la fixture"
  fi
}
trap remettre EXIT
if [ "$A" != - ]; then a install -r "$A" 2>&1 | tail -1; a shell appops set fr.sc2partout MANAGE_EXTERNAL_STORAGE allow; fi
a shell rm -rf /sdcard/SC2Partout/test-docs-bar; a push "$PROG" /sdcard/SC2Partout/test-docs-bar >/dev/null
a push "$R/scripts/entrees/bar-lancer-mission-fold.txt" /sdcard/SC2Partout/entrees-bar.txt >/dev/null
{ grep -v -E '^(SC2_ARGS|SC2_BANC_ENTREES|SC2_DOCUMENTS|SC2_MANQUE_CONTINUE|SC2_FRAMES|SC2_FRAMES_PAS|SC2_RUN_SANS_BNET|SC2_ECRAN)=' \
      "$O/sc2.env.avant"; echo
  [ "${BAR_FOLD_ECRAN:-1280x720}" != defaut ] && printf 'SC2_ECRAN=%s\n' "${BAR_FOLD_ECRAN:-1280x720}"   # defaut : aucune ligne
  printf 'SC2_RUN_SANS_BNET=1\nSC2_MANQUE_CONTINUE=1\nSC2_DOCUMENTS=/sdcard/SC2Partout/test-docs-bar\n'
  printf 'SC2_BANC_ENTREES=@/sdcard/SC2Partout/entrees-bar.txt\nSC2_FRAMES=1\nSC2_FRAMES_PAS=100\n'; for v in "$@"; do echo "$v"; done; } \
  | grep -v '^$' > "$O/sc2.env"
a push "$O/sc2.env" /sdcard/SC2Partout/sc2.env >/dev/null
a shell am force-stop fr.sc2partout; a logcat -c
DEBUT=$(a shell date +%Y%m%d%H%M%S)          # le shell adb ne peut pas vider le dossier privé : on trie par date
a shell am start -n fr.sc2partout/android.app.NativeActivity >/dev/null; echo "$(date +%T) [$N] lancé ($DUREE s)"
adb -s "$D" logcat -v time -s SC2Partout:I libc:F DEBUG:F > "$O/logcat.txt" 2>&1 & P=$!
# deux écrans sur le Fold : screencap exige -d <id> (sinon un avertissement remplace l'image)
ECRANS=$(a shell dumpsys SurfaceFlinger --display-id | sed -n 's/^Display \([0-9]*\) .*/\1/p')
capturer() { local i=0; for e in $ECRANS; do a exec-out screencap -d "$e" -p > "$O/ecran-$1-$i.png"; i=$((i + 1)); done; }
sleep 100; capturer bar
sleep $((DUREE - 100)); capturer fin
a shell input keyevent --longpress KEYCODE_BACK; sleep 12; kill $P 2>/dev/null; wait $P 2>/dev/null
a shell am force-stop fr.sc2partout
mkdir -p "$O/frames"
for f in $(a shell "find $F -name 'frame_*.ppm' -newermt '${DEBUT:0:4}-${DEBUT:4:2}-${DEBUT:6:2} ${DEBUT:8:2}:${DEBUT:10:2}:${DEBUT:12:2}'" \
           | tr -d '\r' | sort); do
  a pull "$f" "$O/frames/" >/dev/null 2>&1 && sips -s format png "$O/frames/$(basename "$f")" \
    --out "$O/frames/$(basename "${f%.ppm}").png" >/dev/null 2>&1 && rm -f "$O/frames/$(basename "$f")"; done
(cd "$O/frames" && ls frame_*.png 2>/dev/null | awk 'NR % 2 == 1' > ../planche.lst && \
  ffmpeg -v error -y -f concat -safe 0 -i <(sed "s|^|file '$O/frames/|; s|$|'|" ../planche.lst) \
    -filter_complex "scale=256:-1,tile=8x8:padding=3" -frames:v 1 ../planche.png) && echo "planche : $O/planche.png"
{ grep -a '\[RUN\]' "$O/logcat.txt" | cut -c40-220 | head -3
  for c in TStory01 TRaynor02; do echo "$c : $(grep -ac "$c\.SC2Map" "$O/logcat.txt") ligne(s)"; done
  grep -a '\[BILAN\] draws\|frames présentées (Present)\|Fatal signal' "$O/logcat.txt" | cut -c40-220
  echo "$(ls "$O/frames" | grep -c png) images"; } | tee "$O/resume.txt"
