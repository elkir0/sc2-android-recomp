#!/usr/bin/env bash
# valider-apk.sh — contrôle d'un APK SC2 Partout ; sort PASS ou FAIL (code 0 / 1). Linux (serveur Linux) et macOS (M4).
#
#   scripts/valider-apk.sh [apk]            défaut : android/build/sc2-partout.apk
#
# Contrôles :
#   1. aapt2 dump badging : paquet fr.sc2partout, minSdk ≥ 26, native-code arm64-v8a, activité lanceur (fr.sc2partout.LanceurActivity, défaut) ;
#   2. lib arm64-v8a embarquée : ELF AArch64, symboles ANativeActivity_onCreate et android_main ;
#   3. signature (apksigner verify) et alignement (zipalign -P 16) ;
#   4. (V0.2, 22/10) clé de signature = celle que VOUS épinglez : un seul signataire, empreinte SHA-256 du certificat
#      = SC2_APK_CLE_SHA256 (apksigner verify --print-certs sur un APK signé par votre clé de debug). Une autre clé ne
#      met pas à jour l'APK déjà installé (désinstallation = progression et sc2.env perdus). Sans SC2_APK_CLE_SHA256 :
#      contrôle 4 signalé non épinglé (INFO), sans échec ;
#   5. (V0.2) lift optimisé, jamais -O0 (android/build-apk-mac.sh est en -O0 par défaut) :
#      a. marqueur assets/sc2-build.txt (ligne « lift_opt=… »), s'il existe — écrit au build (ligne proposée pour
#         android/build-apk-mac.sh : printf 'lift_opt=%s\n…' "$LIFT_OPT" > "$ASSETS/sc2-build.txt") ;
#      b. sinon, preuve par les symboles : en -O0, les aides SC2_EN_LIGNE de recomp-sc2/runtime/cpu.h (rd32, push32,
#         flags_add…) ne sont pas intégrées et chaque fichier lifté en garde une copie LOCALE dans .symtab ; dès -O1,
#         always_inline les fait disparaître. Mesuré le 22/10 (NDK r28, aarch64-android34) : lift entier en -O0 =
#         4 272 symboles locaux de ce nom (133 fichiers × 11 à 40 aides) ; v26 et v30 (lift -O2) = 0. Seuil : 10.
#         La taille ne prouve rien : .text du seul lift en -O0 = 113,2 Mio, .text ENTIER de v30 (-O2 + runtime) = 95,5 Mio ;
#      c. ni marqueur ni .symtab (lib strippée) : preuve absente → FAIL (SC2_APK_LIFT_NON_PROUVE=1 l'accepte).
#
# Outillage : build-tools dans $ANDROID_SDK_ROOT, sinon ${SC2_TOOLS:-$HOME/tools}/android-sdk (la plus récente).
# NM : llvm-nm du NDK ($ANDROID_NDK ou $SC2_TOOLS/android-ndk-r28, prebuilt linux-x86_64 ou darwin-x86_64), repli sur le
# système. Java (apksigner) : $JAVA_HOME, $SC2_TOOLS/jdk-21*, openjdk@17 de Homebrew, /usr/libexec/java_home.
# Remplacements : SC2_APK_PACKAGE=<nom>, SC2_APK_ACTIVITE=<activité>, SC2_APK_CLE_SHA256=<empreinte attendue>.
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"
APK="${1:-$W/android/build/sc2-partout.apk}"
PKG="${SC2_APK_PACKAGE:-fr.sc2partout}"
# Passe 10c : depuis la 10b, l'icône ouvre le lanceur ; SC2_APK_ACTIVITE=android.app.NativeActivity pour un APK d'avant.
ACTIVITE="${SC2_APK_ACTIVITE:-fr.sc2partout.LanceurActivity}"
LIB="lib/arm64-v8a/libSC2Partout.so"
# Empreinte SHA-256 du certificat de VOTRE clé de signature (le dépôt public n'en épingle aucune) :
CLE="$(printf '%s' "${SC2_APK_CLE_SHA256:-}" | tr -d ': ' | tr 'A-F' 'a-f')"
AIDES_SEUIL=10

echo "=== valider-apk — $APK"
if [ ! -s "$APK" ]; then echo "[FAIL] APK absent ou vide : $APK"; echo; echo "FAIL"; exit 1; fi

SDK="${ANDROID_SDK_ROOT:-${SC2_TOOLS:-$HOME/tools}/android-sdk}"
BT="$(ls -d "$SDK"/build-tools/*/ 2>/dev/null | sort -V | tail -1)"; BT="${BT%/}"
NDK="${ANDROID_NDK:-${SC2_TOOLS:-$HOME/tools}/android-ndk-r28}"
NDK_BIN="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin 2>/dev/null | head -1)"
AAPT2="$BT/aapt2"; APKSIGNER="$BT/apksigner"; ZIPALIGN="$BT/zipalign"
JDK="${JAVA_HOME:-}"                                                    # apksigner = enveloppe java
for j in "$JDK" $(ls -d "${SC2_TOOLS:-$HOME/tools}"/jdk-21* 2>/dev/null) /opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home \
         "$( [ -x /usr/libexec/java_home ] && /usr/libexec/java_home 2>/dev/null)"; do
  [ -n "$j" ] && [ -x "$j/bin/java" ] && { JDK="$j"; break; }
done
[ -n "$JDK" ] && [ -x "$JDK/bin/java" ] && export JAVA_HOME="$JDK" PATH="$JDK/bin:$PATH"
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -c1-16; }

PASS=0; FAIL=0
ok()  { printf '[PASS] %s\n' "$*"; PASS=$((PASS + 1)); }
ko()  { printf '[FAIL] %s\n' "$*"; FAIL=$((FAIL + 1)); }
info(){ printf '[INFO] %s\n' "$*"; }

info "taille $(du -h "$APK" | cut -f1), sha256 $(sha256 "$APK")…, build-tools $(basename "$BT")"

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# --- 1. badging ------------------------------------------------------------------------------------
if [ -x "$AAPT2" ]; then
  BAD="$("$AAPT2" dump badging "$APK" 2>/dev/null)"
  nom=$(printf '%s\n' "$BAD" | sed -n "s/^package: name='\([^']*\)'.*/\1/p")
  minsdk=$(printf '%s\n' "$BAD" | sed -n "s/^minSdkVersion:'\([0-9]*\)'.*/\1/p")
  [ -n "$minsdk" ] || minsdk=$(printf '%s\n' "$BAD" | sed -n "s/^sdkVersion:'\([0-9]*\)'.*/\1/p")
  cible=$(printf '%s\n' "$BAD" | sed -n "s/^targetSdkVersion:'\([0-9]*\)'.*/\1/p")
  natif=$(printf '%s\n' "$BAD" | sed -n "s/^native-code: \(.*\)$/\1/p")
  act=$(printf '%s\n' "$BAD" | sed -n "s/^launchable-activity: name='\([^']*\)'.*/\1/p")
  [ "$nom" = "$PKG" ] && ok "paquet $nom" || ko "paquet « $nom » (attendu $PKG)"
  if [ -n "$minsdk" ] && [ "$minsdk" -ge 26 ] 2>/dev/null; then ok "minSdk $minsdk (≥ 26)"; else ko "minSdk « $minsdk » < 26"; fi
  case "$natif" in *arm64-v8a*) ok "native-code : $natif";; *) ko "native-code « $natif » sans arm64-v8a";; esac
  case "$act" in *"$ACTIVITE"*) ok "activité $act";; *) ko "activité « $act » (attendu …$ACTIVITE)";; esac
  info "targetSdk $cible"
else
  ko "aapt2 introuvable (build-tools absents dans $SDK ?)"
fi

# --- 2. lib arm64-v8a ------------------------------------------------------------------------------
noms="$(unzip -Z1 "$APK" 2>/dev/null)"
case "$noms" in *"$LIB"*) ok "$LIB présente";; *) ko "$LIB absente de l'APK";; esac
NM=""
for c in "$NDK_BIN/llvm-nm" "$(command -v llvm-nm)" "$(command -v nm)"; do [ -n "$c" ] && [ -x "$c" ] && NM="$c" && break; done
SO=""
if unzip -p "$APK" "$LIB" > "$TMP/lib.so" 2>/dev/null && [ -s "$TMP/lib.so" ]; then
  SO="$TMP/lib.so"
  mach="$(od -An -tx1 -j18 -N2 "$SO" | tr -d ' \n' | tr 'A-F' 'a-f')"   # e_machine, ELF little-endian
  if [ "$mach" = "b700" ]; then ok "lib ELF AArch64 (e_machine 0xB7)"; else ko "lib ELF e_machine 0x$mach ≠ AArch64"; fi
  if [ -n "$NM" ]; then
    "$NM" -D --defined-only "$SO" 2>/dev/null | awk '{print $NF}' > "$TMP/syms.txt"   # fichier : grep -q sous pipefail meurt sur SIGPIPE
    for s in ANativeActivity_onCreate android_main; do
      grep -qx "$s" "$TMP/syms.txt" && ok "symbole $s exporté" || ko "symbole $s absent de la table dynamique"
    done
  else
    ko "ni llvm-nm ni nm pour lire les symboles"
  fi
else
  ko "extraction de $LIB impossible"
fi

# --- 3 et 4. signature, clé épinglée, alignement -----------------------------------------------
if [ -x "$APKSIGNER" ]; then
  if SERT="$("$APKSIGNER" verify --print-certs "$APK" 2>&1)"; then
    ok "signature vérifiée (apksigner verify)"
    # build-tools ≤ 36 : « Signer #1 certificate … » ; 37 : « V3.0 Signer: certificate … » (une ligne par schéma) — empreintes
    # distinctes, tampon de source (« Source Stamp ») exclu
    printf '%s\n' "$SERT" | grep -v Stamp | grep -E "[Ss]igner.*certificate DN" | head -1 | sed 's/^/       /'
    printf '%s\n' "$SERT" | grep -v Stamp | sed -n -E 's/^.*[Ss]igner[^:]*:? *certificate SHA-256 digest: *([0-9a-fA-F]+).*/\1/p' \
      | tr 'A-F' 'a-f' | sort -u > "$TMP/cles.txt"
    ncles=$(wc -l < "$TMP/cles.txt" | tr -d ' ')
    cle="$(head -1 "$TMP/cles.txt")"
    if [ -z "$CLE" ] && [ "$ncles" = 1 ]; then
      info "clé de signature non épinglée (SC2_APK_CLE_SHA256 absent) : ${cle:0:16}…"
    elif [ "$ncles" = 1 ] && [ "$cle" = "$CLE" ]; then
      ok "clé de signature = clé épinglée (SHA-256 ${CLE:0:16}…)"
    elif [ "$ncles" = 1 ]; then
      ko "clé de signature ${cle:0:16}… ≠ clé épinglée ${CLE:0:16}… (une autre clé n'installe pas par-dessus l'APK déjà installé)"
    else
      ko "clé de signature : $ncles certificat(s) distinct(s) au lieu d'un seul"
    fi
  else
    ko "signature invalide : $(printf '%s\n' "$SERT" | head -1)"
    ko "clé de signature : non vérifiable (signature invalide)"
  fi
else
  ko "apksigner introuvable (build-tools absents dans $SDK ?)"
fi
if [ -x "$ZIPALIGN" ]; then
  "$ZIPALIGN" -c -P 16 4 "$APK" >/dev/null 2>&1 && ok "aligné (zipalign -c -P 16 4)" || ko "non aligné (zipalign -P 16)"
else
  ko "zipalign introuvable (build-tools absents dans $SDK ?)"
fi

# --- 5. lift optimisé (jamais -O0) -----------------------------------------------------------------
marq="$(unzip -p "$APK" assets/sc2-build.txt 2>/dev/null | tr -d '\r')"
if [ -n "$marq" ]; then
  opt="$(printf '%s\n' "$marq" | sed -n 's/^lift_opt=//p' | head -1)"
  case " $opt " in
    *" -O0 "*|"  ") ko "lift « ${opt:-?} » (marqueur assets/sc2-build.txt) : -O0 refusé — SC2_APK_LIFT_OPT=-O2 (recette §3)" ;;
    *" -O1 "*|*" -O2 "*|*" -O3 "*|*" -Os "*) ok "lift $opt (marqueur assets/sc2-build.txt)" ;;
    *) ko "lift « $opt » (marqueur assets/sc2-build.txt) : optimisation inconnue" ;;
  esac
elif [ -n "$SO" ] && [ -n "$NM" ]; then
  # aides SC2_EN_LIGNE de cpu.h (liste tirée de l'arbre ; repli : les 11 aides présentes dans chaque fichier lifté)
  CPU_H="$W/recomp-sc2/runtime/cpu.h"
  if [ -r "$CPU_H" ]; then
    grep -o 'SC2_EN_LIGNE [^(]*(' "$CPU_H" | sed -E 's/.*[ *]([A-Za-z_0-9]+) *\($/\1/' | grep -v '^__' | sort -u > "$TMP/aides.txt"
  else
    printf '%s\n' rd8 rd32 wr32 push32 pop32 parity8 mask_sz sign_sz flags_add flags_sub flags_logicz > "$TMP/aides.txt"
  fi
  "$NM" "$SO" 2>/dev/null > "$TMP/symtab.txt"
  nloc=$(awk '$2 == "t" || $2 == "T" {n++} END {print n + 0}' "$TMP/symtab.txt")
  if [ "$nloc" -eq 0 ]; then
    if [ "${SC2_APK_LIFT_NON_PROUVE:-0}" = 1 ]; then info "lift : ni marqueur ni .symtab — accepté sans preuve (SC2_APK_LIFT_NON_PROUVE=1)"
    else ko "lift : ni marqueur assets/sc2-build.txt ni .symtab (lib strippée) — -O0 non exclu (SC2_APK_LIFT_NON_PROUVE=1 pour passer outre)"; fi
  else
    naides=$(awk 'NR == FNR {h[$1] = 1; next} $2 == "t" && ($3 in h) {n++} END {print n + 0}' "$TMP/aides.txt" "$TMP/symtab.txt")
    exemple=$(awk 'NR == FNR {h[$1] = 1; next} $2 == "t" && ($3 in h) {c[$3]++} END {for (k in c) printf "%d %s\n", c[k], k}' \
              "$TMP/aides.txt" "$TMP/symtab.txt" | sort -rn | head -3 | awk '{printf "%s×%s ", $2, $1}')
    if [ "$naides" -ge "$AIDES_SEUIL" ]; then
      ko "lift en -O0 : $naides copies locales des aides de cpu.h dans .symtab (${exemple% }) ; -O2 = 0, -O0 ≈ 4 272 — SC2_APK_LIFT_OPT=-O2 (recette §3)"
    else
      ok "lift optimisé : $naides copie(s) locale(s) des aides de cpu.h (seuil $AIDES_SEUIL ; -O0 ≈ 4 272, -O2 = 0), pas de marqueur"
    fi
  fi
  SIZE="${NM%nm}size"
  if [ -x "$SIZE" ]; then
    txt=$("$SIZE" -A "$SO" 2>/dev/null | awk '$1 == ".text" {printf "%.1f", $2 / 1048576}')
    [ -n "$txt" ] && info ".text $txt Mio (indicatif seulement : lift -O0 seul = 113,2 Mio, v30 entière = 95,5 Mio)"
  fi
else
  ko "lift : ni marqueur assets/sc2-build.txt ni lib/llvm-nm pour lire .symtab — -O0 non exclu"
fi

# --- verdict ---------------------------------------------------------------------------------------
echo
if [ "$FAIL" -eq 0 ]; then echo "PASS ($PASS contrôles)"; exit 0; fi
echo "FAIL ($FAIL échec(s) sur $((PASS + FAIL)) contrôles)"; exit 1
