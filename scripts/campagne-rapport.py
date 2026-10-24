#!/usr/bin/env python3
"""campagne-rapport.py — tableau Markdown du parcours de campagne du banc (20/10).

  scripts/campagne-rapport.py <dossier de sortie>

Lit chaque sous-dossier <sortie>/<carte>/ produit par campagne-banc.sh :
  run.log    journal du run (VICTOIRE détectée, [BILAN] frames / draws / [MANQUE], [ARRÊT], replis de texture)
  resume.txt « rc=N en S s »
  shaders/   bytecodes capturés (vs_/ps_<hash>.bin, SC2_SHADERS_DUMP)
Imprime un tableau Markdown (une ligne par mission) puis la liste agrégée des adresses MANQUE (mission,
appelant). Tolérant : mission sans run.log ou sans resume.txt → tirets, jamais d'erreur.

V1.4 (22/10) : dernière colonne « replis de texture » — échantillonnages servis par la texture factice, lus sur
« [BILAN] samplers des draws réels : … replis sur texture factice : A sans texture liée, B cube, C texture sans
données, D format non géré » (sc2_d3d9.c), avec le nombre de textures distinctes des lignes « [BILAN]   repli :
texture … — <cause> ×N ». « sans données » : 0 EXIGÉ (texture créée mais jamais remplie : rendu faux) ; une mission
qui en a est nommée sous le tableau. « — » : ligne absente (bilan non atteint). lire_mission() rend aussi, pour
scripts/nonreg.py, les échantillons (t, tour) des lignes « [INPUT] banc t=…s tour N : … » (un tour de pompe = une
frame présentée, à une près) et le total « [BILAN] [INPUT] banc … N tours de pompe, S s ».
"""
import re
import sys
from pathlib import Path

RE_VICTOIRE = re.compile(r"\[INPUT\] banc t=([0-9.]+)s(?: tour \d+)? : VICTOIRE détectée \(([^)]*)\)")
RE_RC = re.compile(r"rc=(-?\d+)")
RE_DUREE_RESUME = re.compile(r"en (\d+) s")
RE_DUREE_BILAN = re.compile(r"\[BILAN\] \[INPUT\] banc \(SC2_BANC_ENTREES\) :.* ([0-9.]+) s$", re.MULTILINE)
RE_FRAMES = re.compile(r"\[BILAN\] frames présentées \(Present\) : (\d+)")
RE_DRAWS = re.compile(r"\[BILAN\] draws : (\d+) RÉELS \(shaders traduits\), (\d+) « lumière »")
RE_MQ_CONTINUE = re.compile(r"\[MANQUE-CONTINUE\] .*? (0x[0-9A-F]{8}) n=\d+ .*?appelant (0x[0-9A-F]{8})")
RE_ARRET = re.compile(r"\[ARRÊT\] \[thread \d+\] (.*)")
RE_SAMPLERS = re.compile(r"\[BILAN\] samplers des draws réels : (\d+) servis \(texture ou cible\), replis sur texture "
                         r"factice : (\d+) sans texture liée, (\d+) cube, (\d+) texture sans données, (\d+) format non géré")
RE_REPLI = re.compile(r"\[BILAN\]\s+repli : texture (\d+)×(\d+) D3DFMT (0x[0-9A-Fa-f]+) \(genre (\d+), usage (0x[0-9A-Fa-f]+), "
                      r"pool (\d+)\) — (sans données|cube|format non géré) ×(\d+)")
RE_ECHANTILLON = re.compile(r"^\[INPUT\] banc t=([0-9.]+)s tour (\d+) : (.*)$", re.MULTILINE)
RE_BILAN_BANC = re.compile(r"^\[BILAN\] \[INPUT\] banc \(SC2_BANC_ENTREES\) : .*?(\d+) tours de pompe, ([0-9.]+) s$", re.MULTILINE)
CAUSES = {"sans données": "sans_donnees", "cube": "cube", "format non géré": "format"}


def _manque_bilan(ligne):
    """« [BILAN] [MANQUE] 0x…  1re frame N  occurrences M  …  appelant 0x… » → (adresse, M, appelant)."""
    t = ligne.split()
    if len(t) < 3 or not t[2].startswith("0x"):
        return None
    try:
        occ = int(t[t.index("occurrences") + 1])
        ap = t[t.index("appelant") + 1]
    except (ValueError, IndexError):
        return None
    return t[2], occ, ap


def lire_mission(d):
    """Toute la lecture d'un dossier de mission, tolérante aux fichiers absents."""
    m = {"carte": d.name, "rc": None, "victoire": None, "fichier": "", "duree": None, "frames": None,
         "reels": None, "lumiere": None, "manque": [], "arrets": [], "shaders": 0,
         "replis": None, "replis_detail": [], "echantillons": [], "victoire_tour": None, "banc_tours": None,
         "banc_s": None}
    resume = d / "resume.txt"
    if resume.exists():
        r = resume.read_text(errors="replace")
        rc = RE_RC.search(r)
        if rc:
            m["rc"] = int(rc.group(1))
        du = RE_DUREE_RESUME.search(r)
        if du:
            m["duree"] = int(du.group(1))
    log = d / "run.log"
    texte = log.read_text(errors="replace") if log.exists() else ""
    v = RE_VICTOIRE.search(texte)
    if v:
        m["victoire"] = float(v.group(1))
        m["fichier"] = v.group(2)
    fr = RE_FRAMES.search(texte)
    if fr:
        m["frames"] = int(fr.group(1))
    dr = RE_DRAWS.search(texte)
    if dr:
        m["reels"], m["lumiere"] = int(dr.group(1)), int(dr.group(2))
    if m["duree"] is None:
        db = RE_DUREE_BILAN.search(texte)
        if db:
            m["duree"] = float(db.group(1))
    for ligne in texte.splitlines():
        if ligne.startswith("[BILAN] [MANQUE] 0x"):
            q = _manque_bilan(ligne)
            if q:
                m["manque"].append(q)
    if not m["manque"]:                       # bilan pas atteint (arrêt brutal) : adresses vues au fil du run
        vus = {}
        for ligne in texte.splitlines():
            q = RE_MQ_CONTINUE.search(ligne)
            if q:
                vus.setdefault(q.group(1), (q.group(1), 0, q.group(2)))
        m["manque"] = list(vus.values())
    for ligne in texte.splitlines():
        a = RE_ARRET.match(ligne)
        if not a:
            continue
        raison = a.group(1).strip()
        if "arrêt volontaire" in raison or raison.startswith("SC2_BANC_ENTREES : QUIT"):
            continue
        m["arrets"].append(raison)
    sp = RE_SAMPLERS.search(texte)
    if sp:
        m["replis"] = {"servis": int(sp.group(1)), "sans_texture": int(sp.group(2)), "cube": int(sp.group(3)),
                       "sans_donnees": int(sp.group(4)), "format": int(sp.group(5))}
    for r in RE_REPLI.finditer(texte):
        m["replis_detail"].append({"l": int(r.group(1)), "h": int(r.group(2)), "fmt": r.group(3), "genre": int(r.group(4)),
                                   "usage": r.group(5), "pool": int(r.group(6)), "cause": CAUSES[r.group(7)],
                                   "n": int(r.group(8))})
    if m["replis"] is None and m["replis_detail"]:     # résumé absent, détail présent : on somme le détail
        m["replis"] = {"servis": None, "sans_texture": None, "cube": 0, "sans_donnees": 0, "format": 0}
        for r in m["replis_detail"]:
            m["replis"][r["cause"]] += r["n"]
    for e in RE_ECHANTILLON.finditer(texte):
        t, tour = float(e.group(1)), int(e.group(2))
        m["echantillons"].append((t, tour))
        if m["victoire_tour"] is None and e.group(3).startswith("VICTOIRE détectée"):
            m["victoire_tour"] = tour
    bb = RE_BILAN_BANC.search(texte)
    if bb:
        m["banc_tours"], m["banc_s"] = int(bb.group(1)), float(bb.group(2))
    sh = d / "shaders"
    if sh.is_dir():
        m["shaders"] = len(list(sh.glob("*.bin")))
    return m


def texte_replis(m):
    """Cellule « replis de texture » : 0, — (inconnu) ou le détail par cause, « sans données » en tête."""
    r = m["replis"]
    if r is None:
        return "—"
    noms = (("sans_donnees", "sans données"), ("cube", "cube"), ("format", "format non géré"),
            ("sans_texture", "sans texture liée"))
    parts = []
    for cle, nom in noms:
        n = r.get(cle) or 0
        if n:
            ntex = sum(1 for x in m["replis_detail"] if x["cause"] == cle)
            parts.append(f"{nom} {n}" + (f" ({ntex} tex.)" if ntex else ""))
    return ", ".join(parts) if parts else "0"


def _cell(x):
    return "—" if x is None else str(x)


def rapport(racine):
    """Le rapport complet en Markdown (texte), une ligne par mission trouvée sous <racine>."""
    missions = sorted((lire_mission(p) for p in racine.glob("*") if p.is_dir()), key=lambda m: m["carte"]) \
        if racine.is_dir() else []
    l = ["| carte | rc | victoire | durée (s) | frames | draws réels / lumière | MANQUE | arrêts | shaders "
         "| replis de texture |",
         "|---|---|---|---|---|---|---|---|---|---|"]
    for m in missions:
        vic = f"oui t={m['victoire']:.1f}s" if m["victoire"] is not None else "non"
        dr = f"{_cell(m['reels'])} / {_cell(m['lumiere'])}"
        mq = f"{len(m['manque'])} ({sum(q[1] for q in m['manque'])} occ.)" if m["manque"] else "0"
        ar = str(len(m["arrets"]))
        if m["arrets"]:
            ar += f" ({m['arrets'][0][:40]}…)" if len(m["arrets"][0]) > 40 else f" ({m['arrets'][0]})"
        l.append(f"| {m['carte']} | {_cell(m['rc'])} | {vic} | {_cell(m['duree'])} | {_cell(m['frames'])} "
                 f"| {dr} | {mq} | {ar} | {m['shaders']} | {texte_replis(m)} |")
    gagnees = sum(1 for m in missions if m["victoire"] is not None)
    l.append("")
    l.append(f"{len(missions)} mission(s), {gagnees} victoire(s) détectée(s).")
    sd = [m["carte"] for m in missions if m["replis"] and m["replis"].get("sans_donnees")]
    if sd:
        l.append(f"Replis « sans données » : 0 exigé — {len(sd)} mission(s) en défaut : {', '.join(sd)}.")
    agg = {}
    for m in missions:
        for adresse, occ, appelant in m["manque"]:
            agg.setdefault(adresse, {"appelant": appelant, "missions": [], "occ": 0})
            agg[adresse]["missions"].append(m["carte"])
            agg[adresse]["occ"] += occ
    if agg:
        l.append("")
        l.append("## Adresses MANQUE agrégées")
        for adresse in sorted(agg, key=lambda a: -agg[a]["occ"]):
            a = agg[adresse]
            l.append(f"- {adresse} (appelant {a['appelant']}) : {a['occ']} occurrence(s) — "
                     f"{', '.join(sorted(set(a['missions'])))}")
    return "\n".join(l) + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    print(rapport(Path(sys.argv[1])), end="")
