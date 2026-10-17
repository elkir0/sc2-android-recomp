#!/usr/bin/env bash
# verdict-fold.sh - passe 9e : verdict d'affichage Android a partir d'un dump logcat (traces passe 9d).
#   scripts/verdict-fold.sh dump.log      ou      adb logcat -d | scripts/verdict-fold.sh
# Derniere ligne : VERDICT: CPU-OK | SWAPCHAIN-OK | MIXTE | H1-fenetre | SWAPCHAIN-FATAL | H1-verrou | H1-geometrie
#                  | AUCUN-AFFICHAGE
# SWAPCHAIN-FATAL (passe 9f) : 1er vkQueuePresentKHR negatif, rien de livre ; prime sur H1-verrou, car le repli CPU
# echoue alors au verrou (fenetre deja liee a la surface Vulkan) : cause = la swapchain, pas le verrou.
# Exit : 0 = un chemin d'affichage livre (tampons postes > 0 ou frames presentees > 0), 1 = aucun, 2 = pas de trace SC2.
# Le bilan "[ANDROID] affichage" n'existe qu'a la sortie propre du jeu ; sans lui, les compteurs viennent des
# lignes au fil de l'eau (1er tampon poste, 1re frame presentee, echecs journalises).
set -eu
export LC_ALL=C   # logcat est en UTF-8 : motifs ASCII seulement, accents couverts par [^ ]*

src="${1:--}"
if [ "$src" != "-" ] && [ ! -r "$src" ]; then echo "verdict-fold: $src illisible" >&2; exit 2; fi
tmp="$(mktemp)"; trap 'rm -f "$tmp"' EXIT
cat -- "$src" | grep -a 'SC2Partout' > "$tmp" || true

if [ ! -s "$tmp" ]; then
  echo "aucune ligne SC2Partout dans le dump"
  echo "VERDICT: AUCUN-AFFICHAGE | dump sans traces SC2"
  exit 2
fi

compte() { grep -acE -- "$1" "$tmp" || true; }   # ERE : grep BSD (macOS) compris
# valeur numerique apres un libelle dans la DERNIERE ligne qui le contient
champ() { grep -a -- "$1" "$tmp" | tail -1 | sed -nE "s/.*$2 ([0-9]+).*/\1/p"; }

# --- bilan de sortie (passe 9d), s'il existe ---
bil="$(grep -a '\[ANDROID\] affichage : ' "$tmp" | tail -1 || true)"
if [ -n "$bil" ]; then
  src_cpt="bilan de sortie"
  nb_present="$(champ '\[ANDROID\] affichage : ' 'affichage :')"
  postes="$(champ '\[ANDROID\] affichage : ' 'tampons post[^ ]*')"
  verrou="$(champ '\[ANDROID\] affichage : ' 'de verrou')"
  post_ko="$(champ '\[ANDROID\] affichage : ' 'de post')"
  geo="$(champ '\[ANDROID\] affichage : ' 'vides')"
  perdues="$(champ '\[ANDROID\] affichage : ' 'perdues')"
  sans_fen="$(champ '\[ANDROID\] affichage : ' 'sans fen[^ ]*')"
  refusees="$(champ '\[ANDROID\] affichage : ' 'refus[^ ]*')"
else
  src_cpt="lignes au fil de l'eau, sans bilan de sortie : 1 = au moins un"
  nb_present="?"
  postes="$(compte '\[AFFICHAGE\] 1er tampon post')"
  verrou="$(compte '\[AFFICHAGE\] ANativeWindow_lock : ')"
  post_ko="$(compte '\[AFFICHAGE\] ANativeWindow_unlockAndPost : ')"
  geo="$(compte 'om[^ ]*trie NON pos')"
  perdues="$(compte '\[FENETRE\] TERM_WINDOW')"
  sans_fen="$(compte 'rendu sans fen')"
  refusees="$(compte 'CPU refus')"
fi
term="$(compte '\[FENETRE\] TERM_WINDOW')"
init="$(compte '\[FENETRE\] INIT_WINDOW')"
instr9d="$(compte '\[FENETRE\]|\[AFFICHAGE\]|\[ANDROID\] affichage')"

# --- swapchain ---
vk="$(grep -a '\[VKP\] 1er vkQueuePresentKHR' "$tmp" | tail -1 | sed -nE 's/.* (-?[0-9]+)[[:space:]]*$/\1/p' || true)"
sc_bil="$(grep -a '\[ANDROID\] swapchain ' "$tmp" | tail -1 || true)"
if [ -n "$sc_bil" ]; then
  presentees="$(printf '%s\n' "$sc_bil" | sed -nE 's/.*swapchain [^:]*: ([0-9]+) frames.*/\1/p')"
else
  presentees="$(compte '\[VKP\] 1re frame pr')"
fi
: "${postes:=0}" "${verrou:=0}" "${post_ko:=0}" "${geo:=0}" "${perdues:=0}" "${sans_fen:=0}" "${refusees:=0}" "${presentees:=0}"

vk_etat="absent"
case "$vk" in
  "") ;;
  0) vk_etat="SUCCESS" ;;
  1000001003) vk_etat="SUBOPTIMAL (acceptable)" ;;
  -*) vk_etat="FATAL" ;;
  *) vk_etat="positif non standard" ;;
esac

# --- rapport ---
echo "lignes SC2Partout : $(wc -l < "$tmp") ; instrumentation 9d : $([ "$instr9d" -gt 0 ] && echo presente || echo ABSENTE)"
echo "compteurs ($src_cpt) : Present $nb_present ; tampons postes $postes ; echecs de verrou $verrou ; echecs de post $post_ko ;" \
     "geometries vides $geo ; fenetres perdues $perdues ; Present sans fenetre $sans_fen ; cibles refusees $refusees"
echo "fenetre : INIT_WINDOW x$init, TERM_WINDOW x$term"
echo "swapchain : 1er vkQueuePresentKHR = ${vk:-absent} ($vk_etat) ; frames presentees $presentees${sc_bil:+ (bilan)}"
[ -n "$bil" ] && echo "bilan : ${bil#*\[ANDROID\] }"
[ -n "$sc_bil" ] && echo "bilan : ${sc_bil#*\[ANDROID\] }"

cpu_ok=0; sc_ok=0
[ "$postes" -gt 0 ] && cpu_ok=1
[ "$presentees" -gt 0 ] && [ "$vk_etat" != "FATAL" ] && sc_ok=1
perte=0; { [ "$sans_fen" -gt 0 ] || [ "$term" -gt 0 ]; } && perte=1

if   [ $cpu_ok = 1 ] && [ $sc_ok = 1 ]; then v=MIXTE
elif [ $sc_ok = 1 ]; then v=SWAPCHAIN-OK
elif [ $cpu_ok = 1 ]; then v=CPU-OK
elif [ $perte = 1 ]; then v=H1-fenetre
elif [ "$vk_etat" = "FATAL" ]; then v=SWAPCHAIN-FATAL
elif [ "$verrou" -gt 0 ]; then v=H1-verrou
elif [ "$geo" -gt 0 ]; then v=H1-geometrie
else v=AUCUN-AFFICHAGE
fi
[ "$vk_etat" = "FATAL" ] && echo "ATTENTION : vkQueuePresentKHR fatal ($vk), la swapchain ne presente pas"
[ $perte = 1 ] && { [ $cpu_ok = 1 ] || [ $sc_ok = 1 ]; } && echo "ATTENTION : fenetre perdue en cours de run (TERM_WINDOW x$term, Present sans fenetre $sans_fen)"
[ "$instr9d" -eq 0 ] && echo "ATTENTION : aucune trace 9d ([FENETRE]/[AFFICHAGE]) : APK anterieur a la passe 9d ?"

echo "VERDICT: $v | postes=$postes presentees=$presentees vk=${vk:-absent} verrou=$verrou post=$post_ko geo=$geo perdues=$perdues sans_fenetre=$sans_fen refusees=$refusees present=$nb_present"
if [ $cpu_ok = 1 ] || [ $sc_ok = 1 ]; then exit 0; fi
exit 1
