#!/bin/bash
# banc-ab.sh — A/B d'images/s au banc Mac : TRaynor01 scriptée, runs A, B, A, B… sans profileur (V1.4, 22/10).
#   scripts/banc-ab.sh <étiq>:<binaire|main> <étiq>:<binaire|main> [n]      n paires entrelacées A_i B_i (défaut 3)
#   ex. scripts/banc-ab.sh A:main B:~/dev/sc2-wt-x/recomp-sc2/build-mac-O2radx/recomp-sc2 3
#   main = binaire de référence du dépôt principal (<dépôt principal>/recomp-sc2/build-mac-O2radx/recomp-sc2).
# Un run : SC2_ARGS=-run Maps/Campaign/TRaynor01.SC2Map, scénario scripts/entrees/jeu-actif-traynor01-mac.txt (aide
#   fermée à 40-55 s, puis caméra fixe sur l'armée au départ et curseur déplacé toutes les 2 s, sans clic : la
#   sélection par cadre et la caméra qui suit du scénario Fold ne marchent pas au Mac, voir son en-tête) complété de
#   « <fin>:QUIT » (BANC_AB_SCENARIO : un autre scénario) ; reste de l'environnement comme campagne-banc.sh
#   (SC2_RUN_SANS_BNET, SC2_MANQUE_CONTINUE, SC2_SANS_VIDEOS, polices du dépôt, SC2_SPIRV_PACK / SC2_DOCUMENTS transmis
#   s'ils sont posés) ; captures réduites (SC2_FRAMES=1 explicite, SC2_FRAMES_BASE=0, SC2_FRAMES_PAS=BANC_AB_PAS,
#   défaut 2000 : une capture toutes les ≈ 20 s au lieu de la règle 1/10/100 qui relit le back buffer chaque seconde) ;
#   cache de pipelines Vulkan par étiquette HORS de sc2-c (<sortie>/vk-pipelines-<étiq>.bin) ; jamais de `sample`.
# Chauffe (BANC_AB_CHAUFFE=1, défaut) : une paire 0 (A0, B0) jouée en entier AVANT les paires mesurées, affichée mais
#   exclue des médianes. Mesuré le 22/10 : le cache de shaders Metal est commun à tout le système ; le premier run qui
#   rencontre des shaders absents de ce cache passe des secondes à créer ses pipelines (A1 : 351 pipelines en 9,6 s,
#   jusqu'à 391 ms par appel ; B1, même binaire, juste après : 262 en 0,45 s), d'où +48 % d'images/s pour B1 à binaire
#   égal. La colonne « pipelines » du rapport le montre. BANC_AB_CHAUFFE=0 : pas de chauffe.
# Plateau : de BANC_AB_DEBUT (défaut 65 s : aide fermée depuis 10 s) au QUIT à BANC_AB_FIN (défaut 125 s, soit 60 s),
#   en tours de pompe par seconde (« [INPUT] banc t=…s tour N », un échantillon par événement du scénario : un tour =
#   une frame présentée).
# Dépouillement : scripts/nonreg.py ab — équivalent Mac de fold-ab.py (qui lit les [FPS] du logcat, absentes du
#   run.log Mac) : images/s de chaque run (frames/durée et médiane des fenêtres de 5 s), écart B/A par paire, médiane
#   des écarts, écart des médianes, triplets A_i B_i A_i+1 (dérive thermique compensée).
# Sortie : BANC_AB_SORTIE (défaut ~/dev/sc2-bancs/<aaaammjj>/banc-ab-<hhmmss>) : A1/ B1/ A2/ … (run.log, resume.txt,
#   empreintes.txt, vignettes 640 px), scenario.txt, ab.txt, banc-ab.json, banc-ab.md (imprimé).
# Verrou banc tenu pendant TOUTE la série (lib-banc.sh) : aucune autre mesure ne s'intercale entre deux runs.
#   Garde-disque : SC2_DISQUE_MIN_GO + 2 Gio en tête et avant chaque run. Garde-fou : kill après BANC_AB_FIN + 120 s.
# Coordonnées du scénario (client 1280×720, défaut Mac de SC2_ECRAN) vérifiées le 22/10 sur les captures de deux
#   runs : voir l'en-tête de scripts/entrees/jeu-actif-traynor01-mac.txt.
# Code : 0 si au moins une paire mesurée, 1 sinon, 2 usage, 3 garde-disque.
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"
usage() { sed -n '3,5p' "$0" | sed 's/^# *//' >&2; exit 2; }
[ $# -ge 2 ] && [ $# -le 3 ] || usage
ORIG=("$@")
N=${3:-3}
case $N in ''|*[!0-9]*|0) echo "banc-ab : n = « $N » : nombre de paires ≥ 1 attendu" >&2; exit 2 ;; esac
DEBUT=${BANC_AB_DEBUT:-65}; FIN=${BANC_AB_FIN:-125}; PAS=${BANC_AB_PAS:-2000}
SCEN=${BANC_AB_SCENARIO:-$R/scripts/entrees/jeu-actif-traynor01-mac.txt}
[ -f "$SCEN" ] || { echo "banc-ab : scénario absent : $SCEN" >&2; exit 2; }
PRINC_BIN() {
    local c; c="$(git -C "$R" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" || return 1
    echo "$(dirname "$c")/recomp-sc2/build-mac-O2radx/recomp-sc2"
}
ETIQ=(); BINS=()
for a in "$1" "$2"; do
    e=${a%%:*}; b=${a#*:}
    [[ $a == *:* ]] && [[ $e =~ ^[A-Za-z0-9_-]+$ ]] || { echo "banc-ab : « $a » : <étiquette>:<binaire> attendu" >&2; exit 2; }
    [ "$b" = main ] && b=$(PRINC_BIN)
    [ -f "$b" ] && [ -x "$b" ] || { echo "banc-ab : binaire « $b » absent ou non exécutable" >&2; exit 2; }
    ETIQ+=("$e"); BINS+=("$(cd "$(dirname "$b")" && pwd)/$(basename "$b")")
done
[ "${ETIQ[0]}" != "${ETIQ[1]}" ] || { echo "banc-ab : deux étiquettes distinctes attendues" >&2; exit 2; }
S=${BANC_AB_SORTIE:-$HOME/dev/sc2-bancs/$(date +%Y%m%d)/banc-ab-$(date +%H%M%S)}
. "$R/scripts/lib-banc.sh"
banc_garde_disque "$S" 2 || exit 3
banc_verrou "$0" "${ORIG[@]}"
# ---- verrou banc tenu ----
X=${SC2_EXTRACTION:-~/sc2-partout/extraction}
[ -f "$X/install/Versions/Base15405/SC2.exe" ] || { echo "données du jeu absentes : $X" >&2; exit 1; }
if [ -e "$S" ] && [ -n "$(ls -A "$S" 2>/dev/null)" ]; then echo "banc-ab : $S n'est pas vide" >&2; exit 2; fi
mkdir -p "$S" && S=$(cd "$S" && pwd) || exit 2
{ cat "$SCEN"; echo; echo "# banc-ab.sh : fin du plateau"; echo "$FIN:QUIT"; } > "$S/scenario.txt"
GARDE_FOU=$(( ${FIN%.*} + 120 ))
echo "# run étiquette paire binaire sha256" > "$S/ab.txt"
echo "banc-ab : ${ETIQ[0]}=${BINS[0]} ; ${ETIQ[1]}=${BINS[1]} ; $N paire(s) ; plateau $DEBUT → $FIN s ; sortie $S ($(date +%T))"
un_run() {   # <étiquette> <binaire> <paire>
    local e=$1 bin=$2 i=$3 D="$S/$1$3" t=$SECONDS P n=0 RC
    banc_garde_disque "$S" 2 || { echo "banc-ab : arrêt avant $e$i (garde-disque)" >&2; return 3; }
    echo "=== $e$i ($(date +%H:%M:%S)) ==="
    mkdir -p "$D/frames"
    local -a C=(SC2_ROOT="$D/sc2-c" SC2_EXE="$X/install/Versions/Base15405/SC2.exe" SC2_INSTALL="$X/install"
                SC2_FRAMES=1 SC2_FRAMES_BASE=0 SC2_FRAMES_PAS="$PAS" SC2_FRAMES_DIR="$D/frames"
                SC2_POLICES="${SC2_POLICES:-$R/android/assets/polices}" SC2_VK_CACHE_PIPELINES="$S/vk-pipelines-$e.bin")
    [ "${SC2_SPIRV_PACK:-}" ] && C[${#C[*]}]="SC2_SPIRV_PACK=$SC2_SPIRV_PACK"
    [ "${SC2_DOCUMENTS:-}" ] && C[${#C[*]}]="SC2_DOCUMENTS=$SC2_DOCUMENTS"
    ( cd "$D" && exec env "${C[@]}" SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 SC2_SANS_VIDEOS=1 \
        SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_BANC_ENTREES="@$S/scenario.txt" "$bin" > "$D/run.log" 2>&1 ) &
    P=$!
    while kill -0 "$P" 2>/dev/null && [ "$n" -lt "$GARDE_FOU" ]; do sleep 1; n=$((n + 1)); done
    if kill -0 "$P" 2>/dev/null; then
        echo "garde-fou murale : $e$i arrêté après $GARDE_FOU s" >&2
        kill -TERM "$P" 2>/dev/null; sleep 2; kill -KILL "$P" 2>/dev/null
    fi
    wait "$P"; RC=$?
    echo "rc=$RC en $((SECONDS - t)) s" > "$D/resume.txt"
    banc_compacter "$D" >> "$D/resume.txt"
    rm -rf -- "${D:?}/sc2-c"
    echo "$e$i $e $i $bin $(shasum -a 256 "$bin" | cut -d' ' -f1)" >> "$S/ab.txt"
    echo "    $(head -1 "$D/resume.txt") ; $(grep -a -m1 'frames présentées' "$D/run.log" | sed 's/.*: /images /')"
}
P0=1; [ "${BANC_AB_CHAUFFE:-1}" = 0 ] || P0=0
for i in $(seq "$P0" "$N"); do
    un_run "${ETIQ[0]}" "${BINS[0]}" "$i" || break
    un_run "${ETIQ[1]}" "${BINS[1]}" "$i" || break
done
echo
python3 -I "$R/scripts/nonreg.py" ab "$S" --debut "$DEBUT"
