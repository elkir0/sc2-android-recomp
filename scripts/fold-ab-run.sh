#!/bin/bash
# fold-ab-run.sh <nom> <apk|-> [jeu_s] [profil_s] (passe 10t) : un run « jeu actif » sur le Fold (protocole TRACE-PASSE10R §14-15).
#   1. attend le statut thermique 0 ; installe l'APK (install -r, données conservées) sauf « - » ;
#   2. lance TRaynor01 ; Anthony ferme le panneau d'aide (1er WM_LBUTTONDOWN) puis joue ;
#   3. plateau SANS profileur : du clic + 10 s au clic + 10 + jeu_s (défaut 60 s) ;
#   4. simpleperf profil_s (défaut 60 s, 4 kHz, fil sc2-jeu) ; 0 = pas de profil ;
#   5. appui long sur Retour → bilan de sortie ; rapports simpleperf sur l'appareil, sans seuil.
# Sortie (hors dépôt) : ~/code/sc2-apk/mesures/<jour>/<nom>/ (logcat.txt, resume.txt, deroule.txt, rapport-*.txt),
# à dépouiller par scripts/fold-ab.py.
# logcat restreint aux étiquettes du jeu et des plantages (rien d'autre du téléphone).
D=adb-R3GL70HKY3N-ftUTvh._adb-tls-connect._tcp; N=$1; A=$2; J=${3:-60}; S=${4:-60}
O=$HOME/code/sc2-apk/mesures/$(date +%Y%m%d)/$N; mkdir -p "$O"
a() { adb -s $D "$@"; }
[ "$(adb -s $D get-state 2>/dev/null)" = device ] || { echo "Fold absent d'adb : run annulé"; exit 1; }
temp() { a shell 'dumpsys thermalservice 2>/dev/null | grep -m1 -E "Thermal Status|mStatus:" ; dumpsys thermalservice 2>/dev/null | grep "mName=AP," | tail -1' | tr -s ' \t\n' ' ' | cut -c1-160; }
statut() { a shell 'dumpsys thermalservice 2>/dev/null | grep -m1 "Thermal Status"' | tr -dc '0-9'; }
dit() { echo "$(date +%T) $*" | tee -a "$O/deroule.txt"; }
: > "$O/deroule.txt"
until [ "$(statut)" = 0 ]; do dit "statut thermique $(statut) : refroidissement…"; sleep 30; done
echo "$(date +%T) [$N] température avant : $(temp)" | tee "$O/resume.txt"
if [ "$A" != - ]; then
  a install -r "$A" 2>&1 | tail -1 | tee -a "$O/resume.txt"
  a shell appops set fr.sc2partout MANAGE_EXTERNAL_STORAGE allow
fi
echo "apk : $A $( [ "$A" != - ] && md5 -q "$A")" >> "$O/resume.txt"
a shell dumpsys package fr.sc2partout | grep -m1 lastUpdateTime | tee -a "$O/resume.txt"
a shell cat /sdcard/SC2Partout/sc2.env > "$O/sc2.env"
a shell am force-stop fr.sc2partout; a logcat -c
a shell am start -n fr.sc2partout/android.app.NativeActivity >/dev/null
dit "jeu lancé — Anthony : ferme le panneau d'aide quand il apparaît, puis joue sans t'arrêter"
adb -s $D logcat -v time -s SC2Partout:I libc:F DEBUG:F AndroidRuntime:E > "$O/logcat.txt" 2>&1 & P=$!
until grep -aq "WM_LBUTTONDOWN" "$O/logcat.txt"; do
  sleep 1; kill -0 $P 2>/dev/null || { dit "logcat interrompu"; exit 1; }
  grep -aqE 'Fatal signal|FATAL-JEU' "$O/logcat.txt" && { dit "plantage avant le 1er clic"; break; }
done
dit "1er clic (panneau fermé) ; plateau sans profileur de $((J + 10)) s"
sleep $((J + 10))
if [ "$S" -gt 0 ]; then
  a shell rm -f /data/local/tmp/perf.data
  dit "simpleperf $S s"
  a shell simpleperf record --app fr.sc2partout -e cpu-clock -f 4000 --call-graph fp --duration $S -o /data/local/tmp/perf.data 2>&1 | tail -2 | tee -a "$O/deroule.txt"
  dit "profil fini"
fi
a shell input keyevent --longpress KEYCODE_BACK
dit "appui long sur Retour (bilan de sortie) — Anthony : tu peux lâcher"
sleep 12
echo "$(date +%T) [$N] température après : $(temp)" | tee -a "$O/resume.txt"
kill $P; wait $P 2>/dev/null
a shell am force-stop fr.sc2partout
if [ "$S" -gt 0 ]; then
  a shell simpleperf report -i /data/local/tmp/perf.data --sort comm -n > "$O/rapport-threads.txt" 2>&1
  a shell simpleperf report -i /data/local/tmp/perf.data --comms sc2-jeu --sort dso,symbol -n --percent-limit 0 > "$O/rapport-jeu-self.txt" 2>&1
  a shell simpleperf report -i /data/local/tmp/perf.data --comms sc2-jeu --sort dso -n > "$O/rapport-jeu-dso.txt" 2>&1
  a shell simpleperf report -i /data/local/tmp/perf.data --comms sc2-jeu --children --sort symbol -n --percent-limit 1 > "$O/rapport-jeu-inclusif.txt" 2>&1
fi
echo "plantages : $(grep -aE 'Fatal signal|FATAL-JEU|\[ARRÊT\] \[thread' "$O/logcat.txt" | grep -vc 'sortie demandée') ; Present (bilan) : $(grep -a 'frames présentées (Present)' "$O/logcat.txt" | tail -1 | sed 's/.*: //')" | tee -a "$O/resume.txt"
dit "fini → $O"
