#!/usr/bin/env bash
# Passe 7y — vue d'installation selon android/manifest-install.txt : liens symboliques vers les archives (aucune
# copie), cherchées dans les dossiers sources dans l'ordre. Outil générique : tout le contenu vient du manifest.
#   android/outils/vue-manifest-7y.sh <dossier vue> [classes (défaut AB)] [--sans <chemin>]… [--avec <chemin>]… [--manifest <f>]
#   SOURCES="<dossier> <dossier>…" (défaut : extraction/install extraction/install-ajouts-7h du dépôt principal)
# Sortie : « vue <classes> : N liens, M dossiers, T octets (… Go) ; absents : … » ; rc=1 si une entrée manque.
set -euo pipefail
VUE="$1"; shift; CL="AB"; SANS=(); AVEC=(); ICI="$(cd "$(dirname "$0")/../.." && pwd)"; MAN="$ICI/android/manifest-install.txt"
[ $# -gt 0 ] && [[ "$1" != --* ]] && { CL="$1"; shift; }
while [ $# -gt 0 ]; do case "$1" in --sans) SANS+=("$2"); shift 2 ;; --avec) AVEC+=("$2"); shift 2 ;; --manifest) MAN="$2"; shift 2 ;; *) echo "argument inconnu : $1" >&2; exit 2 ;; esac; done
DEPOT="$HOME/code/reverse/sc2-partout"; SOURCES="${SOURCES:-$DEPOT/extraction/install $DEPOT/extraction/install-ajouts-7h}"
rm -rf "$VUE"; mkdir -p "$VUE"
n=0; nd=0; t=0; absents=""
while IFS=$'\t' read -r c p o r; do
  [[ -z "$c" || "$c" == \#* ]] && continue
  pris=0; [[ "$CL" == *"$c"* ]] && pris=1
  for s in "${AVEC[@]+"${AVEC[@]}"}"; do [ "$p" = "$s" ] && pris=1; done
  [ $pris = 1 ] || continue
  for s in "${SANS[@]+"${SANS[@]}"}"; do [ "$p" = "$s" ] && continue 2; done
  if [[ "$p" == */ ]]; then mkdir -p "$VUE/$p"; nd=$((nd + 1)); continue; fi
  src=""; for d in $SOURCES; do [ -e "$d/$p" ] && { src="$d/$p"; break; }; done
  if [ -z "$src" ]; then absents="$absents $p"; continue; fi
  mkdir -p "$VUE/$(dirname "$p")"; ln -s "$src" "$VUE/$p"; n=$((n + 1)); t=$((t + $(stat -L -c %s "$src")))
done < "$MAN"
echo "vue $CL${SANS[*]+ sans ${SANS[*]}}${AVEC[*]+ avec ${AVEC[*]}} : $n liens, $nd dossier(s), $t octets ($(awk -v t=$t 'BEGIN{printf "%.3f", t/1e9}') Go) ; absents :${absents:- aucun} → $VUE"
[ -z "$absents" ]
