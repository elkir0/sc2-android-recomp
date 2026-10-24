#!/bin/bash
# paquet-mac.sh — V3, lot M9 (24/10) : assemble le paquet distribuable SC2Partout.app (Release) à partir d'un build
# Mac du dépôt. Le paquet est PRIVÉ (code recompilé du jeu) : jamais dans le dépôt, jamais hors de ~/code/sc2-apk/
# (v3-conception-B.md §3.4). Signature ad hoc, sans sandbox ni notarisation (décisions D8/D9, à signer par le mainteneur).
#   scripts/paquet-mac.sh <build> --dest <dossier sous ~/code/sc2-apk/> [--mvk <fichier> <sha256>]
# Refus nommés (code ≠ 0) :
#   65 destination hors ~/code/sc2-apk/ (après normalisation : ~, ., .., lien symbolique résolu) ;
#   66 lift -O0 : options.SC2_LIFTED_OPT du manifeste du répertoire de SC2_LIFTED_PREBUILT (CMakeCache.txt du build) ;
#   67 src/shaders/sc2_shaders_spv.h absent du dépôt source (CMAKE_HOME_DIRECTORY) : SPIR-V non compilé ;
#   68 sha256 de libMoltenVK.dylib ≠ e21bcead…d542 (realpath de /opt/homebrew/lib/libMoltenVK.dylib), sauf --mvk
#      <fichier> <sha256> (autre MoltenVK référencé : le sha exigé devient celui donné) ;
#   69 autre (build absent, cache illisible, ninja, copie, codesign, plutil, mv).
# Assemblage : ninja -C <build> sc2partout-app (clone APFS de l'exécutable, Info.plist configuré, polices et
# inits-stricts en LIENS du dépôt), puis staging <dest>/.SC2Partout.app.tmp/ et copies RÉELLES : exécutable (cp -c),
# les 6 polices Liberation TTF + licence (android/assets/polices), recomp-sc2/inits-stricts.txt,
# Frameworks/libMoltenVK.dylib. SC2Partout.app.sha256 (exécutable + dylib, FICHIER VOISIN du paquet, calculé APRÈS
# codesign qui re-signe l'exécutable) ; codesign ad hoc (--force --deep -s -) puis --verify --deep --strict ;
# plutil -lint ; lsregister -f (risque R19 :
# LaunchServices peut garder le LSEnvironment d'une version précédente du paquet — re-vérifier depuis le Finder).
# Sans --dest : refuse (65) — le paquet distribuable ne vit jamais dans le dépôt ; le paquet de DÉVELOPPEMENT
# <build>/SC2Partout.app, lui, reste fait par ninja sc2partout-app.
set -u

REFUS_DEST=65
REFUS_OPT=66
REFUS_SPV=67
REFUS_MVK=68
REFUS_AUTRE=69

MVK_SHA_DEFAUT="e21bceade8d0638bb2bc041f3166fbd54da19fa0958d8bbaff584afd7958d542"
MVK_LIEN="/opt/homebrew/lib/libMoltenVK.dylib"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"

refus() { echo "paquet-mac : REFUS — $*" >&2; exit "$1"; }

# ---- arguments -----------------------------------------------------------------------------------------------------

B=""
DEST=""
MVK_FICHIER="$MVK_LIEN"
MVK_SHA="$MVK_SHA_DEFAUT"
while [ $# -gt 0 ]; do
    case "$1" in
        --dest)
            [ $# -ge 2 ] || refus "$REFUS_AUTRE" "--dest <dossier> : argument absent"
            DEST="$2"; shift 2 ;;
        --mvk)
            [ $# -ge 3 ] || refus "$REFUS_AUTRE" "--mvk <fichier> <sha256> : arguments absents"
            MVK_FICHIER="$2"; MVK_SHA="$3"; shift 3 ;;
        -h|--help)
            sed -n '2,24p' "$0" | sed 's/^# *//'; exit 64 ;;
        *)
            if [ -z "$B" ]; then B="$1"; shift; else refus "$REFUS_AUTRE" "argument « $1 » inconnu"; fi ;;
    esac
done
[ -n "$B" ] || { sed -n '5,24p' "$0" | sed 's/^# *//'; exit 64; }
[ -n "$DEST" ] || refus "$REFUS_DEST" "--dest absent : le paquet distribuable ne vit jamais dans le dépôt"

# <build> en absolu, sans exiger qu'il existe déjà (le refus de destination précède la vérification du build).
case "$B" in
    /*) ;;
    *)  B="$(cd "$(dirname "$B")" 2>/dev/null && pwd)/$(basename "$B")" ;;
esac

# ---- refus nommés (avant toute écriture) ---------------------------------------------------------------------------

# destination : parent de SC2Partout.app, obligatoirement sous ~/code/sc2-apk/ après normalisation complète
# (~ du shell, composants « . » et « .. », liens symboliques résolus par mkdir -p + cd -P).
DEST="${DEST/#\~\//$HOME/}"
case "$DEST" in
    /*) ;;
    *)  DEST="$PWD/$DEST" ;;
esac
# pré-contrôle LEXICAL : rien n'est écrit (même un mkdir) hors de ~/code/sc2-apk/.
case "$DEST" in
    "$HOME/code/sc2-apk"|"$HOME/code/sc2-apk"/*) ;;
    *) refus "$REFUS_DEST" "destination $DEST hors de ~/code/sc2-apk/ (le paquet est privé)" ;;
esac
mkdir -p "$DEST" 2>/dev/null || refus "$REFUS_DEST" "destination $DEST : dossier impossible à créer"
DEST="$(cd -P "$DEST" 2>/dev/null && pwd)" || refus "$REFUS_DEST" "destination $DEST introuvable"
case "$DEST" in
    "$HOME/code/sc2-apk"|"$HOME/code/sc2-apk"/*) ;;
    *) refus "$REFUS_DEST" "destination $DEST hors de ~/code/sc2-apk/ (le paquet est privé)" ;;
esac

# lift -O0 : manifeste du prébuilt pointé par le cache du build.
[ -d "$B" ] || refus "$REFUS_AUTRE" "build $B absent (cmake -B $B)"
CACHE="$B/CMakeCache.txt"
[ -f "$CACHE" ] || refus "$REFUS_AUTRE" "$CACHE absent (build pas configuré)"
PREBUILT=$(sed -n 's/^SC2_LIFTED_PREBUILT:FILEPATH=//p' "$CACHE" | head -1)
[ -n "$PREBUILT" ] || refus "$REFUS_OPT" "SC2_LIFTED_PREBUILT absent de CMakeCache.txt : lift inconnu (-O0 suspect)"
MANIFESTE="$(dirname "$PREBUILT")/manifeste.txt"
[ -f "$MANIFESTE" ] || refus "$REFUS_AUTRE" "$MANIFESTE absent : lift inconnu"
OPTS=$(sed -n 's/^options\.SC2_LIFTED_OPT *= *//p' "$MANIFESTE" | head -1)
[ -n "$OPTS" ] || refus "$REFUS_OPT" "options.SC2_LIFTED_OPT absente de $MANIFESTE : lift inconnu (-O0 suspect)"
case ";$OPTS;" in
    *";-O0;"*) refus "$REFUS_OPT" "lift -O0 dans options.SC2_LIFTED_OPT ($MANIFESTE) : paquet Release interdit" ;;
esac

# SPIR-V : le banc ne compile pas les shaders, il lit sc2_shaders_spv.h. SRC = racine CMake (recomp-sc2/) ;
# DEPOT = racine du dépôt (polices de android/assets).
SRC=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$CACHE" | head -1)
[ -n "$SRC" ] || refus "$REFUS_AUTRE" "CMAKE_HOME_DIRECTORY absent de CMakeCache.txt"
[ -f "$SRC/src/shaders/sc2_shaders_spv.h" ] \
    || refus "$REFUS_SPV" "$SRC/src/shaders/sc2_shaders_spv.h absent (SPIR-V non compilé)"
DEPOT=$(git -C "$SRC" rev-parse --show-toplevel 2>/dev/null)
[ -n "$DEPOT" ] || refus "$REFUS_AUTRE" "racine du dépôt introuvable depuis $SRC"

# MoltenVK : référence sha256 de la conception, sauf --mvk.
MVK_REEL=$(realpath "$MVK_FICHIER" 2>/dev/null) || refus "$REFUS_MVK" "libMoltenVK.dylib introuvable ($MVK_FICHIER)"
MVK_OBTENU=$(shasum -a 256 "$MVK_REEL" | awk '{print $1}')
[ "$MVK_OBTENU" = "$MVK_SHA" ] || refus "$REFUS_MVK" "sha256 de $MVK_REEL : $MVK_OBTENU ≠ $MVK_SHA (référence attendue — --mvk <fichier> <sha> pour une autre)"

# ---- assemblage ---------------------------------------------------------------------------------------------------

echo "paquet-mac : build $B → $DEST/SC2Partout.app"
ninja -C "$B" sc2partout-app || refus "$REFUS_AUTRE" "ninja sc2partout-app a échoué"
APP="$B/SC2Partout.app"

# staging DANS la destination : rien n'est jamais écrit hors de ~/code/sc2-apk/, et le mv final est atomique.
STAGING="$DEST/.SC2Partout.app.tmp"
rm -rf "$STAGING"
mkdir -p "$STAGING/Contents/MacOS" "$STAGING/Contents/Resources/polices" "$STAGING/Contents/Frameworks" \
    || refus "$REFUS_AUTRE" "mkdir $STAGING"

# 1. exécutable : copie réelle (clone APFS si possible : même contenu, aucun lien vers le dépôt).
cp -c "$APP/Contents/MacOS/recomp-sc2" "$STAGING/Contents/MacOS/recomp-sc2" 2>/dev/null \
    || cp "$APP/Contents/MacOS/recomp-sc2" "$STAGING/Contents/MacOS/recomp-sc2" \
    || refus "$REFUS_AUTRE" "copie de l'exécutable"

# 2. Info.plist : celui du build, configuré par mac.cmake (révision git, OS minimum).
cp "$B/SC2Partout-Info.plist" "$STAGING/Contents/Info.plist" || refus "$REFUS_AUTRE" "copie de l'Info.plist"

# 3. polices : les 6 TTF Liberation + la licence, COPIES RÉELLES (le lien du paquet de développement désigne le dépôt).
SRC_POLICES="$DEPOT/android/assets/polices"
[ -d "$SRC_POLICES" ] || refus "$REFUS_AUTRE" "polices $SRC_POLICES absentes du dépôt"
for f in LICENCE-Liberation.txt LiberationMono-Regular.ttf LiberationSans-Bold.ttf \
         LiberationSans-BoldItalic.ttf LiberationSans-Italic.ttf LiberationSans-Regular.ttf \
         LiberationSerif-Regular.ttf; do
    cp "$SRC_POLICES/$f" "$STAGING/Contents/Resources/polices/$f" || refus "$REFUS_AUTRE" "copie de la police $f"
done

# 4. inits-stricts.txt : copie réelle (à la racine CMake, à côté de CMakeLists.txt).
cp "$SRC/inits-stricts.txt" "$STAGING/Contents/Resources/inits-stricts.txt" \
    || refus "$REFUS_AUTRE" "copie de inits-stricts.txt"

# 5. MoltenVK : copie réelle du realpath, sha contrôlé avant.
cp "$MVK_REEL" "$STAGING/Contents/Frameworks/libMoltenVK.dylib" || refus "$REFUS_AUTRE" "copie de libMoltenVK.dylib"


echo "paquet-mac : codesign ad hoc (sans sandbox ni notarisation — décisions D8/D9)"
codesign --force --deep -s - "$STAGING" || refus "$REFUS_AUTRE" "codesign"
codesign --verify --deep --strict "$STAGING" 2>&1 || refus "$REFUS_AUTRE" "codesign --verify"
plutil -lint "$STAGING/Contents/Info.plist" || refus "$REFUS_AUTRE" "plutil -lint"

# finalisation : mv atomique (un lecteur ne voit jamais un paquet à moitié signé).
rm -rf "$DEST/SC2Partout.app"
mv "$STAGING" "$DEST/SC2Partout.app" || refus "$REFUS_AUTRE" "mv $STAGING → $DEST/SC2Partout.app (à finir à la main)"

# 7. SHA256SUMS.txt, FICHIER VOISIN du paquet (pas dedans : codesign --deep refuse tout fichier non scellé à la
#    racine du bundle, et une somme DANS le paquet serait fausse au moment où elle serait scellée — codesign --force
#    RE-SIGNE l'exécutable, qui contient la signature dans ses propres octets : une somme calculée avant serait
#    périmée). Chemins relatifs à la racine du .app : « shasum -a 256 -c SC2Partout.app.sha256 » depuis $DEST
#    (ou depuis le dossier copié en USB) vérifie la copie.
(
    cd "$DEST" || exit 1
    shasum -a 256 SC2Partout.app/Contents/MacOS/recomp-sc2 SC2Partout.app/Contents/Frameworks/libMoltenVK.dylib \
        > SC2Partout.app.sha256
) || refus "$REFUS_AUTRE" "SC2Partout.app.sha256"

echo "paquet-mac : inscription à LaunchServices (risque R19 : LSEnvironment du Finder à re-vérifier)"
"$LSREGISTER" -f "$DEST/SC2Partout.app" || refus "$REFUS_AUTRE" "lsregister -f"
echo "paquet-mac : $DEST/SC2Partout.app prêt — shasum -a 256 -c SC2Partout.app.sha256 pour vérifier une copie USB"
