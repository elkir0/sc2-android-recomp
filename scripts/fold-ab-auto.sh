#!/bin/bash
# fold-ab-auto.sh <nom:apk> [<nom:apk> …] (21/10) : série A/B sur le Fold SANS joueur — partie scriptée identique
# (scripts/entrees/jeu-actif-traynor01.txt : aide refermée, sélection par cadre, attaque le long de la route, caméra
# qui suit ; 225 événements, plafond du module : 256). Le sc2.env du mainteneur est sauvegardé (local + /sdcard), complété
# le temps de la série (mission TRaynor01, scénario, progression de TEST dans /sdcard/SC2Partout/test-docs) et remis à
# l'identique à la sortie, même en cas d'erreur. Chaque run = scripts/fold-ab-run.sh ; dépouillement par fold-ab.py.
# Le débogage sans fil du Fold doit être actif (il se coupe la nuit). ex. : fold-ab-auto.sh v20a:~/code/sc2-apk/sc2-v20.apk v21:…
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"
O=$HOME/code/sc2-apk/mesures/$(date +%Y%m%d); mkdir -p "$O"; T=$(date +%H%M%S)
# appareil : SC2_ADB_SERIAL, sinon le Fold trouvé dans « adb devices -l » (lib-fold.sh) ; exporté pour que chaque
# fold-ab-run.sh vise le même appareil sans refaire la détection
. "$R/scripts/lib-fold.sh"; D=$(fold_serie) || exit 1; export SC2_ADB_SERIAL=$D; echo "Fold : $D"
a() { adb -s "$D" "$@"; }
a shell cat /sdcard/SC2Partout/sc2.env > "$O/sc2.env.avant-$T"
[ -s "$O/sc2.env.avant-$T" ] || { echo "sc2.env illisible : abandon"; exit 1; }
a shell cp /sdcard/SC2Partout/sc2.env /sdcard/SC2Partout/sc2.env.sauvegarde-$T
a push "$R/scripts/entrees/jeu-actif-traynor01.txt" /sdcard/SC2Partout/jeu-actif.txt >/dev/null
# SC2_FRAMES=1 explicite (V0.1) : captures 1/10/100 comme aux mesures d'ancrage (v26), alors que la vague 0 bis fait
# de SC2_FRAMES=0 le défaut des lanceurs
{ grep -v -E '^(SC2_ARGS|SC2_BANC_ENTREES|SC2_DOCUMENTS|SC2_MANQUE_CONTINUE|SC2_ECRAN|SC2_FRAMES)=' "$O/sc2.env.avant-$T"; echo
  echo "SC2_MANQUE_CONTINUE=1"; echo "SC2_ECRAN=1280x720";   # scénario en coordonnées 1280x720
  echo "SC2_ARGS=-run Maps/Campaign/TRaynor01.SC2Map"; echo "SC2_FRAMES=1"
  echo "SC2_BANC_ENTREES=@/sdcard/SC2Partout/jeu-actif.txt"; echo "SC2_DOCUMENTS=/sdcard/SC2Partout/test-docs"; } \
  | grep -v '^$' > "$O/sc2.env.test-$T"
a push "$O/sc2.env.test-$T" /sdcard/SC2Partout/sc2.env >/dev/null
remettre() { a push "$O/sc2.env.avant-$T" /sdcard/SC2Partout/sc2.env >/dev/null && echo "$(date +%T) sc2.env d'origine remis"; }
trap remettre EXIT
cd "$R" || exit 1
dirs=()
for na in "$@"; do n=${na%%:*}; apk=${na#*:}; bash scripts/fold-ab-run.sh "$n" "$apk"; dirs+=("$O/$n"); done
python3 scripts/fold-ab.py "${dirs[@]}"
