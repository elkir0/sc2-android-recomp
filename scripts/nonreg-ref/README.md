# scripts/nonreg-ref/ — références de `scripts/nonreg.sh` (V1.4)

*Dépôt public : ce dossier ne contient que ce README. Chaque utilisateur pose SA référence sur sa machine
(`--maj-ref`, ou `NONREG_REF=<dossier>`) : empreintes de frames, sha256 du binaire et du pack restent locaux.*

Contenu (rapports et empreintes seulement : **jamais d'image du jeu, de run.log, de shader ni de carte**) :

- `fumee.json` / `complet.json` : le `nonreg.json` du run qui a posé la référence (sha256 du binaire et du pack
  SPIR-V, mesures et critères de chaque contrôle), plus `ref_posee` (date, dossier du run, refus forcés) ;
- `fumee/` / `complet/` : `nonreg.md` (tableau du run), `rapport-campagne.md` (campagne-rapport.py) et, par contrôle,
  `resume.txt` et `empreintes.txt` (empreintes des frames, sorties de `android/outils/ppm-sequence.py`).

Seul `scripts/nonreg.py` lit `<mode>.json` ; les autres fichiers servent à relire la référence à la main.

**Seul [SUP] pose une référence** (`scripts/nonreg.sh <binaire> <sortie> [--complet] --maj-ref`), en citant le
chantier dans une ligne de `NOTES.md`. La pose est refusée si un contrôle n'est pas PASS hors images/s (`--forcer`
passe outre et le note dans `ref_posee`). Critères et tolérances : en-tête de `scripts/nonreg.py`.
