#!/usr/bin/env bash
# Passe 7x — run de COMBAT : mission TRaynor01 (vue enUS), ordres injectés, SC2_MANQUE_CONTINUE=1.
#   android/valider-7x.sh [nom] [VAR=val…]   → recomp-sc2/build-7x/runs-combat/run-<nom>/ (run.log, frames/, empreintes.txt,
#                                              manques.txt, deroule.txt)
#   BIN    (défaut recomp-sc2/build-7x/recomp-sc2 ; build CMake du main, mis à jour par `make -C recomp-sc2/build-7x
#          recomp-sc2` incrémental avant le run si MAKE=1)
#   VUE    (défaut : vue complète enUS recomp-sc2/build-7l-valider/vue-complet ; android/outils/vue-install.sh <d> complet)
#   FRAMES (défaut 1200) ; PAS (une capture toutes les PAS frames, 50) ; DUREE (butoir du run en s, 3600)
#   SIM    (défaut android/outils/mission-combat-7x.sim : CLOSE, sélection, attaque-déplacement, formation, B ;
#          SIM= pour aucun)
# Sorties : bilan des manques (android/outils/manques-7v.py), md5 des frames, déroulé (android/outils/deroule-7x.py :
#   textes et dialogues journalisés, lectures de fichiers, phases). Un run à la fois ; free -h AVANT/APRÈS consigné.
# Pas d'ulimit -v sur le run : l'arène réserve ~8 Go d'adresses (mmap), le run mourrait « Cannot allocate memory ».
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7x"; O="$B/runs-combat"; mkdir -p "$O"
VUE="${VUE:-$R/build-7l-valider/vue-complet}"; EXE="$VUE/Versions/Base15405/SC2.exe"
BIN="${BIN:-$B/recomp-sc2}"; FRAMES="${FRAMES:-1200}"; PAS="${PAS:-50}"; DUREE="${DUREE:-3600}"
SIM="${SIM-$W/android/outils/mission-combat-7x.sim}"
NOM="${1:-combat}"; shift || true
L="$O/valider.log"; j() { echo "$*" | tee -a "$L"; }
if [ "${MAKE:-0}" = 1 ]; then ( ulimit -v 8000000; make -C "$B" recomp-sc2 -j2 2>&1 | tail -2 ) || exit 2; fi
[ -x "$BIN" ] || { echo "binaire absent : $BIN (cmake du main dans recomp-sc2/build-7x, ou MAKE=1)"; exit 2; }
d="$O/run-$NOM"; rm -rf "$d"; mkdir -p "$d"
j "=== passe 7x $NOM — $(date -u '+%F %T') UTC ; binaire $(md5sum < "$BIN" | cut -c1-12) ; vue $(basename "$VUE") ; $FRAMES f ; sim ${SIM:+$(basename "$SIM")} ; ${*:-}"
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
t0=$SECONDS
(cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$VUE" SC2_EXE="$EXE" SC2_FRAMES_MAX="$FRAMES" SC2_FRAMES_PAS="$PAS" \
   SC2_FRAMES_DIR="$d/frames" SC2_RUN_SANS_BNET=1 SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_MANQUE_CONTINUE=1 \
   SC2_TEXTE_MAX="${TEXTE_MAX:-100000}" ${SIM:+SC2_INPUT_SIM="$SIM"} "$@" timeout -s KILL "$DUREE" "$BIN" > run.log 2>&1; echo $? > rc)
j "rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; frames présentées : $(grep -oE 'frames présentées \(Present\) : [0-9]+' "$d/run.log" | grep -oE '[0-9]+$')"
grep -E '^\[ARRÊT\] \[|^\[BILAN\] \[INPUT\]' "$d/run.log" | cut -c1-220 | sed 's/^/  | /' | tee -a "$L"
j "  draws réels : $(grep -oE 'draws RÉELS \(shaders du jeu en SPIR-V\) : [0-9]+' "$d/run.log" | grep -oE '[0-9]+$') ; « lumière » : $(grep -oE '[0-9]+ « lumière »' "$d/run.log" | head -1)"
(cd "$d/frames" 2>/dev/null && md5sum *.ppm > "$d/empreintes.txt")
python3 -I "$W/android/outils/manques-7v.py" "$d" 2>&1 | tee "$d/manques.txt" | head -30 | sed 's/^/  /' | tee -a "$L"
python3 -I "$W/android/outils/deroule-7x.py" "$d" 2>&1 | tee "$d/deroule.txt" | head -60 | sed 's/^/  /' | tee -a "$L"
python3 -I "$W/android/outils/ppm-blocs.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
