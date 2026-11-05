#!/bin/bash
# worktree-neuf.sh — V0.3 (22/10) : worktree d'agent prêt à construire, lift lié depuis le cache partagé. Mesuré le
# 22/10 : configuration 4-5 s, runtime + édition de liens 9-11 s (au lieu de 6 à 10 min avec la compilation du lift).
#   scripts/worktree-neuf.sh <branche> [dossier]
#     dossier par défaut : <parent du dépôt principal>/sc2-wt-<sujet>, sujet = branche sans préfixe (claude/) ni date
#     (20261022-) : claude/20261022-cache-lift → ~/dev/sc2-wt-cache-lift
#   WORKTREE_BASE=<réf.>    base d'une branche NOUVELLE (défaut main) ; une branche existante est extraite telle quelle
#   WORKTREE_CONSTRUIRE=1   enchaîne ninja -C <build> recomp-sc2 (runtime + édition de liens, verrou edl), chronométré
# Étapes :
#   1. git worktree add depuis le dépôt principal (celui du git-common-dir, ou SC2_DEPOT_PRINCIPAL) ;
#   2. cp -c (clones APFS : inodes DISTINCTS, aucun octet recopié) de recomp-sc2/src/lifted/* et de
#      src/shaders/sc2_shaders_spv.h du dépôt principal — jamais de lien dur, qu'une réécriture en place propagerait à tous
#      les worktrees ; contrôle : inode différent du principal, empreinte du lift identique ;
#   3. entrée du cache Mac de référence (scripts/cache-lift.sh construire mac, DEPUIS LES SOURCES DU NOUVEAU worktree :
#      construite si elle manque, sous le verrou build-lift), puis configuration du build du banc
#      recomp-sc2/build-mac-O2radx contre elle (-DSC2_LIFTED_PREBUILT ; options du banc, PROMPT-REPRISE §3).
# Pack SPIR-V (extraction/sc2-spirv.pak, 1,6 Go) : AUCUN clone. Les scripts de banc posent
# SC2_INSTALL=${SC2_EXTRACTION:-<dépôt principal>/extraction}/install, et le runtime ouvre <parent de SC2_INSTALL>/sc2-spirv.pak
# en mmap lecture seule (runtime/sc2_spv_pack.c) : le pack de main sert tel quel (« [SPV] pack SPIR-V externe ouvert :
# ~/sc2-partout/extraction/sc2-spirv.pak » au run du canon depuis un worktree, 22/10). Un chantier qui
# change le traducteur produit son pack dans son worktree et le désigne par SC2_SPIRV_PACK.
# Budget (mesuré le 22/10) : ≈ 60 Mo de checkout (lift et table SPIR-V clonés : blocs partagés tant qu'ils ne sont pas
# réécrits) + ≈ 0,27 Go de build (0,5 Go avec runner-difftest ; le lift reste dans le cache). À la fin : git worktree
# remove, puis suppression du dossier de build s'il reste.
set -uo pipefail
meurt() { echo "worktree-neuf : $* — arrêt." >&2; exit 1; }
log() { echo "worktree-neuf : $*" >&2; }
br="${1:-}"; [ -n "$br" ] || { sed -n '4,9p' "$0" | sed 's/^# *//' >&2; exit 64; }
ICI="$(cd "$(dirname "$0")/.." && pwd)"
COMMUN="$(git -C "$ICI" rev-parse --path-format=absolute --git-common-dir)" || meurt "pas de dépôt git autour de $ICI"
PRINC="${SC2_DEPOT_PRINCIPAL:-$(dirname "$COMMUN")}"
s="${br##*/}"; s="${s#[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]-}"
W="${2:-$(dirname "$PRINC")/sc2-wt-$s}"
[ -e "$W" ] && meurt "$W existe déjà"
L="$PRINC/recomp-sc2/src/lifted"; SPV="$PRINC/recomp-sc2/src/shaders/sc2_shaders_spv.h"
[ -d "$L" ] && ls "$L"/recomp_*.c >/dev/null 2>&1 || meurt "lift absent de $L (dépôt principal)"
t0=$SECONDS
# 1. worktree
if git -C "$PRINC" show-ref --verify --quiet "refs/heads/$br"; then
  git -C "$PRINC" worktree add "$W" "$br" >&2 || meurt "git worktree add $W $br"
else
  git -C "$PRINC" worktree add -b "$br" "$W" "${WORKTREE_BASE:-main}" >&2 || meurt "git worktree add -b $br $W ${WORKTREE_BASE:-main}"
fi
W="$(cd "$W" && pwd)"
[ -f "$W/recomp-sc2/cmake/lift_objets.cmake" ] && [ -x "$W/scripts/cache-lift.sh" ] \
  || meurt "branche $br sans V0.3 (recomp-sc2/cmake/lift_objets.cmake, scripts/cache-lift.sh) : base trop ancienne"
# 2. clones APFS du lift et de la table SPIR-V
mkdir -p "$W/recomp-sc2/src/lifted" "$W/recomp-sc2/src/shaders"
cp -c "$L"/* "$W/recomp-sc2/src/lifted/" || meurt "cp -c du lift (APFS requis : jamais de lien dur)"
if [ -f "$SPV" ]; then cp -c "$SPV" "$W/recomp-sc2/src/shaders/" || meurt "cp -c de $SPV"
else log "ATTENTION : $SPV absent — le runtime compilera sans shaders (écran noir silencieux)"; fi
f0="$(ls "$L"/recomp_*.c | LC_ALL=C sort | head -1)"; f1="$W/recomp-sc2/src/lifted/$(basename "$f0")"
i0="$(stat -f %i "$f0")"; i1="$(stat -f %i "$f1")"
[ "$i0" != "$i1" ] && [ "$(stat -f %l "$f1")" = 1 ] || meurt "$(basename "$f0") : inode $i1 partagé avec le principal ($i0)"
e0="$("$W/scripts/garde-lift.sh" empreinte "$L")"; e1="$("$W/scripts/garde-lift.sh" empreinte "$W/recomp-sc2/src/lifted")"
[ "$e0" = "$e1" ] || meurt "empreinte du lift cloné $e1 ≠ principal $e0"
log "lift cloné : $(ls "$W/recomp-sc2/src/lifted" | wc -l | tr -d ' ') fichiers, empreinte $e1, $(basename "$f0") inode $i1 (principal $i0)"
# 3. cache Mac de référence puis configuration du build du banc
LIB="$("$W/scripts/cache-lift.sh" construire mac | tail -1)"
[ -f "$LIB" ] || meurt "pas d'archive du cache Mac (ci-dessus)"
B="$W/recomp-sc2/build-mac-O2radx"; mkdir -p "$B"
cmake -S "$W/recomp-sc2" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      "-DSC2_LIFTED_OPT=-O2;-fno-strict-aliasing;-fwrapv;-g0;-fno-stack-protector" -DSC2_REGISTRES_LOCAUX=ON \
      -DSC2_APPELS_DIRECTS=ON -DSC2_LIFTED_PREBUILT="$LIB" > "$B/configure.log" 2>&1 \
  || { tail -20 "$B/configure.log" >&2; meurt "configuration CMake (journal $B/configure.log)"; }
log "configuré en $((SECONDS - t0)) s contre $(dirname "$LIB")"
if [ "${WORKTREE_CONSTRUIRE:-0}" = 1 ]; then
  t1=$SECONDS
  ninja -C "$B" -j"${JOBS:-8}" recomp-sc2 > "$B/build.log" 2>&1 || { tail -20 "$B/build.log" >&2; meurt "ninja recomp-sc2 (journal $B/build.log)"; }
  log "construit en $((SECONDS - t1)) s : $B/recomp-sc2"
fi
echo "worktree $W (branche $br) ; build : ninja -C $B recomp-sc2 (toujours nommer la cible)"
