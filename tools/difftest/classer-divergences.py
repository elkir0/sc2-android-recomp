#!/usr/bin/env python3
"""classer-divergences.py — familles de divergences d'un ou plusieurs rapports difftest v2 (passe 7k).

    classer-divergences.py rapport.json [...] [--top 30] [--exemples 3] [--famille MOTIF] [--adresses]

Une fonction en échec reçoit UNE famille, tirée de sa première divergence par ordre de
priorité : arrêt du runner > registre > mémoire > drapeau. La clé de famille nomme le champ
et le MNÉMONIQUE du dernier écrivain Unicorn (l'instruction qui a produit la valeur
divergente) : c'est le point de départ de la bissection (DIVERGENCES.md).
  --famille MOTIF : ne garde que les familles contenant MOTIF ; --adresses : liste « ADDR SEED »
  (réutilisable par difftest-sc2.py --adresses pour re-certifier la famille).
"""
import argparse, collections, json, re

def mnemo(dd):
    w = dd.get("ecrivain") or {}
    asm = w.get("asm", "")
    if not asm:
        return "(aucun écrivain Unicorn)"
    m = asm.split()[0]
    if m in ("rep", "repe", "repne", "repz", "repnz", "lock") and len(asm.split()) > 1:
        m += " " + asm.split()[1]
    # précision utile : x87 / sse vers mémoire
    return m

def famille(d):
    if "runner" in d:
        r = d["runner"]
        if r["type"] != "rc":
            return f"runner:{r['type']}"
        a = r.get("arret", "")
        a = re.sub(r"0x[0-9A-Fa-f]{8}", "X", a)
        a = re.sub(r"\(adresse de retour.*", "", a).strip()
        a = re.sub(r"TODO du lifter à X : (\S+).*", r"TODO \1", a)
        a = re.sub(r"\[SIGNAL\] sig (\d+).*", r"signal \1", a)
        return f"runner:rc{r.get('rc')} {a}"
    diffs = d.get("diffs", [])
    def prio(x):
        q = x["quoi"]
        return 0 if q in ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi") else 1 if q == "mem" else 2
    x = sorted(diffs, key=prio)[0]
    q = x["quoi"]
    if q == "mem":
        zone = "pile-morte" if x.get("sous_esp_final") else ("pile" if x.get("rel_esp0") is not None else "scratch")
        return f"mem[{zone}] <- {mnemo(x)}"
    if q in ("cf", "pf", "af", "zf", "sf", "of"):
        ind = x.get("indefini")
        return f"drapeau {q}{' (oracle qemu)' if ind == 'qemu' else ' (indéfini)' if ind else ''} <- {mnemo(x)}"
    if q in ("tf", "if", "df"):
        return f"drapeau {q}"
    return f"reg {'esp' if q == 'esp' else 'gpr'} <- {mnemo(x)}"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rapports", nargs="+")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--exemples", type=int, default=2)
    ap.add_argument("--famille")
    ap.add_argument("--adresses", action="store_true")
    ap.add_argument("--json")
    a = ap.parse_args()
    tot = collections.Counter()
    fam = collections.defaultdict(list)
    masq = collections.Counter()
    for p in a.rapports:
        r = json.load(open(p))
        for k in ("ok", "echec", "skip", "masques"):
            tot[k] += r.get(k, 0)
        for d in r["details"]:
            for m in d.get("masques", []):
                masq[f"{m['quoi']} <- {mnemo(m)}"] += 1
            if d["statut"] != "echec":
                continue
            fam[famille(d)].append(d)
    if a.adresses:
        for f, ds in fam.items():
            if a.famille and a.famille not in f:
                continue
            for d in ds:
                print(f"{d['addr']} {d['seed']}")
        return
    n = sum(len(v) for v in fam.values())
    print(f"ok {tot['ok']}  echec {tot['echec']}  skip {tot['skip']}  (fonctions avec écart masqué : {tot['masques']})")
    print(f"{len(fam)} familles ; échecs classés {n}")
    cum = 0
    for f, ds in sorted(fam.items(), key=lambda kv: -len(kv[1]))[:a.top]:
        if a.famille and a.famille not in f:
            continue
        cum += len(ds)
        print(f"{len(ds):6d} {100*cum/max(n,1):5.1f}%  {f}")
        for d in ds[:a.exemples]:
            x = (d.get("diffs") or [{}])[0]
            print(f"           {d['addr']} s{d['seed']}  {json.dumps(d.get('runner') or x, ensure_ascii=False)[:230]}")
    if masq:
        print("masqués (conventions) :")
        for k, v in masq.most_common(15):
            print(f"  {v:6d}  {k}")
    if a.json:
        json.dump({f: [(d["addr"], d["seed"]) for d in ds] for f, ds in fam.items()}, open(a.json, "w"), indent=0)

if __name__ == "__main__":
    main()
