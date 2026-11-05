#!/usr/bin/env bash
# Passe 7o — RELINK LÉGER du banc ARM64 sc2-sim (règle : aucun build massif).
#
# Comme relink-runtime.sh (7l) pour x86 : ne recompile QUE le runtime écrit à la main et la glue Android
# (aarch64-linux-gnu-gcc, -j2) et les relie aux objets liftés aarch64 d'un build sc2-sim EXISTANT, copiés
# une fois dans <build>/lifted-o (le lifté ne change pas ici). Chemin CPU (sans SC2_SWAPCHAIN), comme
# l'APK par défaut quand la swapchain est indisponible.
#   prérequis : un build sc2-sim de référence (REF, objets liftés + stormlib/libstorm.a) ET, dans ce
#               worktree, src/lifted/recomp_sc2_list.h DU MÊME LIFT (table de dispatch) et
#               src/shaders/sc2_shaders_spv.h (générés, hors git)
#   usage : REF=<build sc2-sim> android/outils/relink-sim.sh [build]   (défaut : android/build-sim-7o)
set -euo pipefail
W="$(cd "$(dirname "$0")/../.." && pwd)"; R="$W/recomp-sc2"; J="$W/android/jni"
B="${1:-$W/android/build-sim-7o}"; case "$B" in /*) ;; *) B="$W/$B" ;; esac
REF="${REF:?REF=<dossier de build sc2-sim existant> requis}"
O="$B/runtime-o"; L="$B/lifted-o"; mkdir -p "$O" "$L" "$B/vk-entetes"
for d in vulkan vk_video; do [ -e "$B/vk-entetes/$d" ] || ln -s "/usr/include/$d" "$B/vk-entetes/$d"; done
if [ -z "$(ls "$L" 2>/dev/null)" ]; then
  find "$REF" -name 'recomp_*.c.o' -exec cp {} "$L/" \;
  cp "$REF/stormlib/libstorm.a" "$B/libstorm.a"
  echo "objets liftés copiés depuis $REF : $(ls "$L" | wc -l)"
fi
for f in "$R/src/lifted/recomp_sc2_list.h" "$R/src/shaders/sc2_shaders_spv.h"; do
  [ -f "$f" ] || { echo "manque $f (généré, hors git)"; exit 1; }
done
CC=aarch64-linux-gnu-gcc
INC=(-I"$J" -I"$R/src" -I"$R/runtime" -I"$R/src/lifted" -I"$R/src/shaders" -I"$B/vk-entetes" -I"$HOME/tools/StormLib/src")
BASE=(-fsigned-char -fPIE -std=gnu17 -D__STORMLIB_NO_STATIC_LINK__ -w "${INC[@]}")
RT=(src/main.c runtime/image_base.c runtime/sc2_runtime.c runtime/sc2_iat.c runtime/sc2_mpq.c runtime/sc2_iat_p4.c
    runtime/sc2_d3d9.c runtime/sc2_d3d9v.c runtime/win32_compat.c runtime/stubs_sc2.c runtime/sc2_input_sim.c
    runtime/sc2_audio_fmod.c runtime/sc2_audio_mix.c runtime/sc2_audio_stb_vorbis.c runtime/sc2_audio_mpq.c
    runtime/sc2_audio_sortie.c runtime/sc2_audio_sortie_compteur.c)
RTOPT=(-O2 -include "$J/compat/sc2_android_compat.h" -include "$R/runtime/sc2_lifted.h" -include "$R/runtime/cpu.h")
compiler() {   # <source> <objet> <options…>
  local s="$1" o="$2"; shift 2
  if [ "$o" -nt "$s" ] && [ "$o" -nt "$J/sc2_geo.h" ] && [ "$o" -nt "$R/runtime/cpu.h" ]; then return 0; fi
  echo "cc $(basename "$s")"; "$CC" "${BASE[@]}" "$@" -c "$s" -o "$o"
}
export -f compiler; export CC J R
pids=()
for s in "${RT[@]}"; do
  d=(); [ "$s" = src/main.c ] && d=(-Dmain=sc2_poc_main)
  compiler "$R/$s" "$O/$(basename "$s").o" "${RTOPT[@]}" "${d[@]}" & pids+=($!)
  [ ${#pids[@]} -ge 2 ] && { wait "${pids[0]}"; pids=("${pids[@]:1}"); }
done
for s in "$W/android/sim/sc2_sim_main.c" "$J/sc2_android_hooks.c" "$J/sc2_ucontext.c" "$J/sc2_audio_android.c" \
         "$J/sc2_ucontext_aarch64.S"; do
  compiler "$s" "$O/$(basename "$s").o" -O2 & pids+=($!)
  [ ${#pids[@]} -ge 2 ] && { wait "${pids[0]}"; pids=("${pids[@]:1}"); }
done
wait
aarch64-linux-gnu-g++ -pie -Wl,--wrap=mmap -Wl,--wrap=dlopen -Wl,--wrap=d3dv_present "$O"/*.o "$L"/*.o \
    "$B/libstorm.a" -lstdc++ -lm -ldl -lpthread -o "$B/sc2-sim"
echo "relié : $B/sc2-sim"
