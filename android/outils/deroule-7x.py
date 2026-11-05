#!/usr/bin/env python3
"""Passe 7x — déroulé d'une mission d'après le journal d'un run (run.log) : ce qui AVANCE et quand.

  deroule-7x.py <run> [--tout]

Chaque événement est daté par la dernière frame connue du journal à cet endroit : lignes « [VK] Present frame n°N »
(toutes les SC2_FRAMES_PAS frames), « [D3D9] Present : FRAME n°N » et « [INPUT] frame N ». Précision : ± PAS frames.
Événements relevés :
  - réplique  : 1re lecture d'un fichier de voix de mission (…\\CampaignVO\\<nom>.ogg) — le script de mission parle ;
  - visage    : animation faciale (.fxa) — portrait parlant ;
  - interface : 1re lecture d'un .swf / .SC2Layout (nouvel élément d'interface) ;
  - texte     : chaînes mises en page ([TEXTE], distinctes) ;
  - ordre     : messages injectés livrés à la wndproc ([INPUT] … WM_…) ;
  - manque    : [MANQUE-CONTINUE] ;
  - nommé     : 1re lecture d'une ressource au nom en clair (textures/modèles de l'archive de langue, sons hors VO).
Puis un histogramme des 1res lectures MPQ par tranche de 250 frames (de nouvelles ressources = du nouveau à l'écran ou
à l'oreille). --tout : aussi les textes et ressources nommées en entier. Bibliothèque standard seulement.
"""
import os
import re
import sys
from collections import Counter

R_FRAME = [re.compile(r'^\[VK\] Present frame n°(\d+)'), re.compile(r'^\[D3D9\] Present : FRAME n°(\d+)'),
           re.compile(r'^\[INPUT\] frame (\d+)')]
R_LECT = re.compile(r'^\[MPQ\] (.+?) → (.+?) \((\d+) o\) — 1re lecture')
R_TEXTE = re.compile(r'^\[TEXTE\] n°\d+ \S+ \(\d+ car\.\) « (.*) »')
R_ORDRE = re.compile(r'^\[INPUT\] frame (\d+) : (WM_\w+) .*?= \((\d+), (\d+)\)\)')
R_TOUCHE = re.compile(r'^\[INPUT\] frame (\d+) : (WM_KEY\w+) \(0x\w+, wParam 0x(\w+)')
R_MANQUE = re.compile(r'^\[MANQUE-CONTINUE\] (.*)')


def lire(run):
    ev, lect, frame = [], [], 0
    textes_vus = set()
    with open(os.path.join(run, 'run.log'), errors='replace') as f:
        for l in f:
            for r in R_FRAME:
                m = r.match(l)
                if m:
                    frame = max(frame, int(m[1]))
            m = R_LECT.match(l)
            if m:
                arch, nom = m[1], m[2]
                lect.append(frame)
                if '\\CampaignVO\\' in nom:
                    ev.append((frame, 'réplique', nom.rsplit('\\', 1)[1]))
                elif nom.endswith('.fxa'):
                    ev.append((frame, 'visage', nom.rsplit('\\', 1)[1]))
                elif nom.endswith(('.swf', '.SC2Layout')):
                    ev.append((frame, 'interface', nom.rsplit('\\', 1)[1]))
                elif not re.match(r'File\d+\.', nom):
                    ev.append((frame, 'nommé', nom))
                continue
            m = R_TEXTE.match(l)
            if m and m[1] not in textes_vus:
                textes_vus.add(m[1])
                ev.append((frame, 'texte', m[1]))
                continue
            m = R_TOUCHE.match(l)
            if m:
                ev.append((int(m[1]), 'ordre', f'{m[2]} VK 0x{m[3]}'))
                continue
            m = R_ORDRE.match(l)
            if m and 'MOUSEMOVE' not in m[2] and 'CHAR' not in m[2]:
                ev.append((int(m[1]), 'ordre', f'{m[2]} ({m[3]}, {m[4]})'))
                continue
            m = R_MANQUE.match(l)
            if m:
                ev.append((frame, 'manque', m[1][:160]))
    return ev, lect, frame


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    run, tout = sys.argv[1], '--tout' in sys.argv
    ev, lect, fin = lire(run)
    n = Counter(k for _, k, _ in ev)
    print(f'# déroulé de {run} : dernière frame {fin} ; ' + ', '.join(f'{k} {v}' for k, v in n.items()))
    for fr, k, s in ev:
        if k in ('réplique', 'visage', 'ordre', 'manque') or (k == 'interface' and fr > 0) or tout:
            print(f'{fr:6d}  {k:9s}  {s}')
    if not tout:
        t = [s for _, k, s in ev if k == 'texte']
        print(f'# textes distincts : {len(t)} — ' + ' | '.join(t[:40])[:1500])
        nm = [(fr, s) for fr, k, s in ev if k == 'nommé']
        print(f'# ressources nommées : {len(nm)} ; les 15 dernières : '
              + ' | '.join(f'f{fr} {s.rsplit(chr(92), 1)[-1]}' for fr, s in nm[-15:]))
    h = Counter(fr // 250 * 250 for fr in lect)
    print('# 1res lectures MPQ par tranche de 250 frames : '
          + ' '.join(f'{k}:{h[k]}' for k in sorted(h)))


if __name__ == '__main__':
    main()
