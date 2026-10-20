#!/usr/bin/env python3
"""correctif-movhps.py — passe 7n : corrige SANS RELIFT les movhps/movhpd mal liftés d'un build existant.

Le lifter pcrecomp (lift32_cpu.py) testait `m[4] == "h"` pour choisir la voie 64 bits de
movlps/movlpd/movhps/movhpd ; dans « movhps » le h est m[3] : la voie HAUTE n'était jamais
choisie (movhps = movlps). Sur SC2.exe, les 8 sites sont dans L_009CE7F0, le produit de matrices
4×4 SSE : toutes les matrices de vue-projection valaient (a, b, a, b) → triangles dégénérés,
menu de campagne NOIR. Le vrai correctif est android/outils/pcrecomp-movhps-7n.patch (+ relift).

Ici, pour chaque fonction liftée qui contient un movhps/movhpd, l'outil :
  1. extrait sa définition du C lifté <src> (elle doit être identique à celle des objets : une
     fonction feuille sans SC2_SAUT convient, cf. le contrôle ci-dessous), corrige la voie ;
  2. la compile seule → <objets>/zz_correctif_movhps_<VA>.o (symbole FORT) ;
  3. rend le symbole FAIBLE dans l'objet d'origine (objcopy --weaken-symbol) : l'édition de liens
     retient la version corrigée. Rien d'autre ne change.

    python3 -I android/outils/correctif-movhps.py <src/lifted> <dossier des objets liftés> [--cc-args "…"]
puis relier (android/outils/relink-runtime.sh). Idempotent (l'objet d'origine est sauvegardé en .orig).
"""
import os, re, shutil, subprocess, sys

def main():
    src, objs = sys.argv[1], sys.argv[2]
    racine = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'recomp-sc2'))
    cc = ['cc', '-fno-pie', '-std=gnu17', '-w', '-O0', '-I' + racine + '/src', '-I' + racine + '/runtime',
          '-I' + racine + '/src/lifted', '-include', racine + '/runtime/sc2_lifted.h']
    site = re.compile(r'/\* ([0-9A-F]{8}): movh(ps|pd) ')
    n_fn = n_sites = 0
    for f in sorted(os.listdir(src)):
        if not (f.startswith('recomp_') and f.endswith('.c')):
            continue
        lignes = open(os.path.join(src, f)).read().split('\n')
        if not any(site.search(l) for l in lignes):
            continue
        entetes = [l for l in lignes[:20] if l.startswith('#')]
        debuts = [i for i, l in enumerate(lignes) if re.match(r'^void L_[0-9A-F]{8}\(CPU \*c\)$', l)]
        for k, d in enumerate(debuts):
            fin = (debuts[k + 1] if k + 1 < len(debuts) else len(lignes))
            corps = lignes[d:fin]
            idx = [i for i, l in enumerate(corps) if site.search(l)]
            if not idx:
                continue
            nom = corps[0].split()[1].split('(')[0]
            if any('SC2_SAUT' in l for l in corps):
                print(f'REFUS {nom} : contient SC2_SAUT (source d\'un autre lift que les objets ?)'); continue
            for i in idx:
                l2 = re.sub(r'(c->xmm\[\d\]\.u64\[)0(\])', r'\g<1>1\2', corps[i], count=1)
                if l2 != corps[i]:
                    corps[i] = l2; n_sites += 1
            c_fix = os.path.join(objs, f'zz_correctif_movhps_{nom}.c')
            with open(c_fix, 'w') as o:
                o.write('/* GÉNÉRÉ par android/outils/correctif-movhps.py (passe 7n) : movhps/movhpd sur la voie HAUTE */\n')
                o.write('\n'.join(entetes + corps) + '\n')
            subprocess.run(cc + ['-c', c_fix, '-o', c_fix[:-2] + '.o'], check=True)
            obj = os.path.join(objs, f + '.o')
            if not os.path.exists(obj + '.orig'):
                shutil.copy2(obj, obj + '.orig')
            shutil.copy2(obj + '.orig', obj)
            subprocess.run(['objcopy', '--weaken-symbol=' + nom, obj], check=True)
            n_fn += 1
            print(f'{nom} ({f}) : {len(idx)} sites movhps/movhpd → voie haute ; symbole d\'origine affaibli')
    print(f'{n_fn} fonction(s), {n_sites} site(s) corrigé(s)')

if __name__ == '__main__':
    main()
