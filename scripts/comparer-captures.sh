#!/bin/bash
# comparer-captures.sh — écart chiffré entre deux séries de captures (V1.4, 22/10) : critères visuels « pas de
# visible sans chiffre » (docs/METHODE.md §4).
#   scripts/comparer-captures.sh <A> <B> [--zone x,y,l,h] [--fuzz 6%] [--sortie <dossier>] [--max-ae N] [--min-ae N]
#   <A>, <B>      deux dossiers (images appariées par NOM : .png, .ppm, .jpg ; une image sans pendant est signalée)
#                 ou deux fichiers
#   --zone        zone nommée x,y,largeur,hauteur, en pixels de A ; appliquée après rééchantillonnage de B
#   --fuzz        tolérance de couleur de l'AE (défaut 6 %)
#   --sortie      dossier des différences (défaut ~/dev/sc2-bancs/<aaaammjj>/comparer-<hhmmss> : images du jeu,
#                 JAMAIS dans git) : diff/<nom>.png (pixels différents en rouge), planche.png (A | B | différence,
#                 8 premières paires, 480 px), comparaison.tsv
#   --max-ae N    code 1 si une paire dépasse N pixels différents ; --min-ae N : code 1 si une paire en a moins de N
#                 (« le rendu doit avoir changé »)
# Par paire : AE (pixels hors tolérance, et en % de l'image ou de la zone), PSNR (dB ; « inf » = identiques, IM
#   plafonne à 120), saturation moyenne de A et de B (canal S de HSL, 0-1). Tailles différentes : AVERTISSEMENT, puis B
#   rééchantillonné à la taille de A (-resize LxH!).
# Outils : ImageMagick 7 (magick compare -metric AE -fuzz / -metric PSNR), vérifié par command -v ; à défaut ffmpeg
#   (décodage en RGB 8 bits) et python3 : AE = pixels dont UN canal R, G ou B s'écarte de plus de √2 × fuzz × 255 —
#   règle d'ImageMagick 7.1.2 (Q16-HDRI) retrouvée le 22/10 : mêmes AE au pixel près sur deux frames du canon à 3, 6,
#   10 et 15 % (la moyenne quadratique des canaux donnait 255 646 au lieu de 195 618 à 6 %) ; PSNR = 10·log10(255² /
#   EQM), identique à celui d'IM. Seul le rééchantillonnage diffère (filtres d'ffmpeg et d'IM). COMPARER_SANS_MAGICK=1
#   force le repli (tests).
# Dernière ligne : « comparer : N/P paire(s) comparée(s), K différente(s), AE max M px ; E en échec ; … » (une paire
#   illisible, à la conversion en échec ou d'AE illisible est EN ÉCHEC : jamais comptée comme identique).
# Code 0, 1 (seuil franchi), 2 (usage, ou au moins une paire en échec : prime sur 1).
# Fichiers de travail : dossier propre <sortie>/.comparer-tmp.XXXXXX (mktemp), seul supprimé à la sortie.
set -u
usage() { sed -n '4p' "$0" | sed 's/^# *//' >&2; echo "(détails : en-tête de $0)" >&2; exit 2; }
[ $# -ge 2 ] || usage
A=$1; B=$2; shift 2
ZONE=""; FUZZ=6%; SORTIE=""; MAXAE=""; MINAE=""
while [ $# -gt 0 ]; do
    case $1 in
    --zone) ZONE=${2:?}; shift ;;
    --fuzz) FUZZ=${2:?}; shift ;;
    --sortie) SORTIE=${2:?}; shift ;;
    --max-ae) MAXAE=${2:?}; shift ;;
    --min-ae) MINAE=${2:?}; shift ;;
    *) echo "comparer : option inconnue « $1 »" >&2; usage ;;
    esac
    shift
done
if [ -n "$ZONE" ]; then
    [[ $ZONE =~ ^[0-9]+,[0-9]+,[0-9]+,[0-9]+$ ]] || { echo "comparer : --zone x,y,l,h attendu (« $ZONE »)" >&2; exit 2; }
    IFS=, read -r ZX ZY ZL ZH <<< "$ZONE"
    [ "$ZL" -gt 0 ] && [ "$ZH" -gt 0 ] || { echo "comparer : zone vide" >&2; exit 2; }
fi
[[ $FUZZ =~ ^[0-9]+(\.[0-9]+)?%?$ ]] || { echo "comparer : --fuzz « $FUZZ » : pourcentage attendu" >&2; exit 2; }
FUZZ=${FUZZ%\%}%
for s in "$MAXAE" "$MINAE"; do case $s in *[!0-9]*) echo "comparer : seuil « $s » : entier attendu" >&2; exit 2 ;; esac; done
OUTIL=magick
if [ "${COMPARER_SANS_MAGICK:-0}" = 1 ] || ! command -v magick >/dev/null 2>&1; then
    command -v ffmpeg >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 \
        || { echo "comparer : ni magick ni ffmpeg + python3 : rien pour comparer" >&2; exit 2; }
    OUTIL=ffmpeg
fi
# paires (nom A B)
PAIRES=(); ORPH=()
if [ -d "$A" ] && [ -d "$B" ]; then
    while IFS= read -r f; do
        if [ -f "$B/$f" ]; then PAIRES+=("$f"); else ORPH+=("$f (absent de B)"); fi
    done < <(ls "$A" | grep -iE '\.(png|ppm|jpe?g)$' | LC_ALL=C sort)
    while IFS= read -r f; do [ -f "$A/$f" ] || ORPH+=("$f (absent de A)"); done \
        < <(ls "$B" | grep -iE '\.(png|ppm|jpe?g)$' | LC_ALL=C sort)
    DA=$A; DB=$B
elif [ -f "$A" ] && [ -f "$B" ]; then
    DA=$(dirname "$A"); DB=$(dirname "$B"); PAIRES=("$(basename "$A")"); FB=$(basename "$B")
else
    echo "comparer : deux dossiers ou deux fichiers attendus ($A, $B)" >&2; exit 2
fi
[ ${#PAIRES[@]} -gt 0 ] || { echo "comparer : aucune image appariée entre $A et $B" >&2; exit 2; }
S=${SORTIE:-$HOME/dev/sc2-bancs/$(date +%Y%m%d)/comparer-$(date +%H%M%S)}
mkdir -p "$S/diff" && S=$(cd "$S" && pwd) || exit 2
T=$(mktemp -d "$S/.comparer-tmp.XXXXXX") || exit 2   # à soi : un tmp/ déjà présent dans --sortie n'est jamais touché
trap 'rm -rf -- "${T:?}"' EXIT

taille() {   # <image> → « L H »
    if [ "$OUTIL" = magick ]; then magick identify -format '%w %h' "$1[0]" 2>/dev/null
    else ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x "$1" 2>/dev/null | tr x ' '; fi
}
vers_png() {   # <source> <cible> [LxH à forcer] [zone l:h:x:y]
    local src=$1 dst=$2 rs=${3:-} z=${4:-}
    if [ "$OUTIL" = magick ]; then
        magick "$src[0]" -alpha off ${rs:+-resize "${rs}!"} ${z:+-crop "$z" +repage} -depth 8 "PNG24:$dst"
    else
        local vf=""
        [ -n "$rs" ] && vf="scale=${rs/x/:}"
        [ -n "$z" ] && vf="${vf:+$vf,}crop=$z"
        ffmpeg -nostdin -loglevel error -y -i "$src" ${vf:+-vf "$vf"} -pix_fmt rgb24 "$dst"
    fi
}
# repli ffmpeg : AE, EQM, saturations et image de différence en Python sur le RGB brut
mesure_py() {   # <a.png> <b.png> <L> <H> <fuzz %> <diff.png> → « AE PSNR satA satB »
    local ra="$T/a.rgb" rb="$T/b.rgb" rd="$T/d.rgb"
    ffmpeg -nostdin -loglevel error -y -i "$1" -f rawvideo -pix_fmt rgb24 "$ra" || return 1
    ffmpeg -nostdin -loglevel error -y -i "$2" -f rawvideo -pix_fmt rgb24 "$rb" || return 1
    python3 -I - "$ra" "$rb" "$5" "$rd" <<'PY' || return 1
import math, sys
a = open(sys.argv[1], "rb").read(); b = open(sys.argv[2], "rb").read()
fz = float(sys.argv[3].rstrip("%")) / 100 * 255
seuil = 2 * fz * fz                                   # un canal : écart² > 2·fuzz² (règle d'IM 7.1.2)
n = min(len(a), len(b)) // 3
ae = 0; se = 0
d = bytearray(n * 3)
def sat(px, i):
    r, g, bb = px[i], px[i + 1], px[i + 2]
    mx, mn = max(r, g, bb), min(r, g, bb)
    if mx == mn:
        return 0.0
    l = (mx + mn) / 510
    return (mx - mn) / 255 / (1 - abs(2 * l - 1))
sa = sb = 0.0
for k in range(n):
    i = 3 * k
    dr, dg, db = a[i] - b[i], a[i + 1] - b[i + 1], a[i + 2] - b[i + 2]
    se += dr * dr + dg * dg + db * db
    sa += sat(a, i); sb += sat(b, i)
    if dr * dr > seuil or dg * dg > seuil or db * db > seuil:
        ae += 1
        d[i] = 241; d[i + 1] = 0; d[i + 2] = 30
    else:                                              # fond : A éclairci, comme l'image de différence d'IM
        g = 200 + (a[i] + a[i + 1] + a[i + 2]) // 15
        d[i] = d[i + 1] = d[i + 2] = min(g, 255)
open(sys.argv[4], "wb").write(d)
eqm = se / (3 * n) if n else 0
psnr = "inf" if eqm == 0 else f"{10 * math.log10(255 * 255 / eqm):.4f}"
print(ae, psnr, f"{sa / n:.6f}" if n else 0, f"{sb / n:.6f}" if n else 0)
PY
    ffmpeg -nostdin -loglevel error -y -f rawvideo -pix_fmt rgb24 -s "$3x$4" -i "$rd" "$6"
}

printf 'image\ttaille A\ttaille B\tAE (px)\tAE (%%)\tPSNR (dB)\tsaturation A\tsaturation B\n' > "$S/comparaison.tsv"
echo "comparer : $A ↔ $B ; outil $OUTIL ; fuzz $FUZZ${ZONE:+ ; zone $ZONE} ; sortie $S"
printf '%-28s %-10s %-10s %10s %9s %10s %7s %7s\n' image "taille A" "taille B" "AE (px)" "AE (%)" "PSNR (dB)" "sat. A" "sat. B"
NDIFF=0; AEMAX=0; SEUIL=0; NERR=0; NOK=0; k=0
for f in "${PAIRES[@]}"; do
    fa="$DA/$f"; fb="$DB/${FB:-$f}"; k=$((k + 1))
    ta=$(taille "$fa"); tb=$(taille "$fb")
    [ -n "$ta" ] && [ -n "$tb" ] || { echo "comparer : ÉCHEC : $f illisible (A « $ta », B « $tb »)" >&2; NERR=$((NERR + 1)); continue; }
    la=${ta% *}; ha=${ta#* }; rs=""
    if [ "$ta" != "$tb" ]; then
        echo "ATTENTION : $f : tailles différentes (A ${la}×${ha}, B ${tb/ /×}) — B rééchantillonné à ${la}×${ha}" >&2
        rs="${la}x${ha}"
    fi
    z=""; L=$la; H=$ha
    if [ -n "$ZONE" ]; then
        [ $((ZX + ZL)) -le "$la" ] && [ $((ZY + ZH)) -le "$ha" ] \
            || { echo "comparer : zone $ZONE hors de l'image $f (${la}×${ha})" >&2; exit 2; }
        if [ "$OUTIL" = magick ]; then z="${ZL}x${ZH}+${ZX}+${ZY}"; else z="$ZL:$ZH:$ZX:$ZY"; fi
        L=$ZL; H=$ZH
    fi
    pa="$T/a-$k.png"; pb="$T/b-$k.png"; pd="$S/diff/${f%.*}.png"
    vers_png "$fa" "$pa" "" "$z" && vers_png "$fb" "$pb" "$rs" "$z" \
        || { echo "comparer : ÉCHEC : conversion de $f" >&2; NERR=$((NERR + 1)); continue; }
    if [ "$OUTIL" = magick ]; then
        ae=$(magick compare -metric AE -fuzz "$FUZZ" "$pa" "$pb" "$pd" 2>&1 >/dev/null | awk '{print $1}')
        ps=$(magick compare -metric PSNR "$pa" "$pb" null: 2>&1 | awk '{print $1}')
        sa=$(magick "$pa" -colorspace HSL -channel G -separate +channel -format '%[fx:mean]' info:)
        sb=$(magick "$pb" -colorspace HSL -channel G -separate +channel -format '%[fx:mean]' info:)
        case $ps in inf|1.#INF) ps=inf ;; *) awk -v p="$ps" 'BEGIN { exit !(p >= 120) }' && ps=inf ;; esac
    else
        mesure_py "$pa" "$pb" "$L" "$H" "$FUZZ" "$pd" > "$T/mesure.txt"   # synchrone : diff écrite avant la planche
        read -r ae ps sa sb < "$T/mesure.txt"
    fi
    case $ae in ''|*[!0-9.e+]*) echo "comparer : ÉCHEC : AE illisible pour $f (« $ae »)" >&2; NERR=$((NERR + 1)); continue ;; esac
    ae=$(printf '%.0f' "$ae"); NOK=$((NOK + 1))
    pct=$(awk -v a="$ae" -v n=$((L * H)) 'BEGIN { printf "%.4f", n ? 100 * a / n : 0 }')
    printf '%-28s %-10s %-10s %10s %9s %10s %7.4f %7.4f\n' "$f" "${la}x${ha}" "${tb/ /x}" "$ae" "$pct" "$ps" "$sa" "$sb"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$f" "${la}x${ha}" "${tb/ /x}" "$ae" "$pct" "$ps" "$sa" "$sb" >> "$S/comparaison.tsv"
    [ "$ae" -gt 0 ] && NDIFF=$((NDIFF + 1))
    [ "$ae" -gt "$AEMAX" ] && AEMAX=$ae
    [ -n "$MAXAE" ] && [ "$ae" -gt "$MAXAE" ] && { echo "SEUIL : $f : AE $ae > --max-ae $MAXAE" >&2; SEUIL=1; }
    [ -n "$MINAE" ] && [ "$ae" -lt "$MINAE" ] && { echo "SEUIL : $f : AE $ae < --min-ae $MINAE" >&2; SEUIL=1; }
    if [ "$k" -le 8 ]; then   # ligne de planche : A | B | différence, 480 px de large chacune
        if [ "$OUTIL" = magick ]; then
            magick "$pa" "$pb" "$pd" -resize 480x +append "$T/ligne-$(printf %02d "$k").png"
        else
            ffmpeg -nostdin -loglevel error -y -i "$pa" -i "$pb" -i "$pd" -filter_complex \
                "[0]scale=480:-2[a];[1]scale=480:-2[b];[2]scale=480:-2[c];[a][b][c]hstack=3" "$T/ligne-$(printf %02d "$k").png"
        fi
    fi
done
for o in ${ORPH[@]+"${ORPH[@]}"}; do echo "sans pendant : $o" >&2; done
LIGNES=("$T"/ligne-*.png); PL="pas de planche"     # une planche d'un run précédent dans --sortie n'est pas citée
if [ -f "${LIGNES[0]}" ]; then
    if [ "$OUTIL" = magick ]; then magick "${LIGNES[@]}" -append "$S/planche.png"
    elif [ ${#LIGNES[@]} = 1 ]; then cp "${LIGNES[0]}" "$S/planche.png"
    else
        IN=(); for l in "${LIGNES[@]}"; do IN+=(-i "$l"); done
        ffmpeg -nostdin -loglevel error -y "${IN[@]}" -filter_complex "vstack=${#LIGNES[@]}" "$S/planche.png"
    fi && PL="planche $S/planche.png"
fi
echo "comparer : $NOK/${#PAIRES[@]} paire(s) comparée(s), $NDIFF différente(s), AE max $AEMAX px ; $NERR en échec ;" \
     "${#ORPH[@]} sans pendant ; $PL"
[ "$NERR" -gt 0 ] && exit 2
exit "$SEUIL"
