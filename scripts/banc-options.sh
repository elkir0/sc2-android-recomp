#!/bin/bash
# banc-options.sh — V1.5 (22/10) : matrice des préréglages graphiques au banc Mac, canon épinglé, contrôle des préréglages.
#   scripts/banc-options.sh <binaire> <sortie> [préréglage…]   matrice (défaut : low medium high ultra), un run par
#       préréglage (scripts/prereglages/<nom>.txt, ou chemin d'un fichier) sur TRaynor01 : Variables.txt semé et FIGÉ
#       (SC2_VARIABLES + SC2_VARIABLES_FIGE=1), scénario scripts/entrees/options-accept.txt (mesure, puis changement neutre
#       accepté dans le menu Options : le jeu RÉÉCRIT alors Variables.txt avec ses réglages en mémoire), puis
#       <sortie>/rapport.md, une ligne par préréglage :
#         rc ; application = clés du préréglage retrouvées à l'identique dans le Variables.txt réécrit par le jeu
#         (TextureQuality et texQualityLevel cités) ; images/s en jeu (tours de pompe entre les deux témoins
#         « MOVE (953, 588) » du scénario, 56 et 70 s : aide de mission fermée, transmission finie ; un Present par tour)
#         et captures PPM par seconde dans cette fenêtre (chaque capture vide la file GPU : biais ∝ images/s) ;
#         scène entre les témoins, vérifiée sur les images capturées : « nette » sans aide ouverte (bouton « Close »
#         60×16+923+580 en surbrillance) ni cadre de portrait, sinon AIDE k/n, PORTRAIT k/n ; « lumière » ; replis de
#         texture ; Mo de textures téléversés ; pipelines créés ; portrait de transmission, mesuré sur les SEULES images
#         où son cadre est présent (liseré gauche 4×180+6+244 : luminosité 0,22-0,40, écart-type < 0,10), à tout
#         instant du run : nombre d'images, puis la MÉDIANE de la saturation de la zone 120×190+26+236 (image gardée
#         sans perte en frames/portrait-<nom>.png) avec sa luminosité, moyennes HSL (magick … -colorspace HSL) —
#         portrait gris ⇔ saturation ≈ 0, luminosité ≈ 0,76 ; « portrait absent » si aucune image n'a le cadre.
#       Code 0 si chaque run a rc=0, 0 « lumière », toutes les clés appliquées et une scène nette ; 1 sinon.
#   scripts/banc-options.sh --canon <binaire> <sortie>   canon ÉPINGLÉ : scripts/banc-mac.sh defaut avec Variables.txt
#       figé (BANC_VARIABLES=scripts/prereglages/canon.txt, réglages que le jeu détecte et écrit lui-même pour la
#       GeForce 8600 GT annoncée) et SC2_GPU_PROFIL=historique (sans effet tant que le runtime ne lit pas la variable) ;
#       empreintes.txt comparé au canon : image 1 be01318cdf69, image 10 470ebae2b462 (variante tolérée b5f7db71833c),
#       images 100-300 ae1ff947a8df. Code 0 si identique.
#   scripts/banc-options.sh --analyser <dossier d'un run> [nom]   hors banc : refait les mesures d'images (portrait, scène
#       entre les témoins) d'un run de la matrice, sur ses PPM ou, après compactage, ses vignettes PNG ; n'écrit rien.
#   scripts/banc-options.sh --controle   hors banc : préréglages bien formés (clé=valeur par ligne, 1 o à 64 Kio,
#       « GraphicsOption », aucune clé de son, de langue, d'affichage ni de compte) ; VARS_FOLD d'android/jni/
#       sc2_android_main.c identique à scripts/prereglages/fold.txt.
# Variables : SC2_EXTRACTION (données du jeu, défaut <clone principal>/extraction), BANC_OPTIONS_CARTE (TRaynor01),
#   BANC_OPTIONS_SCENARIO (scripts/entrees/options-accept.txt), BANC_OPTIONS_PAS (SC2_FRAMES_PAS, défaut 100 : une
#   capture toutes les 100 frames, ≥ 4 pendant la transmission du portrait même en Ultra, ≈ 2/s en Low),
#   BANC_OPTIONS_CACHE (cache de pipelines partagé par les runs, défaut <parent de la sortie>/vk-pipelines-options.bin :
#   chaud dès la 2e matrice — le premier run d'une matrice froide compile ses pipelines pendant la mesure ; la colonne
#   « pipelines » le montre). SC2_GPU_PROFIL, SC2_SPIRV_PACK… de l'appelant passent au jeu tels quels.
# Garde-disque et verrou banc : scripts/lib-banc.sh (≈ 0,3 Go de PPM par run avant compactage).
set -u
export LC_ALL=C
R="$(cd "$(dirname "$0")/.." && pwd)"
PRE="$R/scripts/prereglages"
FOLD_C="$R/android/jni/sc2_android_main.c"
# clés hors réglages graphiques (son, langue, affichage, compte, appareil) : interdites dans les préréglages, sauf canon.txt
# (copie exacte de ce que le jeu écrit seul, lastDeviceId compris)
INTERDITES='^(sound|Sound|locale|width|height|windowwidth|windowheight|windowx|windowy|displaymode|refreshrate|gamma|vsync|lastDeviceId|account|Account|battlenet|MoviesSeen|MusicHeard)'

controle() {
    local f n=0 ko=0 c k
    for f in "$PRE"/*.txt; do
        n=$((n + 1)); k=$(basename "$f" .txt)
        c=$(wc -c < "$f" | tr -d ' ')
        if [ "$c" -lt 1 ] || [ "$c" -gt 65536 ]; then echo "[ÉCHEC] $k : $c o (1 o à 64 Kio)"; ko=1; continue; fi
        grep -q GraphicsOption "$f" || { echo "[ÉCHEC] $k : sans « GraphicsOption »"; ko=1; }
        # fins de ligne CRLF, comme les écrit le jeu
        tr -d '\r' < "$f" | grep -vnE '^[A-Za-z][A-Za-z0-9_]*(\[[0-9]+\])?=[-0-9.A-Za-z]+$' | head -3 \
            | sed "s/^/[ÉCHEC] $k : ligne mal formée /" | grep . && ko=1
        if [ "$k" != canon ] && grep -nE "$INTERDITES" "$f" | head -3 | sed "s/^/[ÉCHEC] $k : clé hors réglages graphiques /" | grep .; then
            ko=1
        fi
    done
    if [ -f "$FOLD_C" ] && [ -f "$PRE/fold.txt" ]; then
        if python3 -I - "$FOLD_C" "$PRE/fold.txt" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"/\* VARS_FOLD début[^*]*\*/(.*?)/\* VARS_FOLD fin \*/", src, re.S)
if not m: sys.exit(2)
lits = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
txt = "".join(lits).encode().decode("unicode_escape")
sys.exit(0 if txt == open(sys.argv[2], encoding="utf-8", newline="").read() else 1)
PY
        then echo "[OK]    VARS_FOLD (sc2_android_main.c) identique à scripts/prereglages/fold.txt"
        else echo "[ÉCHEC] VARS_FOLD (sc2_android_main.c) différent de scripts/prereglages/fold.txt"; ko=1; fi
    fi
    [ $ko = 0 ] && echo "[OK]    $n préréglages bien formés ($(cd "$PRE" && ls *.txt | tr '\n' ' '))"
    return $ko
}

# empreintes.txt du canon : ligne « frame … empreinte » → OK si 1, 10, 100, 200, 300 portent les empreintes du canon
comparer_canon() {
    awk 'BEGIN { a[1] = "be01318cdf69"; a[10] = "470ebae2b462|b5f7db71833c"
                 a[100] = a[200] = a[300] = "ae1ff947a8df"; k = 0 }
         $1 ~ /^[0-9]+$/ && ($1 in a) { v[$1] = $NF }
         END { for (f in a) { k++; if (!(f in v) || v[f] !~ "^(" a[f] ")$") { printf "image %d : %s, attendu %s\n", f, \
                   (f in v) ? v[f] : "absente", a[f]; bad++ } }
               if (bad) exit 1; print "images 1, 10, 100, 200, 300 identiques au canon (be01318, " v[10] ", ae1ff947)" }' "$1"
}

# Mesures par capture frame_<k> (.ppm du run, ou .png des vignettes après compactage, ramenées au client 1280×720),
# HSL, une ligne « k bL bE zS zL cS cL » : liseré gauche du cadre du portrait (4×180+6+244 : luminosité, écart-type),
# zone du portrait (120×190+26+236 : saturation, luminosité), bouton « Close » de l'aide (60×16+923+580 : saturation,
# luminosité). Seuils tirés de la matrice du 22/10 (vignettes et PNG sans perte) : cadre présent 0,29-0,31 (écart-type
# 0,04-0,06) ; terrain 0,10-0,17, cinématique ≈ 0,27 mais écart-type 0,14-0,16, aide ou menu ≤ 0,035 ; « Close »
# survolé de l'aide ouverte : saturation 0,56-0,58, luminosité 0,38 (cinématique ≤ 0,45 et 0,28, console 0,05-0,15).
mesurer_images() {   # <dossier des images>
    local fr k
    for fr in "$1"/frame_[0-9]*.ppm "$1"/frame_[0-9]*.png; do
        [ -f "$fr" ] || continue
        k=${fr##*/frame_}; k=$((10#${k%.*}))
        magick "$fr" -scale '1280x720!' -colorspace HSL \
            \( -clone 0 -crop 4x180+6+244 +repage \) \( -clone 0 -crop 120x190+26+236 +repage \) \
            \( -clone 0 -crop 60x16+923+580 +repage \) -delete 0 \
            -format '%[fx:mean.g] %[fx:mean.b] %[fx:standard_deviation.b] ' info: 2>/dev/null \
            | awk -v k="$k" 'NF == 9 { print k, $2, $3, $4, $5, $7, $8 }'
    done | sort -n -k1,1
}

# <mesures> <n1> <n2> → « nc img sat lum premiere derniere m aide cadre » : nc images avec le cadre du portrait (de
# premiere à derniere), médiane de leur saturation (image img, sat, lum) ; m images entre les tours n1 et n2 (témoins),
# dont « aide » avec l'aide ouverte et « cadre » avec le cadre du portrait (n1 vide : m = 0).
analyser_images() {
    awk -v lo="$2" -v hi="$3" '
        { cadre = ($2 >= 0.22 && $2 <= 0.40 && $3 < 0.10); aide = ($6 > 0.45 && $7 > 0.30)
          if (cadre) { nc++; ks[nc] = $1; s[nc] = $4; l[nc] = $5; if (nc == 1) p = $1; d = $1 }
          if (lo != "" && $1 >= lo + 0 && $1 <= hi + 0) { m++; a += aide; c += cadre } }
        END { img = "-"; sat = "-"; lum = "-"
              for (i = 2; i <= nc; i++)                        # tri par saturation (insertion : quelques dizaines)
                  for (j = i; j > 1 && s[j] < s[j - 1]; j--) {
                      t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; t = l[j]; l[j] = l[j - 1]; l[j - 1] = t
                      t = ks[j]; ks[j] = ks[j - 1]; ks[j - 1] = t }
              if (nc) { i = int((nc + 1) / 2); img = ks[i]; sat = sprintf("%.4f", s[i]); lum = sprintf("%.3f", l[i]) }
              printf "%d %s %s %s %s %s %d %d %d\n", nc, img, sat, lum, nc ? p : "-", nc ? d : "-", m + 0, a + 0, c + 0 }' "$1"
}

# verdict de scène : <m> <aide> <cadre> → « nette (m images) » / « AIDE a/m PORTRAIT c/m » / « non vérifiée »
verdict_scene() {
    if [ "$1" = 0 ]; then echo "non vérifiée (aucune image entre les témoins)"
    elif [ "$2" = 0 ] && [ "$3" = 0 ]; then echo "nette ($1 images)"
    else echo "$([ "$2" != 0 ] && echo "AIDE $2/$1 ")$([ "$3" != 0 ] && echo "PORTRAIT $3/$1")" | sed 's/ $//'; fi
}

# témoins : les deux premiers « MOVE (953, 588) » du journal → « t1 n1 t2 n2 »
temoins() {
    grep -a -E '^\[INPUT\] banc t=[0-9.]+s tour [0-9]+ : MOVE \(953, 588\)' "$1" \
        | sed -E 's/.*t=([0-9.]+)s tour ([0-9]+) .*/\1 \2/' | head -2 | tr '\n' ' '
}

case "${1:-}" in
--controle) controle; exit $? ;;
--analyser)
    D=${2:?dossier d\'un run (run.log, frames/)}; N=${3:-$(basename "$D")}
    command -v magick > /dev/null || { echo "magick absent"; exit 1; }
    read -r t1 n1 t2 n2 <<< "$(temoins "$D/run.log")"
    mesurer_images "$D/frames" > "${TMPDIR:-/tmp}/banc-options-$$.txt"
    read -r nc img sat lumi p1 p2 m na ncf <<< "$(analyser_images "${TMPDIR:-/tmp}/banc-options-$$.txt" "${n1:-}" "${n2:-}")"
    rm -f "${TMPDIR:-/tmp}/banc-options-$$.txt"
    echo "$N : témoins tours ${n1:-?}→${n2:-?} (t=${t1:-?}s → ${t2:-?}s) ; scène entre les témoins : $(verdict_scene "$m" "$na" "$ncf")"
    if [ "$nc" = 0 ]; then echo "$N : portrait absent (aucune image avec le cadre)"
    else echo "$N : portrait sur $nc images (cadre présent, $p1 à $p2), médiane : image $img, saturation $sat, luminosité $lumi"; fi
    exit 0 ;;
''|-h|--help) awk 'NR > 1 && !/^#/ { exit } NR > 1' "$0" | sed 's/^# \{0,1\}//'; exit 64 ;;
esac
. "$R/scripts/lib-banc.sh"
case "${1:-}" in --canon) S0=${3:-} ;; *) S0=${2:-} ;; esac
[ -n "$S0" ] || { echo "sortie manquante (scripts/banc-options.sh -h)"; exit 64; }
banc_garde_disque "$S0" 2 || exit 3
banc_verrou "$0" "$@"

if [ "$1" = --canon ]; then
    BIN=${2:?binaire}; S=$3
    BANC_VARIABLES="$PRE/canon.txt" SC2_GPU_PROFIL="${SC2_GPU_PROFIL:-historique}" "$R/scripts/banc-mac.sh" defaut "$BIN" "$S"
    rc=$?
    if [ "$rc" = 0 ] && comparer_canon "$S/empreintes.txt" > "$S/canon-epingle.txt"; then
        echo "canon épinglé : IDENTIQUE au canon — $(cat "$S/canon-epingle.txt")" | tee -a "$S/resume.txt"; exit 0
    fi
    echo "canon épinglé : DIFFÉRENT du canon (rc=$rc) — $(tr '\n' ' ' < "$S/canon-epingle.txt" 2>/dev/null)" | tee -a "$S/resume.txt"
    exit 1
fi

BIN=$(cd "$(dirname "${1:?binaire}")" && pwd)/$(basename "$1"); S=$2; shift 2
[ $# -gt 0 ] || set -- low medium high ultra
X=${SC2_EXTRACTION:-~/sc2-partout/extraction}
[ -f "$X/install/Versions/Base15405/SC2.exe" ] || { echo "données du jeu absentes : $X"; exit 1; }
CARTE=${BANC_OPTIONS_CARTE:-TRaynor01}
SCEN=${BANC_OPTIONS_SCENARIO:-$R/scripts/entrees/options-accept.txt}
mkdir -p "$S"; S=$(cd "$S" && pwd)
CACHE=${BANC_OPTIONS_CACHE:-$(dirname "$S")/vk-pipelines-options.bin}
VARS="Users/sc2/Documents/StarCraft II/Variables.txt"
command -v magick > /dev/null || echo "magick absent : ni portrait ni vérification de la scène" >&2

# application : chaque clé=valeur du préréglage retrouvée telle quelle dans le Variables.txt relu → « n/N » + écarts
appliquer() {   # <préréglage> <relu>
    awk -F= '{ sub(/\r$/, "") }
             NR == FNR { if ($0 ~ /^[A-Za-z]/) { k = $1; v = substr($0, length(k) + 2); p[k] = v; ordre[++n] = k }; next }
             { r[$1] = substr($0, length($1) + 2) }
             END { for (i = 1; i <= n; i++) { k = ordre[i]; if ((k in r) && r[k] == p[k]) ok++
                       else ecarts = ecarts sprintf(" %s=%s→%s", k, p[k], (k in r) ? r[k] : "absente") }
                   printf "%d/%d%s\n", ok, n, ecarts }' "$1" "$2"
}

LIGNES=""; ECHEC=0
for P in "$@"; do
    if [ -f "$PRE/$P.txt" ]; then F="$PRE/$P.txt"; N=$P
    elif [ -f "$P" ]; then F=$(cd "$(dirname "$P")" && pwd)/$(basename "$P"); N=$(basename "$P" .txt)
    else echo "préréglage introuvable : $P (ni $PRE/$P.txt ni un fichier)"; ECHEC=1; continue; fi
    banc_garde_disque "$S" 1 || { ECHEC=1; break; }
    D="$S/$N"; rm -rf "$D"; mkdir -p "$D"
    echo "== $N : $F" >&2
    t=$SECONDS
    ( cd "$D" && env SC2_ROOT="$D/sc2-c" SC2_EXE="$X/install/Versions/Base15405/SC2.exe" SC2_INSTALL="$X/install" \
        SC2_FRAMES=1 SC2_FRAMES_PAS="${BANC_OPTIONS_PAS:-100}" SC2_FRAMES_DIR="$D/frames" \
        SC2_POLICES="${SC2_POLICES:-$R/android/assets/polices}" SC2_VK_CACHE_PIPELINES="$CACHE" \
        SC2_VARIABLES="$F" SC2_VARIABLES_FIGE=1 SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 SC2_SANS_VIDEOS=1 \
        SC2_ARGS="-run Maps/Campaign/$CARTE.SC2Map" SC2_BANC_ENTREES="@$SCEN" "$BIN" > "$D/run.log" 2>&1 )
    rc=$?
    L="$D/run.log"
    echo "rc=$rc en $((SECONDS - t)) s — $N ($F)" > "$D/resume.txt"
    cp "$D/sc2-c/$VARS" "$D/Variables-relu.txt" 2>/dev/null || : > "$D/Variables-relu.txt"
    rm -rf "$D/sc2-c"
    reecrit=non; cmp -s "$F" "$D/Variables-relu.txt" || reecrit=oui
    [ -s "$D/Variables-relu.txt" ] || reecrit="absent"
    app=$(appliquer "$F" "$D/Variables-relu.txt")
    tq=$(grep -E '^(GraphicsOptionTextureQuality|texQualityLevel)' "$D/Variables-relu.txt" | tr -d '\r' | tr '\n' ' ')
    present=$(grep -a -m1 -oE 'frames présentées \(Present\) : [0-9]+' "$L" | grep -oE '[0-9]+$')
    lum=$(grep -a -m1 -E '^\[BILAN\] draws : [0-9]+ RÉELS' "$L" | sed -E 's/.*shaders traduits\), ([0-9]+) .*/\1/')
    rep=$(grep -a -m1 'replis sur texture factice' "$L" | sed -E 's/.*factice : ([0-9]+) [^,]*, ([0-9]+) [^,]*, ([0-9]+) [^,]*, ([0-9]+) .*/\1 \2 \3 \4/' \
          | awk 'NF == 4 { print $1 + $2 + $3 + $4 }')
    tex=$(grep -a -m1 -E 'textures : [0-9]+, [0-9]+ téléversements \(' "$L" | sed -E 's/.*téléversements \(([0-9.]+) Mo\).*/\1/')
    pip=$(grep -a -m1 'PIPECACHE' "$L" | sed -nE 's/.* ([0-9]+) pipelines créés en ([0-9.]+) ms.*/\1 en \2 ms/p')
    # témoins : les deux premiers « MOVE (953, 588) » du scénario → images/s en jeu
    read -r t1 n1 t2 n2 <<< "$(temoins "$L")"
    ips=$(awk -v a="${t1:-0}" -v b="${n1:-0}" -v c="${t2:-0}" -v d="${n2:-0}" 'BEGIN { if (c > a) printf "%.1f", (d - b) / (c - a); else print "?" }')
    # images (PPM, avant compactage) : portrait sur les images où son cadre est présent, scène entre les témoins
    nc=0; img=""; sat="—"; lumi="—"; p1=""; p2=""; scene="—"; cps="?"; ok_scene=1
    if [ -d "$D/frames" ] && command -v magick > /dev/null; then
        mesurer_images "$D/frames" > "$D/images.txt"
        read -r nc img sat lumi p1 p2 m na ncf <<< "$(analyser_images "$D/images.txt" "${n1:-}" "${n2:-}")"
        scene=$(verdict_scene "$m" "$na" "$ncf")
        case $scene in nette*) ;; *) ok_scene=0 ;; esac
        cps=$(awk -v m="$m" -v a="${t1:-0}" -v c="${t2:-0}" 'BEGIN { if (c > a) printf "%.1f", m / (c - a); else print "?" }')
    fi
    if [ "${nc:-0}" != 0 ] && [ -f "$D/frames/$(printf 'frame_%05d.ppm' "$img")" ]; then
        fr="$D/frames/$(printf 'frame_%05d.ppm' "$img")"
        cp "$fr" "$D/frames/portrait-$N.ppm"                           # capture : gardée SANS PERTE par banc_compacter
        magick "$fr" -crop 120x190+26+236 +repage "$D/portrait-zone.png"
        portrait="médiane de $nc images avec le cadre (frames $p1 à $p2) : image $img, saturation $sat, luminosité $lumi"
    elif command -v magick > /dev/null; then
        portrait="portrait absent (aucune image capturée avec le cadre du portrait)"; sat="absent"; lumi="—"
    else portrait="non mesuré (magick absent)"; fi
    {
        echo "Variables.txt réécrit par le jeu : $reecrit ; clés appliquées : $app"
        echo "relu : ${tq:-(ni TextureQuality ni texQualityLevel)}"
        echo "Present $present ; images/s en jeu ${ips} (tours $n1→$n2 entre t=${t1}s et t=${t2}s, $cps captures/s) ;" \
             "scène entre les témoins : $scene ; « lumière » ${lum:-?} ; replis de texture ${rep:-?} ;" \
             "textures ${tex:-?} Mo ; pipelines ${pip:-?}"
        echo "portrait (zone 120×190+26+236) : $portrait"
    } >> "$D/resume.txt"
    banc_compacter "$D" >> "$D/resume.txt"
    grep -a "SIGSEGV\|SIGBUS\|ARRÊT\] faute\|DEVICE_LOST" "$L" | head -3 >> "$D/resume.txt"
    cat "$D/resume.txt" >&2
    napp=${app%% *}
    [ "$rc" = 0 ] && [ "${lum:-1}" = 0 ] && [ "$reecrit" = oui ] && [ "${napp%%/*}" = "${napp##*/}" ] && [ $ok_scene = 1 ] \
        || ECHEC=1
    LIGNES="$LIGNES| $N | $rc | ${napp} | ${tq:-—} | ${ips} | ${cps} | ${scene} | ${lum:-?} | ${rep:-?} | ${tex:-?} | ${pip:-?} | ${nc:-0} | $sat | $lumi |
"
done

{
    echo "# banc-options — $(date '+%d/%m/%Y %H:%M'), $(basename "$BIN") ($(shasum -a 256 "$BIN" | cut -c1-12)), carte $CARTE"
    echo
    echo "Scénario \`$(basename "$SCEN")\`, Variables.txt semé figé, relu après « Accept » (réécrit par le jeu). Images/s : tours de"
    echo "pompe entre les deux témoins « MOVE (953, 588) » (banc Mac M4), scène vérifiée sur les captures de cette fenêtre"
    echo "(« nette » : ni aide ouverte ni portrait). Portrait : zone 120×190+26+236 (HSL, moyennes), médiane des images où son"
    echo "cadre est présent (« absent » sinon)."
    echo
    echo "| préréglage | rc | clés appliquées | relu : TextureQuality, texQualityLevel | images/s | captures/s | scène | « lumière » | replis | textures (Mo) | pipelines | images du portrait | saturation portrait | luminosité portrait |"
    echo "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
    printf '%s' "$LIGNES"
} > "$S/rapport.md"
cat "$S/rapport.md"
exit $ECHEC
