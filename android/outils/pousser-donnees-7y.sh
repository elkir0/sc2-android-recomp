#!/usr/bin/env bash
# Passe 7y — pousse SUR L'APPAREIL les données listées par android/manifest-install.txt (côté Mac ; bash 3.2 suffit).
#   android/outils/pousser-donnees-7y.sh [--classes AB] [--manifest <f>] <dossier source> [<dossier source> …]
# Chaque fichier des classes demandées (défaut A et B = campagne) est cherché dans les dossiers sources, dans l'ordre
# (ex. une copie de extraction/install puis de extraction/install-ajouts-7h, ou une vue « rsync -L » de
# vue-manifest-7y.sh), et poussé dans $SD/install/<chemin> SEULEMENT s'il est absent ou de taille différente :
# relancer après une coupure reprend où l'on en était. Puis accès « tous les fichiers » pour l'app.
#   ADB=<chemin d'adb> (défaut : adb du PATH, sinon ~/Library/Android/sdk ou $ANDROID_SDK_ROOT)
#   SD=/sdcard/SC2Partout (défaut) ; PAQUET=fr.sc2partout
set -uo pipefail
ICI="$(cd "$(dirname "$0")/../.." && pwd)"; MAN="$ICI/android/manifest-install.txt"; CL=AB; SRC=""
while [ $# -gt 0 ]; do case "$1" in
  --classes) CL="$2"; shift 2 ;; --manifest) MAN="$2"; shift 2 ;; *) SRC="$SRC $1"; shift ;; esac; done
[ -n "$SRC" ] || { echo "usage : $0 [--classes AB] <dossier source>…" >&2; exit 2; }
SD="${SD:-/sdcard/SC2Partout}"; PAQUET="${PAQUET:-fr.sc2partout}"
if [ -z "${ADB:-}" ]; then
  ADB=$(command -v adb || true)
  for c in "${ANDROID_SDK_ROOT:-}/platform-tools/adb" "$HOME/Library/Android/sdk/platform-tools/adb" "$HOME/tools/android-sdk/platform-tools/adb"; do
    [ -z "$ADB" ] && [ -x "$c" ] && ADB="$c"; done
fi
[ -n "$ADB" ] || { echo "adb introuvable (ADB=…)" >&2; exit 2; }
[ "$("$ADB" get-state 2> /dev/null)" = device ] || { echo "aucun appareil adb (USB/débogage sans fil activé ?)" >&2; exit 2; }
if [ "$(uname)" = Darwin ]; then taille() { stat -f %z "$1"; }; else taille() { stat -L -c %s "$1"; }; fi
n=0; deja=0; total=0; pousse=0; absents=""; echecs=""
"$ADB" shell "mkdir -p '$SD/install'"
while IFS="$(printf '\t')" read -r c p o r; do
  case "$c" in ''|\#*) continue ;; esac
  case "$CL" in *"$c"*) ;; *) continue ;; esac
  case "$p" in */) "$ADB" shell "mkdir -p '$SD/install/$p'"; continue ;; esac
  f=""; for d in $SRC; do [ -e "$d/$p" ] && { f="$d/$p"; break; }; done
  if [ -z "$f" ]; then absents="$absents $p"; continue; fi
  t=$(taille "$f"); total=$((total + t)); n=$((n + 1))
  dist=$("$ADB" shell "stat -c %s '$SD/install/$p' 2> /dev/null" | tr -d '\r')
  if [ "$dist" = "$t" ]; then deja=$((deja + 1)); echo "  déjà là ($t o) : $p"; continue; fi
  echo "  push ($t o) : $p"
  "$ADB" shell "mkdir -p '$SD/install/$(dirname "$p")'"
  if "$ADB" push "$f" "$SD/install/$p" > /dev/null; then pousse=$((pousse + t)); else echecs="$echecs $p"; fi
done < "$MAN"
"$ADB" shell appops set --uid "$PAQUET" MANAGE_EXTERNAL_STORAGE allow 2> /dev/null \
  || echo "  (appops : installer l'APK d'abord, ou accorder « tous les fichiers » dans les réglages)"
echo "classes $CL : $n fichiers, $total octets ($(awk -v t=$total 'BEGIN{printf "%.2f", t/1e9}') Go) ; déjà présents $deja ; poussés $pousse octets"
[ -z "$absents" ] || echo "ABSENTS des sources :$absents"
[ -z "$echecs" ] || echo "ÉCHECS de push :$echecs"
[ -z "$absents$echecs" ]
