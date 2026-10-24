#!/bin/bash
# cache-lift.sh — V0.3 (22/10) : lift précompilé partagé entre worktrees, ~/dev/sc2-lift-cache/<plateforme>-<empreinte>/
# (libsc2_lifted.a + manifeste.txt lisible). Le lift transformé (cmake/passes_lift.cmake) se compile UNE fois par
# empreinte ; les worktrees se lient contre l'archive (-DSC2_LIFTED_PREBUILT=…, cmake/lift_objets.cmake).
#   scripts/cache-lift.sh empreinte  <mac|android> [-D…]   nom de l'entrée pour CE dépôt et ces options
#   scripts/cache-lift.sh chemin     <mac|android> [-D…]   archive de l'entrée si elle existe et passe la garde (sinon 1)
#   scripts/cache-lift.sh construire <mac|android> [-D…]   construit l'entrée si elle manque (verrou build-lift), puis
#                                                          imprime l'archive en DERNIÈRE ligne de stdout (journal : stderr)
# -D : options CMake du lift (SC2_LIFTED_OPT, SC2_APPELS_DIRECTS, SC2_REGISTRES_LOCAUX, SC2_RL_*, SC2_DRAPEAUX_MORTS,
# CMAKE_BUILD_TYPE, ANDROID_PLATFORM) et chemins d'outils (CMAKE_C_COMPILER, ANDROID_NDK, Python3_EXECUTABLE) ; défauts :
# banc Mac de référence (-O2;-fno-strict-aliasing;-fwrapv;-g0;-fno-stack-protector, appels directs, registres locaux,
# x87), qui est aussi la recette de l'APK (PROMPT-REPRISE §3). Composantes de l'empreinte : scripts/garde-lift.sh
# composantes (lift, cpu.h, sc2_lifted.h et tous les en-têtes vus par le compilateur, passes et scripts, options, cible,
# chaîne d'outils). Une entrée existante n'est JAMAIS réécrite : refusée par la garde, elle se supprime à la main.
# Construction : dossier .construction-<entrée> de la racine du cache (repris après un échec), configuration CMake avec TOUTES les options explicites
# (environnement CFLAGS/CPATH/SDKROOT/MACOSX_DEPLOYMENT_TARGET retiré), ninja -j${JOBS:-8} (8 au plus) de la seule
# cible sc2_lifted, contrôle des options dans CMakeCache.txt et des dépendances RÉELLES de chaque objet (ninja -t deps :
# tout en-tête hors empreinte → refus), puis entrée posée par mv atomique et relue par la garde. Mac : ≈ 6 min en -O2.
# SC2_LIFT_CACHE : autre racine (défaut ~/dev/sc2-lift-cache).
set -uo pipefail
DEPOT="$(cd "$(dirname "$0")/.." && pwd)"
GARDE="$DEPOT/scripts/garde-lift.sh"
RAC="${SC2_LIFT_CACHE:-$HOME/dev/sc2-lift-cache}"
H="$(command -v sha256sum >/dev/null && echo sha256sum || echo 'shasum -a 256')"
log() { echo "cache-lift : $*" >&2; }
meurt() { echo "cache-lift : $* — arrêt." >&2; exit 1; }
cmd="${1:-}"; plat="${2:-}"
case "$cmd" in empreinte|chemin|construire) ;; *) sed -n '5,8p' "$0" | sed 's/^# *//' >&2; exit 64 ;; esac
case "$plat" in mac|android) ;; *) meurt "plateforme « $plat » (mac ou android)" ;; esac
shift 2
COMP="$("$GARDE" composantes "$plat" "$@")" || meurt "composantes incalculables"
E="$plat-$(printf '%s\n' "$COMP" | $H | cut -c1-16)"
ENT="$RAC/$E"
val() { printf '%s\n' "$COMP" | awk -v k="$1" 'index($0, k " = ") == 1 { print substr($0, length(k) + 4); exit }'; }
present() {   # entrée présente : conforme → chemin de l'archive ; refusée → arrêt (jamais réécrite)
  [ -d "$ENT" ] || return 1
  "$GARDE" cache "$ENT" "$plat" >&2 || meurt "entrée $ENT présente mais refusée par la garde (ci-dessus) : la supprimer (rm -rf) puis relancer"
  echo "$ENT/libsc2_lifted.a"
}
case "$cmd" in
empreinte) echo "$E"; exit 0 ;;
chemin) present || { log "aucune entrée $ENT"; exit 1; }; exit 0 ;;
esac
present && exit 0
# construire : sous le verrou build-lift (une compilation du lift à la fois sur le Mac), puis nouvelle vérification —
# l'entrée a pu être posée par un autre agent pendant l'attente
if [ -z "${CACHE_LIFT_SOUS_VERROU:-}" ]; then
  log "entrée $E absente : construction sous le verrou build-lift"
  CACHE_LIFT_SOUS_VERROU=1 exec "$DEPOT/scripts/verrou.sh" build-lift "$0" construire "$plat" "$@"
fi
present && exit 0
mkdir -p "$RAC" || meurt "racine $RAC"
# dossier de construction nommé par l'empreinte (une construction à la fois : verrou) : après un échec, la relance le
# reprend et ninja ne recompile que ce qui manque ; supprimé au succès, ou s'il vient d'un autre dépôt source
B="$RAC/.construction-$E"; T="$RAC/.$E.tmp-$$"
trap 'rm -rf "$T"' EXIT
[ "$plat" = mac ] && SRC="$DEPOT/recomp-sc2" || SRC="$DEPOT/android"
if [ -f "$B/CMakeCache.txt" ] && [ "$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$B/CMakeCache.txt")" != "$SRC" ]; then
  log "$B vient d'un autre dépôt source : supprimé"; rm -rf "$B"
fi
[ -d "$B" ] && log "reprise de $B (construction précédente interrompue)"
mkdir -p "$B" || meurt "dossier $B"
J="${JOBS:-8}"; [ "$J" -gt 8 ] 2>/dev/null && J=8
t0=$SECONDS
# options : celles de l'EMPREINTE, toutes explicites (aucun défaut CMake ne s'applique)
DOPTS=(); while IFS= read -r l; do DOPTS+=("-D${l%% = *}=${l#* = }"); done < <(printf '%s\n' "$COMP" | sed -n 's/^options\.//p')
PY="$(val chaine.python)"; PY="${PY%% | *}"
CC_="$(val chaine.cc)"; CC_="${CC_%% | *}"
if [ "$plat" = mac ]; then
  OUTILS=(-DCMAKE_C_COMPILER="$CC_")
else
  NDK="$(val chaine.ndk)"; NDK="${NDK%% | *}"
  OUTILS=(-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a)
fi
log "configuration $SRC → $B (${DOPTS[*]})"
env -u CFLAGS -u CPPFLAGS -u LDFLAGS -u CPATH -u C_INCLUDE_PATH -u SDKROOT -u MACOSX_DEPLOYMENT_TARGET \
  cmake -S "$SRC" -B "$B" -G Ninja "${OUTILS[@]}" "${DOPTS[@]}" -DPython3_EXECUTABLE="$PY" -DSC2_LIFTED_PREBUILT= \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "$B/configure.log" 2>&1 || { tail -20 "$B/configure.log" >&2; meurt "configuration (journal $B/configure.log)"; }
for l in $(printf '%s\n' "$COMP" | sed -n 's/^options\.\([A-Za-z0-9_]*\) = .*/\1/p'); do   # options vues par CMake
  [ "$l" = ANDROID_PLATFORM ] && continue      # variable du toolchain, reprise par la cible (cible = …androidNN)
  vu="$(sed -n "s/^$l:[A-Z]*=//p" "$B/CMakeCache.txt" | head -1)"
  [ "$vu" = "$(val "options.$l")" ] || meurt "option $l : CMakeCache.txt « $vu » ≠ empreinte « $(val "options.$l") »"
done
log "configuration : $((SECONDS - t0)) s ; compilation du lift (ninja -j$J sc2_lifted)"
env -u CFLAGS -u CPPFLAGS -u CPATH -u C_INCLUDE_PATH -u SDKROOT -u MACOSX_DEPLOYMENT_TARGET \
  ninja -C "$B" -j"$J" sc2_lifted > "$B/build.log" 2>&1 || { tail -20 "$B/build.log" >&2; meurt "compilation (journal $B/build.log)"; }
LIB="$B/libsc2_lifted.a"
[ -f "$LIB" ] || meurt "$LIB absente après la compilation"
AR="$(sed -n 's/^CMAKE_AR:FILEPATH=//p' "$B/CMakeCache.txt")"   # llvm-ar du NDK : format GNU, mal lu par l'ar de macOS
NOBJ="$("${AR:-ar}" t "$LIB" | grep -c '\.o$')"
# dépendances réelles de chaque objet du lift : tout chemin hors build dérivé (lift transformé, cpu.h delta-propre) et hors
# src/lifted (lift.empreinte, lift.annexes) doit être une composante entetes.* de l'empreinte
VERIF="$(ninja -C "$B" -t deps | python3 -I -c '
import os, sys
depot, build, nobj = sys.argv[1], sys.argv[2], int(sys.argv[3])
cles = {l.split(" = ", 1)[0] for l in open(sys.argv[4], encoding="utf-8")}
hors, vus, objets, cur = set(), set(), 0, False
for l in sys.stdin:
    if not l.startswith(" "):
        cur = l.startswith("CMakeFiles/sc2_lifted.dir/")
        objets += cur
        continue
    if not cur or not l.strip():
        continue
    p = os.path.normpath(os.path.join(build, l.strip()))
    if p.startswith(build + "/"):
        r = p[len(build) + 1:]
        if r.startswith("lifted-") or r == "cpu-delta/cpu.h":
            continue
    if p.startswith(depot + "/recomp-sc2/src/lifted/"):
        continue
    k = "entetes." + (p[len(depot) + 1:] if p.startswith(depot + "/") else p)
    vus.add(k)
    if k not in cles:
        hors.add(p)
if objets != nobj:
    print("ÉCHEC %d objets dans ninja -t deps, %d dans l archive" % (objets, nobj)); sys.exit(1)
if hors:
    print("ÉCHEC en-têtes réels hors empreinte : " + " ".join(sorted(hors)[:8])); sys.exit(1)
print("%d en-têtes réels sur %d objets, tous dans l empreinte" % (len(vus), objets))
' "$DEPOT" "$B" "$NOBJ" <(printf '%s\n' "$COMP"))" || meurt "dépendances : $VERIF"
log "dépendances : $VERIF"
CMDE="$(python3 -I -c '
import json, sys
for e in json.load(open(sys.argv[1])):
    if "/sc2_lifted.dir/" in e.get("output", e.get("command", "")) or "/sc2_lifted.dir/" in e.get("command", ""):
        print(e["command"].replace(sys.argv[2], "<build>").replace(sys.argv[3], "<depot>")); break
' "$B/compile_commands.json" "$B" "$DEPOT")"
rm -rf "$T"; mkdir -p "$T" || meurt "dossier $T"
cp "$LIB" "$T/libsc2_lifted.a" || meurt "copie de l'archive"
S="$($H "$T/libsc2_lifted.a" | cut -c1-64)"
{
  echo "# manifeste d'une entrée du cache de lift (V0.3, scripts/cache-lift.sh) — contrôle : scripts/garde-lift.sh cache <entrée>"
  echo "# empreinte = 16 premiers hex du sha256 des lignes de composantes ci-dessous (format … passes.*) ; info.* hors empreinte"
  printf '%s\n' "$COMP"
  echo "empreinte = $E"
  echo "lib.sha256 = $S"
  echo "lib.taille = $(stat -f %z "$T/libsc2_lifted.a" 2>/dev/null || stat -c %s "$T/libsc2_lifted.a")"
  echo "info.objets = $NOBJ"
  echo "info.dependances = $VERIF"
  echo "info.construit = $(date '+%Y-%m-%d %H:%M:%S') en $((SECONDS - t0)) s (ninja -j$J)"
  echo "info.depot = $DEPOT ($(git -C "$DEPOT" rev-parse --short HEAD 2>/dev/null) ; $(git -C "$DEPOT" status --porcelain 2>/dev/null | wc -l | tr -d ' ') fichier(s) modifié(s))"
  echo "info.commande = $CMDE"
} > "$T/manifeste.txt"
mv "$T" "$ENT" || meurt "mv $T → $ENT"
rm -rf "$B"
"$GARDE" cache "$ENT" "$plat" >&2 || meurt "entrée $ENT posée mais refusée par la garde"
log "entrée $ENT posée en $((SECONDS - t0)) s ($NOBJ objets, $(du -h "$ENT/libsc2_lifted.a" | cut -f1))"
echo "$ENT/libsc2_lifted.a"
