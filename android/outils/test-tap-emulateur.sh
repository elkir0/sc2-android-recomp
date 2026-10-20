#!/usr/bin/env bash
# Passe 7o — test du TOUCHER sur l'émulateur (AVD sc2partout du Mac mini M4) ou un appareil, par adb.
# À lancer CÔTÉ MAC (le devbox n'a pas d'adb : règle de supervision). Aucune donnée modifiée sur l'appareil
# hors de /sdcard/SC2Partout/sc2.env, sauvegardé puis restauré.
#
#   android/outils/test-tap-emulateur.sh <apk> [attente_s=150] [sortie=./tap-<date>]
#
# 1. installe l'APK (-r), AJOUTE à sc2.env SC2_FPS_PERIODE=5 (+ SC2_ENV_EXTRA si fourni), lance l'activité ;
# 2. lit dans logcat la ligne de l'APK « bouton client (767, 301) → adb shell input tap X Y » (calculée
#    par sc2_geo.h depuis la géométrie RÉELLE de la fenêtre) ;
# 3. attend <attente_s> (dialogue / menu affiché), capture l'écran AVANT, tape le bouton, attend 5 s,
#    capture APRÈS, tape le centre, capture ;
# 4. extrait logcat : [TOUCHER] (conversion écran → client), [INPUT] (messages livrés à la wndproc du
#    jeu : WM_LBUTTONDOWN/UP aux coordonnées client), [FPS], [VKP] mode de présentation, ARRÊT.
# Verdict : OK si un WM_LBUTTONDOWN puis un WM_LBUTTONUP en (767, 301) apparaissent après le tap.
set -uo pipefail
APK="${1:?usage : $0 <apk> [attente_s] [sortie]}"; ATT="${2:-150}"; OUT="${3:-./tap-$(date +%Y%m%d-%H%M%S)}"
PKG=fr.sc2partout; ACT="$PKG/android.app.NativeActivity"; ENV=/sdcard/SC2Partout/sc2.env
mkdir -p "$OUT"; L="$OUT/rapport.txt"; j() { echo "$*" | tee -a "$L"; }
adb get-state > /dev/null || { echo "aucun appareil adb"; exit 2; }
j "=== passe 7o — test du toucher $(date '+%F %T') ; appareil $(adb shell getprop ro.product.model | tr -d '\r') ; $(adb shell wm size | tr -d '\r')"
adb install -r "$APK" > "$OUT/install.log" 2>&1 && j "APK installé : $APK" || { j "ÉCHEC install"; cat "$OUT/install.log"; exit 1; }
adb shell "cp $ENV $ENV.avant-7o 2>/dev/null; true"          # le sc2.env existant est GARDÉ, on y ajoute
{ adb shell "cat $ENV.avant-7o 2>/dev/null" | tr -d '\r'; printf 'SC2_FPS_PERIODE=5\n%s\n' "${SC2_ENV_EXTRA:-}"; } \
  | adb shell "cat > $ENV"
adb shell am force-stop "$PKG"; adb logcat -c
adb shell am start -n "$ACT" > /dev/null; j "activité lancée ; attente de la géométrie…"
TAP=""; for _ in $(seq 1 60); do
  TAP=$(adb logcat -d -s SC2Partout | grep -o 'bouton client (767, 301) → adb shell input tap [0-9]* [0-9]*' | tail -1)
  [ -n "$TAP" ] && break; sleep 1; done
[ -n "$TAP" ] || { j "ÉCHEC : pas de ligne de géométrie dans logcat (APK antérieur à la 7o ?)"; }
read -r BX BY <<< "$(echo "$TAP" | awk '{print $(NF-1), $NF}')"
CTR=$(adb logcat -d -s SC2Partout | grep -o 'centre → adb shell input tap [0-9]* [0-9]*' | tail -1 | awk '{print $(NF-1), $NF}')
j "géométrie : $(adb logcat -d -s SC2Partout | grep -o 'fenêtre [0-9]*x[0-9]*, image du jeu.*×[0-9.]*' | tail -1)"
j "bouton (767, 301) → tap $BX $BY ; centre → tap $CTR"
j "attente ${ATT} s (frames : lignes [FPS])…"; sleep "$ATT"
adb exec-out screencap -p > "$OUT/1-avant.png"
adb shell input tap "$BX" "$BY"; j "tap bouton $BX $BY"; sleep 5
adb exec-out screencap -p > "$OUT/2-apres-bouton.png"
adb shell input tap $CTR; j "tap centre $CTR"; sleep 5
adb exec-out screencap -p > "$OUT/3-apres-centre.png"
adb logcat -d -s SC2Partout > "$OUT/logcat.txt"
j "--- conversion (sc2_geo) ---"; grep -E '\[TOUCHER\]' "$OUT/logcat.txt" | head -8 | tee -a "$L"
j "--- messages livrés au jeu ---"; grep -E '\[INPUT\].*WM_LBUTTON' "$OUT/logcat.txt" | head -8 | tee -a "$L"
j "--- cadence / présentation ---"; grep -E '\[FPS\]|\[VKP\] mode de présentation' "$OUT/logcat.txt" | tail -4 | tee -a "$L"
grep -E 'ARRÊT|SIGSEGV|MANQUE' "$OUT/logcat.txt" | head -3 | sed 's/^/  ! /' | tee -a "$L"
if grep -q 'WM_LBUTTONDOWN.*(767, 301)' "$OUT/logcat.txt" && grep -q 'WM_LBUTTONUP.*(767, 301)' "$OUT/logcat.txt"; then
  j "VERDICT : OK — le toucher est arrivé au jeu en (767, 301) (captures : $OUT/*.png)"
else j "VERDICT : ÉCHEC — pas de WM_LBUTTONDOWN/UP en (767, 301)"; fi
adb shell "if [ -f $ENV.avant-7o ]; then mv $ENV.avant-7o $ENV; else rm -f $ENV; fi"
