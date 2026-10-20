#!/usr/bin/env bash
# Passe 7h — vue combinée de l'installation : liens symboliques vers l'installation de référence (passe 6a,
# extraction/install) + archives retrouvées sur le DVD (extraction/install-ajouts-7h, AJOUTS_7H=1
# scripts/extraire-install.sh). Aucune copie (5,9 Go + 1,3 Go restent en place).
#   android/outils/vue-install.sh <dossier vue> [base|complet]
#     base    : + Liberty.SC2Mod/Base.SC2Data et LibertyMulti.SC2Mod/Base.SC2Data (dépendances des cartes)
#     complet : + les 8 archives de langue enUS (défaut : base)
#     liste   : référence + SEULEMENT les archives de $AJOUTS (chemins relatifs, séparés par des espaces,
#               pris dans install-ajouts-7h) — passe 7m : isoler ce que le jeu valide
set -euo pipefail
VUE="$1"; NIV="${2:-base}"
DEPOT="$HOME/code/reverse/sc2-partout"; REF="$DEPOT/extraction/install"; AJ="$DEPOT/extraction/install-ajouts-7h"
rm -rf "$VUE"; mkdir -p "$VUE"
lier() { mkdir -p "$VUE/$(dirname "$2")"; ln -s "$1/$2" "$VUE/$2"; }
(cd "$REF" && find . -type f ! -name .manifeste.txt | sed 's|^\./||') | while read -r f; do lier "$REF" "$f"; done
(cd "$AJ" && find . -type f | sed 's|^\./||') | while read -r f; do
  if [ "$NIV" = liste ]; then case " ${AJOUTS:-} " in *" $f "*) ;; *) continue ;; esac; fi
  case "$f" in *enUS.SC2*) [ "$NIV" = complet ] || [ "$NIV" = liste ] || continue ;; esac
  lier "$AJ" "$f"
done
echo "vue $NIV : $(find "$VUE" -type l | wc -l) liens → $VUE"
