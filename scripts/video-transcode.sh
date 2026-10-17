#!/usr/bin/env bash
# video-transcode.sh — transcodage LOCAL .ogv (Theora) → .mp4 (H.264 Baseline) pour le décodage matériel Android
# (AMediaCodec). Générique : aucun nom de fichier, chemin d'installation ni empreinte en dur ; les originaux ne sont
# jamais modifiés. Ne travaille QUE sur les fichiers de votre propre copie légitime, sur votre machine.
#
#   scripts/video-transcode.sh [<fichier.ogv | dossier>...]   chaque .ogv trouvé → <même base>.mp4
#   SRC_DIR=<dossier>  source si aucun argument n'est donné (parcours récursif des .ogv)
#   OUT_DIR=<dossier>  sortie dans un arbre parallèle (même chemin relatif) ; défaut : .mp4 frère, à côté du .ogv
#   FFMPEG=… FFPROBE=…  binaires à utiliser (défaut : ffmpeg / ffprobe du PATH ; libx264 requis)
#   FORCE=1          refaire les .mp4 déjà à jour (défaut : sautés si plus récents que leur .ogv)
#   CRF=20 PRESET=slow   qualité / vitesse libx264 (défaut 20 / slow)
#   GOP=<n>          distance max entre images clés (défaut : 1 s, = fps arrondi) — recherche rapide (boucles)
#   AUDIO=aac        ajoute la piste son en AAC (défaut : aucune — le moteur lit le son dans le .ogv lui-même)
#
# Pourquoi ces paramètres :
#   -map 0:v:0 -an   image seule : le lecteur matériel ne remplace que l'IMAGE, le son et l'horloge restent ceux du
#                    moteur, lus dans le .ogv ;
#   -profile:v baseline -level 3.1 -pix_fmt yuv420p   accepté par tous les décodeurs AVC matériels Android
#                    (pas de B-frames : sortie dans l'ordre d'entrée, pas de latence de réordonnancement) ;
#   -fps_mode cfr -r <fps source>   une image H.264 par paquet Theora, y compris les images dupliquées (paquets de
#                    0 octet) : la frame n du moteur = l'image n du .mp4 (horodatage n / fps) ;
#   -g <fps>         une image clé par seconde : les recherches (boucles, retour arrière) restent sous 1 s de décodage ;
#   -movflags +faststart   index en tête (ouverture sans lire tout le fichier).
set -euo pipefail
ulimit -v 8000000 2>/dev/null || true   # garde-fou mémoire : 8 Go virtuels maximum

FFMPEG="${FFMPEG:-ffmpeg}" FFPROBE="${FFPROBE:-ffprobe}"
SRC_DIR="${SRC_DIR:-}" OUT_DIR="${OUT_DIR:-}"
if [ $# -eq 0 ]; then
    [ -n "$SRC_DIR" ] || { sed -n '2,13p' "$0" >&2; exit 2; }
    set -- "$SRC_DIR"
fi
command -v "$FFMPEG" >/dev/null && command -v "$FFPROBE" >/dev/null \
    || { echo "ffmpeg/ffprobe introuvables ($FFMPEG / $FFPROBE) : installer ffmpeg ou régler FFMPEG/FFPROBE" >&2; exit 2; }
case "$("$FFMPEG" -hide_banner -encoders 2>/dev/null)" in *" libx264 "*) ;; *) echo "$FFMPEG sans libx264" >&2; exit 2;; esac

CRF="${CRF:-20}" PRESET="${PRESET:-slow}" FORCE="${FORCE:-0}" AUDIO="${AUDIO:-}"
fait=0 saute=0 echec=0

# nombre d'images d'un flux vidéo (décodage complet : exact, y compris pour l'Ogg sans index)
images() { "$FFPROBE" -v error -count_packets -select_streams v:0 -show_entries stream=nb_read_packets -of csv=p=0 "$1"; }

# chemin du .mp4 : frère du .ogv, ou même chemin relatif (à la racine $2) sous $OUT_DIR
cible() {
    local src="$1" racine="$2" rel
    if [ -z "$OUT_DIR" ]; then echo "${src%.*}.mp4"; return; fi
    if [ -n "$racine" ]; then rel="${src#"$racine"/}"; else rel="$(basename "$src")"; fi
    echo "$OUT_DIR/${rel%.*}.mp4"
}

transcoder() {
    local src="$1" dst tmp fps g a
    dst="$(cible "$1" "${2:-}")"
    if [ "$FORCE" != 1 ] && [ -s "$dst" ] && [ "$dst" -nt "$src" ]; then echo "à jour : $dst"; saute=$((saute + 1)); return 0; fi
    fps=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$src")
    [ -n "$fps" ] && [ "$fps" != 0/0 ] || { echo "ÉCHEC $src : pas de flux vidéo" >&2; echec=$((echec + 1)); return 0; }
    g="${GOP:-$(awk -F/ '{ d = ($2 ? $2 : 1); printf "%d", ($1 / d) + 0.5 }' <<<"$fps")}"
    a=(-an); [ "$AUDIO" = aac ] && a=(-map '0:a:0?' -c:a aac -b:a 160k)
    mkdir -p "$(dirname "$dst")"
    tmp="${dst%.mp4}.tmp-$$.mp4"
    echo "→ $src ($fps i/s, GOP $g, CRF $CRF)"
    if "$FFMPEG" -hide_banner -loglevel error -nostdin -y -i "$src" -map 0:v:0 "${a[@]}" \
         -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p -preset "$PRESET" -crf "$CRF" \
         -fps_mode cfr -r "$fps" -g "$g" -keyint_min "$g" -sc_threshold 0 -movflags +faststart "$tmp"; then
        local ni no; ni=$(images "$src"); no=$(images "$tmp")
        mv -f "$tmp" "$dst"
        echo "   $dst : $(stat -c %s "$dst") o, images $no (source : $ni paquets Theora)"
        [ "$ni" = "$no" ] || echo "   ATTENTION : $no images ≠ $ni paquets — la correspondance frame n ↔ image n est décalée" >&2
        fait=$((fait + 1))
    else
        rm -f "$tmp"; echo "ÉCHEC ffmpeg : $src" >&2; echec=$((echec + 1))
    fi
}

for arg in "$@"; do
    if [ -d "$arg" ]; then
        racine="${arg%/}"
        while IFS= read -r -d '' f; do transcoder "$f" "$racine"; done < <(find "$racine" -type f -iname '*.ogv' -print0 | sort -z)
    elif [ -f "$arg" ]; then transcoder "$arg"
    else echo "introuvable : $arg" >&2; echec=$((echec + 1)); fi
done
echo "video-transcode : $fait transcodé(s), $saute à jour, $echec échec(s)"
[ "$echec" = 0 ]
