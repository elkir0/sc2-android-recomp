#!/bin/bash
# injecter-bugs-v22.sh — V2.2 (22/10) : injection de bugs dans l'instanciation ; chaque mutant doit être TUÉ par son test.
#   scripts/injecter-bugs-v22.sh [purs|gpu|tout]        (défaut : tout)
# Chaque mutant est appliqué à une COPIE des fichiers, dans build-tests/v22-mutants/<M>/, jamais en place. La substitution
# est littérale (perl \Q…\E) et contrôlée par cmp : une copie inchangée vaut « mutant non appliqué » = ÉCHEC. Un témoin
# sans mutation doit d'abord passer (sinon le juge lui-même est faux).
#   purs (M1-M6) : runtime/sc2_d3d9_inst.h muté + tests/test_instanciation.c, cc -O0 ; tué ⇔ code ≠ 0.
#   gpu  (M7-M9) : runtime/sc2_d3d9v.c muté, lié à tests/test_d3d9v.c (dessin instancié contre N dessins de
#                  référence, MoltenVK au Mac) ; code 77 (Vulkan absent) = « GPU : non jugé ».
# Dernière ligne : « v22 : x/6 mutants purs tués ; y/3 mutants GPU tués » ; code 0 si tous tués, 77 si le GPU n'a pas pu
# juger (et les purs tous tués), 1 sinon. Durée : ≈ 10 s pour les purs, ≈ 1 min pour le GPU (compilation du backend).
set -u
W="$(cd "$(dirname "$0")/.." && pwd)"
RT="$W/recomp-sc2/runtime"; TR="$W/recomp-sc2/tests"
O="$W/build-tests/v22-mutants"
QUOI="${1:-tout}"
case "$QUOI" in purs|gpu|tout) ;; *) echo "usage : $0 [purs|gpu|tout]" >&2; exit 64 ;; esac
limite() { perl -e 'alarm shift; exec @ARGV' "$@"; }
mkdir -p "$O"
# substitution littérale de $2 par $3 dans le fichier $1 (première occurrence seulement)
muter() { AVANT="$2" APRES="$3" perl -0pi -e 's/\Q$ENV{AVANT}\E/$ENV{APRES}/' "$1"; }

# ---- mutants purs -------------------------------------------------------------------------------------------------
PURS=(
  "M1|BaseVertexIndex appliqué au flux d'instance|*debut = 0;|*debut = (uint64_t)premier * stride;"
  "M2|N ignoré|p.n = n ? n : 1;|p.n = 1;"
  "M3|diviseur > 1 ignoré|if (v > 1 && !p.repli)|if (0)"
  "M4|drapeaux INSTANCEDATA/INDEXEDDATA inversés|} else if (f & D3DSSF_INSTANCEDATA) {|} else if (f & D3DSSF_INDEXEDDATA) {"
  "M5|borne de troncature omise|return k < n ? (uint32_t)k : n;|return n;"
  "M6|plage d'instance = plage de sommets|*octets = (uint64_t)n * stride;|*octets = (uint64_t)nsommets * stride;"
)
GPU=(
  "M7|instanceCount en dur|vkCmdDrawIndexed(CB, d->nombre, ninst,|vkCmdDrawIndexed(CB, d->nombre, 1,"
  "M8|taux par instance oublié|(k->inst >> i) & 1u ?|0 ?"
  "M9|firstInstance décalé|d->decalage_sommet, 0);|d->decalage_sommet, 1);"
)
NPT=0; NGT=0; NGJ=1; KO=0
pur() {   # $1 = dossier, $2 = mutant (vide : témoin) ; code du test
  local D="$O/$1"
  rm -rf "$D"; mkdir -p "$D"
  cp "$RT/sc2_d3d9_inst.h" "$TR/test_instanciation.c" "$D/"
  if [ -n "${2:-}" ]; then
    IFS='|' read -r _ _ av ap <<< "$2"
    muter "$D/sc2_d3d9_inst.h" "$av" "$ap"
    cmp -s "$RT/sc2_d3d9_inst.h" "$D/sc2_d3d9_inst.h" && return 200
  fi
  cc -O0 -w -I"$D" "$D/test_instanciation.c" -o "$D/t" > "$D/cc.log" 2>&1 || return 201
  limite 120 "$D/t" > "$D/sortie.txt" 2>&1
}
if [ "$QUOI" != gpu ]; then
  pur temoin ""; r=$?
  if [ "$r" != 0 ]; then echo "[ÉCHEC] témoin pur (sans mutation) : code $r ($O/temoin/)"; KO=1
  else
    echo "[OK]    témoin pur : $(grep -m1 '^RÉSULTAT' "$O/temoin/sortie.txt")"
    for m in "${PURS[@]}"; do
      IFS='|' read -r id nom _ _ <<< "$m"
      pur "$id" "$m"; r=$?
      case "$r" in
        0)   echo "[ÉCHEC] $id $nom : SURVIVANT (le test passe)"; KO=1 ;;
        200) echo "[ÉCHEC] $id $nom : mutant non appliqué (texte absent de sc2_d3d9_inst.h)"; KO=1 ;;
        201) echo "[ÉCHEC] $id $nom : ne compile pas ($O/$id/cc.log)"; KO=1 ;;
        *)   NPT=$((NPT + 1))
             echo "[OK]    $id $nom : tué (code $r) — $(grep -m1 '^FAIL' "$O/$id/sortie.txt" | cut -c1-150)" ;;
      esac
    done
  fi
fi

# ---- mutants GPU --------------------------------------------------------------------------------------------------
VK_INC=""
for d in /opt/homebrew/include "${VULKAN_SDK:-/nonexistent}/include" /usr/include; do
  [ -f "$d/vulkan/vulkan.h" ] && { VK_INC="$d"; break; }
done
gpu() {   # $1 = dossier, $2 = mutant (vide : témoin)
  local D="$O/$1" L=()
  rm -rf "$D"; mkdir -p "$D"
  cp "$RT/sc2_d3d9v.c" "$D/"
  if [ -n "${2:-}" ]; then
    IFS='|' read -r _ _ av ap <<< "$2"
    muter "$D/sc2_d3d9v.c" "$av" "$ap"
    cmp -s "$RT/sc2_d3d9v.c" "$D/sc2_d3d9v.c" && return 200
  fi
  [ "$(uname)" = Darwin ] && L=(-Wl,-U,_sc2_att_debut -Wl,-U,_sc2_att_fin -Wl,-U,_sc2_att_present)
  cc -O0 -w -I"$RT" -I"$VK_INC" "$TR/test_d3d9v.c" "$D/sc2_d3d9v.c" -o "$D/t" -lpthread -ldl "${L[@]}" > "$D/cc.log" 2>&1 || return 201
  (cd "$D" && limite 300 ./t > sortie.txt 2>&1)
}
if [ "$QUOI" != purs ]; then
  if [ -z "$VK_INC" ]; then echo "[SKIP]  GPU : en-têtes Vulkan absents — non jugé"; NGJ=0
  else
    gpu temoin-gpu ""; r=$?
    if [ "$r" = 77 ]; then echo "[SKIP]  GPU : Vulkan indisponible (témoin en 77) — non jugé"; NGJ=0
    elif [ "$r" != 0 ]; then echo "[ÉCHEC] témoin GPU (sans mutation) : code $r ($O/temoin-gpu/)"; KO=1
    else
      echo "[OK]    témoin GPU : $(grep -m1 '^RÉSULTAT' "$O/temoin-gpu/sortie.txt")"
      for m in "${GPU[@]}"; do
        IFS='|' read -r id nom _ _ <<< "$m"
        gpu "$id" "$m"; r=$?
        case "$r" in
          0)   echo "[ÉCHEC] $id $nom : SURVIVANT (le test passe)"; KO=1 ;;
          77)  echo "[SKIP]  $id $nom : Vulkan indisponible — non jugé"; NGJ=0 ;;
          200) echo "[ÉCHEC] $id $nom : mutant non appliqué (texte absent de sc2_d3d9v.c)"; KO=1 ;;
          201) echo "[ÉCHEC] $id $nom : ne compile pas ($O/$id/cc.log)"; KO=1 ;;
          *)   NGT=$((NGT + 1))
               echo "[OK]    $id $nom : tué (code $r) — $(grep -m1 '^FAIL' "$O/$id/sortie.txt" | cut -c1-150)" ;;
        esac
      done
    fi
  fi
fi
case "$QUOI" in
  purs) echo "v22 : $NPT/${#PURS[@]} mutants purs tués" ;;
  gpu)  echo "v22 : $NGT/${#GPU[@]} mutants GPU tués$([ "$NGJ" = 0 ] && echo ' (GPU : non jugé)')" ;;
  *)    echo "v22 : $NPT/${#PURS[@]} mutants purs tués ; $NGT/${#GPU[@]} mutants GPU tués$([ "$NGJ" = 0 ] && echo ' (GPU : non jugé)')" ;;
esac
[ "$KO" = 0 ] || exit 1
[ "$NGJ" = 1 ] || [ "$QUOI" = purs ] || exit 77
exit 0
