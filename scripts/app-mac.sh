#!/bin/bash
# app-mac.sh — V3, lot M7 (24/10) : lance SC2Partout.app par LaunchServices (open), pour le seul essai qui exige
# LSEnvironment (SC2_FENETRE=1 posée par l'Info.plist) ; tous les autres essais exécutent directement
# $APP/Contents/MacOS/recomp-sc2, qui rend son code (v3-critique.md §3.1 greffe 3, fiche M7 critère 10).
#   scripts/app-mac.sh <app> <dossier> <scénario.txt> [délai_s=600] [VAR=valeur…]
#     <dossier> est vidé puis reçoit : support/ (SC2_MAC_SUPPORT isolé), j.log (journal de l'app, SC2_MAC_JOURNAL),
#     open.log (sorties de open et de l'app avant l'ouverture du journal), resume.txt.
#     L'app est lancée sans être activée (open -g : rejeu isolé, SC2_MAC_ISOLE=1 par défaut avec un scénario), nouvelle
#     instance (-n), attendue (-W) sous perl alarm (délai dépassé : 142). Sous le verrou banc (scripts/verrou.sh, réentrant).
#     Délai dépassé (revue adverse MAJ-1) : l'alarme ne tue que open, l'app est un fils de launchd. TOUJOURS sous le
#     verrou, son pid est relevé dans j.log (« [MAC] mode fenêtre … pid N, paquet … ») — à défaut, le SEUL processus dont
#     la commande est l'exécutable du paquet —, vérifié (commande = <app>/Contents/MacOS/recomp-sc2), affiché, puis
#     SIGTERM (sortie demandée par la source dispatch de l'app), SIGKILL 10 s plus tard, et sa mort vérifiée avant de
#     rendre le verrou. Plusieurs candidats, ou un pid qui n'est pas l'app : rien n'est tué (code 70, à voir à la main).
#     Environnement de l'app : celui de LaunchServices (PAS celui du shell) + les --env d'ici : SC2_MAC_SUPPORT,
#     SC2_MAC_JOURNAL, SC2_INSTALL (${SC2_EXTRACTION:-<dépôt principal>/extraction}/install), SC2_MAC_EVENEMENTS, et les
#     VAR=valeur donnés en plus. Les polices viennent de Contents/Resources du paquet.
# open -W ne rend pas le code de l'app : le résumé lit « [MAC] code de sortie N » dans j.log (crochet de sortie, M10) et
# compte [MAC] mode fenêtre, [VKP] swapchain, [FPS], [PAUSE], [ACTIVATION] et [MACEV] délai dépassé.
set -u
# verrou banc tenu (relancé par verrou.sh) : open, puis, au délai, l'app elle-même arrêtée et vérifiée morte
sous_verrou() {
  local APP="$1" D="$2" DELAI="$3"; shift 3
  perl -e 'alarm shift; exec @ARGV' "$DELAI" open -n -g -W --stdout "$D/open.log" --stderr "$D/open.log" "$@" "$APP"
  local rc=$? exe="$APP/Contents/MacOS/recomp-sc2" pid cmd l n i
  [ "$rc" = 142 ] || return "$rc"
  echo "app-mac : délai de $DELAI s dépassé (open arrêté) — arrêt de l'app"
  pid=$(sed -n 's/^\[MAC\] mode fenêtre .*, pid \([0-9][0-9]*\), paquet .*/\1/p' "$D/j.log" 2>/dev/null | head -1)
  if [ -z "$pid" ]; then                      # journal pas encore ouvert : l'exécutable du paquet, s'il est seul
    l=$(pgrep -f "^$exe\$" | tr '\n' ' '); n=$(echo $l | wc -w | tr -d ' ')
    echo "app-mac : pid absent du journal ; processus « $exe » : ${l:-aucun}"
    [ "$n" = 0 ] && { echo "app-mac : aucune app à arrêter"; return 142; }
    [ "$n" = 1 ] || { echo "app-mac : $n candidats — RIEN n'est tué, à voir à la main"; return 70; }
    pid=$l; pid=${pid// /}
  fi
  cmd=$(ps -o command= -p "$pid" 2>/dev/null)
  [ -n "$cmd" ] || { echo "app-mac : pid $pid déjà terminé"; return 142; }
  case "$cmd" in "$exe"|"$exe "*) ;; *) echo "app-mac : pid $pid n'est pas l'app ($cmd) — RIEN n'est tué"; return 70 ;; esac
  echo "app-mac : pid $pid ($cmd) : SIGTERM"
  kill -TERM "$pid" 2>/dev/null
  for i in $(seq 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$pid" 2>/dev/null; then
    echo "app-mac : pid $pid vivant 10 s après SIGTERM : SIGKILL"
    kill -KILL "$pid" 2>/dev/null
    for i in $(seq 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  fi
  if kill -0 "$pid" 2>/dev/null; then echo "app-mac : pid $pid TOUJOURS VIVANT — à arrêter à la main"; return 70; fi
  echo "app-mac : pid $pid terminé, verrou banc rendu ensuite"
  return 142
}
[ "${1:-}" = --sous-verrou ] && { shift; sous_verrou "$@"; exit $?; }
[ $# -ge 3 ] || { sed -n '5,19p' "$0" | sed 's/^# *//' >&2; exit 64; }
APP="$(cd "$1" && pwd)"; D="$2"; SCEN="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")"; DELAI=600
shift 3
case "${1:-}" in *=*|'') ;; *) DELAI="$1"; shift ;; esac
R="$(cd "$(dirname "$0")/.." && pwd)"
COMMUN="$(git -C "$R" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"
X="${SC2_EXTRACTION:-$(dirname "${COMMUN:-$R/.git}")/extraction}"
[ -x "$APP/Contents/MacOS/recomp-sc2" ] || { echo "app-mac : $APP/Contents/MacOS/recomp-sc2 absent (ninja … sc2partout-app)" >&2; exit 66; }
[ -f "$SCEN" ] || { echo "app-mac : scénario $SCEN absent" >&2; exit 66; }
[ -d "$X/install" ] || { echo "app-mac : $X/install absent (SC2_EXTRACTION)" >&2; exit 66; }
case "$DELAI" in ''|*[!0-9]*) echo "app-mac : délai « $DELAI » invalide (secondes)" >&2; exit 64 ;; esac
rm -rf "$D"; mkdir -p "$D/support"; D="$(cd "$D" && pwd)"
ENVS=(--env "SC2_MAC_SUPPORT=$D/support" --env "SC2_MAC_JOURNAL=$D/j.log" --env "SC2_INSTALL=$X/install"
      --env "SC2_MAC_EVENEMENTS=@$SCEN")
for kv in "$@"; do ENVS+=(--env "$kv"); done
t=$SECONDS
"$R/scripts/verrou.sh" banc "$R/scripts/app-mac.sh" --sous-verrou "$APP" "$D" "$DELAI" "${ENVS[@]}" | tee "$D/arret.log"
rc=${PIPESTATUS[0]}
{
  echo "open : rc=$rc en $((SECONDS - t)) s ($([ $rc = 142 ] && echo 'délai dépassé, app arrêtée' || { [ $rc = 70 ] && echo 'délai dépassé, APP NON ARRÊTÉE' || echo 'app terminée'; }))"
  grep -a -m1 '^\[MAC\] mode fenêtre' "$D/j.log" 2>/dev/null || echo "[MAC] mode fenêtre : ABSENT du journal"
  grep -a '^\[MAC\] code de sortie' "$D/j.log" 2>/dev/null || echo "[MAC] code de sortie : absent (crochet de sortie, M10)"
  for m in '\[VKP\] swapchain [0-9]' '^\[FPS\]' '^\[PAUSE\]' '\[ACTIVATION\]' '\[MACEV\] délai dépassé' '\[MAC\] sortie demandée'; do
    n=$(grep -a -c "$m" "$D/j.log" 2>/dev/null); echo "$m : ${n:-0}"
  done
} | tee "$D/resume.txt"
rm -rf "$D/support/sc2-c"
exit $rc
