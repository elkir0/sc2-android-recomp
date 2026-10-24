#!/bin/bash
# seance-fold.sh — UNE séance de test groupée du Fold, SANS joueur (PROMPT-REPRISE.md §5 « Séances Fold groupées ») :
#   chaque étape imprime « [étape N] PASS|ÉCHEC|SKIP — raison », garde ses artefacts dans <session>/etape-N/, et la
#   séance rend le téléphone à l'identique (sc2.env et Variables.txt sauvegardés UNE fois, remis et vérifiés par
#   diff, appli arrêtée ; Variables.txt = réglages du mainteneur, qu'aucun run de test ne doit toucher — cf. 23/10).
#   scripts/seance-fold.sh <apk|-> [--rapide] [--etapes 1,2,…,10] [--sortie <dossier>] [--pousser-pack]
#     <apk>    installée (install -r) par l'étape 1 ; « - » : garder l'APK installée
#     --rapide séance S1 : étapes 1,2,3,4,10 ; défaut : toutes
#     --etapes ne jouer que ces étapes (la 10, bilan, est toujours jouée en dernier)
#     --sortie <dossier> ; défaut ~/code/sc2-apk/mesures/<jour>/seance-<n>, premier numéro libre
#     --pousser-pack  re-pousser le pack SPIR-V (1,6 Go) quand son sha256 diffère de celui du téléphone
#   Prévol (chaque échec = refus propre, code 3, téléphone intact) : valider-apk.sh PASS (si APK donnée) ; Fold
#     présent (lib-fold.sh, sinon « Fold absent : séance annulée ») ; avant-plan libre — launcher ou une app
#     tolérée (SEANCE_APPS_LIBRES : paquets Android séparés par des espaces, launcher Samsung par défaut) — sinon « l'utilisateur se sert du téléphone : attente », au plus SEANCE_ATTENTE_MAX s ;
#     statut thermique 0 ; sha256 du pack du téléphone = celui du pack local (SC2_SPIRV_PACK, défaut
#     <checkout principal>/extraction/sc2-spirv.pak).
#   Étapes : 1 install + lancement jusqu'au bar, verdict-fold.sh, lignes [OMBRES] et [VK] fonction ; 2 bar SANS
#     ligne SC2_ECRAN (défaut « auto »), scénario bar-lancer-mission-fold[-evt], captures des deux écrans,
#     TStory01 puis TRaynor02 chargées ; 3 A/B/A jeu actif TRaynor01 (ancre ~/code/sc2-apk/sc2-v26.apk via
#     SC2_ANCRE_APK, APK mesurée, ancre), FPS médian par run (fold-ab.py) ; 4 mission avec Variables.txt Medium
#     puis Ultra (scripts/prereglages/, poussé en test-variables.txt, SC2_VARIABLES_FIGE=1) ; 5 pause/reprise
#     (HOME 30 s, relance, am send-trim-memory RUNNING_LOW) : [PAUSE] puis reprise, sans Fatal signal ;
#     6 endurance SEANCE_ENDURANCE_S (lignes [MEM], température batterie avant/après) ; 7 60 vs 120 Hz — SKIP
#     tant que SC2_HZ n'est pas détecté dans assets/build-info.txt de l'APK (ou forcé par --etapes) ; 8 cinématique
#     (traynor01-victoire-fold, SC2_FRAMES_PAS=50) ; 9 captures aux frames 600/1200/1800 (SC2_FRAMES_PAS=600,
#     SC2_ECRAN=1280x720) ; 10 remise en état vérifiée par diff, rapport.md (tableau des étapes) et planche.png.
#   ADB_FAKE=1 (sur le modèle de bar-fold.sh) : adb est une FONCTION qui rejoue scripts/seance-fold-fixtures/
#     (logcat synthétique, sc2.env, build-info, préréglages, pack factice ; PPM 64×36 synthétiques générés par
#     creer-frames.sh — jamais d'images du jeu) ; un faux binaire adb en tête du PATH ENREGISTRE tout appel qui
#     lui échappe ($O/.faux-adb/appels.log, contrôlé à l'étape 10 : un appel réel = ÉCHEC) ; les attentes passent
#     par dodo(), divisées par 60×SEANCE_ACCELERE (défaut 1). Sortie défaut en fake : ${TMPDIR:-/tmp}/seance-fold-fake.
#   Dernière ligne : « SÉANCE : X PASS, Y SKIP, Z ÉCHEC — rapport : … ». Exit : 0 sans ÉCHEC, 1 avec, 2 usage,
#   3 refus de prévol. Durées surchargeables (s) : SEANCE_BAR_S 120, SEANCE_MISSION_S 260, SEANCE_PLATEAU_S 60,
#   SEANCE_PAUSE_S 20, SEANCE_ENDURANCE_S 1200, SEANCE_HZ_S 40, SEANCE_CINE_S 230, SEANCE_CAPTURES_S 170,
#   SEANCE_CLIC_MAX 300, SEANCE_ATTENTE_MAX 1800. Appareil : SC2_ADB_SERIAL, sinon le Fold (lib-fold.sh).
set -u

# --- arguments ---------------------------------------------------------------------------------------
APK=""; RAPIDE=0; ETAPES_TXT=""; SORTIE=""; POUSSER_PACK=0
while [ $# -gt 0 ]; do
  case "$1" in
    --rapide) RAPIDE=1 ;;
    --etapes) ETAPES_TXT="${2:?--etapes 1,2,…,10}"; shift ;;
    --sortie) SORTIE="${2:?--sortie <dossier>}"; shift ;;
    --pousser-pack) POUSSER_PACK=1 ;;
    -h|--help) awk 'NR > 1 && !/^#/ { exit } NR > 1' "$0"; exit 0 ;;
    -) [ -z "$APK" ] && APK=- || { echo "seance-fold : APK donnée deux fois" >&2; exit 2; } ;;
    -*) echo "seance-fold : option inconnue « $1 »" >&2; exit 2 ;;
    *) [ -z "$APK" ] && APK=$1 || { echo "seance-fold : APK donnée deux fois" >&2; exit 2; } ;;
  esac
  shift
done
[ -n "$APK" ] || { echo "usage : scripts/seance-fold.sh <apk|-> [--rapide] [--etapes 1,2,…,10] [--sortie <dossier>] [--pousser-pack]" >&2; exit 2; }
A=$APK

ETAPES_EXPLICITES=0
if [ -n "$ETAPES_TXT" ]; then
  ETAPES_EXPLICITES=1; ETAPES=$(printf '%s\n' "$ETAPES_TXT" | tr ',' ' ')
else
  [ "$RAPIDE" = 1 ] && ETAPES="1 2 3 4 10" || ETAPES="1 2 3 4 5 6 7 8 9 10"
fi
for n in $ETAPES; do
  case "$n" in [1-9]|10) ;; *) echo "seance-fold : étape inconnue « $n » (1…10)" >&2; exit 2 ;; esac
done
case " $ETAPES " in *" 10 "*) ;; *) ETAPES="$ETAPES 10"; echo "(étape 10, bilan, toujours jouée)" ;; esac

R="$(cd "$(dirname "$0")/.." && pwd)"
PKG=fr.sc2partout
ENV_TEL=/sdcard/SC2Partout/sc2.env
VARS_TEL=/sdcard/SC2Partout/Variables.txt       # réglages du mainteneur : aucun run de test ne doit les toucher
FRAMES=/storage/emulated/0/Android/data/$PKG/files/frames
DOCS=/sdcard/SC2Partout/test-docs-seance          # progression de TEST, jamais les documents du mainteneur
SEANCE_APPS_LIBRES="${SEANCE_APPS_LIBRES:-com.sec.android.app.launcher}"
SEANCE_BAR_S="${SEANCE_BAR_S:-120}"; SEANCE_MISSION_S="${SEANCE_MISSION_S:-260}"
SEANCE_PLATEAU_S="${SEANCE_PLATEAU_S:-60}"; SEANCE_PAUSE_S="${SEANCE_PAUSE_S:-20}"
SEANCE_ENDURANCE_S="${SEANCE_ENDURANCE_S:-1200}"; SEANCE_HZ_S="${SEANCE_HZ_S:-40}"
SEANCE_CINE_S="${SEANCE_CINE_S:-230}"; SEANCE_CAPTURES_S="${SEANCE_CAPTURES_S:-170}"
SEANCE_CLIC_MAX="${SEANCE_CLIC_MAX:-300}"; SEANCE_ATTENTE_MAX="${SEANCE_ATTENTE_MAX:-1800}"

JOUR=$(date +%Y%m%d)
if [ -n "$SORTIE" ]; then O=$SORTIE
elif [ "${ADB_FAKE:-0}" = 1 ]; then O=${TMPDIR:-/tmp}/seance-fold-fake
else
  n=1; while [ -e "$HOME/code/sc2-apk/mesures/$JOUR/seance-$n" ]; do n=$((n + 1)); done
  O=$HOME/code/sc2-apk/mesures/$JOUR/seance-$n
fi

# --- adb factice (ADB_FAKE=1) : reponses tirees des fixtures, aucun appareil touche ------------------
FX=""; ETAT=""; CADRE=""; APPELS=""
if [ "${ADB_FAKE:-0}" = 1 ]; then
  FX=$R/scripts/seance-fold-fixtures
  [ -d "$FX" ] || { echo "seance-fold : fixtures absentes : $FX" >&2; exit 2; }
  rm -rf -- "${O:?}"
  ETAT=$O/.telephone; CADRE=$ETAT/frames
  mkdir -p "$ETAT" "$CADRE" "$O/.faux-adb"
  cp "$FX/sc2.env" "$ETAT/sc2.env"; cp "$FX/sc2-spirv-factice.txt" "$ETAT/sc2-spirv.pak"
  cp "$FX/Variables.txt" "$ETAT/Variables.txt"
  bash "$FX/creer-frames.sh" "$CADRE" $(seq 0 100 1800) || exit 2   # PPM 64×36 synthétiques
  APPELS=$O/.faux-adb/appels.log; : > "$APPELS"
  cat > "$O/.faux-adb/adb" <<FIN
#!/bin/sh
echo "\$(date +%T) adb \$*" >> "$APPELS"
echo "seance-fold : adb RÉEL refusé en ADB_FAKE (adb \$*)" >&2
exit 97
FIN
  chmod +x "$O/.faux-adb/adb"
  PATH="$O/.faux-adb:$PATH"; export PATH
  adb() {                                    # mêmes appels que le mode réel, réponses tirées des fixtures
    [ "$1" = -s ] && shift 2
    case "$*" in
      "devices -l") printf 'List of devices attached\nfake-fold:5555         device product:q8qxeea model:SM_F976B device:q8qxeea transport_id:1\n' ;;
      get-state) echo device ;;
      "shell dumpsys thermalservice"*) echo "Thermal Status: 0" ;;
      "shell dumpsys battery"*) cat "$FX/batterie.txt" ;;
      "shell dumpsys activity activities"*) cat "$FX/avant-plan.txt" ;;
      "shell dumpsys package $PKG"*) echo "  lastUpdateTime=2026-10-22 09:00:00" ;;
      "shell dumpsys SurfaceFlinger --display-id") printf 'Display 0 (HWC display 0): port=0\nDisplay 1 (HWC display 1): port=1\n' ;;
      "shell cat $ENV_TEL") cat "$ETAT/sc2.env" ;;
      "shell cat $VARS_TEL") cat "$ETAT/Variables.txt" 2>/dev/null || true ;;
      "shell cp $ENV_TEL "*) cp "$ETAT/sc2.env" "$ETAT/${4##*/}" ;;
      "shell cp $VARS_TEL "*) cp "$ETAT/Variables.txt" "$ETAT/${4##*/}" ;;
      "shell sha256sum /sdcard/SC2Partout/sc2-spirv.pak") shasum -a 256 "$ETAT/sc2-spirv.pak" | sed 's| .*|  /sdcard/SC2Partout/sc2-spirv.pak|' ;;
      "logcat -v time"*) cat "$FX/logcat.txt" ;;
      "exec-out screencap"*) ffmpeg -nostdin -v error -i "$CADRE/frame_00100.ppm" -f image2pipe -c:v png - ;;
      "shell find "*) for f in "$CADRE"/frame_*.ppm; do printf '%s/%s\r\n' "$FRAMES" "${f##*/}"; done ;;
      "pull "*) cp "$CADRE/${2##*/}" "$3" ;;
      "install -r "*) echo Success ;;
      "shell rm -rf /sdcard/SC2Partout/"*) rm -rf "$ETAT/${4##*/}" ;;
      "shell rm -f $VARS_TEL") rm -f "$ETAT/Variables.txt" ;;
      "shell date "*) date +%Y%m%d%H%M%S ;;
      "push "*" $ENV_TEL") cp "$2" "$ETAT/sc2.env" ;;
      "push "*" $VARS_TEL") cp "$2" "$ETAT/Variables.txt" ;;
      "push "*" /sdcard/SC2Partout/sc2-spirv.pak") cp "$2" "$ETAT/sc2-spirv.pak" ;;
      "push "*" /sdcard/SC2Partout/"*) if [ -d "$2" ]; then cp -R "$2" "$ETAT/${3##*/}"; else cp "$2" "$ETAT/${3##*/}"; fi ;;
      "shell am start -n $PKG/"*)
        # FUITE SIMULÉE (le bug du 23/10) : un lancement avec SC2_VARIABLES dans sc2.env laissait le
        # préréglage de test dans le C:\ émulé, que le lancement suivant recopiait vers /sdcard — ici le
        # faux téléphone l'écrit directement, pour que la sauvegarde/restauration de Variables.txt soit
        # VRAIMENT exercée (sinon la remise à l'identique ne prouverait rien).
        if grep -q '^SC2_VARIABLES=' "$ETAT/sc2.env" && [ -f "$ETAT/test-variables.txt" ]; then
          cp "$ETAT/test-variables.txt" "$ETAT/Variables.txt"
          echo "$(date +%T) (faux adb : fuite simulée — Variables.txt du téléphone écrasé par test-variables.txt)" >&2
        fi ;;
      *) ;;                                  # logcat -c, am, input, appops, send-trim-memory, wm…
    esac
  }
  echo "== MODE ADB_FAKE : fixtures $FX, sortie $O (aucun appel au vrai adb ; tout appel échappé va dans $APPELS)"
fi

dodo() {                                     # attente en secondes ; en ADB_FAKE, divisée par 60×SEANCE_ACCELERE
  local s=$1
  [ "${ADB_FAKE:-0}" = 1 ] && s=$(awk -v s="$s" -v a=$(( ${SEANCE_ACCELERE:-1} * 60 )) 'BEGIN { printf "%.2f", s / a }')
  sleep "$s"
}

mkdir -p "$O"
# en mode réel, refuser d'écrire dans un dossier non vide (en ADB_FAKE il vient d'être réinitialisé)
if [ "${ADB_FAKE:-0}" != 1 ] && [ -n "$(ls -A "$O" 2>/dev/null)" ]; then
  echo "prévol : la sortie $O n'est pas vide — séance annulée"; exit 3
fi
if [ "${ADB_FAKE:-0}" != 1 ]; then PROG="${SC2_PROGRESSION:-$HOME/code/sc2-apk/progression/apres-mission1}"
else PROG=$FX/progression; fi

# --- prévol --------------------------------------------------------------------------------------------
echo "== prévol"
if [ "$A" != - ] && [ "${ADB_FAKE:-0}" != 1 ]; then
  [ -r "$A" ] || { echo "prévol : APK illisible : $A — séance annulée"; exit 3; }
  if ! bash "$R/scripts/valider-apk.sh" "$A" > "$O/valider-apk.log" 2>&1; then
    echo "prévol : APK refusée par valider-apk.sh (détails : $O/valider-apk.log) — séance annulée"; exit 3
  fi
  echo "valider-apk : $(tail -1 "$O/valider-apk.log")"
fi

. "$R/scripts/lib-fold.sh"
if ! D=$(fold_serie 2>"$O/fold-absent.log"); then
  echo "Fold absent : séance annulée (détails : $O/fold-absent.log)"; exit 3
fi
echo "Fold : $D"
a() { adb -s "$D" "$@"; }

attente_tel() {                              # avant-plan libre (launcher, apps tolérées) sinon attente bornée
  local t0 app p libre
  t0=$(date +%s)
  while :; do
    app=$(a shell dumpsys activity activities | grep -m1 ResumedActivity | tr -d '\r')
    libre=0
    for p in $SEANCE_APPS_LIBRES; do case "$app" in *"$p"*) libre=1; break ;; esac; done
    [ "$libre" = 1 ] && return 0
    if [ $(( $(date +%s) - t0 )) -ge "$SEANCE_ATTENTE_MAX" ]; then
      echo "prévol : le téléphone reste utilisé (avant-plan : ${app:-aucune activité reprise}) après" \
           "$SEANCE_ATTENTE_MAX s — séance annulée"; exit 3
    fi
    echo "$(date +%T) l'utilisateur se sert du téléphone : attente (avant-plan : $(printf '%s' "$app" | cut -c1-90))"
    dodo 30
  done
}
attente_tel

statut() { a shell 'dumpsys thermalservice | grep -m1 "Thermal Status"' | tr -dc '0-9'; }
until [ "$(statut)" = 0 ]; do echo "$(date +%T) statut thermique $(statut) : refroidissement…"; dodo 20; done

if [ "${ADB_FAKE:-0}" = 1 ]; then PACK=$FX/sc2-spirv-factice.txt
else
  MAIN=$(git -C "$R" worktree list 2>/dev/null | awk 'NR == 1 { print $1 }')
  PACK="${SC2_SPIRV_PACK:-$MAIN/extraction/sc2-spirv.pak}"
fi
[ -r "$PACK" ] || { echo "prévol : pack SPIR-V illisible : $PACK (SC2_SPIRV_PACK) — séance annulée"; exit 3; }
sha_pack=$(shasum -a 256 "$PACK" | awk '{print $1}')
sha_tel=$(a shell sha256sum /sdcard/SC2Partout/sc2-spirv.pak | tr -d '\r' | awk '{print $1}')
if [ "$sha_tel" != "$sha_pack" ]; then
  if [ "$POUSSER_PACK" = 1 ]; then
    echo "prévol : sha256 du pack du téléphone ≠ pack local — re-poussé (--pousser-pack, $(du -h "$PACK" | cut -f1))"
    a push "$PACK" /sdcard/SC2Partout/sc2-spirv.pak
  else
    indice=""
    [ -z "$sha_tel" ] && indice=" (sha lu sur l'appareil : vide — sha256sum indisponible ?)"
    echo "prévol : sha256 du pack du téléphone ≠ pack local ($PACK)$indice — séance annulée" \
         "(relancer avec --pousser-pack pour le re-pousser, 1,6 Go)"; exit 3
  fi
fi
echo "pack SPIR-V : sha256 conforme ($sha_pack)"

# --- sauvegarde unique de sc2.env et de Variables.txt + remise en état garantie ------------------------
a shell cat $ENV_TEL > "$O/sc2.env.avant"
[ -s "$O/sc2.env.avant" ] || { echo "prévol : sc2.env illisible — séance annulée"; exit 3; }
VARS_AVANT=present
a shell cat $VARS_TEL > "$O/Variables.txt.avant" 2>/dev/null || true
[ -s "$O/Variables.txt.avant" ] || VARS_AVANT=absent     # l'utilisateur peut n'en avoir aucun
SAUVE_FAITE=1
DATE_S=$(date +%Y%m%d%H%M%S)
a shell cp $ENV_TEL /sdcard/SC2Partout/sc2.env.sauvegarde-seance-$DATE_S
if [ "$VARS_AVANT" = present ]; then
  a shell cp $VARS_TEL /sdcard/SC2Partout/Variables.txt.sauvegarde-seance-$DATE_S
  echo "Variables.txt sauvegardé (une seule fois) : $O/Variables.txt.avant + /sdcard/SC2Partout/Variables.txt.sauvegarde-seance-$DATE_S"
fi
echo "sc2.env sauvegardé (une seule fois) : $O/sc2.env.avant + /sdcard/SC2Partout/sc2.env.sauvegarde-seance-$DATE_S"
: > "$O/verdicts.txt"

REMIS_DEJA=0; REMIS_ETAT=""; VARS_REMIS_ETAT=""
remettre() {
  [ "${SAUVE_FAITE:-0}" = 1 ] || return 0
  [ "$REMIS_DEJA" = 1 ] && return 0
  REMIS_DEJA=1
  a push "$O/sc2.env.avant" $ENV_TEL >/dev/null 2>&1 && echo "$(date +%T) sc2.env d'origine remis"
  a shell cat $ENV_TEL > "$O/sc2.env.apres" 2>/dev/null
  if [ -s "$O/sc2.env.apres" ] && diff -q "$O/sc2.env.avant" "$O/sc2.env.apres" >/dev/null 2>&1; then
    REMIS_ETAT=identique; echo "sc2.env remis à l'identique (diff vide)"
  else
    REMIS_ETAT=différent; echo "ÉCHEC : le sc2.env restauré diffère de l'original ($O/sc2.env.apres)"
  fi
  if [ "$VARS_AVANT" = present ]; then
    a push "$O/Variables.txt.avant" $VARS_TEL >/dev/null 2>&1 && echo "$(date +%T) Variables.txt d'origine remis"
  else
    a shell rm -f $VARS_TEL 2>/dev/null || true; echo "$(date +%T) Variables.txt absent au départ : supprimé s'il est apparu"
  fi
  a shell cat $VARS_TEL > "$O/Variables.txt.apres" 2>/dev/null || true
  if { [ "$VARS_AVANT" = present ] && [ -s "$O/Variables.txt.apres" ] \
       && diff -q "$O/Variables.txt.avant" "$O/Variables.txt.apres" >/dev/null 2>&1; } \
     || { [ "$VARS_AVANT" = absent ] && [ ! -s "$O/Variables.txt.apres" ]; }; then
    VARS_REMIS_ETAT=identique; echo "Variables.txt remis à l'identique (diff vide)"
  else
    VARS_REMIS_ETAT=différent; echo "ÉCHEC : le Variables.txt restauré diffère de l'original ($O/Variables.txt.apres)"
  fi
  a shell am force-stop $PKG 2>/dev/null || true
}
trap remettre EXIT
trap 'exit 130' INT TERM

# base commune : lignes d'origine MOINS les clefs que les étapes posent ; SC2_FRAMES=1 toujours ajouté par env_etape
grep -v -E '^(SC2_ARGS|SC2_BANC_ENTREES|SC2_DOCUMENTS|SC2_MANQUE_CONTINUE|SC2_FRAMES|SC2_FRAMES_PAS|SC2_RUN_SANS_BNET|SC2_ECRAN|SC2_VARIABLES|SC2_VARIABLES_FIGE|SC2_HZ)=' \
  "$O/sc2.env.avant" | grep -v '^$' > "$O/sc2.env.base"

env_etape() {                                # env_etape <clef=valeur>… : base + clefs de l'étape + SC2_FRAMES=1
  { cat "$O/sc2.env.base"; for c in "$@"; do printf '%s\n' "$c"; done; echo SC2_FRAMES=1; } > "$O/sc2.env.courant"
  a push "$O/sc2.env.courant" $ENV_TEL >/dev/null
}

ECRANS=$(a shell dumpsys SurfaceFlinger --display-id | sed -n 's/^Display \([0-9]*\) .*/\1/p')
capturer() {                                 # les deux écrans du Fold : screencap exige -d <id>
  local i=0 e
  for e in $ECRANS; do a exec-out screencap -d "$e" -p > "$1-ecran-$i.png"; i=$((i + 1)); done
}

DOCS_FAITS=0
ensure_docs() {                              # progression de TEST poussée une fois (jamais les vrais documents)
  [ "$DOCS_FAITS" = 1 ] && return 0
  [ -d "$PROG/Banks" ] || return 1
  a shell rm -rf $DOCS
  a push "$PROG" $DOCS >/dev/null && DOCS_FAITS=1
}

tirer_frames() {                             # PPM plus récents que <debut> → PNG dans <rep>/frames
  local rep=$1 debut=$2 f
  mkdir -p "$rep/frames"
  for f in $(a shell "find $FRAMES -name 'frame_*.ppm' -newermt '${debut:0:4}-${debut:4:2}-${debut:6:2} ${debut:8:2}:${debut:10:2}:${debut:12:2}'" \
             | tr -d '\r' | sort); do
    a pull "$f" "$rep/frames/" >/dev/null 2>&1 && sips -s format png "$rep/frames/$(basename "$f")" \
      --out "$rep/frames/$(basename "${f%.ppm}").png" >/dev/null 2>&1 && rm -f "$rep/frames/$(basename "$f")"
  done
}

run_jeu() {                                  # run_jeu <rep> <apk|-> <plateau_s> <milieu|-> <clic 0|1> [clefs…]
  local rep=$1 apk=$2 plat=$3 milieu=$4 clic=$5; shift 5
  mkdir -p "$rep"
  until [ "$(statut)" = 0 ]; do echo "  statut thermique $(statut) : refroidissement…"; dodo 20; done
  [ "$apk" = - ] || { a install -r "$apk" 2>&1 | tail -1; a shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow; }
  env_etape "$@"
  a shell am force-stop $PKG; a logcat -c
  local debut p i=0
  debut=$(a shell date +%Y%m%d%H%M%S | tr -d '\r')
  a shell am start -n $PKG/android.app.NativeActivity >/dev/null
  adb -s "$D" logcat -v time -s SC2Partout:I libc:F DEBUG:F > "$rep/logcat.txt" 2>&1 & p=$!
  if [ "$clic" = 1 ]; then                   # jeu actif : plateau à partir du 1er clic (fold-ab-run.sh)
    until grep -aq "WM_LBUTTONDOWN" "$rep/logcat.txt"; do
      dodo 1; i=$((i + 1)); kill -0 $p 2>/dev/null || break
      grep -aqE 'Fatal signal|FATAL-JEU' "$rep/logcat.txt" && break
      if [ $i -gt "$SEANCE_CLIC_MAX" ]; then echo "  (pas de 1er clic après $SEANCE_CLIC_MAX s : plateau quand même)"; break; fi
    done
    dodo $((plat + 10))
  else
    dodo "$plat"                             # bar, cinématique : durée fixe, pas de clic attendu
  fi
  [ "$milieu" != - ] && $milieu
  a shell input keyevent --longpress KEYCODE_BACK; dodo 12
  kill $p 2>/dev/null; wait $p 2>/dev/null
  a shell am force-stop $PKG
  { echo "plantages : $(grep -aE 'Fatal signal|FATAL-JEU|\[ARRÊT\] \[thread' "$rep/logcat.txt" | grep -vc 'sortie demandée' || true)"
    echo "Present (bilan) : $(grep -a 'frames présentées (Present)' "$rep/logcat.txt" | tail -1 | sed 's/.*: //')"
  } > "$rep/resume.txt"
  tirer_frames "$rep" "$debut"
}

mediane_fps() {                              # médiane du plateau d'un run (logique fold-ab.py) ; « ? » sans [FPS]
  grep -aq '\[FPS\]' "$1/logcat.txt" || { echo '?'; return 0; }
  python3 "$R/scripts/fold-ab.py" --json "$1" 2>/dev/null \
    | python3 -c 'import json,sys; m=json.load(sys.stdin)[0]["fps_median"]; print("?" if m is None else "%.1f" % m)' 2>/dev/null || echo '?'
}
temp_batterie() { a shell dumpsys battery | tr -d '\r' | sed -n 's/^ *temperature: //p' | head -1; }
fmt_temp() { [ -n "$1" ] && awk -v t="$1" 'BEGIN { printf "%.1f °C", t / 10 }' || echo '?'; }

milieu_pause() {                             # étape 5 : arrière-plan 30 s, relance, mémoire serrée
  a shell input keyevent KEYCODE_HOME; dodo 30
  a shell am start -n $PKG/android.app.NativeActivity >/dev/null
  a shell am send-trim-memory $PKG RUNNING_LOW; dodo 20
}

ECHECS=0
conclure() {                                 # conclure <n> <PASS|ÉCHEC|SKIP> "raison"
  echo "[étape $1] $2 — $3"
  echo "$1|$2|$3" >> "$O/verdicts.txt"
  [ "$2" = ÉCHEC ] && ECHECS=$((ECHECS + 1))
  return 0
}
chiffre() { mkdir -p "$O/etape-$1"; printf '%s\n' "$2" >> "$O/etape-$1/chiffres.txt"; }
etape_dir() { local d=$O/etape-$1; mkdir -p "$d/frames"; printf '%s\n' "$d"; }

# clefs communes aux runs « jeu actif TRaynor01 » (fold-ab-auto.sh)
cles_jeu="SC2_MANQUE_CONTINUE=1 SC2_ECRAN=1280x720 SC2_ARGS=Maps/Campaign/TRaynor01.SC2Map SC2_BANC_ENTREES=@/sdcard/SC2Partout/jeu-actif.txt SC2_DOCUMENTS=$DOCS"

# --- étape 1 : install + bar + verdict + [OMBRES]/[VK] --------------------------------------------------
e1() {
  local E rv v no nv
  E=$(etape_dir 1)
  ensure_docs || { conclure 1 SKIP "progression de test absente : $PROG"; return; }
  run_jeu "$E/run" "$A" "$SEANCE_BAR_S" - 0 SC2_RUN_SANS_BNET=1 SC2_DOCUMENTS=$DOCS SC2_ECRAN=1280x720
  bash "$R/scripts/verdict-fold.sh" "$E/run/logcat.txt" > "$E/verdict.txt" 2>&1; rv=$?
  v=$(grep -a '^VERDICT: ' "$E/verdict.txt" | tail -1 | sed -E 's/^VERDICT: ([^ |]+).*/\1/')
  grep -a '\[OMBRES\]\|\[VK\] fonction' "$E/run/logcat.txt" > "$E/ombres-vk.txt" || true
  no=$(grep -ac '\[OMBRES\]' "$E/run/logcat.txt" || true)
  nv=$(grep -ac '\[VK\] fonction' "$E/run/logcat.txt" || true)
  chiffre 1 "verdict $v (code $rv)"
  chiffre 1 "[OMBRES] $no ligne(s) ; [VK] fonction $nv ligne(s) → ombres-vk.txt"
  if [ "$rv" = 0 ]; then conclure 1 PASS "affichage livré (verdict $v) ; [OMBRES] $no, [VK] fonction $nv"
  else conclure 1 ÉCHEC "verdict-fold : ${v:-?} (code $rv) — $E/verdict.txt"; fi
}

# --- étape 2 : bar SANS SC2_ECRAN (auto), scénario, 2 écrans, TStory01 puis TRaynor02 --------------------
e2() {
  local E evt c1 c2 img
  E=$(etape_dir 2)
  evt=$R/scripts/entrees/bar-lancer-mission-fold.txt
  [ -f "${evt%.txt}-evt.txt" ] && evt="${evt%.txt}-evt.txt"
  if ! ensure_docs; then conclure 2 SKIP "progression de test absente : $PROG"; return; fi
  [ -f "$evt" ] || { conclure 2 SKIP "scénario absent : $evt"; return; }
  a push "$evt" /sdcard/SC2Partout/entrees-seance.txt >/dev/null
  env_etape SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 SC2_DOCUMENTS=$DOCS \
            SC2_BANC_ENTREES=@/sdcard/SC2Partout/entrees-seance.txt SC2_FRAMES_PAS=100
  # SC2_ECRAN volontairement ABSENT : la ligne d'origine est retirée, l'APK garde son défaut « auto »
  a shell am force-stop $PKG; a logcat -c
  local debut p
  debut=$(a shell date +%Y%m%d%H%M%S | tr -d '\r')
  a shell am start -n $PKG/android.app.NativeActivity >/dev/null
  adb -s "$D" logcat -v time -s SC2Partout:I libc:F DEBUG:F > "$E/logcat.txt" 2>&1 & p=$!
  echo "$(date +%T) [étape 2] bar lancé ($(basename "$evt"), $SEANCE_MISSION_S s, sans ligne SC2_ECRAN)"
  dodo 100; capturer "$E/bar"
  dodo $((SEANCE_MISSION_S - 100)); capturer "$E/fin"
  a shell input keyevent --longpress KEYCODE_BACK; dodo 12
  kill $p 2>/dev/null; wait $p 2>/dev/null
  a shell am force-stop $PKG
  tirer_frames "$E" "$debut"
  c1=$(grep -ac 'TStory01\.SC2Map' "$E/logcat.txt" || true)
  c2=$(grep -ac 'TRaynor02\.SC2Map' "$E/logcat.txt" || true)
  img=$(ls "$E/frames" 2>/dev/null | grep -c png || true)
  chiffre 2 "TStory01 : $c1 ligne(s) ; TRaynor02 : $c2 ligne(s) ; $img images ; captures des écrans $ECRANS"
  if [ "$c1" -ge 1 ] && [ "$c2" -ge 1 ]; then conclure 2 PASS "TStory01 puis TRaynor02 chargées ($c1/$c2 lignes), $img images"
  else conclure 2 ÉCHEC "cartes attendues absentes du logcat (TStory01 $c1, TRaynor02 $c2) — $E/logcat.txt"; fi
}

# --- étape 3 : A/B/A jeu actif TRaynor01, FPS médian par run ---------------------------------------------
e3() {
  local E ancre m r ko3=0
  E=$(etape_dir 3)
  ancre="${SC2_ANCRE_APK:-$HOME/code/sc2-apk/sc2-v26.apk}"
  if ! ensure_docs; then conclure 3 SKIP "progression de test absente : $PROG"; return; fi
  [ -f "$R/scripts/entrees/jeu-actif-traynor01.txt" ] || { conclure 3 SKIP "scénario absent : $R/scripts/entrees/jeu-actif-traynor01.txt"; return; }
  if [ "${ADB_FAKE:-0}" != 1 ] && [ ! -r "$ancre" ]; then
    conclure 3 SKIP "APK ancre illisible : $ancre (SC2_ANCRE_APK)"; return
  fi
  a push "$R/scripts/entrees/jeu-actif-traynor01.txt" /sdcard/SC2Partout/jeu-actif.txt >/dev/null
  { echo "ancre : $ancre ; APK mesurée : $A ; plateau $SEANCE_PLATEAU_S s (A/B/A)"; } > "$E/deroule.txt"
  run_jeu "$E/ancre"  "$ancre" "$SEANCE_PLATEAU_S" - 1 $cles_jeu
  run_jeu "$E/apk"    "$A"     "$SEANCE_PLATEAU_S" - 1 $cles_jeu
  run_jeu "$E/ancre2" "$ancre" "$SEANCE_PLATEAU_S" - 1 $cles_jeu
  # 23/10 [SUP] : l'A/B/A finit sur l'ANCRE installée — remettre l'APK mesurée, sinon l'étape 4 mesurerait l'ancre et le
  # téléphone resterait sur elle (même versionCode : install -r passe dans les deux sens)
  [ "$A" = - ] || { a install -r "$A" 2>&1 | tail -1; a shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow; }
  python3 "$R/scripts/fold-ab.py" "$E/ancre" "$E/apk" "$E/ancre2" > "$E/ab.md" 2>&1 || true
  for r in ancre apk ancre2; do
    m=$(mediane_fps "$E/$r")
    chiffre 3 "$r : FPS médian $m ; $(sed -n 1p "$E/$r/resume.txt" 2>/dev/null)"
    { grep -q '^plantages : 0' "$E/$r/resume.txt" 2>/dev/null && [ "$m" != '?' ]; } || ko3=1
  done
  if [ "$ko3" = 0 ]; then
    conclure 3 PASS "A/B/A : FPS médians $(mediane_fps "$E/ancre") / $(mediane_fps "$E/apk") / $(mediane_fps "$E/ancre2") — $E/ab.md"
  else
    conclure 3 ÉCHEC "run(s) sans lignes [FPS] ou plantage(s) — $E"
  fi
}

# --- étape 4 : Variables.txt Medium puis Ultra ------------------------------------------------------------
e4() {
  local E pre n m ko4=0
  E=$(etape_dir 4)
  if [ "${ADB_FAKE:-0}" = 1 ]; then pre=$FX/prereglages; else pre=$R/scripts/prereglages; fi
  { [ -f "$pre/medium.txt" ] && [ -f "$pre/ultra.txt" ]; } || { conclure 4 SKIP "préréglages absents ($pre/medium.txt, ultra.txt)"; return; }
  if ! ensure_docs; then conclure 4 SKIP "progression de test absente : $PROG"; return; fi
  a push "$R/scripts/entrees/jeu-actif-traynor01.txt" /sdcard/SC2Partout/jeu-actif.txt >/dev/null
  for n in medium ultra; do
    cp "$pre/$n.txt" "$E/$n.txt"
    a push "$pre/$n.txt" /sdcard/SC2Partout/test-variables.txt >/dev/null
    run_jeu "$E/$n" - "$SEANCE_PLATEAU_S" - 1 $cles_jeu \
      SC2_VARIABLES=/sdcard/SC2Partout/test-variables.txt SC2_VARIABLES_FIGE=1
    m=$(mediane_fps "$E/$n")
    chiffre 4 "$n : FPS médian $m ($(wc -l < "$pre/$n.txt" | tr -d ' ') clefs, figées)"
    { grep -q '^plantages : 0' "$E/$n/resume.txt" 2>/dev/null && [ "$m" != '?' ]; } || ko4=1
  done
  if [ "$ko4" = 0 ]; then conclure 4 PASS "medium $(mediane_fps "$E/medium") / ultra $(mediane_fps "$E/ultra") FPS médians"
  else conclure 4 ÉCHEC "run(s) sans lignes [FPS] ou plantage(s) — $E"; fi
}

# --- étape 5 : pause/reprise (HOME 30 s, send-trim-memory) -------------------------------------------------
e5() {
  local E susp repr fatal
  E=$(etape_dir 5)
  if ! ensure_docs; then conclure 5 SKIP "progression de test absente : $PROG"; return; fi
  a push "$R/scripts/entrees/jeu-actif-traynor01.txt" /sdcard/SC2Partout/jeu-actif.txt >/dev/null
  run_jeu "$E/run" - "$SEANCE_PAUSE_S" milieu_pause 1 $cles_jeu
  susp=$(grep -ac 'suspendu au Present\|activité en pause' "$E/run/logcat.txt" || true)
  repr=$(grep -ac '\[PAUSE\] reprise' "$E/run/logcat.txt" || true)
  fatal=$(grep -ac 'Fatal signal' "$E/run/logcat.txt" || true)
  chiffre 5 "suspensions $susp, reprises $repr, Fatal signal $fatal"
  if [ "$susp" -ge 1 ] && [ "$repr" -ge 1 ] && [ "$fatal" = 0 ]; then
    conclure 5 PASS "[PAUSE] puis reprise ($susp/$repr), aucun Fatal signal"
  else
    conclure 5 ÉCHEC "suspensions $susp, reprises $repr, Fatal signal $fatal — $E/run/logcat.txt"
  fi
}

# --- étape 6 : endurance -------------------------------------------------------------------------------------
e6() {
  local E t0 t1 mem pres ok=0
  E=$(etape_dir 6)
  if [ -f "$R/scripts/marathon-bar.sh" ]; then
    echo "  marathon-bar.sh détecté : non piloté ici (il gère son propre sc2.env) — long run TRaynor01 à la place"
  fi
  if ! ensure_docs; then conclure 6 SKIP "progression de test absente : $PROG"; return; fi
  a push "$R/scripts/entrees/jeu-actif-traynor01.txt" /sdcard/SC2Partout/jeu-actif.txt >/dev/null
  t0=$(temp_batterie)
  run_jeu "$E/run" - "$SEANCE_ENDURANCE_S" - 1 $cles_jeu
  t1=$(temp_batterie)
  mem=$(grep -ac '\[MEM\]' "$E/run/logcat.txt" || true)
  pres=$(sed -n 's/^Present (bilan) : //p' "$E/run/resume.txt" 2>/dev/null | tail -1)
  chiffre 6 "endurance $((SEANCE_ENDURANCE_S / 60)) min ; [MEM] $mem ligne(s) ; batterie $(fmt_temp "$t0") → $(fmt_temp "$t1")"
  grep -q '^plantages : 0' "$E/run/resume.txt" 2>/dev/null && ok=1
  { [ -n "$pres" ] || grep -aq '\[FPS\]' "$E/run/logcat.txt"; } && ok=$((ok + 1))
  if [ "$ok" = 2 ]; then conclure 6 PASS "$((SEANCE_ENDURANCE_S / 60)) min sans plantage, Present ${pres:-?}, [MEM] $mem, batterie $(fmt_temp "$t0") → $(fmt_temp "$t1")"
  else conclure 6 ÉCHEC "plantage(s) ou run vide — $E/run/resume.txt"; fi
}

# --- étape 7 : 60 vs 120 Hz (SKIP sans SC2_HZ dans l'APK) -----------------------------------------------------
e7() {
  local E hz=0 m1 m2 note="" ko7=0
  E=$(etape_dir 7)
  if [ "${ADB_FAKE:-0}" = 1 ]; then
    [ -f "$FX/build-info.txt" ] && grep -q '^SC2_HZ' "$FX/build-info.txt" && hz=1
  elif [ "$A" != - ]; then
    unzip -p "$A" assets/build-info.txt 2>/dev/null | grep -q '^SC2_HZ' && hz=1
  fi
  if [ "$hz" != 1 ]; then
    if [ "$ETAPES_EXPLICITES" = 1 ]; then note=" (forcée par --etapes bien que SC2_HZ soit indétectable)"
    else
      if [ "$A" = - ]; then raison="APK « - » : assets/build-info.txt inconnu"
      else raison="SC2_HZ absent de assets/build-info.txt de l'APK"; fi
      conclure 7 SKIP "$raison — Hz commutables non livrées"; return
    fi
  fi
  if ! ensure_docs; then conclure 7 SKIP "progression de test absente : $PROG"; return; fi
  a push "$R/scripts/entrees/jeu-actif-traynor01.txt" /sdcard/SC2Partout/jeu-actif.txt >/dev/null
  run_jeu "$E/hz60"  - "$SEANCE_HZ_S" - 1 $cles_jeu SC2_HZ=60
  run_jeu "$E/hz120" - "$SEANCE_HZ_S" - 1 $cles_jeu SC2_HZ=120
  m1=$(mediane_fps "$E/hz60"); m2=$(mediane_fps "$E/hz120")
  chiffre 7 "60 Hz : $m1 FPS médian ; 120 Hz : $m2 FPS médian$note"
  grep -q '^plantages : 0' "$E/hz60/resume.txt" 2>/dev/null || ko7=1
  grep -q '^plantages : 0' "$E/hz120/resume.txt" 2>/dev/null || ko7=1
  [ "$m1" != '?' ] && [ "$m2" != '?' ] || ko7=1
  if [ "$ko7" = 0 ]; then conclure 7 PASS "60 Hz $m1 vs 120 Hz $m2 FPS médians$note"
  else conclure 7 ÉCHEC "run(s) sans lignes [FPS] ou plantage(s) — $E"; fi
}

# --- étape 8 : cinématique --------------------------------------------------------------------------------------
e8() {
  local E scen pres img ko8=0
  E=$(etape_dir 8)
  scen=$R/scripts/entrees/traynor01-victoire-fold.txt
  [ -f "$scen" ] || { conclure 8 SKIP "scénario absent : $scen"; return; }
  if ! ensure_docs; then conclure 8 SKIP "progression de test absente : $PROG"; return; fi
  a push "$scen" /sdcard/SC2Partout/entrees-cine.txt >/dev/null
  run_jeu "$E/run" - "$SEANCE_CINE_S" - 0 SC2_RUN_SANS_BNET=1 SC2_DOCUMENTS=$DOCS \
    SC2_BANC_ENTREES=@/sdcard/SC2Partout/entrees-cine.txt SC2_FRAMES_PAS=50
  pres=$(sed -n 's/^Present (bilan) : //p' "$E/run/resume.txt" 2>/dev/null | tail -1)
  img=$(ls "$E/run/frames" 2>/dev/null | grep -c png || true)
  chiffre 8 "Present : ${pres:-?} ; $img images (pas 50)"
  grep -q '^plantages : 0' "$E/run/resume.txt" 2>/dev/null || ko8=1
  { [ -n "$pres" ] || [ "$img" -ge 1 ]; } || ko8=1
  if [ "$ko8" = 0 ]; then conclure 8 PASS "cinématique menée au bout (Present ${pres:-?}, $img images)"
  else conclure 8 ÉCHEC "plantage(s) ou run vide — $E/run/resume.txt"; fi
}

# --- étape 9 : captures aux frames 600/1200/1800 ------------------------------------------------------------------
e9() {
  local E f n presentes="" nb
  E=$(etape_dir 9)
  if ! ensure_docs; then conclure 9 SKIP "progression de test absente : $PROG"; return; fi
  a push "$R/scripts/entrees/jeu-actif-traynor01.txt" /sdcard/SC2Partout/jeu-actif.txt >/dev/null
  run_jeu "$E/run" - "$SEANCE_CAPTURES_S" - 1 $cles_jeu SC2_FRAMES_PAS=600
  for n in 600 1200 1800; do
    f=$(printf 'frame_%05d.png' "$n")
    [ -f "$E/run/frames/$f" ] && presentes="$presentes $n"
  done
  nb=$(printf '%s' "$presentes" | wc -w | tr -d ' ')
  chiffre 9 "captures fixes :${presentes:- aucune} (SC2_FRAMES_PAS=600)"
  if [ "$nb" -ge 2 ]; then conclure 9 PASS "$nb captures fixes présentes ($presentes )"
  else conclure 9 ÉCHEC "captures fixes manquantes (${presentes:-aucune}) — $E/run/frames"; fi
}

# --- étape 10 : remise en état, rapport, planche --------------------------------------------------------------------
e10() {
  local E n v d ch art lst nbimg
  E=$(etape_dir 10)
  remettre                                   # sc2.env d'origine remis + vérifié par diff + appli arrêtée
  lst=$(find "$O"/etape-* -name 'frame_*.png' 2>/dev/null | sort || true)
  nbimg=$(printf '%s\n' "$lst" | grep -c . || true)
  if [ "$nbimg" -ge 1 ]; then
    printf '%s\n' "$lst" | awk 'NR % 3 == 1' > "$E/planche.lst"
    nbimg=$(grep -c . "$E/planche.lst" || true)              # retenues pour la planche
    tuile=$(awk -v n="$nbimg" 'BEGIN { c = 8; if (n < c) c = n; r = int((n + c - 1) / c); printf "%dx%d", c, r }')
    (ffmpeg -v error -y -f concat -safe 0 -i <(sed "s|^|file '|; s|$|'|" "$E/planche.lst") \
       -filter_complex "scale=256:-1,tile=$tuile:padding=3" -frames:v 1 "$O/planche.png") \
      && echo "planche : $O/planche.png ($tuile, $nbimg images)"
  else
    echo "planche : aucune image dans la séance"
  fi
  if [ "${ADB_FAKE:-0}" = 1 ] && [ -s "$APPELS" ]; then
    conclure 10 ÉCHEC "$(grep -c . "$APPELS" | tr -d ' ') appel(s) adb réel(s) en ADB_FAKE : $APPELS"
  elif [ "${REMIS_ETAT:-}" != identique ] || [ "${VARS_REMIS_ETAT:-}" != identique ]; then
    conclure 10 ÉCHEC "sc2.env ${REMIS_ETAT:-?}, Variables.txt ${VARS_REMIS_ETAT:-?} — rapport malgré tout : $O/rapport.md"
  else
    conclure 10 PASS "rapport.md, planche ($nbimg images), sc2.env et Variables.txt remis à l'identique"
  fi
  {                                   # écrit APRÈS le verdict de l'étape 10 : la table est complète
    echo "# Séance Fold — $(date '+%d/%m/%Y %H:%M')"
    echo
    [ "${ADB_FAKE:-0}" = 1 ] && echo "MODE ADB_FAKE : séance rejouée sur les fixtures ($FX), aucun téléphone touché."
    echo "- appareil : $D ; APK : $A ; sortie : $O"
    echo "- étapes jouées : $ETAPES"
    echo
    echo "| Étape | Verdict | Chiffres clés | Artefacts |"
    echo "|---|---|---|---|"
    while IFS='|' read -r n v d; do
      [ -n "$n" ] || continue
      ch=""
      [ -f "$O/etape-$n/chiffres.txt" ] && ch=$(tr '\n' ';' < "$O/etape-$n/chiffres.txt" | sed 's/;*$//; s/;/ ; /g')
      art="$(find "$O/etape-$n" -type f 2>/dev/null | wc -l | tr -d ' ') fichiers, $(find "$O/etape-$n" -name '*.png' 2>/dev/null | wc -l | tr -d ' ') images"
      echo "| $n | $v | ${ch:-—} | $art |"
    done < "$O/verdicts.txt"
    echo
    echo "- sc2.env remis à l'identique : ${REMIS_ETAT:-?} ; Variables.txt ($VARS_AVANT au départ) remis à l'identique : ${VARS_REMIS_ETAT:-?} ; planche : $nbimg images retenues"
    [ "${ADB_FAKE:-0}" = 1 ] && echo "- appels adb réels échappés au mode fake : $(grep -c . "$APPELS" 2>/dev/null || true)"
  } > "$O/rapport.md"
}

# --- déroulé ----------------------------------------------------------------------------------------------------------
echo; echo "===== séance : étapes $ETAPES"
for n in $ETAPES; do
  echo; echo "===== étape $n"
  case "$n" in
    1) e1 ;; 2) e2 ;; 3) e3 ;; 4) e4 ;; 5) e5 ;; 6) e6 ;; 7) e7 ;; 8) e8 ;; 9) e9 ;; 10) e10 ;;
  esac
done

NP=$(cut -d'|' -f2 "$O/verdicts.txt" 2>/dev/null | grep -cx 'PASS' || true)
NS=$(cut -d'|' -f2 "$O/verdicts.txt" 2>/dev/null | grep -cx 'SKIP' || true)
NE=$(cut -d'|' -f2 "$O/verdicts.txt" 2>/dev/null | grep -cx 'ÉCHEC' || true)
echo
echo "SÉANCE : $NP PASS, $NS SKIP, $NE ÉCHEC — rapport : $O/rapport.md"
[ "$ECHECS" = 0 ] || exit 1
