#!/usr/bin/env bash
# android/build-apk.sh — passe 9m : APK du devbox aux drapeaux canon du M4 (build-apk-mac.sh) :
#   lib native sous lib/arm64-v8a/ et rangée SANS compression (zip -X -0, « Stored »), comme l'APK du Fold.
# Construit par scripts/build-apk.sh (inchangé : .so, gardes lift, manifest, assets, signature, contrôles), puis
# réemballe la .so en Stored, réaligne (zipalign -P 16, 4), re-signe (keystore de debug) et revérifie.
#
#   SC2_APK_SWAPCHAIN=1 android/build-apk.sh          variante swapchain (android/build-vk/)
#   SC2_APK_LIFT_OPT / SC2_APK_API / SC2_APK_LDFLAGS : variantes, voir scripts/apk-variante.sh
#   toutes les variables de scripts/build-apk.sh passent telles quelles (JOBS, SC2_APK_SANS_SO, SC2_APK_BANC_LIFT_PERIME,
#   SC2_APK_SANS_TABLE_SPIRV…) ; sans table SPIR-V, scripts/build-apk.sh refuse (passe 9x), donc ce script aussi
# Sortie : <APK de scripts/build-apk.sh> avec le suffixe -stocke.apk ; sha256 et taille affichés ; sonde des fonctions
# liftées dans la .so (non strippée) : L_00CED620 (passe 9l), L_010CAB80 et L_0129DC30 (gardes 8f).
set -euo pipefail
ICI="$(cd "$(dirname "$0")" && pwd)"
W="$(cd "$ICI/.." && pwd)"
TOOLS="${SC2_TOOLS:-$HOME/tools}"
NDK="${ANDROID_NDK:-$TOOLS/android-ndk-r28}"
SDK="${ANDROID_SDK_ROOT:-$TOOLS/android-sdk}"
BT="$SDK/build-tools/35.0.0"
LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
JDK="${JAVA_HOME:-$(ls -d "$TOOLS"/jdk-21* 2>/dev/null | head -1)}"
export JAVA_HOME="$JDK" PATH="$JDK/bin:$PATH"
KS="${SC2_KEYSTORE:-$HOME/.android/debug.keystore}"
. "$W/scripts/apk-variante.sh"     # même dossier que scripts/build-apk.sh (variantes SC2_APK_*)

"$W/scripts/build-apk.sh"
# Passe 9x : l'APK produit est celui que scripts/build-apk.sh déclare (suffixes -SANS-TABLE-SPIRV si
# SC2_APK_SANS_TABLE_SPIRV=1 sans table, -BANC-lift-perime si SC2_APK_BANC_LIFT_PERIME=1), jamais deviné ici.
[ -s "$B/apk-produit.txt" ] || { echo "scripts/build-apk.sh n'a pas déclaré d'APK ($B/apk-produit.txt)"; exit 1; }
APK="$(cat "$B/apk-produit.txt")"
[ -f "$APK" ] || { echo "APK de scripts/build-apk.sh introuvable : $APK"; exit 1; }

printf '\n=== réemballage canon : lib/arm64-v8a/libSC2Partout.so en Stored (zip -X -0) ===\n'
S="$B/apk"
[ -f "$S/base.apk" ] && [ -f "$S/lib/arm64-v8a/libSC2Partout.so" ] || { echo "étage $S incomplet"; exit 1; }
cp "$S/base.apk" "$S/base-stocke.apk"
( cd "$S" && TZ=UTC zip -q -X -0 -r base-stocke.apk lib )        # remplace l'entrée compressée par une entrée Stored
"$BT/zipalign" -P 16 -f 4 "$S/base-stocke.apk" "$S/aligne-stocke.apk"
SORTIE="${APK%.apk}-stocke.apk"
"$BT/apksigner" sign --ks "$KS" --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android \
    --out "$SORTIE" "$S/aligne-stocke.apk"

printf '\n=== contrôles de l'"'"'APK canon ===\n'
"$BT/apksigner" verify "$SORTIE" && echo "apksigner : OK"
"$BT/zipalign" -c -P 16 4 "$SORTIE" && echo "zipalign : OK"
LISTE="$(unzip -v "$SORTIE")"
LIGNE="$(printf '%s\n' "$LISTE" | grep ' lib/arm64-v8a/libSC2Partout.so$')" || { echo "lib/arm64-v8a/libSC2Partout.so ABSENTE"; exit 1; }
echo "$LIGNE"
case "$LIGNE" in *" Stored "*) echo "lib : Stored (non compressée)" ;; *) echo "lib : PAS Stored"; exit 1 ;; esac
printf '\n=== sonde des fonctions liftées (.so non strippée : %s) ===\n' "$B/libSC2Partout.so"
SYMS="$("$LLVM/llvm-nm" --defined-only "$B/libSC2Partout.so")"
for f in L_00CED620 L_00EE0490 L_010CAB80 L_0129DC30; do
  l="$(printf '%s\n' "$SYMS" | grep " $f$" || true)"
  echo "  $f : ${l:-ABSENTE}"
done
echo "  fonctions liftées définies : $(printf '%s\n' "$SYMS" | grep -c ' L_[0-9A-F]\{8\}$')"
ls -l "$SORTIE" | awk '{printf "%s : %d o (%.1f Mo)\n", $9, $5, $5/1048576}'
sha256sum "$SORTIE"
