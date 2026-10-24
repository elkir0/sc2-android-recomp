#!/usr/bin/env bash
# verifier.sh — porte de vérification de SC2 Partout (V0.2, 22/10) : moins de 2 min, cache CMake chaud ou froid, sans lift.
#
#   scripts/verifier.sh [--rapide] [--apk <fichier.apk>]
#
# Une ligne [OK] / [SKIP] / [ÉCHEC] par contrôle (détails en retrait, journaux dans build-tests/verifier/) :
#   1. tests hôtes : projet tests-hote/ (-O0, SANS le code lifté) dans build-tests/, ctest --output-on-failure ; un
#      contrôle par test ; un ÉCHEC cite les lignes ÉCHEC/FAIL du test (obtenu/attendu), un SKIP sa raison ;
#   2. test_presentation_10a (clang du NDK, #include de sc2_android_main.c, -DAPRES) : compilé, puis exécuté sur
#      l'AVD emulator-5554 s'il est listé par « adb devices » (SKIP sinon : l'émulateur n'est JAMAIS lancé ici) ;
#      binaire poussé sous un nom propre au processus (deux arbres ne s'écrasent pas sur l'AVD) ;
#   3. python3 -m unittest sur scripts/test_*.py ;
#   4. bash -n sur tous les .sh du dépôt (hors build*, extraction, .venv, tools) ;
#   5. verdict-fold.sh sur scripts/verdict-fixtures (verdict et code attendus : table VERDICTS ci-dessous ; une
#      fixture sans ligne dans la table est un ÉCHEC), lecture sur stdin, dump sans trace SC2 ;
#   6. ADB_FAKE=1 scripts/test-matin.sh : chemin livré, non livré, 2e sc2.env présent au départ ; sorties dans
#      build-tests/verifier/matin-* par TEST_MATIN_SORTIE (jamais /tmp/fold-matin, celui des vrais tests du Fold) —
#      SKIP si test-matin.sh ne connaît pas TEST_MATIN_SORTIE ;
#   7. scripts/verrou.sh (V0.3), s'il existe ET s'il a un mode de test : deux prises simultanées, la seconde refusée
#      (75) avec VERROU_DELAI=0 et servie après attente avec VERROU_DELAI=10. Les vrais verrous de ~/.sc2-verrous ne
#      sont jamais tenus : VERROU_DOSSIER=<dossier> (vrais noms banc et edl, 2 jetons, dans build-tests/verifier/
#      verrous, supprimé ensuite), à défaut un nom test-verifier-<pid> accepté ; sinon SKIP ;
#   8. scripts/variables-sc2.sh verifier (V0.4), s'il existe : 0 = toutes les SC2_* du code documentées dans
#      docs/VARIABLES.md, 1 = il nomme les manquantes — sinon SKIP ;
#   9. ADB_FAKE=1 scripts/bar-fold.sh (V0.1), si ce mode existe — sinon SKIP ;
#  10. ADB_FAKE=1 scripts/seance-fold.sh (V0.2) : la séance Fake complète (rapport + aucune étape en ÉCHEC) ;
#  11. --apk F : scripts/valider-apk.sh F (clé épinglée par SC2_APK_CLE_SHA256, lift optimisé…) ; absent par défaut (il exige une APK).
# --rapide : sans 2 et 3 (NDK, unittest Python), pour itérer ; la porte, c'est la commande sans option.
# Garde-fou : pendant 6, 9 et 10, un « adb » factice en tête du PATH refuse tout appel réel (aucun téléphone touché).
# Deux verifier.sh dans le même arbre : le second attend le premier (verrou build-tests/.verifier.verrou).
# Dernière ligne : « verifier : N OK, M SKIP, K ÉCHEC en S s » ; code de sortie 0 seulement sans ÉCHEC.
# Variables : JOBS (compilation, défaut 8), ANDROID_NDK (défaut ~/tools/android-ndk-r28).
set -u
W="$(cd "$(dirname "$0")/.." && pwd)"
B="$W/build-tests"
J="$B/verifier"
mkdir -p "$J"

# un seul verifier.sh à la fois par arbre (build-tests partagé) ; lockf sur macOS, flock sous Linux
if [ -z "${VERIFIER_VERROU_TENU:-}" ]; then
  export VERIFIER_VERROU_TENU=1
  if [ -x /usr/bin/lockf ]; then exec /usr/bin/lockf -t 600 "$B/.verifier.verrou" bash "$0" "$@"
  elif command -v flock >/dev/null 2>&1; then exec flock -w 600 "$B/.verifier.verrou" bash "$0" "$@"
  fi
fi

RAPIDE=0; APK=""
while [ $# -gt 0 ]; do
  case "$1" in
    --rapide) RAPIDE=1 ;;
    --apk) APK="${2:?--apk <fichier.apk>}"; shift ;;
    -h|--help) awk 'NR > 1 && !/^#/ { exit } NR > 1' "$0"; exit 0 ;;   # l'en-tête, jusqu'à la 1re ligne de code
    *) echo "verifier : option inconnue « $1 » (--rapide, --apk <fichier>)" >&2; exit 2 ;;
  esac
  shift
done

maintenant() { perl -MTime::HiRes=time -e 'printf "%.2f\n", time'; }
duree() { awk -v a="$1" -v b="$(maintenant)" 'BEGIN { printf "%.1f", b - a }'; }
T0="$(maintenant)"
NOK=0; NSKIP=0; NKO=0
ok()     { NOK=$((NOK + 1));     printf '[OK]    %s\n' "$*"; }
skip()   { NSKIP=$((NSKIP + 1)); printf '[SKIP]  %s\n' "$*"; }
ko()     { NKO=$((NKO + 1));     printf '[ÉCHEC] %s\n' "$*"; }
retrait() { sed 's/^/          /'; }
limite() { perl -e 'alarm shift; exec @ARGV' "$@"; }            # ni timeout ni gtimeout sur ce Mac

# adb factice pour les étapes ADB_FAKE : tout appel réel échoue (code 97) et le dit
mkdir -p "$J/faux-adb"
printf '#!/bin/sh\necho "verifier.sh : adb RÉEL refusé pendant un ADB_FAKE (adb $*)" >&2\nexit 97\n' > "$J/faux-adb/adb"
chmod +x "$J/faux-adb/adb"

echo "verifier — $W ($(git -C "$W" rev-parse --short HEAD 2>/dev/null || echo '?')$( [ -n "$(git -C "$W" status --porcelain 2>/dev/null | head -1)" ] && echo ', arbre modifié'))"

# --- 1. tests hôtes (ctest) -------------------------------------------------------------------------
# sortie d'un test dans Testing/Temporary/LastTest.log (blocs « n/N Testing: nom » … « "nom" end time »)
sortie_test() {
  awk -v n="$1" '$0 ~ "^[0-9]+/[0-9]+ Testing: " && $NF == n { p = 1; next }
                 p && index($0, "\"" n "\" end time") == 1 { p = 0 }
                 p' "$B/Testing/Temporary/LastTest.log" 2>/dev/null
}
etape_hote() {
  local t gen="" l nom etat s n=0
  t="$(maintenant)"
  [ -f "$B/CMakeCache.txt" ] || { command -v ninja >/dev/null 2>&1 && gen="-G Ninja"; }
  if ! cmake -S "$W/tests-hote" -B "$B" $gen > "$J/cmake.log" 2>&1; then
    ko "tests hôtes : configuration CMake ($J/cmake.log)"; tail -12 "$J/cmake.log" | retrait; return
  fi
  if ! cmake --build "$B" -j "${JOBS:-8}" > "$J/build.log" 2>&1; then
    ko "tests hôtes : compilation ($J/build.log)"; grep -E 'error|erreur|FAILED' "$J/build.log" | head -15 | retrait; return
  fi
  local tc; tc="$(duree "$t")"
  ctest --test-dir "$B" --output-on-failure -j 4 --timeout 120 > "$J/ctest.log" 2>&1
  while IFS= read -r l; do
    n=$((n + 1))
    nom="$(printf '%s\n' "$l" | sed -E 's/^ *[0-9]+\/[0-9]+ Test +#[0-9]+: ([^ ]+) .*/\1/')"
    s="$(printf '%s\n' "$l" | sed -nE 's/.* ([0-9.]+) sec$/\1/p')"
    case "$l" in
      *" Passed "*) ok "$nom (${s:-?} s)" ;;
      *Skipped*)
        skip "$nom : $(sortie_test "$nom" | grep -m1 -E '^ *SKIP' | sed -E 's/^ *SKIP *//' || true)" ;;
      *)
        etat="$(printf '%s\n' "$l" | sed -E 's/.*\*\*\*([^0-9]*[A-Za-z]).*/\1/')"
        ko "$nom : ${etat:-échec}"
        sortie_test "$nom" | grep -E -A2 '^ *(ÉCHEC|FAIL|\[ÉCHEC\])' | grep -v -E '^(--$| *(PASS|ok) )' | head -16 | retrait ;;
    esac
  done < <(grep -E '^ *[0-9]+/[0-9]+ Test +#' "$J/ctest.log")
  [ "$n" -gt 0 ] || { ko "tests hôtes : ctest n'a lancé aucun test ($J/ctest.log)"; tail -8 "$J/ctest.log" | retrait; }
  printf '          (%d tests hôtes : configuration + compilation %s s, total %s s)\n' "$n" "$tc" "$(duree "$t")"
}

# --- 2. test_presentation_10a (NDK, AVD facultatif) -------------------------------------------------
etape_10a() {
  local t NDK CC sortie
  t="$(maintenant)"
  NDK="${ANDROID_NDK:-$HOME/tools/android-ndk-r28}"
  CC="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/clang 2>/dev/null | head -1)"
  [ -n "$CC" ] && [ -x "$CC" ] || { skip "test_presentation_10a : NDK absent ($NDK)"; return; }
  # recette de l'en-tête du test (symboles non résolus : glue, AAsset, hooks, runtime — jamais appelés par ses scénarios)
  if ! "$CC" --target=aarch64-linux-android26 -static -O1 -I"$W/android/jni" -I"$W/android/jni/compat" \
        -I"$W/recomp-sc2/runtime" -I"$NDK/sources/android/native_app_glue" -DAPRES \
        "$W/android/jni/tests/test_presentation_10a.c" -Wl,--unresolved-symbols=ignore-all \
        -o "$B/test_presentation_10a" > "$J/10a-cc.log" 2>&1; then
    ko "test_presentation_10a : compilation NDK ($J/10a-cc.log)"; grep -E 'error' "$J/10a-cc.log" | head -10 | retrait; return
  fi
  if ! command -v adb >/dev/null 2>&1 || ! adb devices 2>/dev/null | grep -qE '^emulator-5554[[:space:]]+device$'; then
    skip "test_presentation_10a : compilé ($(duree "$t") s) ; emulator-5554 absent d'adb devices — exécution sautée"; return
  fi
  sortie=/data/local/tmp/sc2-test-presentation-10a-$$         # propre au processus : deux arbres, un seul AVD
  if ! limite 60 adb -s emulator-5554 push "$B/test_presentation_10a" "$sortie" > "$J/10a.log" 2>&1; then
    ko "test_presentation_10a : adb push vers emulator-5554"; tail -3 "$J/10a.log" | retrait; return
  fi
  limite 120 adb -s emulator-5554 shell "chmod 755 $sortie && $sortie; echo \"rc=\$?\"; rm -f $sortie" >> "$J/10a.log" 2>&1
  if grep -q '^rc=0' "$J/10a.log" && grep -q '^== bilan' "$J/10a.log" && ! grep -q 'ÉCRASÉE' "$J/10a.log"; then
    ok "test_presentation_10a sur emulator-5554 : $(grep -c 'POST n°' "$J/10a.log") tampons postés, gardes intactes ($(duree "$t") s)"
  else
    ko "test_presentation_10a sur emulator-5554 ($J/10a.log)"
    grep -E 'ÉCRASÉE|^rc=|== bilan' "$J/10a.log" | head -6 | retrait
  fi
}

# --- 3. unittest Python ------------------------------------------------------------------------------
etape_python() {
  local t r
  t="$(maintenant)"
  ls "$W"/scripts/test_*.py > /dev/null 2>&1 || { skip "python3 -m unittest : aucun scripts/test_*.py"; return; }
  (cd "$W" && PYTHONDONTWRITEBYTECODE=1 limite 120 python3 -m unittest discover -s scripts -p 'test_*.py') > "$J/python.log" 2>&1
  r=$?
  if [ "$r" = 0 ]; then ok "python3 -m unittest : $(grep -m1 -E '^Ran ' "$J/python.log" | sed 's/^Ran //') ($(duree "$t") s)"
  else ko "python3 -m unittest : code $r ($J/python.log)"; grep -E '^(FAIL|ERROR|Ran|FAILED)' "$J/python.log" | head -10 | retrait; fi
}

# --- 4. bash -n ---------------------------------------------------------------------------------------
etape_bash() {
  local f n=0 kos=""
  while IFS= read -r f; do
    n=$((n + 1))
    bash -n "$f" > "$J/bash-n.tmp" 2>&1 || kos="$kos${f#$W/} : $(head -1 "$J/bash-n.tmp")
"
  done < <(find "$W" \( -type d \( -name 'build*' -o -name extraction -o -name .venv -o -name tools -o -name .git \) -prune \) \
                -o \( -type f -name '*.sh' -print \) | sort)
  rm -f "$J/bash-n.tmp"
  if [ -z "$kos" ]; then ok "bash -n : $n scripts .sh"
  else ko "bash -n : $(printf '%s' "$kos" | grep -c .) script(s) sur $n en erreur de syntaxe"; printf '%s' "$kos" | head -10 | retrait; fi
}

# --- 5. verdict-fold.sh sur ses fixtures -----------------------------------------------------------
# fixture  verdict attendu  code attendu (TRACE-PASSE9E.md, TRACE-PASSE9F.md)
VERDICTS="cpu-livrant CPU-OK 0
fatal-present SWAPCHAIN-FATAL 1
h1-fenetre-perdue H1-fenetre 1
h1-geometrie H1-geometrie 1
h1-verrou H1-verrou 1
mixte MIXTE 0
swapchain-ok-suboptimal SWAPCHAIN-OK 0"
etape_verdicts() {
  local f b att v r n=0 kos="" V="$W/scripts/verdict-fold.sh" D="$W/scripts/verdict-fixtures"
  [ -f "$V" ] || { ko "verdict-fold : $V absent"; return; }
  for f in "$D"/*.log; do
    [ -f "$f" ] || continue
    n=$((n + 1)); b="$(basename "$f" .log)"
    att="$(printf '%s\n' "$VERDICTS" | awk -v b="$b" '$1 == b { print $2 " " $3 }')"
    [ -n "$att" ] || { kos="$kos$b : aucun verdict attendu dans la table VERDICTS de verifier.sh
"; continue; }
    v="$(bash "$V" "$f" 2>&1 | grep -a '^VERDICT: ' | tail -1 | sed -E 's/^VERDICT: ([^ |]+).*/\1/')"
    bash "$V" "$f" > /dev/null 2>&1; r=$?
    [ "$v $r" = "$att" ] || kos="$kos$b : $v (code $r), attendu $att
"
  done
  # même verdict sur stdin ; dump sans aucune ligne SC2Partout : AUCUN-AFFICHAGE, code 2
  v="$(bash "$V" < "$D/cpu-livrant.log" 2>&1 | grep -a '^VERDICT: ' | sed -E 's/^VERDICT: ([^ |]+).*/\1/')"
  [ "$v" = CPU-OK ] || kos="${kos}stdin (cpu-livrant) : « $v », attendu CPU-OK
"
  printf 'rien de SC2\n' | bash "$V" > "$J/verdict-vide.log" 2>&1; r=$?
  { [ "$r" = 2 ] && grep -q '^VERDICT: AUCUN-AFFICHAGE' "$J/verdict-vide.log"; } || kos="${kos}dump sans SC2 : code $r, attendu 2 et AUCUN-AFFICHAGE
"
  if [ -z "$kos" ] && [ "$n" -gt 0 ]; then ok "verdict-fold : $n fixtures, stdin et dump sans SC2 conformes"
  else ko "verdict-fold : $(printf '%s' "$kos" | grep -c .) écart(s) sur $n fixtures"; printf '%s' "$kos" | retrait; fi
}

# --- 6. test-matin.sh en ADB_FAKE ---------------------------------------------------------------------
# Sorties par cas dans $J/matin-<fixture>-<env2>/ (TEST_MATIN_SORTIE) : le défaut /tmp/fold-matin est celui des VRAIS tests
# du Fold, que test-matin.sh vide au départ (dump, verdict, capture) — et que deux arbres se disputeraient.
etape_matin() {
  local T="$W/scripts/test-matin.sh" D="$W/scripts/verdict-fixtures" r kos="" cas fix env2 attv attr O L
  [ -f "$T" ] || { ko "test-matin : $T absent"; return; }
  grep -q 'TEST_MATIN_SORTIE' "$T" \
    || { skip "test-matin (ADB_FAKE) : test-matin.sh sans TEST_MATIN_SORTIE — il viderait /tmp/fold-matin (vrais tests du Fold)"; return; }
  for cas in "cpu-livrant 0 CPU-OK 0" "h1-verrou 0 H1-verrou 1" "cpu-livrant 1 CPU-OK 0"; do
    set -- $cas; fix=$1; env2=$2; attv=$3; attr=$4
    O="$J/matin-$fix-$env2"; L="$O.log"; rm -rf "$O"
    PATH="$J/faux-adb:$PATH" ADB_FAKE=1 TEST_MATIN_SORTIE="$O" FIXTURE="$D/$fix.log" FAKE_ENV2="$env2" ATTENTE=0 \
      limite 60 bash "$T" faux > "$L" 2>&1; r=$?
    grep -q "^RESUME: verdict=$attv " "$L" && [ "$r" = "$attr" ] \
      || kos="$kos$fix (2e sc2.env $env2) : code $r, $(grep -a '^RESUME' "$L" || echo 'sans RESUME') ; attendu $attv et code $attr
"
    if [ "$env2" = 1 ] && ! grep -q 'present : efface' "$L"; then
      kos="$kos$fix : le 2e sc2.env présent n'a pas été effacé
"; fi
    grep -q 'adb RÉEL refusé' "$L" && kos="$kos$fix : appel adb réel tenté en ADB_FAKE
"
    { [ -s "$O/dump.log" ] && [ -f "$O/verdict.txt" ] && ! grep -q '/tmp/fold-matin' "$L"; } \
      || kos="$kos$fix : sorties hors de TEST_MATIN_SORTIE ($O vide, ou /tmp/fold-matin cité)
"
  done
  if [ -z "$kos" ]; then ok "test-matin (ADB_FAKE) : livré, non livré, 2e sc2.env effacé"
  else ko "test-matin (ADB_FAKE) : $(printf '%s' "$kos" | grep -c .) écart(s)"; printf '%s' "$kos" | retrait; fi
}

# --- 7. verrou.sh (V0.3) ----------------------------------------------------------------------------
# Contrat attendu de verrou.sh (V0.3) : verrou.sh <nom> <commande…> ; VERROU_DELAI=0 → refus immédiat, VERROU_DELAI=n →
# attente bornée ; refus = code 75 (EX_TEMPFAIL) ; edl = 2 jetons edl-1/edl-2. Les VRAIS verrous (~/.sc2-verrous :
# build-lift, banc, emulateur…) ne sont jamais tenus ici — la porte tourne chez tous les agents à la fois. Mode de test :
#   - VERROU_DOSSIER=<d> (remplace ~/.sc2-verrous) : vrais noms banc (1 jeton) puis edl (2 jetons) dans $J/verrous,
#     supprimé à la fin ; une sonde (prise de banc par « true ») vérifie d'abord que le fichier naît bien dans $J/verrous ;
#   - à défaut, noms test-* : test-verifier-<pid> dans ~/.sc2-verrous, fichier et .qui supprimés à la fin ;
#   - aucun des deux (nom de test refusé : code 64) → SKIP.
# verrou_prises <nom> <n> : n porteurs prennent <nom> ENSEMBLE et le tiennent 3 s (démarrés en moins de 2 s, donc
# simultanés) ; pendant ce temps, une prise VERROU_DELAI=0 rend 75 en moins de 2 s, puis une prise VERROU_DELAI=10 attend
# (au moins 1 s) et rend 0. Écarts ajoutés à kos (local d'etape_verrou, portée dynamique), durées dans PR_D1/PR_D2/PR_D3.
verrou_prises() {
  local nom="$1" n="$2" i k p ps="" r t tous=0
  rm -f "$m".*
  t="$(maintenant)"
  for k in $(seq "$n"); do
    VERROU_DELAI=5 limite 20 bash "$V" "$nom" bash -c "touch '$m.$k'; sleep 3" > "$J/verrou-$nom-$k.log" 2>&1 & ps="$ps $!"
  done
  for i in $(seq 50); do
    tous=1; for k in $(seq "$n"); do [ -e "$m.$k" ] || tous=0; done
    [ "$tous" = 1 ] && break; sleep 0.1
  done
  PR_D1="$(duree "$t")"
  if [ "$tous" = 0 ]; then
    kos="${kos}$nom : $n porteur(s) non démarré(s) en 5 s ($J/verrou-$nom-*.log)
"
  else
    awk -v d="$PR_D1" 'BEGIN { exit !(d < 2) }' || kos="${kos}$nom : $n porteurs démarrés en $PR_D1 s (attendu < 2 s : simultanés)
"
    t="$(maintenant)"; VERROU_DELAI=0 limite 20 bash "$V" "$nom" true > "$J/verrou-$nom-0.log" 2>&1; r=$?; PR_D2="$(duree "$t")"
    [ "$r" = 75 ] && awk -v d="$PR_D2" 'BEGIN { exit !(d < 2) }' \
      || kos="${kos}$nom tenu par $n porteur(s), prise VERROU_DELAI=0 : code $r en $PR_D2 s (attendu : 75 immédiat)
"
    t="$(maintenant)"; VERROU_DELAI=10 limite 20 bash "$V" "$nom" true > "$J/verrou-$nom-10.log" 2>&1; r=$?; PR_D3="$(duree "$t")"
    [ "$r" = 0 ] && awk -v d="$PR_D3" 'BEGIN { exit !(d >= 1) }' \
      || kos="${kos}$nom tenu par $n porteur(s), prise VERROU_DELAI=10 : code $r en $PR_D3 s (attendu : attente puis 0)
"
  fi
  for p in $ps; do wait "$p" || kos="${kos}$nom : un porteur a rendu un code non nul ($J/verrou-$nom-*.log)
"; done
  rm -f "$m".*
}
etape_verrou() {
  local V="$W/scripts/verrou.sh" D="$J/verrous" nom=banc mode=dossier m="$J/verrou-tenu" r kos="" b=""
  local PR_D1 PR_D2 PR_D3
  [ -f "$V" ] || { skip "verrou : scripts/verrou.sh absent (V0.3)"; return; }
  rm -rf "$J/verrous"
  if ! grep -q 'VERROU_DOSSIER' "$V"; then D="$HOME/.sc2-verrous"; nom="test-verifier-$$"; mode="noms test-*"; fi
  mkdir -p "$D"
  local -x VERROU_DOSSIER="$D"        # exporté pour ce seul contrôle ; sans effet en mode test-* (dossier par défaut)
  VERROU_DELAI=0 limite 10 bash "$V" "$nom" true > "$J/verrou-sonde.log" 2>&1; r=$?
  if [ "$mode" = dossier ] && { [ "$r" != 0 ] || [ ! -e "$D/banc" ]; }; then
    ko "verrou : sonde VERROU_DOSSIER=$D : code $r, $D/banc $( [ -e "$D/banc" ] && echo créé || echo 'absent (dossier ignoré ?)')"
    head -3 "$J/verrou-sonde.log" | retrait; rm -rf "$J/verrous"; return
  fi
  if [ "$mode" != dossier ] && [ "$r" != 0 ]; then
    rm -f "$D/$nom" "$D/$nom.qui"
    if [ "$r" = 64 ]; then skip "verrou : verrou.sh sans mode de test (ni VERROU_DOSSIER, ni noms test-* : $nom refusé, code 64) — vrais verrous jamais pris ici"
    else ko "verrou : sonde $nom : code $r ($J/verrou-sonde.log)"; head -3 "$J/verrou-sonde.log" | retrait; fi
    return
  fi
  verrou_prises "$nom" 1
  b="$nom refusé en ${PR_D2:-?} s, servi après ${PR_D3:-?} s"
  if [ "$mode" = dossier ]; then
    PR_D2=""; PR_D3=""
    verrou_prises edl 2
    { [ -e "$D/edl-1" ] && [ -e "$D/edl-2" ]; } || kos="${kos}edl tenu deux fois : jetons edl-1 et edl-2 absents de $D
"
    b="$b ; edl tenu 2 fois (edl-1, edl-2), 3e prise refusée en ${PR_D2:-?} s, servie après ${PR_D3:-?} s"
    rm -rf "$J/verrous"
  else rm -f "$D/$nom" "$D/$nom.qui"; fi
  if [ -z "$kos" ]; then ok "verrou ($mode) : $b"
  else ko "verrou ($mode) : $(printf '%s' "$kos" | grep -c .) écart(s)"; printf '%s' "$kos" | retrait; fi
}

# --- 8. variables-sc2.sh (V0.4) -----------------------------------------------------------------------
etape_variables() {
  local V="$W/scripts/variables-sc2.sh" r
  [ -f "$V" ] || { skip "variables SC2_* : scripts/variables-sc2.sh absent (V0.4)"; return; }
  # mode « verifier » : sans argument, variables-sc2.sh LISTE et rend toujours 0 (faux vert)
  (cd "$W" && limite 60 bash "$V" verifier) > "$J/variables.log" 2>&1; r=$?
  if [ "$r" = 0 ]; then
    ok "$(tail -1 "$J/variables.log" | cut -c1-140)"
    grep -E 'plus lues' "$J/variables.log" | cut -c1-240 | retrait
  else ko "variables SC2_* : code $r ($J/variables.log)"; tail -10 "$J/variables.log" | cut -c1-240 | retrait; fi
}

# --- 9. bar-fold.sh en ADB_FAKE (V0.1) --------------------------------------------------------------
etape_bar() {
  local V="$W/scripts/bar-fold.sh" r
  { [ -f "$V" ] && grep -q 'ADB_FAKE' "$V"; } || { skip "bar-fold (ADB_FAKE) : mode absent de scripts/bar-fold.sh (V0.1)"; return; }
  PATH="$J/faux-adb:$PATH" ADB_FAKE=1 limite 120 bash "$V" verifier - > "$J/bar-fold.log" 2>&1; r=$?
  if [ "$r" = 0 ] && ! grep -q 'adb RÉEL refusé' "$J/bar-fold.log"; then ok "bar-fold (ADB_FAKE) : $(tail -1 "$J/bar-fold.log" | cut -c1-120)"
  else ko "bar-fold (ADB_FAKE) : code $r ($J/bar-fold.log)"; tail -6 "$J/bar-fold.log" | retrait; fi
}

# --- 9bis. seance-fold.sh en ADB_FAKE ---------------------------------------------------------------
etape_seance() {
  local V="$W/scripts/seance-fold.sh" S r
  [ -f "$V" ] || { skip "seance-fold (ADB_FAKE) : scripts/seance-fold.sh absent (V0.2)"; return; }
  S="$J/seance-fold"
  PATH="$J/faux-adb:$PATH" ADB_FAKE=1 SEANCE_ACCELERE=1 limite 240 bash "$V" - --sortie "$S" > "$J/seance-fold.log" 2>&1; r=$?
  if [ "$r" = 0 ] && ! grep -q 'adb RÉEL refusé' "$J/seance-fold.log" && [ -s "$S/rapport.md" ] \
     && grep -q 'Variables.txt (.*) remis.*identique : identique' "$S/rapport.md" \
     && grep -q 'fuite simulée' "$J/seance-fold.log" \
     && ! grep -q '\[étape [0-9]*\] ÉCHEC' "$J/seance-fold.log"; then
    ok "seance-fold (ADB_FAKE) : $(grep -c '\[étape.*\] PASS' "$J/seance-fold.log") étape(s) PASS, Variables.txt restauré (fuite simulée) — $(tail -1 "$J/seance-fold.log" | cut -c1-100)"
  else
    ko "seance-fold (ADB_FAKE) : code $r ($J/seance-fold.log)"; tail -8 "$J/seance-fold.log" | retrait
  fi
}

# --- 11. valider-apk.sh (--apk) ---------------------------------------------------------------------
etape_apk() {
  local r t
  t="$(maintenant)"
  bash "$W/scripts/valider-apk.sh" "$APK" > "$J/apk.log" 2>&1; r=$?
  if [ "$r" = 0 ]; then ok "valider-apk $(basename "$APK") : $(tail -1 "$J/apk.log") ($(duree "$t") s)"
  else ko "valider-apk $(basename "$APK") : $(tail -1 "$J/apk.log")"; grep '^\[FAIL\]' "$J/apk.log" | retrait; fi
}

etape_hote
if [ "$RAPIDE" = 1 ]; then
  skip "test_presentation_10a : --rapide"; skip "python3 -m unittest : --rapide"
else
  etape_10a; etape_python
fi
etape_bash
etape_verdicts
etape_matin
etape_verrou
etape_variables
etape_bar
etape_seance
[ -n "$APK" ] && etape_apk

echo "verifier : $NOK OK, $NSKIP SKIP, $NKO ÉCHEC en $(duree "$T0") s"
[ "$NKO" = 0 ]
