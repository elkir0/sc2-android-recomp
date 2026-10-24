#!/usr/bin/env python3
"""nonreg.py — dépouillement de scripts/nonreg.sh (non-régression) et de scripts/banc-ab.sh (A/B d'images/s), V1.4.

  nonreg.py analyser <sortie> --mode fumee|complet --controles canon,bar,TRaynor01,… --binaire <bin>
            [--env VAR=val …] [--duree S] [--ref <dossier>] [--seul] [--tol-fps F] [--tol-images F]
      (F : fraction dans [0, 1), 0.05 = 5 % ; hors bornes ou illisible : refus d'argparse, code 2)
      Lit <sortie>/canon (banc-mac.sh defaut), <sortie>/bar (banc-bar.sh) et <sortie>/missions/<carte>
      (campagne-banc.sh), écrit <sortie>/nonreg.json et <sortie>/nonreg.md (aussi imprimé). Code 0 = PASS,
      1 = RÉGRESSION. Référence : <ref>/<mode>.json ; absente, les attendus sont les invariants (rc 0, victoire,
      0 MANQUE, 0 « lumière », 0 repli « sans données », 0 arrêt, 600 images et empreintes du canon actuel) ;
      images/s et les autres causes de repli ne sont pas jugés (notés).
  nonreg.py maj-ref <sortie> --ref <dossier> [--forcer]
      Pose la référence : <ref>/<mode>.json (copie de nonreg.json) et <ref>/<mode>/ (nonreg.md, rapport de campagne,
      resume.txt et empreintes.txt de chaque contrôle — JAMAIS d'image ni de run.log). Refus (code 4) si un contrôle
      n'est pas PASS hors images/s, ou si le run est partiel (--seul) ; --forcer passe outre.
  nonreg.py ab <sortie> [--debut S] [--fin S]
      A/B de banc-ab.sh (<sortie>/ab.txt : « <run> <étiquette> <paire> <binaire> <sha256> » par ligne) : plateau de
      chaque run, paires entrelacées A_i/B_i (i ≥ 1 : la paire 0 est la chauffe, affichée mais hors statistiques),
      triplets A_i B_i A_i+1, médianes et écart en %, temps de création des pipelines. Écrit banc-ab.json et
      banc-ab.md (aussi imprimé).

Critères d'un contrôle (PASS ou RÉGRESSION, contre la ligne de même nom de la référence) :
  rc          0 exigé (resume.txt « rc=N ») ;
  victoire    missions : VICTOIRE détectée ; bar : TStory01 puis TRaynor02 lues (lignes du journal) ; canon : sans objet ;
  MANQUE      adresses distinctes ≤ référence ;
  lumière     dessins « lumière » ≤ référence (bilan absent : RÉGRESSION) ;
  replis      replis de texture (« [BILAN] samplers des draws réels … »). « sans données » : 0 EXIGÉ, toujours ; une
              autre cause (cube, format non géré, sans texture liée) absente de la référence et présente : RÉGRESSION
              (sans référence : notée, non jugée — TZeratul04 de main a 4926 « sans texture liée » le 22/10) ;
  arrêts      arrêts non volontaires ([ARRÊT] [thread] hors QUIT et SC2_FRAMES_MAX, SIGSEGV, SIGBUS, DEVICE_LOST)
              ≤ référence ;
  images      canon : exactement la référence (600) ; runs minutés : ≥ (1 − tol-images) × référence (50 % par
              défaut : arrêt prématuré) ;
  images/s    ≥ (1 − tol-fps) × référence (5 % par défaut ; plus rapide : PASS). Missions : plateau EN JEU, du premier
              événement du scénario (t ≈ 40 s) à la victoire (t ≈ 67 s), en tours de pompe par seconde (un tour = une
              frame présentée) ; bar : tous les tours sur toute la durée du scénario ([BILAN] [INPUT]) ; canon : sans
              objet (600 frames dominées par le démarrage). SEUL critère dit « de performance » (exempté par maj-ref) ;
  empreintes  canon seulement : frames 1, 10, 100, 200, 300 égales à la référence (8 premiers chiffres), variante
              tolérée de la frame 10 : 470ebae2 ≡ b5f7db71.
Global : PASS si tous les contrôles le sont ET si les images/s des missions AGRÉGÉES (somme des images des plateaux /
somme de leurs durées) restent au-dessus de la référence moins la tolérance (NONREG_TOL_FPS, défaut 10 %). 22/10 [SUP] :
les images/s PAR MISSION sont indicatives (marquées, jamais décisives) — le banc est partagé avec les agents, et un
même binaire y varie de −9 % à +38 % d'une mission à l'autre ; la mesure fine des images/s est celle de banc-ab.sh
(paires entrelacées, machine calme).
Bibliothèque standard seulement ; campagne-rapport.py est chargé par chemin (lire_mission).
"""
import argparse
import hashlib
import importlib.util
import json
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ICI = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("campagne_rapport", ICI / "campagne-rapport.py")
campagne_rapport = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(campagne_rapport)

VERSION = 1
PASS, REG = "PASS", "RÉGRESSION"
TOL_FPS = 0.10
TOL_IMAGES = 0.50
FENETRE_S = 5.0            # largeur minimale d'une fenêtre de cadence (s)
PLATEAU_MIN_S = 5.0        # plateau plus court : images/s non mesuré
FRAMES_CANON = (1, 10, 100, 200, 300)
CANON = {1: "be01318c", 10: "470ebae2", 100: "ae1ff947", 200: "ae1ff947", 300: "ae1ff947"}
VARIANTES = [{"470ebae2", "b5f7db71"}]     # empreintes équivalentes (frame 10 : variante tolérée du canon)
CAUSES_REPLI = ("sans_donnees", "cube", "format", "sans_texture")
RE_PACK_OUVERT = re.compile(r"\[SPV\] pack SPIR-V externe ouvert : (.+?) — (\d+) entrées")
RE_PACK_ABSENT = re.compile(r"\[SPV\] pack SPIR-V externe absent : (.*)")
RE_PACK_SHA = re.compile(r"\[SPV\] pack sha=([0-9a-f]{8,64})")
RE_TABLE_SPV = re.compile(r"table SPIR-V embarquée : (\d+) shaders")
RE_PLANTAGE = re.compile(r"SIGSEGV|SIGBUS|\[ARRÊT\] faute|DEVICE_LOST")
RE_PIPELINES = re.compile(r"\[BILAN\] \[PIPECACHE\] .*? (\d+) pipelines créés en ([0-9.]+) ms de vkCreateGraphicsPipelines "
                          r"\(max ([0-9.]+) ms")
FROID_MS = 2000.0          # création de pipelines au-delà de la référence + FROID_MS : cache de shaders Metal froid


# ---------- mesures ----------

def sha256(chemin, memo={}):
    """sha256 hexadécimal d'un fichier (mémoïsé par chemin, taille et date) ; None s'il est illisible."""
    p = Path(chemin)
    try:
        st = p.stat()
    except OSError:
        return None
    cle = (str(p.resolve()), st.st_size, st.st_mtime_ns)
    if cle not in memo:
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for bloc in iter(lambda: f.read(1 << 22), b""):
                h.update(bloc)
        memo[cle] = h.hexdigest()
    return memo[cle]


def cadence(echantillons, debut=None, fin=None):
    """Images/s d'un plateau à partir des échantillons (t, tour) : tours / secondes entre le premier échantillon à
    t ≥ debut et le dernier à t ≤ fin, plus la médiane [min-max] des fenêtres d'au moins FENETRE_S s. None si moins de
    deux échantillons ou moins de PLATEAU_MIN_S s."""
    e = [(t, n) for t, n in echantillons if (debut is None or t >= debut) and (fin is None or t <= fin)]
    if len(e) < 2 or e[-1][0] - e[0][0] < PLATEAU_MIN_S:
        return None
    (t0, n0), (t1, n1) = e[0], e[-1]
    fen, i = [], 0
    while i < len(e) - 1:
        j = i + 1
        while j < len(e) and e[j][0] - e[i][0] < FENETRE_S:
            j += 1
        if j == len(e):
            break
        fen.append((e[j][1] - e[i][1]) / (e[j][0] - e[i][0]))
        i = j
    return {"debut": t0, "fin": t1, "images": n1 - n0, "s": round(t1 - t0, 3),
            "images_s": round((n1 - n0) / (t1 - t0), 2), "fenetres": len(fen),
            "mediane": round(statistics.median(fen), 2) if fen else None,
            "min": round(min(fen), 2) if fen else None, "max": round(max(fen), 2) if fen else None}


def lire_empreintes(chemin):
    """{frame: empreinte} d'un empreintes.txt (ppm-sequence.py) ; lignes illisibles ou tronquées ignorées."""
    r = {}
    try:
        lignes = Path(chemin).read_text(errors="replace").splitlines()
    except OSError:
        return r
    for l in lignes:
        t = l.split()
        if len(t) >= 2 and t[0].isdigit() and re.fullmatch(r"[0-9a-f]{8,64}", t[-1]):
            r[int(t[0])] = t[-1]
    return r


def equivalentes(a, b):
    a, b = (a or "")[:8], (b or "")[:8]
    return bool(a) and (a == b or any(a in v and b in v for v in VARIANTES))


def mesurer(nom, genre, dossier):
    """Mesures d'un contrôle (dict JSON) depuis son dossier de banc."""
    d = Path(dossier)
    m = campagne_rapport.lire_mission(d)
    texte = (d / "run.log").read_text(errors="replace") if (d / "run.log").exists() else ""
    arrets = list(m["arrets"])
    for l in texte.splitlines():
        if RE_PLANTAGE.search(l) and not l.startswith("[ARRÊT] [thread"):
            arrets.append(l.strip()[:160])
    r = {"nom": nom, "type": genre, "dossier": str(d), "present": d.is_dir() and (d / "run.log").exists(),
         "rc": m["rc"], "duree": m["duree"], "images": m["frames"], "reels": m["reels"], "lumiere": m["lumiere"],
         "manque": len(m["manque"]), "manque_occ": sum(q[1] for q in m["manque"]),
         "manque_adresses": sorted({q[0] for q in m["manque"]}), "replis": m["replis"],
         "replis_textures": len(m["replis_detail"]), "arrets": len(arrets), "arrets_raisons": arrets[:5],
         "victoire": None, "victoire_t": None, "images_s": None, "plateau": None, "empreintes": None, "cartes": None,
         "table_spv": None, "pipelines": None, "pipelines_ms": None, "pipelines_max_ms": None}
    pl = RE_PIPELINES.search(texte)
    if pl:
        r["pipelines"], r["pipelines_ms"], r["pipelines_max_ms"] = int(pl.group(1)), float(pl.group(2)), float(pl.group(3))
    ts = RE_TABLE_SPV.search(texte)
    if ts:
        r["table_spv"] = int(ts.group(1))
    if genre == "mission":
        r["victoire"] = m["victoire"] is not None
        r["victoire_t"] = m["victoire"]
        if m["victoire"] is not None and m["echantillons"]:
            p = cadence(m["echantillons"], m["echantillons"][0][0], m["victoire"])
            r["plateau"] = p
            r["images_s"] = p["images_s"] if p else None
    elif genre == "bar":
        r["cartes"] = {c: texte.count(f"{c}.SC2Map") for c in ("TStory01", "TRaynor02")}
        r["victoire"] = all(r["cartes"].values())
        if m["banc_tours"] and m["banc_s"]:
            r["plateau"] = {"debut": 0.0, "fin": m["banc_s"], "images": m["banc_tours"], "s": m["banc_s"],
                            "images_s": round(m["banc_tours"] / m["banc_s"], 2)}
            r["images_s"] = r["plateau"]["images_s"]
    elif genre == "canon":
        e = lire_empreintes(d / "empreintes.txt")
        r["empreintes"] = {str(f): e.get(f) for f in FRAMES_CANON}
    return r


def pack_du_journal(dossiers):
    """Pack SPIR-V vu par le runtime (premier run.log qui le dit) : chemin, entrées, sha256 (journal ou calculé)."""
    for d in dossiers:
        f = Path(d) / "run.log"
        if not f.exists():
            continue
        t = f.read_text(errors="replace")
        o, a, s = RE_PACK_OUVERT.search(t), RE_PACK_ABSENT.search(t), RE_PACK_SHA.search(t)
        if o:
            return {"etat": "ouvert", "chemin": o.group(1), "entrees": int(o.group(2)),
                    "sha256": s.group(1) if s else sha256(o.group(1))}
        if a:
            return {"etat": "absent", "raison": a.group(1).strip(), "sha256": None}
    return {"etat": "inconnu", "sha256": None}


def git_du_binaire(binaire):
    d = Path(binaire).resolve().parent
    try:
        h = subprocess.run(["git", "-C", str(d), "rev-parse", "--short=12", "HEAD"], capture_output=True, text=True,
                           timeout=20).stdout.strip()
        if not h:
            return None
        sale = subprocess.run(["git", "-C", str(d), "status", "--porcelain", "-uno"], capture_output=True, text=True,
                              timeout=20).stdout.strip()
        return h + ("-modifié" if sale else "")
    except (OSError, subprocess.SubprocessError):
        return None


# ---------- jugement ----------

def _crit(critere, valeur, ref, ok, note="", perf=False, ecart=None, indicatif=False):
    c = {"critere": critere, "valeur": valeur, "ref": ref, "verdict": PASS if ok else REG, "note": note}
    if perf:
        c["perf"] = True
    if indicatif:
        c["indicatif"] = True
    if ecart is not None:
        c["ecart_pct"] = round(ecart, 2)
    return c


def juger(m, ref, tol_fps=TOL_FPS, tol_images=TOL_IMAGES):
    """Critères d'un contrôle mesuré m contre la ligne ref (None : invariants)."""
    cr = []
    if not m["present"]:
        return [_crit("présent", False, True, False, "dossier ou run.log absent")]
    cr.append(_crit("rc", m["rc"], 0, m["rc"] == 0, "" if m["rc"] is not None else "resume.txt absent"))
    if m["type"] in ("mission", "bar"):
        attendu = ref["victoire"] if ref and ref.get("victoire") is not None else True
        note = ""
        if m["type"] == "bar":
            note = ", ".join(f"{c} {n} ligne(s)" for c, n in (m["cartes"] or {}).items())
        cr.append(_crit("victoire", m["victoire"], attendu, bool(m["victoire"]) or not attendu, note))
    rm = ref["manque"] if ref else 0
    cr.append(_crit("MANQUE", m["manque"], rm, m["manque"] <= rm,
                    ", ".join(m["manque_adresses"][:4]) if m["manque"] else ""))
    rl = ref["lumiere"] if ref and ref.get("lumiere") is not None else 0
    cr.append(_crit("lumière", m["lumiere"], rl, m["lumiere"] is not None and m["lumiere"] <= rl,
                    "bilan absent" if m["lumiere"] is None else
                    (f"table SPIR-V embarquée : {m['table_spv']} shaders" if m["lumiere"] > rl else "")))
    rp = m["replis"]
    rr = (ref or {}).get("replis")
    if rp is None:
        cr.append(_crit("replis", None, 0, False, "bilan des samplers absent"))
    else:
        autres = [c for c in CAUSES_REPLI if c != "sans_donnees" and (rp.get(c) or 0) > 0]
        # « sans données » : 0 exigé, toujours. Les autres causes se jugent CONTRE la référence (nouvelle cause :
        # RÉGRESSION) ; sans référence, elles sont l'état constaté (22/10 : TZeratul04 de main, 4926 « sans texture
        # liée »), notées sans être jugées, sinon la première pose de référence serait refusée.
        nouv = [c for c in autres if not (rr.get(c) or 0)] if rr is not None else []
        ok = (rp.get("sans_donnees") or 0) == 0 and not nouv
        note = []
        if rp.get("sans_donnees"):
            note.append(f"« sans données » {rp['sans_donnees']} (0 exigé)")
        if nouv:
            note.append("nouveau(x) : " + ", ".join(f"{c} {rp[c]}" for c in nouv))
        elif autres and rr is None:
            note.append("sans réf. (non jugé) : " + ", ".join(f"{c} {rp[c]}" for c in autres))
        cr.append(_crit("replis", rp.get("sans_donnees"), 0, ok, " ; ".join(note)))
    ra = ref["arrets"] if ref else 0
    cr.append(_crit("arrêts", m["arrets"], ra, m["arrets"] <= ra, "; ".join(m["arrets_raisons"][:2])))
    ri = ref["images"] if ref else (600 if m["type"] == "canon" else None)
    if m["type"] == "canon":
        cr.append(_crit("images", m["images"], ri, m["images"] == ri))
    elif ri:
        seuil = int(ri * (1 - tol_images))
        cr.append(_crit("images", m["images"], ri, (m["images"] or 0) >= seuil, f"seuil {seuil}"))
    else:
        cr.append(_crit("images", m["images"], None, (m["images"] or 0) > 0, "sans réf."))
    if m["type"] != "canon":
        rf = ref.get("images_s") if ref else None
        v = m["images_s"]
        if rf is None:
            cr.append(_crit("images/s", v, None, True, "sans réf.", perf=True))
        elif v is None:
            cr.append(_crit("images/s", None, rf, False, "non mesuré (pas de plateau)", perf=True))
        else:
            seuil = rf * (1 - tol_fps)
            note = f"seuil {seuil:.1f} (−{tol_fps * 100:g} %)"
            pm, rpm = m.get("pipelines_ms"), ref.get("pipelines_ms")
            if pm is not None and rpm is not None and pm > rpm + FROID_MS:
                note += (f" ; création de pipelines {pm:.0f} ms (réf. {rpm:.0f} ms, max {m['pipelines_max_ms']:.0f} ms par "
                         f"appel) : cache de shaders Metal froid probable, relancer avant de conclure")
            cr.append(_crit("images/s", v, rf, v >= seuil, note + " ; indicatif (le global juge l'agrégé)", perf=True,
                            ecart=(v / rf - 1) * 100, indicatif=True))
    if m["type"] == "canon":
        re_ = (ref or {}).get("empreintes") or {str(f): CANON[f] for f in FRAMES_CANON}
        diff = [f for f in map(str, FRAMES_CANON) if not equivalentes(m["empreintes"].get(f), re_.get(f))]
        cr.append(_crit("empreintes", {f: (m["empreintes"].get(f) or "—")[:8] for f in map(str, FRAMES_CANON)},
                        {f: (re_.get(f) or "—")[:8] for f in map(str, FRAMES_CANON)}, not diff,
                        ("frame(s) " + ", ".join(diff)) if diff else ""))
    return cr


def agreger(controles):
    """Images/s agrégé des plateaux des missions (images totales / secondes totales)."""
    p = [c["plateau"] for c in controles if c["type"] == "mission" and c.get("plateau")]
    if not p:
        return None
    im, s = sum(x["images"] for x in p), sum(x["s"] for x in p)
    return {"missions": len(p), "images": im, "s": round(s, 3), "images_s": round(im / s, 2) if s else None}


# ---------- rendu ----------

def _f(x, dec=1):
    if x is None:
        return "—"
    if isinstance(x, float):
        return f"{x:.{dec}f}"
    return str(x)


def _cellule(c, texte):
    return texte if c is None or c["verdict"] == PASS else f"**{texte}**"


def rendre(rapport):
    """Texte Markdown du rapport (une ligne par contrôle, puis le global)."""
    l = []
    b, p, g = rapport["binaire"], rapport["pack"], rapport["global"]
    l.append(f"# nonreg {rapport['mode']}{' (partiel)' if rapport['seul'] else ''} — {g['verdict']} "
             f"({g['pass']}/{g['controles']} contrôle(s) PASS) en {_f(rapport['duree_s'])} s")
    l.append("")
    l.append(f"- binaire : {b['chemin']} (sha256 {(b['sha256'] or '—')[:16]}, git {b.get('git') or '—'})")
    if p["etat"] == "ouvert":
        l.append(f"- pack SPIR-V : {p['chemin']} ({p['entrees']} entrées, sha256 {(p['sha256'] or '—')[:16]})")
    else:
        l.append(f"- pack SPIR-V : {p['etat']}{' — ' + p['raison'] if p.get('raison') else ''}")
    l.append(f"- environnement ajouté : {' '.join(rapport['env']) or 'aucun'}")
    r = rapport["ref"]
    if r:
        l.append(f"- référence : {r['chemin']} (posée le {r.get('date') or '—'}, binaire sha256 "
                 f"{(r.get('binaire_sha256') or '—')[:16]}, pack sha256 {(r.get('pack_sha256') or '—')[:16]})")
    else:
        l.append("- référence : aucune — attendus = invariants ; images/s non jugé")
    l.append(f"- tolérances : images/s −{rapport['tolerances']['fps'] * 100:g} %, images des runs minutés "
             f"−{rapport['tolerances']['images'] * 100:g} %")
    l.append("")
    l.append("| contrôle | verdict | rc | victoire | MANQUE | lumière | replis (sans données) | arrêts | images "
             "| images/s (réf. ; écart) | empreintes |")
    l.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for c in rapport["controles"]:
        k = {x["critere"]: x for x in c["criteres"]}
        if "présent" in k:
            l.append(f"| {c['nom']} | **{c['verdict']}** | absent | | | | | | | | |")
            continue
        vic = "—"
        if c["type"] == "mission":
            vic = f"t={c['victoire_t']:.1f}s" if c["victoire"] else "non"
        elif c["type"] == "bar":
            vic = "TRaynor02 lue" if c["victoire"] else "non"
        rp = c["replis"]
        tot = sum((rp.get(x) or 0) for x in CAUSES_REPLI) if rp else None
        rep = f"{_f(tot)} ({_f(rp.get('sans_donnees'))})" if rp else "—"
        fps = "—"
        if "images/s" in k:
            x = k["images/s"]
            fps = _f(x["valeur"])
            if x["ref"] is not None:
                fps += f" ({_f(x['ref'])} ; {x['ecart_pct']:+.1f} %)" if "ecart_pct" in x else f" ({_f(x['ref'])})"
        emp = "—"
        if "empreintes" in k:
            e = k["empreintes"]["valeur"]
            emp = f"1 {e['1']} · 10 {e['10']} · 100-300 {e['100']}/{e['200']}/{e['300']}"
        l.append(f"| {c['nom']} | {c['verdict'] if c['verdict'] == PASS else '**' + c['verdict'] + '**'} "
                 f"| {_cellule(k.get('rc'), _f(c['rc']))} | {_cellule(k.get('victoire'), vic)} "
                 f"| {_cellule(k.get('MANQUE'), _f(c['manque']))} | {_cellule(k.get('lumière'), _f(c['lumiere']))} "
                 f"| {_cellule(k.get('replis'), rep)} | {_cellule(k.get('arrêts'), _f(c['arrets']))} "
                 f"| {_cellule(k.get('images'), _f(c['images']))} | {_cellule(k.get('images/s'), fps)} "
                 f"| {_cellule(k.get('empreintes'), emp)} |")
    l.append("")
    pl = [f"{c['nom']} {c['pipelines_ms']:.0f}" for c in rapport["controles"] if c.get("pipelines_ms") is not None]
    if pl:
        l.append("création de pipelines (ms de vkCreateGraphicsPipelines ; des centaines de ms par appel = cache de shaders "
                 "Metal froid, qui ralentit images/s) : " + ", ".join(pl))
    a, ra = g.get("images_s_missions"), g.get("ref_images_s_missions")
    if a:
        t = f"images/s des missions (plateaux en jeu, agrégé sur {a['missions']}) : {_f(a['images_s'])}"
        if ra and ra.get("images_s"):
            t += f" (réf. {_f(ra['images_s'])} ; {(a['images_s'] / ra['images_s'] - 1) * 100:+.1f} %)"
        l.append(t)
    regs = [(c["nom"], x) for c in rapport["controles"] for x in c["criteres"] if x["verdict"] != PASS and not x.get("indicatif")]
    if (g.get("fps_global") or {}).get("verdict") == REG:
        regs.append(("missions", g["fps_global"]))
    ind = [(c["nom"], x) for c in rapport["controles"] for x in c["criteres"] if x["verdict"] != PASS and x.get("indicatif")]
    if ind:
        l.append("")
        l.append("Hors tolérance, à titre indicatif (images/s par mission sur un banc partagé) : "
                 + ", ".join(f"{n} {x.get('ecart_pct', 0):+.1f} %" for n, x in ind))
    if regs:
        l.append("")
        l.append("Régressions :")
        for nom, x in regs:
            l.append(f"- {nom} : {x['critere']} = {_f(x['valeur'])} (réf. {_f(x['ref'])})"
                     f"{' — ' + x['note'] if x['note'] else ''}")
    l.append("")
    l.append(f"nonreg {rapport['mode']} : {g['verdict']} en {_f(rapport['duree_s'])} s")
    return "\n".join(l) + "\n"


# ---------- commandes ----------

def dossier_controle(sortie, nom):
    if nom in ("canon", "bar"):
        return Path(sortie) / nom, nom
    return Path(sortie) / "missions" / nom, "mission"


def charger_ref(ref_dossier, mode):
    if not ref_dossier:
        return None, None
    f = Path(ref_dossier) / f"{mode}.json"
    if not f.exists():
        return None, None
    return json.loads(f.read_text()), f


def analyser(a):
    sortie = Path(a.sortie)
    noms = [x for x in a.controles.split(",") if x]
    ref, ref_chemin = charger_ref(a.ref, a.mode)
    ref_par_nom = {c["nom"]: c for c in ref["controles"]} if ref else {}
    controles = []
    for nom in noms:
        d, genre = dossier_controle(sortie, nom)
        m = mesurer(nom, genre, d)
        rc_ = ref_par_nom.get(nom) if ref else None
        m["criteres"] = juger(m, rc_, a.tol_fps, a.tol_images)
        if ref and rc_ is None:
            m["criteres"].append(_crit("référence", None, None, True, "contrôle absent de la référence"))
        m["verdict"] = PASS if all(x["verdict"] == PASS for x in m["criteres"] if not x.get("indicatif")) else REG
        controles.append(m)
    pack = pack_du_journal([dossier_controle(sortie, n)[0] for n in noms])
    g = {"controles": len(controles), "pass": sum(c["verdict"] == PASS for c in controles),
         "images_s_missions": agreger(controles),
         "ref_images_s_missions": (agreger([c for c in ref["controles"] if c["nom"] in noms]) if ref else None)}
    ag, rag = g["images_s_missions"], g["ref_images_s_missions"]
    g["fps_global"] = None
    if ag and rag and ag.get("images_s") and rag.get("images_s"):
        seuil = rag["images_s"] * (1 - a.tol_fps)
        g["fps_global"] = _crit("images/s agrégé", ag["images_s"], rag["images_s"], ag["images_s"] >= seuil,
                                f"seuil {seuil:.1f} (−{a.tol_fps * 100:g} %)", perf=True,
                                ecart=(ag["images_s"] / rag["images_s"] - 1) * 100)
    fps_ok = g["fps_global"] is None or g["fps_global"]["verdict"] == PASS
    g["verdict"] = PASS if g["pass"] == g["controles"] and controles and fps_ok else REG
    rapport = {"version": VERSION, "outil": "scripts/nonreg.sh", "mode": a.mode, "seul": bool(a.seul),
               "date": time.strftime("%Y-%m-%d %H:%M:%S"), "duree_s": a.duree, "sortie": str(sortie),
               "binaire": {"chemin": a.binaire, "sha256": sha256(a.binaire), "git": git_du_binaire(a.binaire)},
               "pack": pack, "env": a.env or [], "tolerances": {"fps": a.tol_fps, "images": a.tol_images},
               "ref": ({"chemin": str(ref_chemin), "date": ref.get("date"),
                        "binaire_sha256": ref["binaire"].get("sha256"), "pack_sha256": ref["pack"].get("sha256"),
                        "env": ref.get("env")} if ref else None),
               "controles": controles, "global": g}
    (sortie / "nonreg.json").write_text(json.dumps(rapport, ensure_ascii=False, indent=1) + "\n")
    md = rendre(rapport)
    (sortie / "nonreg.md").write_text(md)
    print(md, end="")
    return 0 if g["verdict"] == PASS else 1


def maj_ref(a):
    sortie = Path(a.sortie)
    rapport = json.loads((sortie / "nonreg.json").read_text())
    refus = []
    if rapport["seul"]:
        refus.append("run partiel (--seul) : une référence couvre tous les contrôles du mode")
    for c in rapport["controles"]:
        for x in c["criteres"]:
            if x["verdict"] != PASS and not x.get("perf"):
                refus.append(f"{c['nom']} : {x['critere']} = {_f(x['valeur'])} (réf. {_f(x['ref'])})"
                             f"{' — ' + x['note'] if x['note'] else ''}")
    if refus and not a.forcer:
        print("maj-ref : REFUSÉE — le run n'est pas entièrement PASS hors images/s (--forcer pour passer outre) :",
              file=sys.stderr)
        for r in refus:
            print(f"  - {r}", file=sys.stderr)
        return 4
    ref = Path(a.ref)
    mode = rapport["mode"]
    ref.mkdir(parents=True, exist_ok=True)
    d = ref / mode
    if d.exists():
        shutil.rmtree(d)
    d.mkdir()
    for c in rapport["controles"]:
        src = Path(c["dossier"])
        cible = d / c["nom"]
        for nom in ("resume.txt", "empreintes.txt"):          # rapports et empreintes SEULEMENT : jamais d'image
            if (src / nom).is_file():
                cible.mkdir(exist_ok=True)
                shutil.copyfile(src / nom, cible / nom)
    if (sortie / "missions" / "rapport.md").is_file():
        shutil.copyfile(sortie / "missions" / "rapport.md", d / "rapport-campagne.md")
    shutil.copyfile(sortie / "nonreg.md", d / "nonreg.md")
    rapport["ref_posee"] = {"date": time.strftime("%Y-%m-%d %H:%M:%S"), "forcee": bool(refus),
                            "depuis": str(sortie), "refus_ignores": refus}
    (ref / f"{mode}.json").write_text(json.dumps(rapport, ensure_ascii=False, indent=1) + "\n")
    print(f"maj-ref : référence {mode} posée dans {ref} ({len(rapport['controles'])} contrôle(s)"
          f"{', FORCÉE malgré ' + str(len(refus)) + ' refus' if refus else ''})")
    froids = [f"{c['nom']} {c['pipelines_ms']:.0f} ms" for c in rapport["controles"]
              if c.get("type") != "canon" and (c.get("pipelines_ms") or 0) > FROID_MS]
    if froids:      # 22/10 : TZeratul04 10 s de pipelines au 1er run, cache de shaders Metal froid → images/s sous-estimé
        print(f"maj-ref : AVERTISSEMENT — création de pipelines > {FROID_MS:.0f} ms ({', '.join(froids)}) : cache de "
              "shaders Metal froid probable, images/s de la référence sous-estimé ; mieux : relancer le run puis "
              "--maj-ref", file=sys.stderr)
    return 0


def ab(a):
    sortie = Path(a.sortie)
    runs = []
    for l in (sortie / "ab.txt").read_text().splitlines():
        t = l.split()
        if len(t) < 5 or t[0].startswith("#"):
            continue
        run, etiq, paire, binaire, sha = t[0], t[1], int(t[2]), t[3], t[4]
        m = campagne_rapport.lire_mission(sortie / run)
        fin = a.fin if a.fin is not None else (m["echantillons"][-1][0] if m["echantillons"] else None)
        p = cadence(m["echantillons"], a.debut, fin)
        texte = (sortie / run / "run.log").read_text(errors="replace") if (sortie / run / "run.log").exists() else ""
        pl = RE_PIPELINES.search(texte)
        runs.append({"run": run, "etiquette": etiq, "paire": paire, "binaire": binaire, "sha256": sha,
                     "rc": m["rc"], "images": m["frames"], "arrets": len(m["arrets"]), "lumiere": m["lumiere"],
                     "pipelines_ms": float(pl.group(2)) if pl else None,
                     "pipelines_max_ms": float(pl.group(3)) if pl else None,
                     "plateau": p, "images_s": p["images_s"] if p else None})
    etiq = []
    for r in runs:
        if r["etiquette"] not in etiq:
            etiq.append(r["etiquette"])
    if len(etiq) != 2:
        print(f"banc-ab : deux étiquettes attendues, trouvé {etiq}", file=sys.stderr)
        return 2
    ea, eb = etiq
    par = {(r["etiquette"], r["paire"]): r for r in runs}
    paires, triplets = [], []
    for i in sorted({r["paire"] for r in runs if r["paire"] > 0}):        # paire 0 : chauffe, hors statistiques
        ra_, rb_ = par.get((ea, i)), par.get((eb, i))
        if ra_ and rb_ and ra_["images_s"] and rb_["images_s"]:
            paires.append({"paire": i, ea: ra_["images_s"], eb: rb_["images_s"],
                           "ecart_pct": round((rb_["images_s"] / ra_["images_s"] - 1) * 100, 2)})
            rn = par.get((ea, i + 1))                                     # A_i B_i A_i+1
            if rn and rn["images_s"]:
                moy = (ra_["images_s"] + rn["images_s"]) / 2
                triplets.append({"paire": i, "ecart_pct": round((rb_["images_s"] / moy - 1) * 100, 2)})
    va = [r["images_s"] for r in runs if r["etiquette"] == ea and r["images_s"] and r["paire"] > 0]
    vb = [r["images_s"] for r in runs if r["etiquette"] == eb and r["images_s"] and r["paire"] > 0]
    res = {"version": VERSION, "outil": "scripts/banc-ab.sh", "sortie": str(sortie), "debut": a.debut, "fin": a.fin,
           "etiquettes": [ea, eb], "runs": runs, "paires": paires, "triplets": triplets,
           "mediane": {ea: statistics.median(va) if va else None, eb: statistics.median(vb) if vb else None},
           "ecart_median_paires_pct": statistics.median(x["ecart_pct"] for x in paires) if paires else None,
           "ecart_median_triplets_pct": statistics.median(x["ecart_pct"] for x in triplets) if triplets else None}
    if va and vb:
        res["ecart_medianes_pct"] = round((res["mediane"][eb] / res["mediane"][ea] - 1) * 100, 2)
    (sortie / "banc-ab.json").write_text(json.dumps(res, ensure_ascii=False, indent=1) + "\n")
    l = [f"# banc-ab {ea} / {eb} — TRaynor01 scriptée, plateau t ≥ {a.debut:g} s"
         f"{' et ≤ ' + format(a.fin, 'g') + ' s' if a.fin is not None else ' jusqu au QUIT'}, sans profileur", "",
         "| run | binaire (sha256) | rc | arrêts | lumière | pipelines (ms ; max/appel) | plateau (s) "
         "| images/s (frames/durée) | fenêtres de 5 s : médiane [min-max] |", "|---|---|---|---|---|---|---|---|---|"]
    for r in runs:
        p = r["plateau"] or {}
        l.append(f"| {r['run']}{' (chauffe)' if r['paire'] == 0 else ''} | {Path(r['binaire']).name} ({r['sha256'][:12]}) | {_f(r['rc'])} | {r['arrets']} "
                 f"| {_f(r['lumiere'])} | {_f(r['pipelines_ms'], 0)} ; {_f(r['pipelines_max_ms'], 0)} "
                 f"| {_f(p.get('s'))} | {_f(r['images_s'])} "
                 f"| {_f(p.get('mediane'))} [{_f(p.get('min'))}-{_f(p.get('max'))}] |")
    l += ["", f"| paire | {ea} | {eb} | écart {eb}/{ea} |", "|---|---|---|---|"]
    for x in paires:
        l.append(f"| {x['paire']} | {_f(x[ea])} | {_f(x[eb])} | {x['ecart_pct']:+.2f} % |")
    l.append("")
    l.append(f"médianes : {ea} {_f(res['mediane'][ea])}, {eb} {_f(res['mediane'][eb])} images/s"
             + (f" ; écart des médianes {res['ecart_medianes_pct']:+.2f} %" if "ecart_medianes_pct" in res else ""))
    if triplets:
        l.append(f"triplets {ea}_i {eb}_i {ea}_i+1 (dérive compensée) : écart médian "
                 f"{res['ecart_median_triplets_pct']:+.2f} % sur {len(triplets)}")
    em = res["ecart_median_paires_pct"]
    l.append(f"banc-ab : écart {eb}/{ea} = {'—' if em is None else format(em, '+.2f') + ' %'} "
             f"(médiane de {len(paires)} paire(s) entrelacée(s))")
    md = "\n".join(l) + "\n"
    (sortie / "banc-ab.md").write_text(md)
    print(md, end="")
    return 0 if paires else 1


def fraction(v):
    """Type argparse de --tol-fps / --tol-images : fraction dans [0, 1) (0.05 = 5 %). Un « 5 » (pour 5 %) donnerait
    seuil = ref × (1 − 5) < 0 : critère désactivé sans le dire ; refusé, comme NaN et l'infini."""
    try:
        x = float(v)
    except ValueError:
        x = None
    if x is None or not 0 <= x < 1:
        raise argparse.ArgumentTypeError(f"« {v} » : fraction dans [0, 1) attendue (0.05 = 5 %)")
    return x


def main(argv=None):
    ap = argparse.ArgumentParser(description="dépouillement de nonreg.sh et banc-ab.sh (V1.4)")
    sp = ap.add_subparsers(dest="cmd", required=True)
    x = sp.add_parser("analyser")
    x.add_argument("sortie")
    x.add_argument("--mode", choices=("fumee", "complet"), required=True)
    x.add_argument("--controles", required=True)
    x.add_argument("--binaire", required=True)
    x.add_argument("--env", action="append", default=[])
    x.add_argument("--duree", type=float, default=None)
    x.add_argument("--ref", default=None)
    x.add_argument("--seul", action="store_true")
    x.add_argument("--tol-fps", type=fraction, default=TOL_FPS)
    x.add_argument("--tol-images", type=fraction, default=TOL_IMAGES)
    y = sp.add_parser("maj-ref")
    y.add_argument("sortie")
    y.add_argument("--ref", required=True)
    y.add_argument("--forcer", action="store_true")
    z = sp.add_parser("ab")
    z.add_argument("sortie")
    z.add_argument("--debut", type=float, default=65.0)
    z.add_argument("--fin", type=float, default=None)
    a = ap.parse_args(argv)
    return {"analyser": analyser, "maj-ref": maj_ref, "ab": ab}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
