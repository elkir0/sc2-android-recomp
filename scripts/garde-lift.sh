#!/usr/bin/env bash
# garde-lift.sh — passe 10p : garde commune des chaînes APK (scripts/build-apk.sh devbox, android/build-apk-mac.sh M4).
#
#   scripts/garde-lift.sh sources <dossier src/lifted>     avant la compilation
#   scripts/garde-lift.sh lib <libSC2Partout.so> [llvm-nm]  après (la .so réellement emballée)
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
*) echo "usage : $0 sources <src/lifted> | lib <libSC2Partout.so> [llvm-nm] | empreinte <src/lifted>" >&2; exit 64 ;;
esac
