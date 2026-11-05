#!/bin/bash
# nonreg.sh — non-régression à deux niveaux au banc Mac (V1.4, 22/10).
#   scripts/nonreg.sh <binaire|main> <sortie> [--complet] [--maj-ref [--forcer]] [--ref <dossier>] [--seul a,b,…]
#                     [--env VAR=val …]
#   fumée (défaut)  canon « defaut » (banc-mac.sh), entrée de campagne par le bar (banc-bar.sh), puis TRaynor01,
#                   THanson03A et TZeratul04 en victoire (campagne-banc.sh) ; durée mesurée le 22/10 : voir ci-dessous
#   --complet       canon, bar, puis les 29 missions (≈ 2 h de banc)
#   main            binaire de référence du dépôt principal (<dépôt principal>/recomp-sc2/build-mac-O2radx/recomp-sc2)
#   --env VAR=val   ajoutée à l'environnement de TOUS les runs (répétable) ; les variables que les scripts de banc
#                   posent eux-mêmes (SC2_ROOT, SC2_FRAMES*, SC2_ARGS, SC2_BANC_ENTREES…) gardent leur valeur
#   --ref <dossier> référence (défaut : scripts/nonreg-ref/ de CE dépôt, ou NONREG_REF) : <dossier>/<mode>.json
#   --maj-ref       pose la référence après le run ([SUP] seul, en citant le chantier dans NOTES) : refusée (code 4) si
#                   un contrôle n'est pas PASS hors images/s, sauf --forcer ; incompatible avec --seul
#   --seul a,b      sous-ensemble des contrôles (canon, bar, noms de cartes) : vérification rapide, jamais une référence
# Sortie (dossier neuf ou vide) : <sortie>/canon/, <sortie>/bar/, <sortie>/missions/<carte>/ (dossiers des scripts de
#   banc, déjà compactés : empreintes.txt, vignettes PNG, plus aucun PPM), <sortie>/<étape>.sortie (leur sortie),
#   <sortie>/nonreg.json (sha256 du binaire et du pack SPIR-V, une entrée par contrôle : rc, victoire, MANQUE,
#   lumière, replis « sans données », arrêts, images, images/s, critères et verdict) et <sortie>/nonreg.md, imprimé :
#   PASS ou RÉGRESSION par ligne et au global. Critères, tolérances et forme de la référence : en-tête de
#   scripts/nonreg.py (dépouillement ; replis et échantillons lus par campagne-rapport.py).
# Code de sortie : 0 PASS (avec --maj-ref : référence posée), 1 RÉGRESSION, 2 usage, 3 garde-disque, 4 maj-ref refusée.
# Verrou : le banc est tenu pendant TOUT le run (banc_verrou, scripts/lib-banc.sh : ré-exécution sous
#   scripts/verrou.sh banc) ; les scripts de banc appelés le voient tenu et ne le reprennent pas. Un second nonreg.sh
#   (ou tout run de banc) attend la fin du premier.
# Variables : NONREG_TOL_FPS (0.10 : images/s AGRÉGÉES des missions jusqu’à −10 % ; par mission : indicatif), NONREG_TOL_IMAGES (0.5 : images des runs minutés jusqu'à
#   −50 %) — des FRACTIONS dans [0, 1), contrôlées avant le banc (code 2 sinon) —, NONREG_REF, et celles des scripts
#   de banc (SC2_EXTRACTION, SC2_DISQUE_MIN_GO, SC2_POLICES).
# Durée de la fumée (banc M4, 22/10) : voir docs du chantier V1.4 dans le rapport de l'agent (mesurée par ce script :
#   champ duree_s du JSON, dernière ligne du rapport).
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"
usage() { sed -n '3,4p' "$0" | sed 's/^# *//' >&2; echo "(détails : en-tête de $0)" >&2; exit 2; }
[ $# -ge 2 ] || usage
ORIG=("$@")                                     # pour la ré-exécution sous le verrou
ARG_BIN=$1; S=$2; shift 2
MODE=fumee; MAJ=0; FORCER=0; SEUL=""; REF=${NONREG_REF:-$R/scripts/nonreg-ref}; ENVS=()
while [ $# -gt 0 ]; do
    case $1 in
    --complet) MODE=complet ;;
    --maj-ref) MAJ=1 ;;
    --forcer) FORCER=1 ;;
    --ref) REF=${2:?--ref <dossier>}; shift ;;
    --seul) SEUL=${2:?--seul <contrôle,…>}; shift ;;
    --env)
        v=${2:-}; shift
        [[ $v =~ ^[A-Za-z_][A-Za-z0-9_]*= ]] || { echo "nonreg : --env « $v » : VAR=valeur attendu" >&2; exit 2; }
        ENVS+=("$v") ;;
    *) echo "nonreg : option inconnue « $1 »" >&2; usage ;;
    esac
    shift
done
[ "$MAJ" = 1 ] && [ -n "$SEUL" ] && { echo "nonreg : --maj-ref avec --seul : une référence couvre tous les contrôles" >&2; exit 2; }
# tolérances contrôlées AVANT le banc (sinon rejetées au dépouillement, après 13 min de fumée ou 2 h de --complet) :
# une fraction dans [0, 1) — NONREG_TOL_FPS=5 (« 5 % ») donnerait un seuil de −400 % et désactiverait le critère
for tol in "NONREG_TOL_FPS=${NONREG_TOL_FPS:-0.10}" "NONREG_TOL_IMAGES=${NONREG_TOL_IMAGES:-0.5}"; do
    [[ ${tol#*=} =~ ^0?\.[0-9]+$|^0$ ]] \
        || { echo "nonreg : ${tol%%=*} « ${tol#*=} » : fraction dans [0, 1) attendue (0.05 = 5 %)" >&2; exit 2; }
done
if [ "$ARG_BIN" = main ]; then
    COMMUN="$(git -C "$R" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" \
        || { echo "nonreg : pas de dépôt git autour de $R pour « main »" >&2; exit 2; }
    ARG_BIN="$(dirname "$COMMUN")/recomp-sc2/build-mac-O2radx/recomp-sc2"
fi
[ -x "$ARG_BIN" ] && [ -f "$ARG_BIN" ] || { echo "nonreg : binaire « $ARG_BIN » absent ou non exécutable" >&2; exit 2; }
BIN=$(cd "$(dirname "$ARG_BIN")" && pwd)/$(basename "$ARG_BIN")
if [ -e "$S" ] && [ -n "$(ls -A "$S" 2>/dev/null)" ]; then
    echo "nonreg : $S existe et n'est pas vide : sortie neuve attendue (rien n'est effacé)" >&2; exit 2
fi
FUMEE="TRaynor01 THanson03A TZeratul04"
TOUTES="TRaynor01 TRaynor02 TRaynor03 THanson01 THanson02 THanson03A THanson03B TTosh01 TTosh02 TTosh03A TTosh03B \
TTychus01 TTychus02 TTychus03 TTychus04 TTychus05 THorner01 THorner02 THorner03 THorner04 THorner05S TValerian01 \
TValerian02A TValerian02B TValerian03 TZeratul01 TZeratul02 TZeratul03 TZeratul04"
[ "$MODE" = complet ] && CARTES=$TOUTES || CARTES=$FUMEE
CTRL="canon bar $CARTES"
if [ -n "$SEUL" ]; then
    for c in ${SEUL//,/ }; do
        case " $CTRL " in *" $c "*) ;; *) echo "nonreg : --seul : « $c » n'est pas un contrôle du mode $MODE ($CTRL)" >&2; exit 2 ;; esac
    done
    CTRL=" ${SEUL//,/ } "
fi
. "$R/scripts/lib-banc.sh"
banc_garde_disque "$S" 3 || exit 3
banc_verrou "$0" "${ORIG[@]}"                   # ré-exécution sous le verrou banc, sauf s'il est déjà tenu
# ---- verrou banc tenu ----
mkdir -p "$S" && S=$(cd "$S" && pwd) || exit 2
for v in ${ENVS[@]+"${ENVS[@]}"}; do export "$v"; done
echo "nonreg $MODE : $BIN → $S ; contrôles :$(echo $CTRL | sed 's/^/ /') ; env : ${ENVS[*]:-aucun} ; réf. $REF ($(date +%T))"
etape() {   # <nom> <commande…> : un script de banc, sortie dans <sortie>/<nom>.sortie, résumé à l'écran
    local nom=$1 t=$SECONDS rc; shift
    echo "=== $nom ($(date +%H:%M:%S)) ==="
    "$@" > "$S/$nom.sortie" 2>&1; rc=$?
    grep -aE '^rc=|^=== |garde-|refus|attente|SIGSEGV|SIGBUS|ARRÊT\] faute|DEVICE_LOST|victoire' "$S/$nom.sortie" \
        | grep -av '^| ' | head -40 | sed 's/^/    /'
    echo "    → $nom : code $rc en $((SECONDS - t)) s"
}
T0=$SECONDS
case " $CTRL " in *" canon "*) etape canon "$R/scripts/banc-mac.sh" defaut "$BIN" "$S/canon" ;; esac
case " $CTRL " in *" bar "*) etape bar "$R/scripts/banc-bar.sh" "$BIN" "$S/bar" ;; esac
M=""; for c in $CTRL; do case $c in canon|bar) ;; *) M="$M $c" ;; esac; done
[ -n "$M" ] && etape missions "$R/scripts/campagne-banc.sh" "$BIN" "$S/missions" $M
DUREE=$((SECONDS - T0))
A=(analyser "$S" --mode "$MODE" --controles "$(echo $CTRL | tr ' ' ',')" --binaire "$BIN" --duree "$DUREE" --ref "$REF"
   --tol-fps "${NONREG_TOL_FPS:-0.10}" --tol-images "${NONREG_TOL_IMAGES:-0.5}")
[ -n "$SEUL" ] && A+=(--seul)
for v in ${ENVS[@]+"${ENVS[@]}"}; do A+=(--env "$v"); done
echo
python3 -I "$R/scripts/nonreg.py" "${A[@]}"; RC=$?
[ "$RC" -le 1 ] || { echo "nonreg : dépouillement en échec (code $RC)" >&2; exit 2; }
if [ "$MAJ" = 1 ]; then
    if [ "$FORCER" = 1 ]; then python3 -I "$R/scripts/nonreg.py" maj-ref "$S" --ref "$REF" --forcer
    else python3 -I "$R/scripts/nonreg.py" maj-ref "$S" --ref "$REF"; fi || exit 4
    exit 0
fi
exit "$RC"
