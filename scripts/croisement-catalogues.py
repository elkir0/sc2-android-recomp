#!/usr/bin/env python3
"""Croisement des catalogues de fonctions de SC2.exe (build 15405) — SC2 Partout, passe 3.

  A = ghidra/exports/fonctions.csv           (Ghidra : nom, entree, taille_octets, appelants)
  B = extraction/pcrecomp/lots/catalogue.jsonl (disasm32 patché, fonctions >= 64 octets)

Écrit :
  research/analyse-init/croisement-stats.json         chiffres + tableaux (relus par CROISEMENT.md)
  research/analyse-init/seeds-ghidra-complement.json  adresses vues seulement par A, au format
                                                      --seed-functions de disasm32 ("0x...")
Léger (quelques centaines de Mo au plus), pas de capstone.
"""
import bisect, csv, json, os, sys
from collections import Counter

DEPOT = os.path.expanduser("~/code/reverse/sc2-partout")
ICI = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ICI, "..", "research", "analyse-init")
MIN_B = 64


def charger():
    A = {}
    with open(f"{DEPOT}/ghidra/exports/fonctions.csv") as f:
        for r in csv.DictReader(f):
            A[int(r["entree"], 16)] = {"nom": r["nom"], "taille": int(r["taille_octets"]),
                                        "appelants": int(r["appelants"])}
    B = {}
    with open(f"{DEPOT}/extraction/pcrecomp/lots/catalogue.jsonl") as f:
        for l in f:
            d = json.loads(l)
            B[d["address"]] = d
    return A, B


class Etendues:
    """Qui contient une adresse : [entrée, entrée+taille) d'un catalogue."""
    def __init__(self, items):
        self.v = sorted(items)                 # (debut, fin)
        self.k = [d for d, _ in self.v]
        # fin max cumulée pour retrouver un englobant même chevauché
    def englobant(self, a):
        i = bisect.bisect_right(self.k, a) - 1
        # remonter un peu : extents chevauchants (alias) possibles
        for j in range(i, max(-1, i - 64), -1):
            d, f = self.v[j]
            if d < a < f:
                return d
        return None


def hypothese_A_seul(a, x, ext_B):
    h = []
    if x["taille"] < MIN_B:
        h.append("< 64 o : filtré de B (min-size), pas un désaccord")
    hote = ext_B.englobant(a)
    if hote is not None:
        h.append(f"intérieur de B 0x{hote:08X} : disasm32 y voit un label/morceau, pas une entrée")
    if x["nom"].startswith("thunk_") or x["taille"] <= 6:
        h.append("thunk/jmp")
    if x["appelants"] == 0:
        h.append("0 appelant direct : atteint par pointeur (vtable, table) ou code mort")
    i = bisect.bisect_right(ext_B.k, a)
    if x["taille"] >= MIN_B and i < len(ext_B.k) and ext_B.k[i] - a < MIN_B:
        h.append(f"entrée B suivante 0x{ext_B.k[i]:08X} à {ext_B.k[i] - a} o : rognée < 64 o "
                 "par clamp_extents puis filtrée (min-size)")
    if x["nom"].startswith("Catch") or "Unwind" in x["nom"]:
        h.append("funclet EH (catch/unwind)")
    return "; ".join(h) or "non expliqué (à revoir)"


def hypothese_B_seul(a, d, ext_A):
    h = []
    if d["entry_kind"] == "alias":
        h.append("alias disasm32 (label/stub EH/cible de branche rendue dispatchable)")
    hote = ext_A.englobant(a)
    if hote is not None:
        h.append(f"intérieur de Ghidra 0x{hote:08X} : pour Ghidra c'est du code de cette "
                 "fonction ; disasm32 en fait un start (cible de jmp/prologue/pointeur) — "
                 "moitié de la paire « coupe »")
    else:
        h.append("hors de toute fonction Ghidra : code non désassemblé par Ghidra "
                 "(atteint par pointeur de données ?)")
    if d["num_blocks"] == 1 and d["num_instructions"] <= 2:
        h.append("thunk")
    return "; ".join(h)


def main():
    A, B = charger()
    sa, sb = set(A), set(B)
    inter = sa & sb
    a_seul = sorted(sa - sb)
    b_seul = sorted(sb - sa)
    ext_A = Etendues((a, a + x["taille"]) for a, x in A.items())
    ext_B = Etendues((a, a + d["size"]) for a, d in B.items())

    a_seul_64 = [a for a in a_seul if A[a]["taille"] >= MIN_B]
    a_seul_64_interieur_B = [a for a in a_seul_64 if ext_B.englobant(a) is not None]
    b_seul_alias = [a for a in b_seul if B[a]["entry_kind"] == "alias"]
    b_seul_start = [a for a in b_seul if B[a]["entry_kind"] == "start"]
    b_seul_start_hors_A = [a for a in b_seul_start if ext_A.englobant(a) is None]

    # Écarts : (octets en jeu, catégorie, adresse, détail, hypothèse)
    ecarts = []
    for a in a_seul_64:
        x = A[a]
        ecarts.append((x["taille"], "A seul", a,
                       f"{x['nom']}, {x['taille']} o, {x['appelants']} appelants",
                       hypothese_A_seul(a, x, ext_B)))
    for a in b_seul_start:
        d = B[a]
        ecarts.append((d["size"], "B seul", a,
                       f"{d['name']} ({d['entry_kind']}), {d['size']} o, {d['num_instructions']} instr.",
                       hypothese_B_seul(a, d, ext_A)))
    starts_B = sorted(a for a, d in B.items() if d["entry_kind"] == "start")
    k_A = sorted(A)
    def dans(liste, lo, hi):
        i, j = bisect.bisect_right(liste, lo), bisect.bisect_left(liste, hi)
        return liste[i:j]
    for a in inter:
        ta, tb = A[a]["taille"], B[a]["size"]
        if ta == tb:
            continue
        if ta > tb:
            coupes = dans(starts_B, a, a + ta)
            if coupes:
                h = (f"disasm32 COUPE la fonction Ghidra : {len(coupes)} start(s) B à l'intérieur "
                     f"(1er 0x{coupes[0]:08X}), clamp_extents tronque l'hôte à l'entrée suivante ; "
                     "intérieur pris pour une entrée (cible de jmp, prologue, pointeur)")
            else:
                h = ("Ghidra plus grand sans start B dedans : blocs non contigus (froids/EH) "
                     "comptés par Ghidra, hors étendue linéaire disasm32")
        else:
            avale = dans(k_A, a, a + tb)
            h = (f"étendue disasm32 plus longue : avale {len(avale)} fonction(s) Ghidra "
                 f"(1re 0x{avale[0]:08X}) qu'il ne voit pas comme starts (alias ou < 64 o)"
                 if avale else
                 "étendue disasm32 plus longue, aucune fonction Ghidra dedans : linéaire "
                 "jusqu'à l'entrée suivante — padding/données/table après le ret, "
                 "ou corps que Ghidra a arrêté tôt (appel noreturn ?)")
        ecarts.append((abs(ta - tb), "taille ≠", a,
                       f"{A[a]['nom']} : Ghidra {ta} o / disasm32 {tb} o", h))
    ecarts.sort(key=lambda e: -e[0])

    tailles_diff = sum(1 for a in inter if A[a]["taille"] != B[a]["size"])
    stats = {
        "A": len(A), "B": len(B), "A_inter_B": len(inter),
        "A_seul": len(a_seul), "A_seul_lt64": len(a_seul) - len(a_seul_64),
        "A_seul_ge64": len(a_seul_64), "A_seul_ge64_interieur_B": len(a_seul_64_interieur_B),
        "B_seul": len(b_seul), "B_seul_alias": len(b_seul_alias), "B_seul_start": len(b_seul_start),
        "B_seul_start_hors_A": len(b_seul_start_hors_A),
        "inter_taille_identique": len(inter) - tailles_diff, "inter_taille_differente": tailles_diff,
        "A_noms": dict(Counter(x["nom"].split("_")[0] for x in A.values()).most_common(8)),
        "A_ge64": sum(1 for x in A.values() if x["taille"] >= MIN_B),
        "A_ge64_dans_B": sum(1 for a, x in A.items() if x["taille"] >= MIN_B and a in sb),
        "top20": [{"octets": o, "categorie": c, "adresse": f"0x{a:08X}", "detail": d, "hypothese": h}
                  for o, c, a, d, h in ecarts[:20]],
        "A_seul_ge64_rognee_lt64": sum(1 for a in a_seul_64
                                       if "rognée" in hypothese_A_seul(a, A[a], ext_B)),
        "A_seul_ge64_liste": [{"adresse": f"0x{a:08X}", "nom": A[a]["nom"], "taille": A[a]["taille"],
                               "appelants": A[a]["appelants"],
                               "hypothese": hypothese_A_seul(a, A[a], ext_B)} for a in a_seul_64],
    }
    with open(os.path.join(OUT, "croisement-stats.json"), "w") as f:
        json.dump(stats, f, indent=1, ensure_ascii=False)
    with open(os.path.join(OUT, "seeds-ghidra-complement.json"), "w") as f:
        json.dump([f"0x{a:08X}" for a in a_seul], f, indent=0)
    print(json.dumps({k: v for k, v in stats.items()
                      if k not in ("top20", "A_seul_ge64_liste")}, ensure_ascii=False, indent=1))
    for e in stats["top20"]:
        print(f"{e['octets']:>7} {e['categorie']:<9} {e['adresse']} {e['detail']}\n          -> {e['hypothese']}")


if __name__ == "__main__":
    sys.exit(main())
