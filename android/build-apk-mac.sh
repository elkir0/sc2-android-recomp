#!/bin/bash
# build-apk-mac.sh — chaîne APK 100 % locale M4 (écrite par Reverso-SC2 le 16/10
# après l'asphyxie réseau du devbox). Remplace build-apk.sh (devbox) pour le Mac.
# Sortie : artifacts-mac/sc2-partout-mac.apk (signé debug)
set -euo pipefail
RACINE="$(cd "$(dirname "$0")/.." && pwd)"     # sc2-partout-mac/
AND="$RACINE/android"
RECOMP="$RACINE/recomp-sc2"
SDK=~/tools/android-sdk
NDK=~/tools/android-ndk-r28
BUILD="$AND/build-mac"
OUT="$RACINE/artifacts-mac"
JH=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
export JAVA_HOME=$JH PATH="$JH/bin:$PATH"
BT="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"   # passe 10r : la plus récente (28.0.3 = x86_64, sans Rosetta)
AAPT2=$BT/aapt2
D8=$BT/d8
ZIPALIGN=$BT/zipalign
APKSIGNER=$BT/apksigner
API=34
# Passe 10r : variantes, comme scripts/apk-variante.sh côté devbox. SC2_APK_LIFT_OPT (défaut -O0), SC2_APK_API (défaut 34
# ici), SC2_APK_LDFLAGS ; dossier et APK suffixés hors défauts. SC2_KEYSTORE : clé de signature (défaut
# artifacts-mac/debug.keystore) ; celle du devbox permet de mettre à jour une APK du devbox sans désinstaller.
API="${SC2_APK_API:-34}"
LIFT_OPT="${SC2_APK_LIFT_OPT:--O0}"
LDFLAGS_APK="${SC2_APK_LDFLAGS:-}"
# Passe 10t : appels directs et registres locaux par défaut dès que le lift est optimisé (=0 pour les couper)
case " $LIFT_OPT " in *" -O1 "*|*" -O2 "*|*" -O3 "*|*" -Os "*) OPTIM=1 ;; *) OPTIM=0 ;; esac
SUF=""
[ "$LIFT_OPT" = "-O0" ] || SUF="$SUF-lift-$(set -- $LIFT_OPT; echo "${1#-}")"
[ "$API" = 34 ] || SUF="$SUF-api$API"
[ -z "$LDFLAGS_APK" ] || SUF="$SUF-ld$(printf '%s' "$LDFLAGS_APK" | cksum | cut -c1-5)"
if [ "${SC2_APK_APPELS_DIRECTS:-$OPTIM}" = 1 ]; then SUF="$SUF-ad"; OPT_AD=-DSC2_APPELS_DIRECTS=ON; else OPT_AD=-DSC2_APPELS_DIRECTS=OFF; fi
if [ "${SC2_APK_DRAPEAUX_MORTS:-0}" = 1 ]; then SUF="$SUF-dm"; OPT_AD="$OPT_AD -DSC2_DRAPEAUX_MORTS=ON"; else OPT_AD="$OPT_AD -DSC2_DRAPEAUX_MORTS=OFF"; fi
if [ "${SC2_APK_REGISTRES_LOCAUX:-$OPTIM}" = 1 ]; then SUF="$SUF-rl"; OPT_AD="$OPT_AD -DSC2_REGISTRES_LOCAUX=ON"; else OPT_AD="$OPT_AD -DSC2_REGISTRES_LOCAUX=OFF"; fi
if [ "${SC2_APK_RL_VIVACITE:-0}" = 1 ]; then SUF="$SUF-v"; OPT_AD="$OPT_AD -DSC2_RL_VIVACITE=ON"; else OPT_AD="$OPT_AD -DSC2_RL_VIVACITE=OFF"; fi
if [ "${SC2_APK_RL_X87:-$OPTIM}" = 1 ]; then SUF="$SUF-x87"; OPT_AD="$OPT_AD -DSC2_RL_X87=ON"; else OPT_AD="$OPT_AD -DSC2_RL_X87=OFF"; fi
BUILD="$AND/build-mac$SUF"
NOM="sc2-partout-mac$SUF"
# SC2_APK_SWAPCHAIN=1 : compile la presentation par swapchain (-DSC2_SWAPCHAIN=ON,
# cf android/CMakeLists.txt option SC2_SWAPCHAIN) ; activee ensuite a l execution par
# SC2_SWAPCHAIN=1 dans sc2.env. Sans la variable : copie CPU (comportement historique).
mkdir -p "$OUT"
rm -f "$OUT/$NOM.apk" "$OUT/$NOM-BANC-lift-perime.apk"   # passe 10p : jamais l'APK d'un build précédent

# Passe 10p : garde lift commune (scripts/garde-lift.sh, comme scripts/build-apk.sh). Cette chaîne n'en avait AUCUNE :
# l'APK v7 a été compilée ici avec une clôture d'avant la 9n (talon « fonction non liftable » à 0x00C72900, mission 2
# morte à la 1re vidéo). Refus si src/lifted est périmé ; SC2_APK_BANC_LIFT_PERIME=1 → APK de banc, nommé comme tel.
GARDE="$RACINE/scripts/garde-lift.sh"
[ -x "$GARDE" ] || { echo "GARDE LIFT : $GARDE absent — resynchroniser scripts/ depuis le canon ; build refusé"; exit 1; }
BANC=0; rc_garde=0; "$GARDE" sources "$RECOMP/src/lifted" || rc_garde=$?
case $rc_garde in 0) ;; 2) BANC=1 ;; *) echo "→ resynchroniser recomp-sc2/src/lifted depuis le canon (devbox) avant de construire"; exit 1 ;; esac

echo "[1/5] CMake + compilation aarch64-linux-android (NDK r28c)..."
cmake -S "$AND" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
      -DRECOMP_DIR="$RECOMP" -DCMAKE_BUILD_TYPE=Release ${SC2_APK_SWAPCHAIN:+-DSC2_SWAPCHAIN=ON} \
      -DSC2_LIFTED_OPT="${LIFT_OPT// /;}" -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS_APK" $OPT_AD > "$OUT/cmake.log" 2>&1 || { tail -20 "$OUT/cmake.log"; exit 1; }
cmake --build "$BUILD" -j"${JOBS:-8}" > "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log"; exit 1; }
SO=$(find "$BUILD" -name "libSC2Partout.so" | head -1)
[ -n "$SO" ] || { echo "libSC2Partout.so introuvable"; exit 1; }
echo "   .so : $SO ($(du -h "$SO" | cut -f1))"
NM="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-nm 2>/dev/null | head -1)"
[ -n "$NM" ] || { echo "llvm-nm du NDK introuvable — garde lift (lib) impossible, APK refusé"; exit 1; }
rc_garde=0; "$GARDE" lib "$SO" "$NM" || rc_garde=$?
case $rc_garde in 0) ;; 2) BANC=1 ;; *) exit 1 ;; esac

echo "[2/5] Ressources + assets (aapt2)..."
$AAPT2 compile --dir "$AND/res" -o "$BUILD/res.zip" 2>/dev/null || $AAPT2 compile --dir "$AND/res" -o "$BUILD/res.zip"
# Passe 9g etape 2 (trou T2) : l APK Mac embarque enfin les assets que le devbox posait
# via scripts/build-apk.sh — inits-stricts.txt (lu par la glue -> SC2_STRICT_INITS) et
# polices Liberation (SIL OFL, repli metrique d Arial/Times/Courier -> SC2_POLICES).
ASSETS="$BUILD/assets"; rm -rf "$ASSETS"; mkdir -p "$ASSETS/polices"
cp "$RECOMP/inits-stricts.txt" "$ASSETS/" || { echo "inits-stricts.txt absent de $RECOMP"; exit 1; }
NFPOL=0
if [ -d "$AND/assets/polices" ]; then
  for f in "$AND/assets/polices/"*.ttf; do [ -f "$f" ] && { cp "$f" "$ASSETS/polices/"; NFPOL=$((NFPOL+1)); }; done
  cp "$AND/assets/polices/LICENCE-Liberation.txt" "$ASSETS/polices/" 2>/dev/null || true
fi
echo "   assets : inits-stricts.txt + $NFPOL police(s) Liberation"
$AAPT2 link -o "$BUILD/base.apk" -I "$SDK/platforms/android-$API/android.jar" \
    --manifest "$AND/AndroidManifest.xml" -A "$ASSETS" -R "$BUILD/res.zip" \
    --java "$BUILD/gen" --auto-add-overlay || true

echo "[3/5] DEX (javac + d8)..."
# Passe 10b : le manifest déclare le lanceur Java fr.sc2partout.LanceurActivity (android/src/fr/sc2partout, point
# d'entrée de l'icône) : sources android/src/fr + R.java généré, Java 8 contre android.jar (pas de lambda), d8 pour
# minSdk 26. Échec = ARRÊT (avant : erreurs javac masquées, APK sans code → l'icône planterait).
rm -rf "$BUILD/classes" "$BUILD/dex"; mkdir -p "$BUILD/classes" "$BUILD/dex"
AJAR="$SDK/platforms/android-$API/android.jar"
SRCJ="$(find "$AND/src/fr" -name '*.java' | sort; [ -d "$BUILD/gen" ] && find "$BUILD/gen" -name '*.java' | sort || true)"
javac -source 8 -target 8 -bootclasspath "$AJAR" -Xlint:-options -encoding UTF-8 -d "$BUILD/classes" $SRCJ \
  || { echo "javac : ÉCHEC — APK refusé (le manifest exige fr.sc2partout.LanceurActivity)"; exit 1; }
$D8 --release --min-api 26 --lib "$AJAR" --output "$BUILD/dex" $(find "$BUILD/classes" -name '*.class')
[ -f "$BUILD/dex/classes.dex" ] || { echo "classes.dex absent — APK refusé"; exit 1; }
echo "   lanceur Java : $(find "$AND/src/fr" -name '*.java' | wc -l | tr -d ' ') source(s) → classes.dex"

echo "[4/5] Paquetage + lib native..."
cp "$BUILD/base.apk" "$BUILD/unsigned.apk"
mkdir -p "$BUILD/apklib/lib/arm64-v8a"
cp "$SO" "$BUILD/apklib/lib/arm64-v8a/"
# passe 10b : classes.dex à la RACINE de l'APK (avant : « dex/classes.dex », jamais chargé)
cd "$BUILD/dex" && zip -q -X "$BUILD/unsigned.apk" classes.dex; cd "$BUILD/apklib" && zip -q -r -X -0 "$BUILD/unsigned.apk" lib/; cd "$RACINE"

echo "[5/5] Alignement + signature debug..."
$ZIPALIGN -P 16 -f 4 "$BUILD/unsigned.apk" "$BUILD/aligned.apk"   # -P 16 : .so non compressée alignée sur 16 Ko
KS="${SC2_KEYSTORE:-$OUT/debug.keystore}"
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
    -alias sc2 -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=SC2 Partout Debug" 2>/dev/null
$APKSIGNER sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/$NOM.apk" "$BUILD/aligned.apk"
$APKSIGNER verify "$OUT/$NOM.apk" && echo "=== APK PRÊT : $OUT/$NOM.apk ($(du -h "$OUT/$NOM.apk" | cut -f1)) ==="
# passe 10b/10c : l'icône ouvre le lanceur ; le jeu reste déclaré, exporté (adb am start -n), en processus « :jeu »
BADGING="$($AAPT2 dump badging "$OUT/$NOM.apk")"
case "$BADGING" in *"launchable-activity: name='fr.sc2partout.LanceurActivity'"*) echo "   lanceur : launchable-activity = fr.sc2partout.LanceurActivity" ;;
  *) echo "launchable-activity ≠ fr.sc2partout.LanceurActivity — APK refusé"; exit 1 ;; esac
MANIF="$($AAPT2 dump xmltree --file AndroidManifest.xml "$OUT/$NOM.apk")"
JEU="$(printf '%s\n' "$MANIF" | awk '/E: activity/{c=1; b=""; next} c && /A: /{b=b $0 "\n"; next} c{if(b ~ /android.app.NativeActivity/){printf "%s", b; exit} c=0}')"
case "$JEU" in *'exported(0x01010010)=true'*'process(0x01010011)=":jeu"'*) echo "   jeu : android.app.NativeActivity exportée, processus :jeu" ;;
  *) echo "android.app.NativeActivity absente, non exportée ou hors du processus :jeu — APK refusé"; exit 1 ;; esac
# passe 10p : un APK construit sous SC2_APK_BANC_LIFT_PERIME=1 avec un lift périmé porte ce nom — jamais installé pour jouer
if [ "$BANC" = 1 ]; then
  mv "$OUT/$NOM.apk" "$OUT/$NOM-BANC-lift-perime.apk"
  echo "=== APK DE BANC (lift périmé) : $OUT/$NOM-BANC-lift-perime.apk — NE PAS INSTALLER POUR JOUER ==="
fi
