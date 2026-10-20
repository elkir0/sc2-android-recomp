#!/usr/bin/env bash
# Passe 7y — preuve par élagage du manifest de données (android/manifest-install.txt). UN run à la fois, ≤ 300 s.
# Recette de run = celle de android/valider-7q.sh (mission : SC2_RUN_SANS_BNET=1, -run TRaynor01, clics « Close »),
# reprise ici en lecture (aucun fichier de 7q modifié) ; vues construites par android/outils/vue-manifest-7y.sh.
#   ETAPES (défaut : tout) : manifest | sans:<chemin> (vue AB moins ce chemin) | menu-A | menu-AB | menu-ABC
#                            | menu-avec:<chemin C> | menu-sans:<chemin>
#   BIN=<recomp-sc2 x86> (obligatoire : binaire du main) ; sorties hors git : recomp-sc2/build-7y-valider/
set -uo pipefail
W="$(cd "$(dirname "$0")/.." && pwd)"; O="$W/recomp-sc2/build-7y-valider"; mkdir -p "$O"
BIN="${BIN:?BIN=<binaire recomp-sc2 x86>}"; L="$O/valider${TAG:+-$TAG}.log"; T="${TEMPS:-300}"; MF="${MISSION_FRAMES:-1500}"
EXE="$HOME/code/reverse/sc2-partout/extraction/install/Versions/Base15405/SC2.exe"
MISSION=(SC2_ARGS="-run Maps/Campaign/TRaynor01.SC2Map" SC2_RUN_SANS_BNET=1 SC2_INPUT_SIM="$W/android/outils/clic-fermer-aide-7q.txt")
j() { echo "$*" | tee -a "$L"; }
j "=== passe 7y ${TAG:-} — $(date -u '+%F %T') UTC, $(git -C "$W" log --oneline -1 | cut -c1-60), binaire $(md5sum < "$BIN" | cut -c1-12), $T s par run"
j "free -h AVANT : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}') ; charge $(cut -d' ' -f1-3 /proc/loadavg)"
run() {   # <nom> <frames> <args vue…> -- [VAR=val…]
  local nom="$1" fr="$2" d="$O/run${TAG:+-$TAG}-$1"; shift 2; local va=(); while [ "$1" != -- ]; do va+=("$1"); shift; done; shift
  rm -rf "$d"; mkdir -p "$d"; local t0=$SECONDS
  j "--- $nom : $("$W/android/outils/vue-manifest-7y.sh" "$d/vue" "${va[@]}" | sed 's/ → .*//')"
  (cd "$d" && env SC2_ROOT="$d/sc2-c" SC2_INSTALL="$d/vue" SC2_EXE="$EXE" SC2_FRAMES_MAX="$fr" SC2_FRAMES_DIR="$d/frames" \
     SC2_AUDIO_SORTIE=0 "$@" timeout -s KILL "$T" "$BIN" > run.log 2>&1; echo $? > rc)
  local f; f=$(ls "$d/frames" 2>/dev/null | sed -E 's/[^0-9]//g' | sort -n | tail -1)
  j "  rc=$(cat "$d/rc") ($((SECONDS - t0)) s ; 124/137 = coupé au temps imparti) ; dernière frame enregistrée ${f:-aucune} ; 1res lectures MPQ $(grep -ac '1re lecture' "$d/run.log") ; fichiers de carte TRaynor01 $(grep -a 'TRaynor01.SC2Map\\' "$d/run.log" | grep -ac '1re lecture')"
  j "  archives lues : $(grep -a '1re lecture' "$d/run.log" | sed -E 's/^\[MPQ\] ([^ ]+) → .*/\1/' | sort | uniq -c | sort -rn | awk '{printf "%s×%s ", $2, $1}')"
  j "  sons : $(grep -ac 'createSound «' "$d/run.log") créés, $(grep -ac 'playSound «' "$d/run.log") lancés, rappel open en échec $(grep -ac 'rappel open du jeu' "$d/run.log") ; derniers : $(grep -a 'playSound «' "$d/run.log" | tail -2 | grep -oE '[A-Za-z0-9_&]+\.ogg' | tr '\n' ' ')"
  grep -aE '^\[ARRÊT\] \[|MANQUE|SIGSEGV|SIGBUS|introuvable|ne trouve|Fatal|fatal error' "$d/run.log" | sort | uniq -c | sort -rn | head -6 | cut -c1-220 | sed 's/^/  ! /' | tee -a "$L"
  rm -rf "$d/sc2-c"
}
ETAPES=("$@"); [ ${#ETAPES[@]} -eq 0 ] && ETAPES=(manifest "sans:Campaigns/Liberty.SC2Campaign/Base.SC2Maps" "sans:Mods/Liberty.SC2Mod/base.SC2Assets" \
  "sans:Campaigns/Liberty.SC2Campaign/enUS.SC2Assets" "sans:Campaigns/LibertyStory.SC2Campaign/Base.SC2Data" \
  "sans:Versions/Shaders14513/baselineCache.bin" "sans:Campaigns/Liberty.SC2Campaign/base.SC2Assets" \
  "sans:Mods/Liberty.SC2Mod/enUS.SC2Assets" menu-A menu-AB menu-ABC "menu-avec:Battle.net/Battle.net.MPQ")
for e in "${ETAPES[@]}"; do case $e in
manifest) run manifest "$MF" AB -- "${MISSION[@]}" ;;
sans:*)   p="${e#sans:}"; run "sans-$(echo "$p" | tr '/' '_')" "$MF" AB --sans "$p" -- "${MISSION[@]}" ;;
menu-A)   run menu-A 400 A -- ;;
menu-AB)  run menu-AB 400 AB -- ;;
menu-ABC) run menu-ABC 400 ABC -- ;;
menu-avec:*) p="${e#menu-avec:}"; run "menu-AB-avec-$(echo "$p" | tr '/' '_')" 400 AB --avec "$p" -- ;;
menu-sans:*) p="${e#menu-sans:}"; run "menu-AB-sans-$(echo "$p" | tr '/' '_')" 400 AB --sans "$p" -- ;;
esac; done
j "free -h APRÈS : $(free -h | awk '/Mem:/{print $3" utilisés / "$7" dispo"}')"
j "=== fin $(date -u '+%T') UTC"
true
