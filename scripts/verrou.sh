#!/bin/bash
# verrou.sh — V0.3 : verrous partagés des agents (builds, banc, émulateur, pack), fichiers ~/.sc2-verrous/<nom>.
#   scripts/verrou.sh <nom> <commande…>          exécute la commande verrou tenu ; rend son code de sortie
#   VERROU_DELAI=<s>                              attente bornée à <s> secondes ; absent : illimitée ; 0 : échec immédiat
#                                                 (verrou non obtenu : message sur stderr et code 75, EX_TEMPFAIL)
# Noms : build-lift (compilation du code lifté, une seule à la fois : RAM), edl (édition de liens, DEUX jetons edl-1 et
# edl-2 : le premier libre est pris), banc (runs du jeu ; MÊME fichier que les « /usr/bin/lockf -k ~/.sc2-verrous/banc »
# des scripts de banc, les deux formes s'excluent), emulateur, pack (régénération du pack SPIR-V).
# Mécanique : /usr/bin/lockf -k (verrou flock(2) sur le fichier, gardé ; ni flock(1) ni timeout sur ce Mac). Le verrou
# appartient au processus lockf, qui lance la commande en fils et attend sa fin : le noyau le libère à la mort de lockf.
# Un verrou ORPHELIN (porteur tué, kill -9 compris) est donc repris aussitôt par le suivant, sans nettoyage — vérifié le
# 22/10 (V0.3) : lockf porteur tué par kill -9, la prise suivante passe en 0 s. Corollaire : tuer l'ARBRE (lockf ET la
# commande) ; tuer lockf seul libère le verrou pendant que la commande tourne encore.
# Message d'attente « verrou <nom> pris (par …), attente » : le porteur écrit <fichier>.qui (pid, heure, commande) ; un
# .qui dont le pid est mort est ignoré ; à défaut (prise directe par lockf), lsof sur le SEUL fichier du verrou.
# RÉENTRANT (22/10, revue V0.3) : le porteur exporte SC2_VERROUS_TENUS (« :banc:edl: »…) à sa descendance, et
# BANC_VERROU_TENU=1 pour le verrou banc (le court-circuit de banc_verrou, scripts/lib-banc.sh) ; un descendant qui
# redemande un verrou que tient déjà un ancêtre exécute directement sa commande. Sans cela, « verrou.sh banc
# scripts/banc-mac.sh … » s'interbloquait : banc-mac.sh reprend lui-même le verrou banc.
# VERROU_DOSSIER : autre dossier de verrous (tests de scripts/verifier.sh) ; défaut ~/.sc2-verrous.
set -u
L=/usr/bin/lockf
D="${VERROU_DOSSIER:-$HOME/.sc2-verrous}"
if [ "${1:-}" = --porteur ]; then          # interne : relancé PAR lockf, verrou acquis → marqueur, .qui, puis la commande
  m="$2"; f="$3"; n="$4"; shift 4
  echo 1 > "$m"
  printf 'pid %s depuis %s : %s\n' "$$" "$(date '+%H:%M:%S')" "$*" | cut -c1-200 > "$f.qui" 2>/dev/null
  export SC2_VERROUS_TENUS="${SC2_VERROUS_TENUS:-}:$n:"
  [ "$n" = banc ] && export BANC_VERROU_TENU=1
  exec "$@"
fi
usage() { echo "usage : $0 <build-lift|edl|banc|emulateur|pack> <commande…>  (VERROU_DELAI=<s>, 0 = échec immédiat)" >&2; exit 64; }
[ $# -ge 2 ] || usage
nom="$1"; shift
case "$nom" in
  edl) jetons="edl-1 edl-2" ;;
  build-lift|banc|emulateur|pack) jetons="$nom" ;;
  *) echo "verrou.sh : nom inconnu « $nom »" >&2; usage ;;
esac
case ":${SC2_VERROUS_TENUS:-}:" in *":$nom:"*) exec "$@" ;; esac     # déjà tenu par un ancêtre : réentrant
delai="${VERROU_DELAI-}"
case "$delai" in ''|*[!0-9]*) [ -z "$delai" ] || { echo "verrou.sh : VERROU_DELAI=« $delai » n'est pas un nombre de secondes" >&2; exit 64; } ;; esac
[ -x "$L" ] || { echo "verrou.sh : $L absent (macOS seulement)" >&2; exit 69; }
mkdir -p "$D" || exit 73
SOI="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
m="$(mktemp "${TMPDIR:-/tmp}/verrou.XXXXXX")" || exit 73
trap 'rm -f "$m"' EXIT
# essai <jeton> [délai] : une prise (délai absent = attente illimitée). Verrou obtenu : sortie avec le code de la commande
# (le marqueur distingue un 75 de la commande d'un 75 de lockf) ; sinon retour 75.
essai() {
  local f="$D/$1" rc
  : > "$m"
  if [ $# -ge 2 ]; then "$L" -k -s -t "$2" "$f" "$SOI" --porteur "$m" "$f" "$nom" "${CMD[@]}"; rc=$?
  else "$L" -k -s "$f" "$SOI" --porteur "$m" "$f" "$nom" "${CMD[@]}"; rc=$?; fi
  [ -s "$m" ] && exit $rc
  [ $rc = 75 ] || { echo "verrou.sh : lockf a échoué (code $rc) sur $f" >&2; exit $rc; }
  return 75
}
detenteur() {   # <jeton> : qui tient le fichier
  local f="$D/$1" p x s=""
  if [ -f "$f.qui" ]; then
    p="$(sed -n 's/^pid \([0-9][0-9]*\) .*/\1/p' "$f.qui")"
    if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then cat "$f.qui"; return; fi
  fi
  for x in $(lsof -t -- "$f" 2>/dev/null | head -3); do s="${s}pid $x : $(ps -o command= -p "$x" 2>/dev/null | cut -c1-160) ; "; done
  echo "${s:-détenteur inconnu}"
}
CMD=("$@")
for j in $jetons; do essai "$j" 0; done          # première passe : le premier jeton libre
qui=""; for j in $jetons; do qui="$qui${qui:+ ; }$j : $(detenteur "$j")"; done
if [ "$delai" = 0 ]; then echo "verrou $nom pris (par $qui) : VERROU_DELAI=0, échec immédiat" >&2; exit 75; fi
echo "verrou $nom pris (par $qui), attente${delai:+ (au plus $delai s)}" >&2
if [ "$jetons" = "$nom" ]; then                    # un seul jeton : lockf attend lui-même
  if [ -n "$delai" ]; then essai "$nom" "$delai"; else essai "$nom"; fi
else                                               # deux jetons : sondage, chaque seconde, du premier libre
  t0=$SECONDS
  while :; do
    sleep 1
    for j in $jetons; do essai "$j" 0; done
    [ -n "$delai" ] && [ $((SECONDS - t0)) -ge "$delai" ] && break
  done
fi
echo "verrou $nom : toujours pris après ${delai} s, abandon" >&2
exit 75
