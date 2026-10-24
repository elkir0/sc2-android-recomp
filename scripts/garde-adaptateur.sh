#!/bin/bash
# garde-adaptateur.sh — V2.5a (23/10) : garde de l'adaptateur historique sur un run du canon (appelé par banc-mac.sh
# defaut ; lisible à la main sur tout dossier de run).
#   scripts/garde-adaptateur.sh <run.log> [<Variables.txt écrit par le jeu>]
# Ne juge que le canon : SC2_GPU_PROFIL absente, vide ou « historique » ET SC2_DDRAW absente ou vide (environnement de
# l'appelant, celui du run). Sinon : « adaptateur : non contrôlé (…) », code 0.
# Exige, en historique, l'adaptateur d'avant V2.5a à l'octet sur ce que le jeu en tire :
#   - run.log : au moins une ligne « [DDRAW] DirectDrawCreateEx depuis 0x00A010AA → DDERR_NODIRECTDRAWHW » (la sonde de
#     mémoire vidéo L_00A00FC0 refusée) et AUCUNE mention d'« IDirectDraw7 » (objet factice, méthodes) ;
#   - Variables.txt écrit par le jeu, si un 2e argument est donné (le canon sans BANC_VARIABLES) : présent, avec
#     GraphicsOptionTextureQuality[2]=0 (textures suggérées Low : VRAM 0) et lastDeviceId=1026 (8600 GT annoncée).
# Pourquoi : un mutant qui sert DirectDraw en historique garde les empreintes du canon (images 1, 10, 100-400) ; seuls
# ces deux témoins le voient (revue V2.5a du 23/10).
# Sortie : UNE ligne « adaptateur : OK|ÉCHEC|non contrôlé — … » ; code 0 (OK, non contrôlé), 5 (ÉCHEC), 2 (usage).
set -u
L=${1:-}; V=${2:-}
[ -n "$L" ] || { echo "usage : $0 <run.log> [<Variables.txt du jeu>]" >&2; exit 2; }
P=${SC2_GPU_PROFIL:-}; DD=${SC2_DDRAW:-}
if [ -n "$DD" ] || { [ -n "$P" ] && [ "$P" != historique ]; }; then
    echo "adaptateur : non contrôlé (SC2_GPU_PROFIL=${P:-absente}, SC2_DDRAW=${DD:-absente} : seul le canon historique est jugé)"
    exit 0
fi
[ -f "$L" ] || { echo "adaptateur : ÉCHEC — $L absent"; exit 5; }
E=()
n_refus=$(grep -aFc '[DDRAW] DirectDrawCreateEx depuis 0x00A010AA → DDERR_NODIRECTDRAWHW' "$L")
n_dd7=$(grep -aFc 'IDirectDraw7' "$L")
[ "$n_refus" -ge 1 ] || E+=("aucun refus DDERR_NODIRECTDRAWHW depuis 0x00A010AA dans run.log")
[ "$n_dd7" -eq 0 ] || E+=("$n_dd7 ligne(s) « IDirectDraw7 » dans run.log (DirectDraw servi)")
VT=""
if [ -n "$V" ]; then
    if [ -f "$V" ]; then
        tq=$(tr -d '\r' < "$V" | sed -n 's/^GraphicsOptionTextureQuality\[2\]=//p' | head -1)
        ld=$(tr -d '\r' < "$V" | sed -n 's/^lastDeviceId=//p' | head -1)
        [ "$tq" = 0 ] || E+=("Variables.txt du jeu : GraphicsOptionTextureQuality[2]=${tq:-absente} (attendu 0)")
        [ "$ld" = 1026 ] || E+=("Variables.txt du jeu : lastDeviceId=${ld:-absente} (attendu 1026)")
        VT=" ; Variables.txt du jeu : GraphicsOptionTextureQuality[2]=${tq:-absente}, lastDeviceId=${ld:-absente}"
    else
        E+=("Variables.txt du jeu absent ($V)")
    fi
fi
if [ ${#E[@]} -eq 0 ]; then
    echo "adaptateur : OK — historique : DirectDraw refusé ×$n_refus depuis 0x00A010AA, 0 IDirectDraw7$VT"
    exit 0
fi
printf 'adaptateur : ÉCHEC — %s' "${E[0]}"; for e in "${E[@]:1}"; do printf ' ; %s' "$e"; done; echo
exit 5
