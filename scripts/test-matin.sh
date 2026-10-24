#!/usr/bin/env bash
# test-matin.sh - passe 9f : test d'affichage du Fold en UNE commande (recette scripts/test-matin.md, meme ordre).
#   scripts/test-matin.sh <device-id-adb-complet>
#   ADB_FAKE=1 FIXTURE=scripts/verdict-fixtures/cpu-livrant.log scripts/test-matin.sh faux   (sans telephone)
# Variables : ATTENTE (s, defaut 150) ; FAKE_ENV2=1 (fake : le 2e sc2.env existe au depart).
# Sorties : $TEST_MATIN_SORTIE, defaut /tmp/fold-matin/ (hors depot : logcat et capture du jeu ne vont jamais dans git) ;
#   verifier.sh passe le sien en ADB_FAKE, pour ne jamais effacer le dump ni le verdict d'un vrai test du Fold.
# Derniere ligne : RESUME: verdict=<...> png=<N>octets. Exit : celui de verdict-fold.sh (0 livre, 1 non, 2 sans SC2).
set -eu
export LC_ALL=C

[ $# -ge 1 ] && [ -n "$1" ] || { echo "usage: test-matin.sh <device-id-adb-complet>   (adb devices -l, colonne 1)" >&2; exit 2; }
D="$1"
ICI="$(cd "$(dirname "$0")" && pwd)"
O="${TEST_MATIN_SORTIE:-/tmp/fold-matin}"
ATTENTE="${ATTENTE:-150}"
PKG=fr.sc2partout
ENV1=/sdcard/SC2Partout/sc2.env
ENV2=/sdcard/Android/data/$PKG/files/sc2.env
mkdir -p "$O"
rm -f "$O/dump.log" "$O/verdict.txt" "$O/ecran.png" "$O/surfaceflinger.txt"   # pas de reste d'un run precedent

# --- adb factice (ADB_FAKE=1) : memes appels, reponses tirees de $FIXTURE ---
if [ "${ADB_FAKE:-0}" = 1 ]; then
  : "${FIXTURE:?ADB_FAKE=1 demande FIXTURE=<dump logcat>}"
  [ -r "$FIXTURE" ] || { echo "FIXTURE illisible : $FIXTURE" >&2; exit 2; }
  ETAT="$O/fake"; mkdir -p "$ETAT"; rm -f "$ETAT"/*
  [ "${FAKE_ENV2:-0}" = 1 ] && : > "$ETAT/env2"
  adb() {
    [ "$1" = -s ] && shift 2
    case "$*" in
      "logcat -d") cat -- "$FIXTURE" ;;
      logcat*) ;;
      "shell cat $ENV1") cat "$ETAT/env1" 2>/dev/null || true ;;
      "shell echo SC2_RUN_SANS_BNET=1 > $ENV1") echo SC2_RUN_SANS_BNET=1 > "$ETAT/env1" ;;
      "shell [ -e $ENV2 ] && echo present || echo absent") [ -e "$ETAT/env2" ] && echo present || echo absent ;;
      "shell rm -f $ENV2") rm -f "$ETAT/env2" ;;
      "shell getprop ro.product.model") echo "FAUX-TELEPHONE" ;;
      "shell dumpsys SurfaceFlinger"*) ;;
      "pull "*) printf '\211PNG\r\n\032\n000000000000' > "$3" ;;   # PNG factice de 20 octets
      *) ;;                                                       # force-stop, appops, am, input, wm, screencap
    esac
  }
  echo "== MODE ADB_FAKE : fixture $FIXTURE, attente ${ATTENTE} s"
fi
a() { adb -s "$D" "$@"; }

etape() { echo; echo "== $*"; }

etape "0. appareil $D : $(a shell getprop ro.product.model | tr -d '\r')"

etape "1. arret de $PKG"
a shell am force-stop "$PKG"

etape "2. sc2.env = SC2_RUN_SANS_BNET=1 seul (mode menu)"
a shell "echo SC2_RUN_SANS_BNET=1 > $ENV1"
lu="$(a shell cat "$ENV1" | tr -d '\r')"
echo "   $ENV1 : $lu"
[ "$lu" = SC2_RUN_SANS_BNET=1 ] || { echo "ECHEC : $ENV1 ne contient pas exactement SC2_RUN_SANS_BNET=1" >&2; exit 2; }

etape "3. 2e sc2.env (lu APRES le premier, il gagnerait) : doit etre absent"
e2="$(a shell "[ -e $ENV2 ] && echo present || echo absent" | tr -d '\r')"
if [ "$e2" = present ]; then
  echo "   present : efface"
  a shell rm -f "$ENV2"
  e2="$(a shell "[ -e $ENV2 ] && echo present || echo absent" | tr -d '\r')"
  [ "$e2" = absent ] || { echo "ECHEC : $ENV2 toujours present" >&2; exit 2; }
fi
echo "   $ENV2 : $e2"

etape "4. appops MANAGE_EXTERNAL_STORAGE allow"
a shell appops set "$PKG" MANAGE_EXTERNAL_STORAGE allow

etape "5. logcat : tampon 16M puis vide"
a logcat -G 16M || echo "   (logcat -G refuse : tampon par defaut, debut du journal a risque)"
a logcat -c

etape "6. reveil, deverrouillage, lancement"
a shell input keyevent KEYCODE_WAKEUP || true
a shell wm dismiss-keyguard || echo "   (wm dismiss-keyguard refuse : deverrouiller a la main)"
a shell am start -n "$PKG/android.app.NativeActivity"

etape "7. attente ${ATTENTE} s"
reste="$ATTENTE"
while [ "$reste" -gt 0 ]; do
  pas=10; [ "$reste" -lt 10 ] && pas="$reste"
  printf '   %3d s restantes\n' "$reste"
  sleep "$pas"
  reste=$((reste - pas))
done

etape "8. logcat -d -> $O/dump.log"
a logcat -d > "$O/dump.log"
echo "   $(wc -l < "$O/dump.log") lignes, dont $(grep -ac SC2Partout "$O/dump.log" || true) SC2Partout"
# grep AVANT tail
grep -a SC2Partout "$O/dump.log" | grep -aE '\[FENETRE\]|\[AFFICHAGE\]|\[VKP\]|\[ANDROID\] (affichage|swapchain)|\[FPS\]' \
  | tail -40 | cut -c1-220 | sed 's/^/   | /' || true

etape "9. verdict"
set +e
"$ICI/verdict-fold.sh" "$O/dump.log" > "$O/verdict.txt"
rv=$?
set -e
sed 's/^/   /' "$O/verdict.txt"
verdict="$(grep -a '^VERDICT: ' "$O/verdict.txt" | tail -1 | sed -E 's/^VERDICT: ([^ |]+).*/\1/')"
echo "   exit verdict-fold : $rv"

etape "10. capture d'ecran"
a shell screencap -p /sdcard/sc2matin.png
a pull /sdcard/sc2matin.png "$O/ecran.png" > /dev/null
png="$(wc -c < "$O/ecran.png" 2>/dev/null | tr -d ' ' || true)"; png="${png:-0}"   # wc : portable Linux et macOS
if   [ "$png" -le 20000 ]; then lecture="image uniforme (noire)"
elif [ "$png" -gt 300000 ]; then lecture="contenu"
else lecture="indetermine (entre 20 Ko et 300 Ko)"
fi
echo "   $O/ecran.png : $png octets -> $lecture"

if [ "$rv" = 0 ] && [ "$png" -le 300000 ]; then
  etape "11. un chemin livre mais l'ecran n'a pas de contenu : dumpsys SurfaceFlinger"
  a shell dumpsys SurfaceFlinger > "$O/surfaceflinger.txt" || true
  sf="$(grep -a -A40 "$PKG" "$O/surfaceflinger.txt" | grep -aE "$PKG|omposition|geomBufferSize" | head -30 || true)"
  if [ -n "$sf" ]; then printf '%s\n' "$sf" | sed 's/^/   | /'; else echo "   (aucune ligne $PKG / composition / geomBufferSize)"; fi
fi

echo
echo "RESUME: verdict=${verdict:-?} png=${png}octets"
exit "$rv"
