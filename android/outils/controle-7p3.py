#!/usr/bin/env python3
"""Passe 8d : verdict des contrôles image du banc 7p3 (android/valider-7p3.sh verdict).

Lit <O>/run-controle-{a,b,t}/run.log (lignes « [VKP] contrôle frame n°… ») et rc :
  A = défaut (effacement complet), B = SC2_PRESENT_BANDES=1 (bandes seules), T = B + SC2_PRESENT_BANDES_TEMOIN=1.
Attendus :
  - A et B se DISTINGUENT par la voie (effacement-complet / bandes-seules) à chaque frame relue ;
  - A et B donnent les MÊMES pixels : empreintes de l'image et des bandes égales, 0 pixel de bande non noir ;
  - T (témoin positif) : voie bandes-temoin, bandes non noires, empreinte des bandes ≠ A, image = A.
Un run absent, sauté (rc « sautee ») ou sans ligne de contrôle donne NON CONCLUANT avec la raison : jamais une liste vide muette.
Sortie : une ligne par contrôle puis « verdict image : … ». Code de sortie 0 si CONFORME, 1 sinon.
"""
import re, sys

O = sys.argv[1]
RX = re.compile(r"contrôle frame n°(\d+) : .*?moyenne RGB de l'image ([\d.]+) ([\d.]+) ([\d.]+), bandes ([\d.]+) .*?; "
                r"voie (\S+), pixels de bande non noirs (\d+), empreinte image ([0-9a-f]+) bandes ([0-9a-f]+)")


def lire(n):
    d = f'{O}/run-controle-{n}'
    try: rc = open(f'{d}/rc').read().strip()
    except OSError: rc = 'absent'
    try: t = open(f'{d}/run.log', 'rb').read().decode('utf-8', 'replace')
    except OSError: t = ''
    c = {int(m[0]): dict(rgb=tuple(float(x) for x in m[1:4]), bandes=float(m[4]), voie=m[5], nnoir=int(m[6]),
                         hi=m[7], hb=m[8]) for m in RX.findall(t)}
    return rc, c


R = {n: lire(n) for n in 'abt'}
raisons = []
for n, (rc, c) in R.items():
    print(f'  controle-{n} : rc {rc}, frames relues {sorted(c) or "AUCUNE"}'
          + ''.join(f" ; f{k} voie {v['voie']} bandes {v['bandes']:.2f} non-noirs {v['nnoir']} img {v['hi'][:12]} "
                    f"bandes {v['hb'][:12]}" for k, v in sorted(c.items())))
    if rc == 'sautee': raisons.append(f'controle-{n} sauté (run concurrent)')
    elif rc != '0': raisons.append(f'controle-{n} rc {rc}')
    elif not c: raisons.append(f'controle-{n} sans ligne de contrôle (SC2_SWAPCHAIN_CONTROLE / build sans swapchain ?)')
if raisons:
    print('  verdict image : NON CONCLUANT — ' + ' ; '.join(raisons)); sys.exit(1)

a, b, t = (R[n][1] for n in 'abt')
fr = sorted(set(a) & set(b) & set(t))
ecarts = []
if not fr: ecarts.append('aucune frame relue commune')
for k in fr:
    if a[k]['voie'] != 'effacement-complet': ecarts.append(f"f{k} A voie {a[k]['voie']}")
    if b[k]['voie'] != 'bandes-seules': ecarts.append(f"f{k} B voie {b[k]['voie']}")
    if t[k]['voie'] != 'bandes-temoin': ecarts.append(f"f{k} T voie {t[k]['voie']}")
    if a[k]['hi'] != b[k]['hi']: ecarts.append(f'f{k} image A≠B')
    if a[k]['hb'] != b[k]['hb'] or a[k]['nnoir'] or b[k]['nnoir']: ecarts.append(f'f{k} bandes A/B non noires ou ≠')
    if a[k]['hb'] == 'cbf29ce484222325': ecarts.append(f'f{k} pas de bandes (empreinte vide) : rien à contrôler')
    elif t[k]['nnoir'] == 0 or t[k]['hb'] == a[k]['hb']: ecarts.append(f'f{k} témoin INVISIBLE (bandes T = A)')
    if a[k]['hi'] != t[k]['hi']: ecarts.append(f'f{k} image A≠T')
if ecarts:
    print('  verdict image : ÉCART — ' + ' ; '.join(ecarts)); sys.exit(1)
temoin = ', '.join(f"f{k} {t[k]['nnoir']} px non noirs, bandes {t[k]['bandes']:.2f}" for k in fr)
print(f'  verdict image : CONFORME — frames {fr} : A/B distingués par la voie (effacement-complet / bandes-seules), '
      f'pixels identiques (empreintes image et bandes A = B, 0 pixel de bande non noir) ; témoin T vu '
      f'({temoin})')
