#!/usr/bin/env bash
# Passe 7l — RELINK LÉGER de recomp-sc2 x86 (règle du 3e slot : aucun build massif).
#
# Ne recompile QUE le runtime écrit à la main (≈ 15 fichiers, -j2) et le relie aux objets liftés
# d'un build existant, copiés une fois dans <build>/lifted-o (le code lifté ne change pas ici).
#   prérequis (une fois) : <build>/lifted-o/*.o, <build>/libstorm.a, <build>/libsc2_win32.a,
#                          src/lifted/recomp_sc2_list.h et src/shaders/sc2_shaders_spv.h (générés, hors git)
#   usage : android/outils/relink-runtime.sh [build]        (défaut : recomp-sc2/build-7l ; relatif = sous recomp-sc2/)
set -euo pipefail
R="$(cd "$(dirname "$0")/../../recomp-sc2" && pwd)"; B="${1:-$R/build-7l}"
case "$B" in /*) ;; *) B="$R/$B" ;; esac          # 7m : chemin relatif = relatif à recomp-sc2/ (pas au dossier courant)
O="$B/runtime-o"; mkdir -p "$O" "$B/vk-entetes"
for d in vulkan vk_video; do [ -e "$B/vk-entetes/$d" ] || ln -s "/usr/include/$d" "$B/vk-entetes/$d"; done
SRC=(src/main.c runtime/image_base.c runtime/sc2_runtime.c runtime/sc2_iat.c runtime/sc2_mpq.c runtime/sc2_iat_p4.c
     runtime/sc2_d3d9.c runtime/sc2_d3d9v.c runtime/sc2_audio_fmod.c runtime/sc2_audio_mix.c runtime/sc2_audio_stb_vorbis.c
     runtime/sc2_audio_mpq.c runtime/sc2_audio_sortie.c runtime/sc2_audio_sortie_compteur.c runtime/sc2_input_sim.c)
INC=(-I"$R/src" -I"$R/runtime" -I"$R/src/lifted" -I"$B/vk-entetes" -I"$R/src/shaders" -I"$HOME/tools/StormLib/src")
ulimit -v 8000000
export CCFLAGS="-D__STORMLIB_NO_STATIC_LINK__ ${INC[*]} -fno-pie -std=gnu17 -w"
for s in "${SRC[@]}"; do
  o="$O/$(basename "$s").o"
  [ "$o" -nt "$R/$s" ] && [ "$o" -nt "$R/runtime/sc2_d3d9v.h" ] && [ "$o" -nt "$R/runtime/sc2_d3d9.h" ] \
    && { [ "$s" != runtime/sc2_d3d9.c ] || [ "$o" -nt "$R/src/shaders/sc2_shaders_spv.h" ]; } && continue   # table SPIR-V (7m)
  printf '%s %s\n' "$R/$s" "$o"
done | xargs -r -n2 -P2 sh -c 'echo "cc $0"; cc $CCFLAGS -c "$0" -o "$1"'
cc -no-pie -Wl,-z,stacksize=8388608 -Wl,-Ttext-segment=0x40000000 "$O"/*.o "$B"/lifted-o/*.o -o "$B/recomp-sc2" \
   -lm "$B/libstorm.a" -lstdc++ "$B/libsc2_win32.a" -lpthread -ldl
echo "relié : $B/recomp-sc2"
