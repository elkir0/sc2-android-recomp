#!/bin/sh
# echecs-connus.sh — enveloppe ctest d'un test hôte qui a des échecs CONNUS sur cette plate-forme (V0.2, 22/10).
#   sh tests-hote/echecs-connus.sh <binaire> <raison du SKIP> <motif connu>…
# Contrat du binaire (celui de test_win32_compat) : une ligne « PASS  … » ou « FAIL  … » par assertion, puis le bilan
# « RÉSULTAT : <p> PASS, <f> FAIL » ; code 0 sans échec, 1 sinon. Verdict de l'enveloppe :
#   0  (PASS)  : code 0 ;
#   77 (SKIP)  : code 1, bilan présent qui compte autant de FAIL que de lignes FAIL (au moins une), et chaque ligne FAIL
#                contient l'un des motifs connus (chaîne fixe) ;
#   1  (ÉCHEC) : tout le reste — FAIL hors des motifs ; code autre que 0 ou 1 (signal, abort, assert : 128+n) ; bilan
#                absent ou incohérent (plantage APRÈS les échecs connus, sortie tronquée, exit prématuré).
# La sortie du binaire est recopiée telle quelle (ctest --output-on-failure, verifier.sh) avant la ligne de verdict.
set -u
[ $# -ge 3 ] || { echo "ÉCHEC  usage : echecs-connus.sh <binaire> <raison> <motif connu>…"; exit 1; }
b="$1"; raison="$2"; shift 2
out="$("$b" 2>&1)"; rc=$?
printf '%s\n' "$out"
[ "$rc" -eq 0 ] && exit 0
[ "$rc" -eq 1 ] || { echo "ÉCHEC  code $rc : plantage ou signal (le test ne rend que 0 ou 1)"; exit 1; }
nf=$(printf '%s\n' "$out" | grep -c '^FAIL')
bilan=$(printf '%s\n' "$out" | sed -nE 's/^RÉSULTAT : [0-9]+ PASS, ([0-9]+) FAIL$/\1/p' | tail -1)
[ -n "$bilan" ] && [ "$bilan" = "$nf" ] && [ "$nf" -gt 0 ] \
  || { echo "ÉCHEC  bilan absent ou incohérent (RÉSULTAT : ${bilan:-absent} FAIL ; $nf ligne(s) FAIL)"; exit 1; }
for m in "$@"; do set -- "$@" -e "$m"; shift; done      # motifs → -e m1 -e m2 … pour grep -F
inconnus=$(printf '%s\n' "$out" | grep '^FAIL' | grep -v -F "$@")
[ -n "$inconnus" ] && { echo "ÉCHEC  hors des échecs connus :"; printf '%s\n' "$inconnus"; exit 1; }
echo "SKIP  $nf échec(s) CONNU(S) seulement : $raison"
exit 77
