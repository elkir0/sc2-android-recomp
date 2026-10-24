#!/bin/bash
# capture-ab.sh — CAPTURE A/B chiffrée au banc Mac (V2.2, 22/10 ; réutilisable par V2.1 et V2.3) : mêmes captures d'une
# mission sous deux réglages, runs entrelacés A1 B1 A2 B2…, AE par capture sur la zone d'un run de diagnostic.
#   scripts/capture-ab.sh <binaire> <sortie> <carte> <scénario> <captures> [n] -- <étiqA>:<VAR=val,…> <étiqB>:<VAR=val,…> [<étiqD>:<VAR=val,…>]
#   ex. scripts/capture-ab.sh $BIN ~/dev/sc2-bancs/20261022/instanciation/ab THanson03A scripts/entrees/inst-ab.txt \
#         3600:f3600,4200:f4200 2 -- A:SC2_INSTANCES=0 B:SC2_INSTANCES=1 D:SC2_INSTANCES_DIAG=1
#   <captures>  « scenario » (23/10) : les actions CAPTURE:<nom> du scénario (V1.1, format 2 : instants de jeu fixes,
#               ex. scripts/entrees/inst-ab-pret.txt, QUAND:PRET puis +30:CAPTURE:pret30), SC2_CAPTURE_TEST non posé ;
#               sinon SC2_CAPTURE_TEST : « <frame>:<nom>,… » (numéros de frame : instants de jeu variables d'un run à l'autre)
#               Si le premier run (D, sinon A1) n'écrit AUCUNE des captures, la série s'arrête là (code 1).
#   n           paires A/B (défaut 2) ; « VAR=val,… » : variables propres à l'étiquette (« - » : aucune)
#   D (3e étiquette, facultative) : un run de diagnostic joué EN PREMIER (il sert aussi de chauffe : cache de shaders
#               Metal commun au système, voir banc-ab.sh de V1.4) ; ses captures donnent la ZONE de chaque capture :
#               pixels magenta (#FF00FF, fuzz 25 %, SC2_INSTANCES_DIAG / SC2_VK_PS_DIAG) : couverture, et zone = boîte de
#               leur plus grande composante connexe (élargie de 8 px).
# Run : environnement de campagne-banc.sh (SC2_ROOT, SC2_EXE, SC2_INSTALL, polices du dépôt, SC2_RUN_SANS_BNET=1,
#   SC2_MANQUE_CONTINUE=1, SC2_SANS_VIDEOS=1, SC2_ARGS=-run Maps/Campaign/<carte>.SC2Map, SC2_BANC_ENTREES=@<scénario>,
#   SC2_SPIRV_PACK / SC2_DOCUMENTS transmis s'ils sont posés) avec SC2_FRAMES=0 (seules les CAPTURE écrivent) et un
#   cache de pipelines Vulkan par étiquette HORS de sc2-c (<sortie>/vk-pipelines-<étiq>.bin). Garde-fou : kill après
#   CAPTURE_AB_GARDE s (défaut 240). Verrou banc tenu pendant TOUTE la série (lib-banc.sh) ; garde-disque 2 Gio.
# Dépouillement (ImageMagick 7) :
#   - AE = pixels hors tolérance (magick compare -metric AE -fuzz CAPTURE_AB_FUZZ, défaut 6 %) dans la zone (sinon
#     l'image entière), et aussi dans TOUT le masque magenta de D (« AE masque ») ;
#   - bruit = A1/A2 et B1/B2 (même réglage, instants voisins) ; effet = Ai/Bi ; rapport = médiane(effet) / médiane(bruit) ;
#   - saturation moyenne de la zone (canal S de HSL) en A1 et B1 ;
#   - images/s : plateau de CAPTURE_AB_DEBUT (défaut 60 s) à CAPTURE_AB_FIN (défaut 98 s), en tours de pompe par seconde
#     sur les lignes « [INPUT] banc t=…s tour N » (un tour = une frame présentée) ; plus les compteurs [BILAN] [INST].
# Sortie : <sortie>/<étiq><i>/ (run.log, resume.txt, empreintes.txt, frames/*.png sans perte), <sortie>/masques/,
#   <sortie>/capture-ab.md (imprimé). Images du jeu : hors git, toujours.
# CAPTURE_AB_DEPOUILLER=1 : aucun run (ni verrou), dépouillement des dossiers déjà là.
# Code : 0 si toutes les paires ont tourné et ont été comparées, 1 sinon, 2 usage, 3 garde-disque.
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"
usage() { sed -n '4,6p' "$0" | sed 's/^# *//' >&2; exit 2; }
[ $# -ge 7 ] || usage
ORIG=("$@")
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); S=$2; CARTE=$3; SCEN=$4; CAPS=$5; shift 5
N=2
if [ "$1" != -- ]; then N=$1; shift; fi
[ "${1:-}" = -- ] || usage
shift
case $N in ''|*[!0-9]*|0) echo "capture-ab : n = « $N » : entier ≥ 1 attendu" >&2; exit 2 ;; esac
[ $# -ge 2 ] && [ $# -le 3 ] || usage
[ -x "$BIN" ] || { echo "capture-ab : binaire absent : $BIN" >&2; exit 2; }
[ -f "$SCEN" ] || { echo "capture-ab : scénario absent : $SCEN" >&2; exit 2; }
SCEN=$(cd "$(dirname "$SCEN")" && pwd)/$(basename "$SCEN")
if [ "$CAPS" = scenario ]; then              # noms des CAPTURE: du scénario (lignes de commentaire exclues), sans doublon
    NOMS=$(grep -v '^[[:space:]]*#' "$SCEN" | grep -oE 'CAPTURE:[A-Za-z0-9_.-]+' | cut -d: -f2 | awk '!v[$0]++')
    [ -n "$NOMS" ] || { echo "capture-ab : captures « scenario » : aucune action CAPTURE:<nom> dans $SCEN" >&2; exit 2; }
    TEST=""; DESC="CAPTURE: du scénario ($(echo $NOMS | tr ' ' ','))"
else
    [[ $CAPS =~ ^[0-9]+:[A-Za-z0-9_.-]+(,[0-9]+:[A-Za-z0-9_.-]+)*$ ]] || { echo "capture-ab : captures « $CAPS » : « scenario » ou <frame>:<nom>,… attendu" >&2; exit 2; }
    NOMS=$(echo "$CAPS" | tr ',' '\n' | cut -d: -f2); TEST=$CAPS; DESC="SC2_CAPTURE_TEST=$CAPS"
fi
ETIQ=(); VARS=()
for a in "$@"; do
    e=${a%%:*}; v=${a#*:}
    [[ $a == *:* ]] && [[ $e =~ ^[A-Za-z][A-Za-z0-9_-]*$ ]] || { echo "capture-ab : « $a » : <étiquette>:<VAR=val,…> attendu" >&2; exit 2; }
    [ "$v" = - ] && v=""
    [ -z "$v" ] || [[ $v =~ ^SC2_[A-Z0-9_]+=[^,]*(,SC2_[A-Z0-9_]+=[^,]*)*$ ]] || { echo "capture-ab : variables « $v » : SC2_X=val,… attendu" >&2; exit 2; }
    ETIQ+=("$e"); VARS+=("$v")
done
command -v magick >/dev/null 2>&1 || { echo "capture-ab : ImageMagick 7 (magick) requis" >&2; exit 2; }
. "$R/scripts/lib-banc.sh"
if [ "${CAPTURE_AB_DEPOUILLER:-0}" != 1 ]; then     # dépouillement seul : ni run, ni verrou, ni garde-disque
    banc_garde_disque "$S" 2 || exit 3
    banc_verrou "$0" "${ORIG[@]}"
fi
# ---- verrou banc tenu ----
X=${SC2_EXTRACTION:-~/sc2-partout/extraction}
[ -f "$X/install/Versions/Base15405/SC2.exe" ] || { echo "données du jeu absentes : $X"; exit 1; }
mkdir -p "$S"; S=$(cd "$S" && pwd)
GARDE=${CAPTURE_AB_GARDE:-240}; FUZZ=${CAPTURE_AB_FUZZ:-6%}; DEBUT=${CAPTURE_AB_DEBUT:-60}; FIN=${CAPTURE_AB_FIN:-98}
KO=0
run() {   # <étiquette> <i> <variables>
    local D="$S/$1$2" t=$SECONDS P n=0 RC
    banc_garde_disque "$S" 2 || { echo "capture-ab : garde-disque avant $1$2" >&2; KO=1; return 1; }
    rm -rf "$D"; mkdir -p "$D/frames"
    local COMMUN=(SC2_ROOT="$D/sc2-c" SC2_EXE="$X/install/Versions/Base15405/SC2.exe" SC2_INSTALL="$X/install"
                  SC2_FRAMES=0 SC2_FRAMES_DIR="$D/frames"
                  SC2_POLICES="${SC2_POLICES:-$R/android/assets/polices}" SC2_VK_CACHE_PIPELINES="$S/vk-pipelines-$1.bin")
    [ -n "$TEST" ] && COMMUN+=("SC2_CAPTURE_TEST=$TEST")
    [ "${SC2_SPIRV_PACK:-}" ] && COMMUN+=("SC2_SPIRV_PACK=$SC2_SPIRV_PACK")
    [ "${SC2_DOCUMENTS:-}" ] && COMMUN+=("SC2_DOCUMENTS=$SC2_DOCUMENTS")
    local PROPRES=()
    [ -n "$3" ] && IFS=, read -r -a PROPRES <<< "$3"
    echo "=== $1$2 ($(date +%H:%M:%S)) : ${3:-(aucune variable)} ==="
    env "${COMMUN[@]}" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 SC2_SANS_VIDEOS=1 \
        SC2_ARGS="-run Maps/Campaign/$CARTE.SC2Map" SC2_BANC_ENTREES="@$SCEN" ${PROPRES[@]+"${PROPRES[@]}"} \
        "$BIN" > "$D/run.log" 2>&1 &
    P=$!
    while kill -0 "$P" 2>/dev/null && [ "$n" -lt "$GARDE" ]; do sleep 1; n=$((n + 1)); done
    if kill -0 "$P" 2>/dev/null; then
        echo "garde-fou : $1$2 arrêté après $GARDE s" >&2
        kill -TERM "$P" 2>/dev/null; sleep 2; kill -KILL "$P" 2>/dev/null
    fi
    wait "$P"; RC=$?
    echo "rc=$RC en $((SECONDS - t)) s" > "$D/resume.txt"
    banc_compacter "$D" >> "$D/resume.txt"
    rm -rf -- "${D:?}/sc2-c"
    [ "$RC" = 0 ] || KO=1
    cat "$D/resume.txt"
}
# ordre : D (diagnostic + chauffe) d'abord, puis A1 B1 A2 B2… ; CAPTURE_AB_DEPOUILLER=1 : aucun run, dépouillement seul
premier() {   # <dossier du premier run> : au moins une des captures écrite (PNG compacté ou PPM), sinon série arrêtée
    local c
    for c in $NOMS; do [ -f "$1/frames/$c.png" ] || [ -f "$1/frames/$c.ppm" ] && return 0; done
    echo "capture-ab : $(basename "$1") n'a écrit aucune capture ($(echo $NOMS)) : série arrêtée (déclencheur du scénario, frames ?)" >&2
    KO=1; return 1
}
if [ "${CAPTURE_AB_DEPOUILLER:-0}" != 1 ]; then
    ok=1
    if [ ${#ETIQ[@]} = 3 ]; then run "${ETIQ[2]}" 1 "${VARS[2]}"; premier "$S/${ETIQ[2]}1" || ok=0; fi
    if [ "$ok" = 1 ]; then
        for i in $(seq 1 "$N"); do
            run "${ETIQ[0]}" "$i" "${VARS[0]}"
            if [ "$i" = 1 ] && [ ${#ETIQ[@]} = 2 ]; then premier "$S/${ETIQ[0]}1" || break; fi
            run "${ETIQ[1]}" "$i" "${VARS[1]}"
        done
    fi
fi

# ---- dépouillement ----------------------------------------------------------------------------------------------
M="$S/masques"; mkdir -p "$M"
ae() {   # <a.png> <b.png> [géométrie] → pixels hors tolérance (« ? » si illisible)
    local g=${3:-} v
    v=$(magick compare -metric AE -fuzz "$FUZZ" "$1${g:+[$g]}" "$2${g:+[$g]}" null: 2>&1 | awk '{ print $1 }')
    [[ $v =~ ^[0-9.e+]+$ ]] && printf '%.0f' "$v" || echo "?"
}
ae_masque() {   # <a.png> <b.png> <masque.png> → pixels du masque où un canal s'écarte de plus de √2 × fuzz × 255
    local s; s=$(awk -v f="${FUZZ%\%}" 'BEGIN { printf "%.4f", f * 1.41421356 }')
    magick "$1" "$2" -compose difference -composite -separate -evaluate-sequence max -threshold "$s%" \
           "$3" -compose multiply -composite -format '%[fx:round(mean*w*h)]' info: 2>/dev/null || echo "?"
}
sat() {   # <png> [géométrie] → saturation moyenne (HSL)
    magick "$1${2:+[$2]}" -colorspace HSL -channel G -separate -format '%[fx:mean]' info: 2>/dev/null | cut -c1-6
}
mediane() { tr ' ' '\n' | grep -E '^[0-9]+$' | sort -n | awk '{ v[NR] = $1 } END { if (!NR) print "?"; else if (NR % 2) print v[(NR + 1) / 2]; else print (v[NR / 2] + v[NR / 2 + 1]) / 2 }'; }
A=${ETIQ[0]}; B=${ETIQ[1]}; DG=${ETIQ[2]:-}
{
echo "# CAPTURE A/B — $CARTE, $(date '+%d/%m/%Y %H:%M')"
echo
echo "Binaire \`$BIN\` ; scénario \`$(basename "$SCEN")\` ; captures \`$DESC\` ; AE -fuzz $FUZZ ; $A = \`${VARS[0]:-—}\`, $B = \`${VARS[1]:-—}\`${DG:+, $DG = \`${VARS[2]:-—}\` (zone)}."
echo
echo "| capture | zone (l×h+x+y) | couverture $DG | AE bruit $A | AE bruit $B | AE effet (par paire) | rapport effet/bruit | effet % zone | AE masque bruit/effet | saturation ${A}1/${B}1 |"
echo "|---|---|---:|---:|---:|---|---:|---:|---|---|"
for c in $NOMS; do
    zone=""; cov="—"; mq=""
    if [ -n "$DG" ] && [ -f "$S/${DG}1/frames/$c.png" ]; then
        magick "$S/${DG}1/frames/$c.png" -fuzz 25% -fill white -opaque '#FF00FF' -fill black +opaque white -colorspace Gray "$M/$c.png"
        cov=$(magick "$M/$c.png" -format '%[fx:mean*100]' info: | awk '{ printf "%.2f %%", $1 }')
        # zone = boîte de la PLUS GRANDE composante connexe du masque (8-connexité, ≥ 50 px), élargie de 8 px : les
        # pixels magenta épars (icônes de la minicarte…) ne l'étirent pas à toute l'image
        zone=$(magick "$M/$c.png" -define connected-components:verbose=true -define connected-components:area-threshold=50 \
                      -connected-components 8 null: 2>/dev/null \
               | awk '$NF ~ /^gray\(/ { g = $NF; gsub(/[^0-9.]/, "", g); if (g + 0 >= 128 && $4 + 0 > best) { best = $4 + 0; b = $2 } }
                      END { if (best) print b }' \
               | awk -F'[x+]' 'NF == 4 { x = $3 - 8; y = $4 - 8; if (x < 0) x = 0; if (y < 0) y = 0
                                         printf "%dx%d+%d+%d", $1 + ($3 - x) + 8, $2 + ($4 - y) + 8, x, y }')
        [[ $zone =~ ^[0-9]+x[0-9]+\+[0-9]+\+[0-9]+$ ]] || zone=""
        [ -n "$zone" ] && mq="$M/$c.png"
    fi
    bA=""; bB=""; ef=""; mqb=""; mqe=""
    for i in $(seq 1 "$N"); do
        fa="$S/$A$i/frames/$c.png"; fb="$S/$B$i/frames/$c.png"
        [ -f "$fa" ] && [ -f "$fb" ] || { KO=1; continue; }
        ef="$ef $(ae "$fa" "$fb" "$zone")"
        [ -n "$mq" ] && mqe="$mqe $(ae_masque "$fa" "$fb" "$mq")"
        if [ "$i" -gt 1 ]; then
            p=$((i - 1))
            bA="$bA $(ae "$S/$A$p/frames/$c.png" "$fa" "$zone")"; bB="$bB $(ae "$S/$B$p/frames/$c.png" "$fb" "$zone")"
            [ -n "$mq" ] && mqb="$mqb $(ae_masque "$S/$A$p/frames/$c.png" "$fa" "$mq") $(ae_masque "$S/$B$p/frames/$c.png" "$fb" "$mq")"
        fi
    done
    me=$(echo "$ef" | mediane); mb=$(echo "$bA $bB" | mediane)
    rap=$(awk -v e="$me" -v b="$mb" 'BEGIN { if (e == "?" || b == "?") print "?"; else if (b == 0) print (e > 0 ? "∞" : "—"); else printf "%.1f", e / b }')
    if [ -n "$zone" ]; then pz=$(( ${zone%%x*} * $(echo "$zone" | sed -E 's/^[0-9]+x([0-9]+)\+.*/\1/') ))
    else pz=$(magick identify -format '%[fx:w*h]' "$S/${A}1/frames/$c.png" 2>/dev/null || echo 0); fi
    pct=$(awk -v e="$me" -v z="$pz" 'BEGIN { if (e == "?" || !z) print "?"; else printf "%.1f %%", 100 * e / z }')
    sa=$(sat "$S/${A}1/frames/$c.png" "$zone"); sb=$(sat "$S/${B}1/frames/$c.png" "$zone")
    echo "| $c | ${zone:-image entière} | $cov |${bA:- —} |${bB:- —} |$ef | $rap | $pct | ${mq:+$(echo "$mqb" | mediane) / $(echo "$mqe" | mediane)} | $sa / $sb |"
done
echo
echo "Images/s (banc M4) : plateau de $DEBUT à $FIN s, tours de pompe par seconde ; compteurs [BILAN] [INST] (D3D9 puis backend)."
echo
echo "| run | rc | frames | images/s plateau | draws réels / lumière | [INST] D3D9 | [INST] backend |"
echo "|---|---|---:|---:|---|---|---|"
for d in "$S"/*/; do
    d=${d%/}; [ -f "$d/run.log" ] || continue
    L="$d/run.log"
    fps=$(grep -a -E '^\[INPUT\] banc t=[0-9.]+s tour [0-9]+' "$L" | sed -E 's/^\[INPUT\] banc t=([0-9.]+)s tour ([0-9]+).*/\1 \2/' \
          | awk -v a="$DEBUT" -v b="$FIN" '$1 >= a && $1 <= b { if (!n++) { t0 = $1; f0 = $2 } t1 = $1; f1 = $2 }
                                          END { if (n > 1 && t1 > t0) printf "%.1f", (f1 - f0) / (t1 - t0); else print "?" }')
    fr=$(grep -a -m1 -oE 'frames présentées \(Present\) : [0-9]+' "$L" | grep -oE '[0-9]+$')
    dr=$(grep -a -m1 -E '^\[BILAN\] draws : ' "$L" | sed -E 's/.*: ([0-9]+) RÉELS.*, ([0-9]+) « lumière ».*/\1 \/ \2/')
    i1=$(grep -a -m1 -E '^\[BILAN\] \[INST\] V2\.2 : ' "$L" | sed -E 's/^\[BILAN\] \[INST\] V2\.2 : //; s/ ; replis.*//' | cut -c1-120)
    i2=$(grep -a -m1 -E '^\[BILAN\] \[INST\] backend' "$L" | sed -E 's/^.*backend V2\.2 : //' | cut -c1-90)
    echo "| $(basename "$d") | $(head -1 "$d/resume.txt" 2>/dev/null | cut -d' ' -f1) | ${fr:-?} | $fps | ${dr:-?} | ${i1:-—} | ${i2:-—} |"
done
} | tee "$S/capture-ab.md"
exit "$KO"
