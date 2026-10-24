#!/bin/bash
# banc-escarmouche.sh — V4.10 (23/10) : une partie d'escarmouche contre l'IA au banc Mac, pilotée par un scénario.
#   scripts/banc-escarmouche.sh <binaire> <dossier> <scénario> [alarme_s]
#     scénario : fichier du banc d'entrées (scripts/entrees/escarmouche-*.txt), passé en SC2_BANC_ENTREES=@<fichier> ;
#     alarme   : arrêt dur (perl alarm) au bout de N secondes, défaut 900 (la partie longue en demande ≈ 1500).
#   La partie se choisit dans l'environnement de l'appelant (docs/VARIABLES.md, « Escarmouche ») : SC2_ESCARMOUCHE
#   (obligatoire), SC2_ESCARMOUCHE_RACE, _RACE_IA, _NB_IA, _IA, _VISION, _FIN — transmises telles quelles.
#   ex. SC2_ESCARMOUCHE=SteppesofWar SC2_ESCARMOUCHE_RACE=terran SC2_ESCARMOUCHE_RACE_IA=zerg SC2_ESCARMOUCHE_IA=facile \
#       SC2_ESCARMOUCHE_VISION=1 scripts/banc-escarmouche.sh <bin> <dossier> scripts/entrees/escarmouche-5min.txt 600
# Données du jeu (hors git) : SC2_EXTRACTION, défaut <clone principal>/extraction, comme banc-mac.sh ; garde-disque et
# verrou ~/.sc2-verrous/banc : scripts/lib-banc.sh (réentrant sous scripts/verrou.sh banc …). Lanceur simulé
# (SC2_RUN_SANS_BNET=1), SC2_MANQUE_CONTINUE=1 (un manque est compté, pas fatal), SC2_SANS_VIDEOS=1 ; SC2_FRAMES=0 : pas
# de frames périodiques, seules les CAPTURE:<nom> du scénario sont écrites (PNG sans perte après compactage).
# <dossier>/resume.txt : rc, lignes [ESCARMOUCHE] et [RUN], montée de la carte, MANQUE, « lumière », QUIT, segments du
# banc d'entrées, banques Escarmouche* écrites par le jeu (SC2_ESCARMOUCHE_FIN), captures. sc2-c est supprimé à la fin
# (la carte dérivée et ses scripts restent privés : jamais copiés ailleurs).
set -u
BIN=$(cd "$(dirname "${1:?binaire}")" && pwd)/$(basename "$1"); D=${2:?dossier}; SCEN=${3:?scénario}; AL=${4:-900}
R="$(cd "$(dirname "$0")/.." && pwd)"
. "$R/scripts/lib-banc.sh"
[ -n "${SC2_ESCARMOUCHE:-}" ] || { echo "banc-escarmouche : SC2_ESCARMOUCHE=<carte> requis (SC2_ESCARMOUCHE=liste les donne)"; exit 64; }
[ -f "$SCEN" ] || { echo "banc-escarmouche : scénario $SCEN absent"; exit 64; }
SCEN=$(cd "$(dirname "$SCEN")" && pwd)/$(basename "$SCEN")
banc_garde_disque "$D" 1 || exit 3
banc_verrou "$0" "$@"
X=${SC2_EXTRACTION:-~/sc2-partout/extraction}
[ -f "$X/install/Versions/Base15405/SC2.exe" ] || { echo "données du jeu absentes : $X"; exit 1; }
rm -rf "$D"; mkdir -p "$D"; D=$(cd "$D" && pwd); cd "$D" || exit 1
t=$SECONDS
{ echo "début $(date '+%F %T') ; binaire $BIN (sha256 $(shasum -a 256 "$BIN" | cut -c1-12)) ; scénario $SCEN ; alarme $AL s"
  env | grep '^SC2_ESCARMOUCHE' | sort; } > resume.txt
env SC2_ROOT="$D/sc2-c" SC2_EXE="$X/install/Versions/Base15405/SC2.exe" SC2_INSTALL="$X/install" \
    SC2_FRAMES=0 SC2_FRAMES_DIR="$D/frames" SC2_POLICES="${SC2_POLICES:-$R/android/assets/polices}" \
    SC2_RUN_SANS_BNET=1 SC2_MANQUE_CONTINUE=1 SC2_SANS_VIDEOS=1 SC2_BANC_ENTREES="@$SCEN" \
    perl -e "alarm $AL; exec @ARGV" "$BIN" > run.log 2>&1
rc=$?
echo "rc=$rc en $((SECONDS - t)) s" >> resume.txt
B="$D/sc2-c/Users/sc2/Documents/StarCraft II/Banks"
{ grep -a '^\[ESCARMOUCHE\]' run.log | cut -c1-400
  grep -a '^\[RUN\] lanceur simulé' run.log | head -1
  grep -a 'MONTÉE par le moteur : .*Escarmouche' run.log | head -1
  grep -a '^\[SPV\] pack SPIR-V externe' run.log | head -1
  echo "MANQUE-CONTINUE : $(grep -a -c 'MANQUE-CONTINUE' run.log) ligne(s)"
  grep -a '^\[BILAN\] \[MANQUE\]' run.log | head -2
  grep -a '^\[BILAN\] draws' run.log | head -1
  grep -a '^\[ARRÊT\]' run.log | head -1
  grep -a '^\[BILAN\] \[INPUT\] banc' run.log
  t_fin=$(grep -a 'QUAND:FINPARTIE déclenché (fin de partie' run.log | head -1 | sed -E 's/.*banc t=([0-9.]+)s.*/\1/')
  q_fin=$(grep -a '^\[MENU\] fin de partie' run.log | head -1 | sed -E 's/.*fin de partie : (.*), Present.*/\1/')
  if [ -n "$t_fin" ]; then echo "fin de partie : t=${t_fin} s (${q_fin:-sans détail})"; else echo "fin de partie : non détectée"; fi
  grep -a -i 'escarmouche[a-z]*\.sc2bank' run.log | head -5 | cut -c1-300
  echo "banques Escarmouche* : $(find "$B/" -iname 'Escarmouche*' 2>/dev/null | sed "s#^$D/##" | tr '\n' ' ')"
  echo "banques (*.SC2Bank) dans tout sc2-c : $(find "$D/sc2-c" -iname '*.SC2Bank' 2>/dev/null | sed "s#^$D/##" | tr '\n' ' ')"
  grep -a 'SIGSEGV\|SIGBUS\|ARRÊT\] faute\|DEVICE_LOST' run.log | head -3; } >> resume.txt
banc_compacter "$D" >> resume.txt
echo "captures : $(ls "$D/frames" 2>/dev/null | grep '\.png$' | tr '\n' ' ')" >> resume.txt
rm -rf "$D/sc2-c"
cat resume.txt
exit $rc
