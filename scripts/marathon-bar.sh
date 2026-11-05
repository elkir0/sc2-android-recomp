#!/bin/bash
# marathon-bar.sh <binaire> <dossier> [plan] [progression] — banc « marathon » : plusieurs missions du mode
#   histoire en UNE SEULE exécution du jeu, pilotées par un plan, sans joueur (24/10).
#   1. scripts/marathon-scenario.py génère le scénario (briques de scripts/entrees/marathon/) dans un fichier
#      temporaire — banc-bar.sh vide <dossier> (rm -rf) à son démarrage, le scénario n'est recopié qu'à la
#      fin dans <dossier>/scenario.txt.
#   2. scripts/banc-bar.sh joue le scénario (verrou du banc ~/.sc2-verrous/banc : attendre un autre
#      utilisateur de la machine est normal) avec la progression donnée (défaut : celle de banc-bar.sh,
#      ~/code/sc2-apk/progression/apres-mission1 — mission 1 terminée). Une campagne NEUVE se joue avec
#      une progression VIDE : un dossier Banks/ sans fichier (l'instantané 0 est alors vide, et l'oracle
#      signale la banque « apparue » à la première victoire).
#   3. En arrière-plan, une boucle de 2 s (tant que le jeu tourne) surveille <dossier>/docs : chaque NOUVEAU
#      *Victory.SC2Save — la sauvegarde « Liberation Day » déjà dans la progression de départ ne compte pas —
#      attend 10 s de calme (la banque s'écrit autour de la victoire) puis copie docs/Banks dans
#      <dossier>/progression/<n>-<nom>/Banks ; l'instantané 0-depart part de la progression de départ, que le
#      run n'écrit jamais. La mémoire du jeu est sondée toutes les 10 s dans <dossier>/memoire.csv
#      (colonnes epoch,rss_ko ; pid par pgrep -n -f « ^<binaire> », seulement une fois run.log ouvert — avant,
#      ce serait le processus d'un autre utilisateur). Rien n'est jamais tué.
#   4. <dossier>/marathon.md : une ligne par mission du plan (carte lue dans resume.txt, victoire, instantanés,
#      verdict de scripts/progression-oracle.py entre deux instantanés consécutifs, clés changées citées),
#      totaux (MANQUE, « lumière », RSS début/max/fin en Mo) et verdict global. MARATHON : PASS exige chaque
#      mission gagnée ET de la progression après chacune, 0 MANQUE, 0 « lumière », tous les segments
#      déclenchés, aucun arrêt anormal et un jeu sorti propre. Code de retour : 0 seulement sur PASS.
set -u
BIN=${1:?usage : marathon-bar.sh <binaire> <dossier> [plan] [progression]}
D=${2:?dossier manquant}
R="$(cd "$(dirname "$0")/.." && pwd)"
PLAN=${3:-$R/scripts/entrees/marathon/marsara.plan}
PROG=${4:-$HOME/code/sc2-apk/progression/apres-mission1}
[ -x "$BIN" ] || { echo "marathon-bar : binaire absent : $BIN"; exit 1; }
[ -f "$PLAN" ] || { echo "marathon-bar : plan absent : $PLAN"; exit 1; }
[ -d "$PROG/Banks" ] || { echo "marathon-bar : progression absente : $PROG"; exit 1; }
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
mkdir -p "$D" 2>/dev/null && D=$(cd "$D" && pwd) || { echo "marathon-bar : dossier invalide : $D"; exit 1; }

# scénario en fichier temporaire (banc-bar.sh vide <dossier> au démarrage) ; journal de banc-bar.sh de même.
# <dossier> est vidé ICI aussi : pendant l'attente du verrou du banc, un run.log/progression d'un run
# précédent ferait croire à la boucle de surveillance que notre jeu tourne déjà.
S=$(mktemp "${TMPDIR:-/tmp}/marathon-scenario.XXXXXX") || exit 1
J=$(mktemp "${TMPDIR:-/tmp}/marathon-banc.XXXXXX") || exit 1
trap 'rm -f "$S" "$J"' EXIT
python3 "$R/scripts/marathon-scenario.py" "$PLAN" -o "$S" || exit 2
rm -rf "${D:?}"; mkdir -p "$D"

BANC_BAR_SCENARIO="$S" "$R/scripts/banc-bar.sh" "$BIN" "$D" "$PROG" > "$J" 2>&1 &
banc=$!

# Surveiller et photographier. « connus » : sauvegardes déjà traitées (ou d'origine, baseline au premier
# passage où run.log existe — docs est alors copié). « attente » : LA sauvegarde nouvelle en cours de calme
# (les missions s'espace de minutes : une à la fois suffit, la suivante est prise au tour suivant).
connus="
"
attente=""
attente_t=0
n=1
i=0
photographier() {   # <nom de sauvegarde> : copie docs/Banks dans progression/<n>-<nom sans Victory.SC2Save>
    local nom=$1 base
    base="${nom%Victory.SC2Save}"; base="${base% }"
    mkdir -p "$D/progression/$n-$base"
    cp -R "$D/docs/Banks" "$D/progression/$n-$base/Banks" 2>/dev/null
    echo "marathon-bar : victoire $n — « $nom » → progression/$n-$base"
    n=$((n + 1))
}
while kill -0 "$banc" 2>/dev/null; do
    if [ -f "$D/run.log" ]; then
        if [ ! -d "$D/progression" ]; then      # instantané 0 : progression de départ, intacte
            mkdir -p "$D/progression/0-depart"
            cp -R "$PROG/Banks" "$D/progression/0-depart/Banks"
            while IFS= read -r f; do
                [ -n "$f" ] && connus="$connus${f##*/}
"
            done <<EOF
$(find "$D/docs" -name '*Victory.SC2Save' 2>/dev/null)
EOF
        fi
        if [ -z "$attente" ]; then              # une sauvegarde nouvelle ? (non encore traitée)
            while IFS= read -r f; do
                [ -n "$f" ] || continue
                case "$connus" in
                    *"${f##*/}"*) ;;
                    *) attente=${f##*/}; attente_t=$SECONDS; break ;;
                esac
            done <<EOF
$(find "$D/docs" -name '*Victory.SC2Save' 2>/dev/null)
EOF
        fi
        if [ -n "$attente" ] && [ $((SECONDS - attente_t)) -ge 25 ]; then
            connus="$connus$attente
"
            photographier "$attente"
            attente=""
        fi
        i=$((i + 1))
        if [ $((i % 5)) -eq 0 ]; then           # sonde mémoire : toutes les 10 s
            pid=$(pgrep -n -f "^$BIN" 2>/dev/null)
            if [ -n "$pid" ]; then
                rss=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
                if [ -n "$rss" ]; then
                    [ -f "$D/memoire.csv" ] || echo "epoch,rss_ko" > "$D/memoire.csv"
                    echo "$(date +%s),$rss" >> "$D/memoire.csv"
                fi
            fi
        fi
    fi
    sleep 2
done
wait "$banc"; rc_banc=$?
# rattrapage : le jeu est sorti, la banque ne bouge plus — une victoire des dernières secondes part telle quelle
if [ -f "$D/run.log" ] && [ -d "$D/docs" ]; then
    if [ -z "$attente" ]; then
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            case "$connus" in
                *"${f##*/}"*) ;;
                *) attente=${f##*/}; break ;;
            esac
        done <<EOF
$(find "$D/docs" -name '*Victory.SC2Save' 2>/dev/null)
EOF
    fi
    if [ -n "$attente" ]; then
        connus="$connus$attente
"
        photographier "$attente"
        attente=""
    fi
fi
mkdir -p "$D"; cp "$S" "$D/scenario.txt"
echo "marathon-bar : banc-bar.sh rc=$rc_banc — rapport…"

# Rapport marathon.md (une ligne par mission, totaux, verdict) ; le code de sortie du Python est celui du
# banc : 0 seulement sur MARATHON : PASS.
if python3 - "$D" "$PLAN" "$R" <<'PY'
import importlib.util
import re
import sys
from pathlib import Path

D, PLAN, R = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
BANQUE = "TCampaign.SC2Bank"

spec = importlib.util.spec_from_file_location("progression_oracle", R / "scripts" / "progression-oracle.py")
oracle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oracle)

def lire(nom):
    p = D / nom
    return p.read_text(errors="replace") if p.is_file() else ""

resume, runlog = lire("resume.txt"), lire("run.log").splitlines()

# cartes du plan, dans l'ordre (une ligne « mission <Carte> » chacune)
missions = [m.split()[1] for m in PLAN.read_text().splitlines()
            if m.strip() and not m.strip().startswith("#") and m.split()[0] == "mission"]

# segments du bilan : « … segment <k> QUAND:<decl> [MAX:…] [SINON:…] : déclenché à t=<s>s, tour N (détail)… »
segments = []
for l in resume.splitlines():
    if not l.startswith("[BILAN] [INPUT] banc segment "):
        continue
    dec = l.split(" : ", 1)
    if len(dec) != 2:
        continue
    m = re.search(r"segment (\d+) QUAND:(\S+)", dec[0])
    if not m:
        continue
    decl, suffixe = m.group(2), dec[1]
    declenche = suffixe.startswith("déclenché à t=")
    t = float(re.match(r"déclenché à t=([\d.]+)s", suffixe).group(1)) if declenche else None
    d = re.search(r"\(([^)]*)\)", suffixe)
    segments.append({"decl": decl, "declenche": declenche, "t": t, "detail": d.group(1) if d else ""})

# par mission (dans l'ordre) : lecture (dernière CARTE non-TStory01, ou CHARGEMENT pour « mission * »,
# déclenchée avant la victoire) et victoire
cartes, vus = [], []
carte = None
for s in segments:
    if s["declenche"] and (s["decl"] == "CHARGEMENT"
                           or (s["decl"].startswith("CARTE:") and s["decl"] != "CARTE:TStory01")):
        carte = s
    if s["decl"] == "VICTOIRE":
        vus.append(s)
        cartes.append(carte)

# instantanés progression/0-depart, 1-<nom>, … (numéros croissants)
snaps = []
if (D / "progression").is_dir():
    snaps = sorted((p for p in (D / "progression").iterdir() if re.match(r"^\d+-", p.name)),
                   key=lambda p: int(p.name.split("-", 1)[0]))
snaps_par_num = {int(p.name.split("-", 1)[0]): p for p in snaps}

def verdict(n):     # oracle entre l'instantané n-1 et n : clés changées citées, ou le mal
    a, b = snaps_par_num.get(n - 1), snaps_par_num.get(n)
    if not a or not b:
        return "instantané manquant", False
    try:
        r = oracle.analyser(a, b, [BANQUE])
    except oracle.ErreurBanque as e:
        return f"banque illisible ({e})", False
    if not r["differences"]:
        return "AUCUNE progression", False
    if r["banques"][0]["etat"] == "absente-avant":
        # campagne neuve : l'instantané 0 est vide, la banque apparaît à la première victoire
        return f"PROGRESSION : banque {BANQUE} apparue (campagne neuve, instantané 0 vide)", True
    # MissionCompletion d'abord (c'est le signal), puis l'âge, le reste ensuite — borné pour tenir la ligne
    cle = lambda s: ({"MissionCompletion": 0, "MissionCompletedAge": 1}.get(s["nom"], 2), s["nom"])
    morceaux = []
    for bque in r["banques"]:
        for s in sorted(bque["sections"], key=cle):
            if s["ajoutes"]:
                morceaux.append("+" + ", +".join(s["ajoutes"]) + f" ({s['nom']})")
            for c, mod in s["modifiees"].items():
                morceaux.append(f"~{c} : {oracle.decrire_valeurs(mod['ancien'])} → "
                                f"{oracle.decrire_valeurs(mod['nouveau'])} ({s['nom']})")
    return "PROGRESSION : " + " ; ".join(morceaux[:6]) + (" ; …" if len(morceaux) > 6 else ""), True

# compteurs d'anomalies (run.log) : MANQUE, « lumière » (dans resume.txt), arrêts non volontaires
manque = sum(1 for l in runlog if "[BILAN] [MANQUE]" in l)
m = re.search(r"\[BILAN\] draws : \d+ RÉELS \(shaders traduits\), (\d+) « lumière »", resume)
lumiere = int(m.group(1)) if m else None
arrets = 0
for l in runlog:
    m = re.match(r"\[ARRÊT\] \[thread \d+\] (.*)", l)
    if m:
        raison = m.group(1)
        if "arrêt volontaire" not in raison and not raison.startswith("SC2_BANC_ENTREES : QUIT"):
            arrets += 1
    elif "SIGSEGV" in l or "SIGBUS" in l or "DEVICE_LOST" in l:
        arrets += 1

m = re.match(r"rc=(\d+) en (\d+) s", resume)
rc_jeu, duree = (int(m.group(1)), int(m.group(2))) if m else (None, None)

rss = [int(l.split(",")[1]) for l in lire("memoire.csv").splitlines()
       if "," in l and l.split(",")[1].isdigit()]
rss_mo = [round(k / 1024) for k in rss]
scen = lire("scenario.txt").splitlines()
attendus = sum(1 for l in scen if l.startswith("QUAND:"))
declenches = sum(1 for s in segments if s["declenche"])

raisons = []
gagnees = [s for s in vus if s["declenche"]]
if rc_jeu != 0:
    raisons.append(f"jeu sorti en rc={rc_jeu}" if rc_jeu is not None else "jeu jamais lancé (resume.txt absent)")
if len(gagnees) < len(missions):
    raisons.append(f"{len(missions) - len(gagnees)} victoire(s) manquante(s) sur {len(missions)}")
if manque:
    raisons.append(f"{manque} MANQUE")
if lumiere:
    raisons.append(f"{lumiere} « lumière »")
if arrets:
    raisons.append(f"{arrets} arrêt(s) anormal(aux)")
if declenches < attendus or len(segments) < attendus:
    raisons.append(f"segments déclenchés {declenches}/{attendus}")

l = [f"# Marathon — {D.name}",
     "",
     f"Plan : {PLAN.name} — {len(missions)} mission(s) : {', '.join(missions)} ; "
     f"run : {'rc=' + str(rc_jeu) + f' en {duree} s' if rc_jeu is not None else 'jamais lancé'} ; "
     f"scénario : {attendus} segments.",
     "",
     "| n | carte | lecture | victoire | instantanés | progression (oracle) |",
     "|---|-------|---------|----------|-------------|----------------------|"]
for k in range(len(missions)):
    n = k + 1
    c = cartes[k] if k < len(cartes) else None
    if not c:
        carte_txt = "—"
    elif c["decl"].startswith("CARTE:"):
        carte_txt = f"{c['decl'].split(':', 1)[1]} à t={c['t']}s"   # « CARTE: » ôté
    else:                       # CHARGEMENT : la carte de « mission * » n'est pas identifiée par le plan
        carte_txt = f"chargement (carte inconnue) à t={c['t']}s"
    vict_txt = (f"t={vus[k]['t']}s ({vus[k]['detail']})" if k < len(vus) and vus[k]["declenche"] else "—")
    s_n, s_av = snaps_par_num.get(n), snaps_par_num.get(n - 1)
    inst_txt = f"{s_av.name if s_av else '—'} → {s_n.name if s_n else '—'}"
    v_txt, ok = verdict(n)
    if k < len(vus) and vus[k]["declenche"] and not ok:
        raisons.append(f"mission {n} : pas de progression constatée")
    l.append(f"| {n} | {missions[k]} | {carte_txt} | {vict_txt} | {inst_txt} | {v_txt} |")

l += ["",
      f"Totaux : {len(gagnees)}/{len(missions)} victoire(s) ; MANQUE : {manque} ; "
      f"« lumière » : {lumiere if lumiere is not None else '—'} ; "
      f"RSS : {' → '.join(str(x) for x in (rss_mo[0], max(rss_mo), rss_mo[-1])) if rss_mo else '—'} Mo "
      f"(début → max → fin) ; segments déclenchés : {declenches}/{attendus} ; arrêts anormaux : {arrets}.",
      "",
      "MARATHON : " + ("PASS" if not raisons else "ÉCHEC — " + " ; ".join(raisons))]
(D / "marathon.md").write_text("\n".join(l) + "\n")
sys.exit(0 if not raisons else 1)
PY
then rc=0; else rc=1; fi
cat "$D/marathon.md"
exit $rc
