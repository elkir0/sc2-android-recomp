#!/usr/bin/env python3
"""Passe 7v — bilan des MANQUE contournés (SC2_MANQUE_CONTINUE=1) d'un run, et comparaison de deux runs.

  manques-7v.py <run>                 liste : adresse, ordre d'apparition, 1re frame, occurrences, ret, appelant
  manques-7v.py --comparer <A> <B>    md5 des frames capturées (empreintes.txt) et listes de manques des deux runs

<run> = dossier d'un run de android/valider-7v.sh (run.log, empreintes.txt). Le bilan exact vient des lignes
« [BILAN] [MANQUE] » (écrites par sc2_stop) ; à défaut (run tué), des lignes « [MANQUE-CONTINUE] » (occurrences
connues seulement à la puissance de 2 près : borne inférieure, marquée « ≥ »). Bibliothèque standard seulement.
"""
import os
import re
import sys

BILAN = re.compile(r'^\[BILAN\] \[MANQUE\] 0x([0-9A-F]{8})  1re frame (-?\d+)  occurrences (\d+)  1er dispatch (\d+)'
                   r'  ret 0x([0-9A-F]{8})  appelant 0x([0-9A-F]{8})')
LIGNE = re.compile(r'\[MANQUE-CONTINUE\] (MANQUE : [^0]*)0x([0-9A-F]{8}) n=(\d+) frame (-?\d+) ret 0x([0-9A-F]{8})'
                   r' appelant 0x([0-9A-F]{8}) ecx ([0-9A-F]{8}) edx ([0-9A-F]{8}) \[ecx\] ([0-9A-F]{8})')


def lire(run):
    """-> (liste de dicts dans l'ordre d'apparition, exact?)"""
    bilan, vus = [], {}
    with open(os.path.join(run, 'run.log'), errors='replace') as f:
        for l in f:
            m = BILAN.match(l)
            if m:
                bilan.append(dict(va=m[1], frame=int(m[2]), n=int(m[3]), disp=int(m[4]), ret=m[5], appelant=m[6]))
                continue
            m = LIGNE.search(l)
            if m:
                va = m[2]
                if va not in vus:
                    vus[va] = dict(va=va, quoi=m[1].strip(), frame=int(m[4]), n=0, ret=m[5], appelant=m[6],
                                   ecx=m[7], vtable=m[9])
                vus[va]['n'] = max(vus[va]['n'], int(m[3]))
    for e in bilan:                      # le bilan est exact ; les lignes donnent this/vtable de la 1re occurrence
        e.update({k: v for k, v in vus.get(e['va'], {}).items() if k in ('quoi', 'ecx', 'vtable')})
    if bilan:
        return bilan, True
    fin_propre = any(l.startswith('[BILAN] ') for l in open(os.path.join(run, 'run.log'), errors='replace'))
    return list(vus.values()), fin_propre and not vus      # aucun manque et bilan écrit : liste exacte (vide)


def afficher(run):
    l, exact = lire(run)
    print(f'# manques de {run} : {len(l)} adresse(s) ; occurrences {"exactes (bilan)" if exact else "≥ (run tué : 2^k)"}')
    print('# rang  adresse     1re_frame  occurrences  ret_invitee  appelant    [ecx]=vtable')
    for i, e in enumerate(l, 1):
        print(f"{i:5d}  0x{e['va']}  {e['frame']:9d}  {('' if exact else '≥') + str(e['n']):>11}  0x{e['ret']}"
              f"   0x{e['appelant']}  {e.get('vtable', '?')}")
    if not l:
        return
    # (a) concentration : part des occurrences portée par l'adresse la plus fréquente
    tot = sum(e['n'] for e in l)
    top = max(l, key=lambda e: e['n'])
    print(f'# total {tot} occurrences ; la plus fréquente : 0x{top["va"]} ({100 * top["n"] / tot:.1f} %)')
    # (b) familles : même vtable [ecx] au site d'appel
    fam = {}
    for e in l:
        if e.get('vtable') and e['vtable'] != '00000000':
            fam.setdefault(e['vtable'], []).append(e['va'])
    for v, vas in sorted(fam.items(), key=lambda x: -len(x[1])):
        if len(vas) > 1:
            print(f'# famille vtable 0x{v} : ' + ' '.join('0x' + a for a in vas))
    # (c) dispersion : écart entre adresses
    vas = sorted(int(e['va'], 16) for e in l)
    print(f'# étendue 0x{vas[0]:08X}..0x{vas[-1]:08X}')


def empreintes(run):
    p = os.path.join(run, 'empreintes.txt')
    if not os.path.exists(p):
        return {}
    return {nom: md5 for md5, nom in (l.split() for l in open(p) if l.strip())}


def comparer(a, b):
    ea, eb = empreintes(a), empreintes(b)
    communs = sorted(set(ea) & set(eb))
    egaux = [n for n in communs if ea[n] == eb[n]]
    print(f'# frames capturées communes {len(communs)} ; md5 identiques {len(egaux)} ; différentes {len(communs) - len(egaux)}')
    for n in communs:
        print(f'{n}  {ea[n][:12]}  {eb[n][:12]}  {"=" if ea[n] == eb[n] else "≠"}')
    la, _ = lire(a)
    lb, _ = lire(b)
    sa, sb = [e['va'] for e in la], [e['va'] for e in lb]
    print(f'# manques A {len(sa)} ; B {len(sb)} ; même ordre : {sa == sb[:len(sa)] or sb == sa[:len(sb)]}')
    print('# seulement A : ' + ' '.join(sorted(set(sa) - set(sb))))
    print('# seulement B : ' + ' '.join(sorted(set(sb) - set(sa))))


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--comparer':
        comparer(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 2:
        afficher(sys.argv[1])
    else:
        sys.exit(__doc__)
