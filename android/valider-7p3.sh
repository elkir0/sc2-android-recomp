#!/usr/bin/env bash
# Passe 7p3 — performance de la présentation Android : mesure (SC2_TRACE_PRESENT=1) puis optimisation mesurée.
# UN run à la fois (pgrep -x recomp-sc2 / sc2-sim vides avant chaque run) ; JAMAIS d'ulimit sur les runs ;
# free -h AVANT/APRÈS consigné. ulimit -v 8000000 sur les BUILDS seulement.
#   sim-vk          : banc aarch64 android/sim (mêmes sources de plate-forme que l'APK, présentateur swapchain
#                     compilé) → android/build-sim-7p3/sc2-sim (toolchain aarch64, ulimit -v)
#   sim-<tag> [VAR=val…] : run du banc sous qemu-aarch64 + Lavapipe arm64, Xvfb :98, fenêtre 2400x1080 (= émulateur
#                     M4), SIM_SORTIE=SIM_FRAMES (défaut 300), SC2_SWAPCHAIN=1 SC2_TRACE_PRESENT=1, fenêtre de mesure
#                     SC2_TRACE_PRESENT_FENETRE (défaut 100) ; variables supplémentaires par VARS="A=1 B=2"
#   controle-<tag>  : idem, CONTROLE_FRAMES frames (défaut 120), SC2_SWAPCHAIN_CONTROLE=1 : image de swapchain relue
#                     aux frames 1/10/100 → moyennes RGB, voie des bandes, pixels de bande non noirs, empreintes FNV
#                     de l'image et des bandes (passe 8d)
#   verdict         : passe 8d — compare controle-a (défaut), controle-b (VARS=SC2_PRESENT_BANDES=1) et controle-t
#                     (VARS="SC2_PRESENT_BANDES=1 SC2_PRESENT_BANDES_TEMOIN=1", témoin magenta) : A et B doivent
#                     DIFFÉRER par la voie et donner les mêmes pixels ; T doit montrer des bandes non noires (preuve
#                     que le contrôle voit les bandes). Un contrôle sauté ou vide est signalé comme tel.
#   SC2_R=<arbre recomp-sc2> : arbre compilé par sim-vk (src/lifted/ généré, hors git ; défaut : celui du worktree)
#   x86             : recomp-sc2 x86 dans recomp-sc2/build-7p3 (cmake + make, ulimit -v) — non-régression du chantier
#   defaut<n>       : 600 f, install de référence, run dir VIERGE, SC2_ROOT + SC2_EXE + SC2_FRAMES_DIR (ENV-EXACT) ;
#                     attendu f1 be01318cdf69, f10 d3cd40be732f, f100/f600 8d1ec057331b
#   menu            : 600 f, install complète (cinématique du menu), 1 frame sur 25 ; empreintes
# Sorties hors git : android/build-7p3/ (journaux, runs).
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; R="$W/recomp-sc2"; O="$W/android/build-7p3"; mkdir -p "$O"
DEPOT="$HOME/code/reverse/sc2-partout"
INSTALL="$DEPOT/extraction/install"; COMPLET="$DEPOT/extraction/install-complet"; EXE="$INSTALL/Versions/Base15405/SC2.exe"
BS="$W/android/build-sim-7p3"; BX="$R/build-7p3"
L="$O/valider${TAG:+-$TAG}.log"
j() { echo "$*" | tee -a "$L"; }
mem() { free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}'; }
libre() {   # aucun run concurrent
  local p; p=$(pgrep -x recomp-sc2)   # pgrep -x EXACT seulement (un pgrep -f se voit lui-même dans une boucle d'attente)
  [ -z "$p" ] || { j "  ⚠ run concurrent en cours ($(echo $p | tr '\n' ' ')) : étape sautée"
                  [ -n "${1:-}" ] && { mkdir -p "$1"; echo sautee > "$1/rc"; }; return 1; }   # passe 8d : saut visible
}
j "=== passe 7p3 ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-70), arbre : $(git -C "$W" status --short | wc -l) modif."
XVFB=""
xvfb() {
  [ -n "$XVFB" ] && return
  Xvfb :98 -screen 0 2400x1200x24 -nolisten tcp > "$O/xvfb.log" 2>&1 & XVFB=$!
  sleep 2; kill -0 "$XVFB" 2>/dev/null || { j "  Xvfb :98 n'a pas démarré"; XVFB=""; return 1; }
  export DISPLAY=:98
}
trap '[ -n "$XVFB" ] && kill "$XVFB" 2>/dev/null' EXIT
VK=(-L "$HOME/tools/arm64-vk-root" -E LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu
    -E VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json -E MALLOC_ARENA_MAX=2)
sim() {   # <nom> <frames> [VAR=val…]
  local nom="$1" d="$O/run-$1" n="$2"; shift 2; rm -rf "$d"
  libre "$d" || return; xvfb || return; mkdir -p "$d"
  j "\$ [$nom] SIM_SORTIE=$n SIM_FENETRE=2400x1080 SC2_SWAPCHAIN=1 $* — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$INSTALL" SC2_FRAMES_DIR="$d/frames" SIM_SORTIE="$n" SIM_FENETRE=2400x1080 SIM_SWAPCHAIN=2400x1080 \
     SC2_SWAPCHAIN=1 "$@" timeout -s KILL 3000 qemu-aarch64 "${VK[@]}" "$BS/sc2-sim" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) — free -h APRÈS : $(mem)"
  grep -aE '^\[PRESENT\] frames|^\[BILAN\] \[PRESENT\]|^\[VKP\] (SC2_PRESENT|swapchain [0-9]|contrôle)|^\[SIM\] (cadence|swapchain)' "$d/run.log" \
    | cut -c1-400 | sed 's/^/  /' | tee -a "$L"
  rm -rf "$d/sc2-c" "$d/frames"
}
run_x86() {   # <nom> <install> <frames> [VAR=val…]
  libre || return
  local nom="$1" d="$O/run-$1" ins="$2" fr="$3"; shift 3; rm -rf "$d"; mkdir -p "$d"
  j "\$ [$nom] binaire $(md5sum < "$BX/recomp-sc2" | cut -c1-12) ${*:-(aucune variable)} SC2_FRAMES_MAX=$fr — free -h AVANT : $(mem)"
  local t0=$SECONDS
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$ins" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" "$@" \
     timeout -s KILL 3000 "$BX/recomp-sc2" > "$d/run.log" 2>&1; echo $? > "$d/rc")
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s) — free -h APRÈS : $(mem)"
  grep -aE '^\[BILAN\] draws :' "$d/run.log" | cut -c1-200 | sed 's/^/  /' | tee -a "$L"
  python3 -I "$W/android/outils/ppm-sequence.py" "$d/frames" 2>&1 | sed 's/^/    /' | tee -a "$L"
  rm -rf "$d/sc2-c" "$d/frames"
}
for e in "${@:-sim-a}"; do case $e in
sim-vk)
  ( ulimit -v 8000000; cmake -S "$W/android/sim" -B "$BS" -DCMAKE_TOOLCHAIN_FILE="$R/cmake/toolchain-aarch64.cmake" -DSC2_SWAPCHAIN=ON \
      ${SC2_R:+-DSC2_R="$SC2_R"} \
      > "$O/sim-cmake.log" 2>&1 && nice -n 10 make -C "$BS" -j"${MAKE_J:-2}" sc2-sim > "$O/sim-make.log" 2>&1 )
  j "  sim-vk make rc=$? — $(grep -E 'SWAPCHAIN' "$O/sim-cmake.log" | head -1 | sed 's/^-- //')" ;;
sim-*)      sim "${e#sim-}" "${SIM_FRAMES:-300}" SC2_TRACE_PRESENT=1 SC2_TRACE_PRESENT_FENETRE="${FENETRE:-100}" ${VARS:-} ;;
controle-*) sim "$e" "${CONTROLE_FRAMES:-120}" SC2_SWAPCHAIN_CONTROLE=1 ${VARS:-} ;;
verdict)    python3 -I "$W/android/outils/controle-7p3.py" "$O" 2>&1 | tee -a "$L" ;;
x86)
  ( ulimit -v 8000000; cmake -S "$R" -B "$BX" > "$O/x86-cmake.log" 2>&1 && nice -n 10 make -C "$BX" -j"${MAKE_J:-2}" recomp-sc2 > "$O/x86-make.log" 2>&1 )
  j "  x86 make rc=$? — binaire $(md5sum < "$BX/recomp-sc2" | cut -c1-12)" ;;
defaut*) run_x86 "$e" "$INSTALL" 600 ;;
menu)    run_x86 menu "$COMPLET" 600 SC2_FRAMES_PAS=25 ;;
esac; done
j "=== fin $(date -u '+%T') UTC"
true
