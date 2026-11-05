#!/usr/bin/env python3
"""croiser-stock-m4.py — situe chaque échec de la campagne M4 (rapports du Mac) dans un rapport v2.

    croiser-stock-m4.py <dossier rapports M4> <rapport v2.json> [--liste-echecs sortie.txt]

Rend la ventilation des échecs M4 : certifiés en v2 / skip en v2 (et raison) / échec v2 (famille).
« --liste-echecs » écrit les échecs M4 au format « ADDR SEED » (lot pour --adresses)."""
import collections, glob, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
spec = importlib.util.spec_from_file_location("cl", os.path.join(os.path.dirname(os.path.abspath(__file__)), "classer-divergences.py"))
cl = importlib.util.module_from_spec(spec); spec.loader.exec_module(cl)

d_m4, p_v2 = sys.argv[1], sys.argv[2]
m4 = {}
for f in glob.glob(os.path.join(d_m4, "rapport-*.json")):
    r = json.load(open(f))
    for d in r["details"]:
        if "diffs" in d:
            m4[d["addr"]] = "M4 mem/reg/drapeaux"
        elif "erreur" in d:
            m4[d["addr"]] = "M4 runner"
v2 = json.load(open(p_v2))
echecs = {d["addr"]: d for d in v2["details"] if d["statut"] == "echec"}
autres = {d["addr"]: (d["statut"] + (" " + d["raison"] if d.get("raison") else "")) for d in v2["details"] if d["statut"] != "echec"}
# les ok et skip v2 ne sont pas dans details : on relit le lot (adresses non listées = ok ou skip)
ok_ou_skip = collections.Counter()
c = collections.Counter()
for a, k in m4.items():
    if a in echecs:
        c[(k, "v2 échec : " + cl.famille(echecs[a]))] += 1
    else:
        c[(k, "v2 " + autres.get(a, "ok ou skip (non détaillé : relancer avec --tout)"))] += 1
print(f"échecs M4 : {len(m4)}")
for (k, v), n in sorted(c.items(), key=lambda kv: -kv[1]):
    print(f"{n:6d}  {k:22s} -> {v}")
if "--liste-echecs" in sys.argv:
    out = sys.argv[sys.argv.index("--liste-echecs") + 1]
    lst = [l.split() for l in open(os.path.join(os.path.dirname(p_v2), "liste-m4.txt"))]
    seed = {a: s for a, s in lst}
    with open(out, "w") as o:
        for a in sorted(m4):
            o.write(f"{a} {seed[a]}\n")
