#!/usr/bin/env bash
# garde-lift.sh — passe 10p : garde commune des chaînes APK (scripts/build-apk.sh serveur Linux, android/build-apk-mac.sh M4).
#
#   scripts/garde-lift.sh sources <dossier src/lifted>     avant la compilation
#   scripts/garde-lift.sh lib <libSC2Partout.so> [llvm-nm]  après (la .so réellement emballée)
#   scripts/garde-lift.sh empreinte <dossier src/lifted>   empreinte de la clôture (recomp-sc2/lift-empreinte.txt)
# V0.3 (22/10), cache du lift précompilé (~/dev/sc2-lift-cache, scripts/cache-lift.sh), Mac ET Android :
#   scripts/garde-lift.sh composantes <mac|android> [-D…]  composantes de l'empreinte d'une entrée, pour CE dépôt
#   scripts/garde-lift.sh cache <entrée> [mac|android]     refus nommé d'une entrée incomplète, périmée ou altérée
#                                                          (appelée par cmake/lift_objets.cmake à chaque configuration)
#   scripts/garde-lift.sh caches [racine]                  état de toutes les entrées, et des deux entrées de référence
#
# Leçon 10p : l'APK v7 (M4) compilait une clôture d'AVANT la passe 9n. Elle avait SC2_SAUT et les racines 7j/7q, donc
# elle passait les gardes 7w8/8f, mais 0x00C72900 (YCbCr→BGRA MMX) y était encore un TALON
# « fonction non liftable (NotImplementedError…) ». La mission 2 (première vidéo Theora en jeu) mourait dessus.
# Un talon de FONCTION ENTIÈRE n'a rien à faire dans un APK de jeu. Les RECOMP_TODO d'instruction isolée (rdtsc,
# cpuid…), traités par le runtime, restent permis.
# Contournement de banc seulement : SC2_APK_BANC_LIFT_PERIME=1 (rend 2, l'appelant renomme l'APK).
set -uo pipefail
TALON="fonction non liftable"
# Forme exacte écrite par les lifters (tools/lift/closure-plus.py, scripts/lift-boot.py) : « fonction non liftable (<msg>) ».
# La garde lib cherche cette forme : le runtime compare le texte au littéral nu (sc2_runtime.c, sc2_todo_c), présent dans toute .so.
TALON_LIFT="$TALON ("
mode="${1:-}"; cible="${2:-}"
contourne() { [ "${SC2_APK_BANC_LIFT_PERIME:-0}" = 1 ]; }
refus() {
  if contourne; then echo "GARDE LIFT CONTOURNÉE (SC2_APK_BANC_LIFT_PERIME=1) : $* — APK DE BANC, NE PAS INSTALLER POUR JOUER" >&2; exit 2; fi
  echo "GARDE LIFT : $* — build refusé." >&2; exit 1
}
# Empreinte d'une clôture : sha256 des sha256 de ses fichiers de code (ordre fixe), 16 hex. Linux (sha256sum) et macOS (shasum).
empreinte() {
  local h; h="$(command -v sha256sum >/dev/null && echo sha256sum || echo 'shasum -a 256')"
  (cd "$1" && ls recomp_*.c recomp_sc2_list.h sc2_imports.h 2>/dev/null | LC_ALL=C sort | xargs $h | awk '{print $1}' | $h | cut -c1-16)
}
REF="$(cd "$(dirname "$0")/.." && pwd)/recomp-sc2/lift-empreinte.txt"   # versionnée : la clôture du canon
DEPOT="$(cd "$(dirname "$0")/.." && pwd)"
H="$(command -v sha256sum >/dev/null && echo sha256sum || echo 'shasum -a 256')"

# ---- V0.3 (22/10) : cache du lift précompilé, ~/dev/sc2-lift-cache/<plateforme>-<empreinte>/ (scripts/cache-lift.sh) ----
# composantes <mac|android> [-DNOM=VAL…] : lignes « clé = valeur », triées, de TOUT ce qui change les objets du lift
# transformé. Entrées : les -D (options CMake du lift et chemins d'outils), défauts = banc Mac de référence ; sources lues
# dans CE dépôt ($DEPOT). Clés :
#   format, plateforme, cible (triple)      chaine.cc (chemin | --version), chaine.sdk (Mac) ou chaine.ndk, chaine.cmake,
#   chaine.python (interpréteur des passes)  options.* (toutes, défauts résolus comme cmake/passes_lift.cmake)
#   lift.empreinte (« empreinte » ci-dessus) lift.annexes (autres fichiers de src/lifted : sc2_image.h, rapport…)
#   passes.<chemin> : sha256 de cmake/passes_lift.cmake, cmake/lift_drapeaux.cmake, des scripts qu'ils exécutent ou importent
#                     (appels-directs.py et sa liste d'exclusions, registres-locaux.py, tools/lift/drapeaux-morts.py) et,
#                     pour Android, de android/cmake/cpu_delta.cmake
#   entetes.<chemin> : sha256 de CHAQUE en-tête que voit le compilateur (clang -M sur un recomp_*.c, mêmes drapeaux que le
#                     build : cpu.h, sc2_lifted.h, SDK ou sysroot du NDK, en-têtes de clang) ; dans le dépôt : chemin relatif.
# Les 133 recomp_*.c n'incluent que cpu.h et sc2_lifted.h (grep du 22/10) : un seul -M suffit ; cache-lift.sh vérifie en
# plus, à la construction, que les dépendances RÉELLES de chaque objet (ninja -t deps) sont toutes dans l'empreinte.
# Empreinte d'une entrée : 16 premiers hex du sha256 de ces lignes ; nom du dossier : <plateforme>-<empreinte>.
CACHE_RACINE="${SC2_LIFT_CACHE:-$HOME/dev/sc2-lift-cache}"
OPT_REF="-O2;-fno-strict-aliasing;-fwrapv;-g0;-fno-stack-protector"
interrupteur() { case "$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')" in ON|1|TRUE|YES|Y) echo ON ;; OFF|0|FALSE|NO|N) echo OFF ;; *) return 1 ;; esac; }
drapeaux_type() { case "$1" in Release) echo "-O3 -DNDEBUG" ;; Debug) echo "-g" ;; RelWithDebInfo) echo "-O2 -g -DNDEBUG" ;; MinSizeRel) echo "-Os -DNDEBUG" ;; '') echo "" ;; *) return 1 ;; esac; }
composantes() {
  local plat="${1:-}"; shift
  case "$plat" in mac|android) ;; *) echo "composantes : plateforme « $plat » (mac ou android ; le cache ne sert pas Linux)" >&2; return 64 ;; esac
  local BT=Release OPT="$OPT_REF" AD="" DM=OFF RL="" VIV=OFF X87="" SSE=OFF SEUIL=16 API=android-34
  local CC_="" NDK_="$HOME/tools/android-ndk-r28" PY_="" a n v
  for a in "$@"; do
    case "$a" in -D?*=*) ;; *) echo "composantes : « $a » (attendu -DNOM=VALEUR)" >&2; return 64 ;; esac
    n="${a#-D}"; v="${n#*=}"; n="${n%%=*}"; n="${n%%:*}"
    case "$n" in
      CMAKE_BUILD_TYPE) BT="$v" ;;
      SC2_LIFTED_OPT) OPT="$v" ;;
      SC2_APPELS_DIRECTS) AD="$(interrupteur "$v")" ;;
      SC2_DRAPEAUX_MORTS) DM="$(interrupteur "$v")" ;;
      SC2_REGISTRES_LOCAUX) RL="$(interrupteur "$v")" ;;
      SC2_RL_VIVACITE) VIV="$(interrupteur "$v")" ;;
      SC2_RL_X87) X87="$(interrupteur "$v")" ;;
      SC2_RL_SSE) SSE="$(interrupteur "$v")" ;;
      SC2_RL_SSE_SEUIL) SEUIL="$v" ;;
      ANDROID_PLATFORM) [ "$plat" = android ] || { echo "composantes : ANDROID_PLATFORM pour $plat" >&2; return 64; }; API="$v" ;;
      CMAKE_C_COMPILER) CC_="$v" ;;
      ANDROID_NDK) NDK_="$v" ;;
      Python3_EXECUTABLE) PY_="$v" ;;
      *) echo "composantes : option $n inconnue du cache de lift (refus : elle pourrait changer les objets)" >&2; return 64 ;;
    esac || { echo "composantes : $n=« $v » n'est pas un interrupteur ON/OFF" >&2; return 64; }
  done
  OPT="$(printf '%s' "$OPT" | tr ' ' ';' | sed 's/;;*/;/g; s/^;//; s/;$//')"
  local opti=OFF; case ";$OPT;" in *";-O1;"*|*";-O2;"*|*";-O3;"*|*";-Os;"*) opti=ON ;; esac   # défaut de passes_lift.cmake
  AD="${AD:-$opti}"; RL="${RL:-$opti}"; X87="${X87:-$opti}"
  case "$SEUIL" in ''|*[!0-9]*) echo "composantes : SC2_RL_SSE_SEUIL=« $SEUIL »" >&2; return 64 ;; esac
  case "$API" in android-[0-9]*) ;; *) echo "composantes : ANDROID_PLATFORM=« $API » (android-NN)" >&2; return 64 ;; esac
  local bt; bt="$(drapeaux_type "$BT")" || { echo "composantes : CMAKE_BUILD_TYPE=« $BT » inconnu" >&2; return 64; }
  PY_="${PY_:-$(command -v python3)}"
  local L="$DEPOT/recomp-sc2/src/lifted" R="$DEPOT/recomp-sc2"
  [ -d "$L" ] && ls "$L"/recomp_*.c >/dev/null 2>&1 || { echo "composantes : $L vide ou absent" >&2; return 66; }
  local rep; rep="$(ls "$L"/recomp_*.c | LC_ALL=C sort | head -1)"
  local triple cc mdeps sdk="" ndk="" M
  local -a CFL=()
  # SC2_LIFTED_OPT en mots, comme CMake le passe ; -DSC2_ACCES_VOLATILS : posé par passes_lift.cmake avec les registres locaux
  local IFS_=$IFS; IFS=';'; local -a OPTS=($OPT); IFS=$IFS_
  [ "$RL" = ON ] && OPTS+=(-DSC2_ACCES_VOLATILS)
  if [ "$plat" = mac ]; then
    cc="$(command -v "${CC_:-cc}")" || { echo "composantes : compilateur ${CC_:-cc} introuvable" >&2; return 69; }
    triple="$("$cc" -dumpmachine)" || return 69
    sdk="$(env -u SDKROOT xcrun --show-sdk-path) | MacOSX $(env -u SDKROOT xcrun --show-sdk-version)" || return 69
    CFL=($bt -std=gnu17 -w)
    M="$(env -u SDKROOT -u MACOSX_DEPLOYMENT_TARGET -u CPATH -u C_INCLUDE_PATH "$cc" "${CFL[@]}" ${OPTS[@]+"${OPTS[@]}"} \
         -include "$R/runtime/sc2_lifted.h" -I"$R/src" -I"$R/runtime" -I"$L" -M "$rep")" \
      || { echo "composantes : $cc -M a échoué sur $rep" >&2; return 70; }
  else
    local P="$NDK_/toolchains/llvm/prebuilt/darwin-x86_64"
    cc="${CC_:-$P/bin/clang}"
    [ -x "$cc" ] && [ -f "$NDK_/source.properties" ] || { echo "composantes : NDK absent ($NDK_)" >&2; return 69; }
    triple="aarch64-none-linux-android${API#android-}"
    ndk="$NDK_ | $(sed -n 's/^Pkg.ReleaseName = //p' "$NDK_/source.properties") $(sed -n 's/^Pkg.Revision = //p' "$NDK_/source.properties")"
    # drapeaux du toolchain du NDK (build/cmake/flags.cmake) + norme, PIC, char signé : ceux du build réel (cache-lift.sh
    # le vérifie) ; -no-canonical-prefixes fixe la forme des chemins des en-têtes de clang
    CFL=(--target="$triple" --sysroot="$P/sysroot" -DANDROID -fdata-sections -ffunction-sections -funwind-tables
         -fstack-protector-strong -no-canonical-prefixes -D_FORTIFY_SOURCE=2 $bt -std=gnu17 -fPIC -fsigned-char -w)
    M="$(env -u CPATH -u C_INCLUDE_PATH "$cc" "${CFL[@]}" ${OPTS[@]+"${OPTS[@]}"} -include "$R/runtime/sc2_lifted.h" -include "$R/runtime/cpu.h" \
         -I"$R/src" -I"$R/runtime" -I"$L" -M "$rep")" || { echo "composantes : $cc -M a échoué sur $rep" >&2; return 70; }
  fi
  local ccv; ccv="$("$cc" --version 2>&1 | tr '\n' '|' | sed 's/|$//; s/|/ | /g')"
  local pyv; pyv="$("$PY_" --version 2>&1)" || { echo "composantes : $PY_ inutilisable" >&2; return 69; }
  local cmv; cmv="$(cmake --version 2>/dev/null | head -1)"
  local -a PASSES=(recomp-sc2/cmake/passes_lift.cmake recomp-sc2/cmake/lift_drapeaux.cmake scripts/appels-directs.py
                   scripts/appels-directs-exclus.txt scripts/registres-locaux.py recomp-sc2/tools/lift/drapeaux-morts.py)
  [ "$plat" = android ] && PASSES+=(android/cmake/cpu_delta.cmake)
  {
    echo "format = 1"
    echo "plateforme = $plat"
    echo "cible = $triple"
    echo "chaine.cc = $cc | $ccv"
    [ -n "$sdk" ] && echo "chaine.sdk = $sdk"
    [ -n "$ndk" ] && echo "chaine.ndk = $ndk"
    echo "chaine.cmake = $(command -v cmake) | $cmv"
    echo "chaine.python = $PY_ | $pyv"
    echo "options.CMAKE_BUILD_TYPE = $BT"
    echo "options.SC2_LIFTED_OPT = $OPT"
    echo "options.SC2_APPELS_DIRECTS = $AD"
    echo "options.SC2_DRAPEAUX_MORTS = $DM"
    echo "options.SC2_REGISTRES_LOCAUX = $RL"
    echo "options.SC2_RL_VIVACITE = $VIV"
    echo "options.SC2_RL_X87 = $X87"
    echo "options.SC2_RL_SSE = $SSE"
    echo "options.SC2_RL_SSE_SEUIL = $SEUIL"
    [ "$plat" = android ] && echo "options.ANDROID_PLATFORM = $API"
    echo "lift.empreinte = $(empreinte "$L")"
    echo "lift.annexes = $(cd "$L" && ls | LC_ALL=C sort | grep -v -e '^recomp_.*\.c$' -e '^recomp_sc2_list\.h$' -e '^sc2_imports\.h$' | xargs $H | $H | cut -c1-16)"
    local p
    for p in "${PASSES[@]}"; do
      [ -f "$DEPOT/$p" ] || { echo "composantes : $p absent" >&2; return 66; }
      echo "passes.$p = $($H "$DEPOT/$p" | cut -c1-64)"
    done
    # en-têtes du -M : tout sauf la cible « x.o: » et le recomp_*.c lui-même (couvert par lift.empreinte)
    printf '%s\n' "$M" | sed 's/\\$//' | tr ' ' '\n' | grep -v -e '^$' -e ':$' | grep -vxF "$rep" | LC_ALL=C sort -u \
      | while IFS= read -r f; do
          [ -f "$f" ] || { echo "composantes : en-tête $f introuvable" >&2; exit 66; }
          echo "entetes.${f#$DEPOT/} = $($H "$f" | cut -c1-64)"
        done
  } | LC_ALL=C sort
}
emp_de() { $H | cut -c1-16; }      # empreinte de lignes de composantes (stdin)
# valeur <manifeste> <clé> : première valeur de la clé
valeur() { awk -v k="$2" 'index($0, k " = ") == 1 { print substr($0, length(k) + 4); exit }' "$1"; }
# cache <entrée> [plateforme] : refus nommé de toute composante absente, changée ou inconnue, d'un dossier mal nommé, d'une
# archive altérée. Strict : pas de contournement (une entrée se reconstruit, scripts/cache-lift.sh construire).
cache_controle() {
  local E="${1%/}" attendue="${2:-}" M LIB plat probs="" t
  M="$E/manifeste.txt"; LIB="$E/libsc2_lifted.a"
  [ -f "$M" ] || { echo "GARDE LIFT (cache) : $M absent — entrée refusée." >&2; return 1; }
  [ -f "$LIB" ] || { echo "GARDE LIFT (cache) : $LIB absente — entrée refusée." >&2; return 1; }
  plat="$(valeur "$M" plateforme)"
  [ -n "$plat" ] || { echo "GARDE LIFT (cache) : composante plateforme absente du manifeste $M — entrée refusée." >&2; return 1; }
  [ -z "$attendue" ] || [ "$attendue" = "$plat" ] || { echo "GARDE LIFT (cache) : entrée $plat pour un build $attendue — refusée." >&2; return 1; }
  # entrées du calcul : options et chemins d'outils du manifeste (les VERSIONS de ces outils sont recalculées)
  local -a A=()
  while IFS= read -r l; do A+=("-D${l%% = *}=${l#* = }"); done < <(sed -n 's/^options\.//p' "$M")
  t="$(valeur "$M" chaine.cc)"; [ -n "$t" ] && A+=("-DCMAKE_C_COMPILER=${t%% | *}")
  t="$(valeur "$M" chaine.python)"; [ -n "$t" ] && A+=("-DPython3_EXECUTABLE=${t%% | *}")
  t="$(valeur "$M" chaine.ndk)"; [ -n "$t" ] && A+=("-DANDROID_NDK=${t%% | *}")
  local tmp; tmp="$(mktemp "${TMPDIR:-/tmp}/garde-cache.XXXXXX")" || return 1
  if ! composantes "$plat" ${A[@]+"${A[@]}"} > "$tmp"; then rm -f "$tmp"; echo "GARDE LIFT (cache) : composantes de $E incalculables — entrée refusée." >&2; return 1; fi
  probs="$(awk '
    function cle(l) { i = index(l, " = "); return i ? substr(l, 1, i - 1) : l }
    function val(l) { i = index(l, " = "); return i ? substr(l, i + 3) : "" }
    NR == FNR { a[cle($0)] = val($0); o[++n] = cle($0); next }
    /^(format|plateforme|cible|chaine\.|options\.|lift\.|passes\.|entetes\.)/ {
      k = cle($0); if (k in m) print "composante " k " en double dans le manifeste"; m[k] = val($0) }
    END {
      for (i = 1; i <= n; i++) { k = o[i]
        if (!(k in m)) print "composante " k " absente du manifeste"
        else if (m[k] != a[k]) print "composante " k " changée : manifeste « " m[k] " », sources « " a[k] " »" }
      for (k in m) if (!(k in a)) print "composante " k " inconnue des sources (en-tête, passe ou option disparus ?)"
    }' "$tmp" "$M")"
  local e; e="$plat-$(emp_de < "$tmp")"; local n; n="$(wc -l < "$tmp" | tr -d ' ')"; rm -f "$tmp"
  [ "$(basename "$E")" = "$e" ] || probs="$probs${probs:+$'\n'}dossier $(basename "$E") ≠ empreinte recalculée $e"
  [ "$(valeur "$M" empreinte)" = "$e" ] || probs="$probs${probs:+$'\n'}ligne empreinte « $(valeur "$M" empreinte) » ≠ $e"
  local s; s="$($H "$LIB" | cut -c1-64)"
  [ "$(valeur "$M" lib.sha256)" = "$s" ] || probs="$probs${probs:+$'\n'}archive altérée : sha256 $s ≠ lib.sha256 « $(valeur "$M" lib.sha256) » du manifeste"
  if [ -n "$probs" ]; then
    printf '%s\n' "$probs" | head -12 | sed 's/^/  refus : /' >&2
    echo "GARDE LIFT (cache) : entrée $E refusée ($(printf '%s\n' "$probs" | wc -l | tr -d ' ') problème(s))." >&2
    return 1
  fi
  echo "garde lift (cache) : $E conforme — $n composantes, lift $(valeur "$M" lift.empreinte), options $(valeur "$M" options.SC2_LIFTED_OPT) AD=$(valeur "$M" options.SC2_APPELS_DIRECTS) RL=$(valeur "$M" options.SC2_REGISTRES_LOCAUX) X87=$(valeur "$M" options.SC2_RL_X87), archive $(cut -c1-12 <<<"$s")…"
}
case "$mode" in
empreinte)                       # après un relift du canon : scripts/garde-lift.sh empreinte recomp-sc2/src/lifted > recomp-sc2/lift-empreinte.txt
  empreinte "$cible" ;;
sources)
  [ -d "$cible" ] && ls "$cible"/recomp_*.c >/dev/null 2>&1 || refus "$cible vide ou absent (la .so exige la clôture liftée)"
  grep -rq "SC2_SAUT" "$cible"/recomp_*.c || refus "aucun SC2_SAUT dans $cible (lift périmé, cf. passe 7w8)"
  n=$(grep -l "$TALON" "$cible"/recomp_*.c 2>/dev/null | wc -l | tr -d ' ')
  if [ "$n" != 0 ]; then
    grep -ho "RECOMP_TODO(0x[0-9A-Fa-f]*u\{0,1\}, \"$TALON[^\"]*\"" "$cible"/recomp_*.c | head -10 | sed 's/^/  talon : /' >&2
    refus "$n fichier(s) de $cible portent des talons « $TALON » (clôture d'avant la relève, ex. 0x00C72900 / passe 9n) — resynchroniser src/lifted depuis le canon"
  fi
  # passe 10p : la clôture doit être CELLE du canon (rsqrt 9w, movhps… sont invisibles aux deux tests ci-dessus)
  e="$(empreinte "$cible")"
  if [ -f "$REF" ]; then
    attendu="$(tr -d ' \n' < "$REF")"
    [ "$e" = "$attendu" ] || refus "empreinte de $cible = $e ≠ canon $attendu ($REF) — clôture d'une autre génération ; resynchroniser depuis le canon (ou, après un relift voulu, mettre à jour lift-empreinte.txt)"
  else
    echo "garde lift (sources) : $REF absent — empreinte non comparée" >&2
  fi
  echo "garde lift (sources) : SC2_SAUT présent, 0 talon « $TALON », empreinte $e = canon ($(ls "$cible"/recomp_*.c | wc -l | tr -d ' ') fichiers)"
  ;;
lib)
  [ -f "$cible" ] || refus "$cible absent"
  NM="${3:-llvm-nm}"
  SYMS="$("$NM" --defined-only "$cible")" || refus "$NM a échoué sur $cible"
  for r in L_010CAB80 L_0129DC30 L_00C72900; do       # racines 7j/7q (8f) + convertisseur MMX (9n)
    grep -q " $r\$" <<<"$SYMS" || refus "$r absente de $cible (lift périmé compilé)"
  done
  # le texte du talon « fonction non liftable (…) » n'existe dans la .so que si une fonction entière est restée non liftée
  if LC_ALL=C grep -aqF "$TALON_LIFT" "$cible"; then refus "la chaîne « $TALON » est compilée dans $cible (talon de fonction entière)"; fi
  echo "garde lift (lib) : L_010CAB80 + L_0129DC30 + L_00C72900 définies, 0 talon « $TALON » ; $(grep -c ' L_[0-9A-F]\{8\}$' <<<"$SYMS") fonctions liftées"
  ;;
composantes)
  shift; composantes "$@"; exit $? ;;
cache)
  [ -n "$cible" ] || { echo "usage : $0 cache <entrée du cache> [mac|android]" >&2; exit 64; }
  cache_controle "$cible" "${3:-}"; exit $? ;;
caches)
  r="${cible:-$CACHE_RACINE}"; n=0; ko=0
  for e in "$r"/mac-* "$r"/android-*; do
    [ -d "$e" ] || continue; n=$((n + 1))
    if out="$(cache_controle "$e" 2>&1)"; then echo "  conforme : $(basename "$e")"
    else ko=$((ko + 1)); echo "  REFUSÉE  : $(basename "$e") — $(printf '%s\n' "$out" | sed -n 's/^ *refus : //p' | head -2 | tr '\n' ';')"; fi
  done
  for p in mac android; do               # entrées de référence (options du banc Mac et de la recette APK) des sources actuelles
    if c="$(composantes "$p" 2>/dev/null)"; then
      e="$p-$(printf '%s\n' "$c" | emp_de)"; [ -d "$r/$e" ] && echo "  référence $p : $e (présente)" || echo "  référence $p : $e (absente)"
    else echo "  référence $p : incalculable ($( composantes "$p" 2>&1 >/dev/null | head -1))"; fi
  done
  echo "garde lift (caches) : $n entrée(s) dans $r, $ko refusée(s)"
  [ "$ko" = 0 ] ;;
*) echo "usage : $0 sources <src/lifted> | lib <libSC2Partout.so> [llvm-nm] | empreinte <src/lifted> | composantes <mac|android> [-D…] | cache <entrée> [mac|android] | caches [racine]" >&2; exit 64 ;;
esac
