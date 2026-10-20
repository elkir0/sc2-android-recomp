#!/usr/bin/env bash
# Passe 7v — route au-delà du premier écran de partie : mission TRaynor01, clic « CLOSE », SC2_MANQUE_CONTINUE=1.
#   android/valider-7v.sh [nom] [VAR=val…]   → recomp-sc2/build-7v/run-<nom>/ (run.log, frames/, empreintes.txt, manques.txt)
#   BIN    (défaut recomp-sc2/build-7v/recomp-sc2) — ABSENT : relié par android/outils/relink-runtime.sh build-7v, à partir
#          des objets liftés de build-7v/lifted-o ; s'ils manquent, BUILD_DE=<build CMake x86 du relift commun> et
#          SRC_LIFTE_DE=<son src/lifted> fournissent (copie, lecture seule) les objets liftés, libstorm.a, libsc2_win32.a
#          et recomp_sc2_list.h. La table SPIR-V src/shaders/sc2_shaders_spv.h (générée, hors git) doit exister.
#   VUE    (défaut : vue complète enUS recomp-sc2/build-7l-valider/vue-complet ; android/outils/vue-install.sh <d> complet)
#   FRAMES (défaut 1200) ; PAS (une capture toutes les PAS frames, 50) ; CLIC (défaut android/outils/mission-fermer-aide.sim,
#          clic sur « CLOSE » aux frames 1040-1048 ; CLIC= pour aucun) ; DUREE (butoir du run en s, 3600).
# Sorties : bilan des manques (android/outils/manques-7v.py) et md5 de chaque frame capturée (comparaison de deux runs :
#   android/outils/manques-7v.py --comparer run-A run-B). Un run à la fois ; free -h AVANT/APRÈS consigné.
# Pas d'ulimit -v sur le run : l'arène réserve ~8 Go d'adresses (mmap), le run mourrait « Cannot allocate memory ».
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; B="$R/build-7v"; mkdir -p "$B"
VUE="${VUE:-$R/build-7l-valider/vue-complet}"; EXE="$VUE/Versions/Base15405/SC2.exe"
BIN="${BIN:-$B/recomp-sc2}"; FRAMES="${FRAMES:-1200}"; PAS="${PAS:-50}"; DUREE="${DUREE:-3600}"
CLIC="${CLIC-$W/android/outils/mission-fermer-aide.sim}"
NOM="${1:-mission}"; shift || true
L="$B/valider.log"; j() { echo "$*" | tee -a "$L"; }
if [ ! -x "$BIN" ]; then
  if [ -z "$(ls "$B/lifted-o/" 2>/dev/null)" ]; then
    [ -n "${BUILD_DE:-}" ] && [ -n "${SRC_LIFTE_DE:-}" ] || { echo "build-7v absent : donner BUILD_DE et SRC_LIFTE_DE"; exit 2; }
    mkdir -p "$B/lifted-o"; cp -p "$BUILD_DE"/CMakeFiles/recomp-sc2.dir/src/lifted/recomp_*.c.o "$B/lifted-o/"
    cp -p "$BUILD_DE/stormlib/libstorm.a" "$BUILD_DE/libsc2_win32.a" "$B/"; cp -p "$SRC_LIFTE_DE/recomp_sc2_list.h" "$R/src/lifted/"
  fi
  "$W/android/outils/relink-runtime.sh" build-7v || exit 2
fi
d="$B/run-$NOM"; rm -rf "$d"; mkdir -p "$d"
j "=== passe 7v $NOM — $(date -u '+%F %T') UTC ; binaire $(md5sum < "$BIN" | cut -c1-12) ; vue $(basename "$VUE") ; $FRAMES f ; clic ${CLIC:+$(basename "$CLIC")} ; ${*:-}"
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
t0=$SECONDS
(cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$VUE" SC2_EXE="$EXE" SC2_FRAMES_MAX="$FRAMES" SC2_FRAMES_PAS="$PAS" \
   SC2_FRAMES_DIR="$d/frames" SC2_RUN_SANS_BNET=1 SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_MANQUE_CONTINUE=1 \
   ${CLIC:+SC2_INPUT_SIM="$CLIC"} "$@" timeout -s KILL "$DUREE" "$BIN" > run.log 2>&1; echo $? > rc)
j "rc=$(cat "$d/rc") ($((SECONDS - t0)) s) ; frames présentées : $(grep -oE 'frames présentées \(Present\) : [0-9]+' "$d/run.log" | grep -oE '[0-9]+$')"
grep -E '^\[ARRÊT\] \[' "$d/run.log" | cut -c1-200 | sed 's/^/  | /' | tee -a "$L"
j "  draws réels : $(grep -oE 'draws RÉELS \(shaders du jeu en SPIR-V\) : [0-9]+' "$d/run.log" | grep -oE '[0-9]+$') ; « lumière » : $(grep -oE '[0-9]+ « lumière »' "$d/run.log" | head -1)"
(cd "$d/frames" 2>/dev/null && md5sum *.ppm > "$d/empreintes.txt")
python3 -I "$W/android/outils/manques-7v.py" "$d" 2>&1 | tee "$d/manques.txt" | head -40 | sed 's/^/  /' | tee -a "$L"
python3 -I "$W/android/outils/ppm-blocs.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
