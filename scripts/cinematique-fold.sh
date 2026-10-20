#!/bin/bash
# cinematique-fold.sh <nom> <apk|-> [VAR=valeur ...] (passe 10t) : rejoue SANS joueur la cinématique du bar d'après la
# mission 1 sur le Fold — TRaynor01, victoire instantanée par code de triche et « Continue » (scripts/entrees/
# traynor01-victoire-fold.txt, posé dans /sdcard/SC2Partout/), une image toutes les 50 frames — et rapatrie les images
# de la cinématique. Options du runtime à tester en arguments (ex. SC2_VK_SYNCHRONE=1). Le sc2.env du Fold est
# REMPLACÉ pendant le run et sauvegardé avant dans <sortie>/sc2.env.avant : le remettre ensuite (adb push).
# Sortie : ~/code/sc2-apk/mesures/<jour>/cine-<nom>/ (sc2.env, logcat.txt, frames/*.png, planche.png).
D=${SC2_ADB_SERIAL:?série adb (adb devices -l)}; N=$1; A=$2; shift 2
R="$(cd "$(dirname "$0")/.." && pwd)"; O=$HOME/code/sc2-apk/mesures/$(date +%Y%m%d)/cine-$N; mkdir -p "$O"
a() { adb -s $D "$@"; }
statut() { a shell 'dumpsys thermalservice | grep -m1 "Thermal Status"' | tr -dc '0-9'; }
until [ "$(statut)" = 0 ]; do sleep 20; done
[ "$A" != - ] && { a install -r "$A" 2>&1 | tail -1; a shell appops set fr.sc2partout MANAGE_EXTERNAL_STORAGE allow; }
a push "$R/scripts/entrees/traynor01-victoire-fold.txt" /sdcard/SC2Partout/entrees-fold.txt >/dev/null
a shell cat /sdcard/SC2Partout/sc2.env > "$O/sc2.env.avant"
{ printf 'SC2_RUN_SANS_BNET=1\nSC2_MANQUE_CONTINUE=1\nSC2_ARGS=-run Maps/Campaign/TRaynor01.SC2Map\n'
  printf 'SC2_BANC_ENTREES=@/sdcard/SC2Partout/entrees-fold.txt\nSC2_FRAMES_PAS=50\n'; for v in "$@"; do echo "$v"; done; } > "$O/sc2.env"
a push "$O/sc2.env" /sdcard/SC2Partout/sc2.env >/dev/null
a shell am force-stop fr.sc2partout; a logcat -c
a shell am start -n fr.sc2partout/android.app.NativeActivity >/dev/null; echo "$(date +%T) [$N] lancé"
adb -s $D logcat -v time -s SC2Partout:I libc:F DEBUG:F > "$O/logcat.txt" 2>&1 & P=$!
sleep 230; a shell input keyevent --longpress KEYCODE_BACK; sleep 12; kill $P; a shell am force-stop fr.sc2partout
a push "$O/sc2.env.avant" /sdcard/SC2Partout/sc2.env >/dev/null; echo "sc2.env d'origine remis"
X=$(grep -a 'Loading' "$O/logcat.txt" | grep -o 'Present n°[0-9]*' | tail -1 | tr -dc '0-9'); echo "chargement du bar au Present $X"
F=/storage/emulated/0/Android/data/fr.sc2partout/files/frames; mkdir -p "$O/frames"
for n in $(seq $(( (X/50)*50 + 50 )) 50 $(( X + 2500 ))); do f=$(printf 'frame_%05d' $n)
  a pull $F/$f.ppm "$O/frames/" >/dev/null 2>&1 && sips -s format png "$O/frames/$f.ppm" --out "$O/frames/$f.png" >/dev/null 2>&1; done
(cd "$O/frames" && ffmpeg -v error -y -pattern_type glob -i 'frame_*.png' -filter_complex "scale=256:-1,tile=6x6:padding=3" \
    -frames:v 1 ../planche.png) && echo "planche : $O/planche.png"
grep -a 'draws : \|frames présentées (Present)' "$O/logcat.txt" | cut -c40-200
echo "$(date +%T) [$N] fini, $(ls "$O/frames" | grep -c png) images"
