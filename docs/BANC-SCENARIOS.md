# Banc par scénarios événementiels et non-régression 🇫🇷

*Méthode, état au 24/10/2026. Aucune donnée du jeu ici : les scénarios sont des suites d'entrées (coordonnées,
touches) et de déclencheurs ; les références de non-régression (empreintes de frames, sha256 du binaire et du pack
SPIR-V) restent sur la machine de chacun.*

Un jeu recompilé ne se teste pas comme une bibliothèque : la plupart des défauts n'apparaissent qu'après des minutes
de menus, de chargements et de partie. Le banc fait donc **jouer le jeu sans joueur** : un fichier de scénario
injecte des entrées Win32 dans la boucle du jeu, des déclencheurs attendent les états du moteur (carte lue, fin de
chargement, victoire), et un dépouillement compare chaque run à une référence. Le même scénario tourne au banc Mac
(MoltenVK), sur l'émulateur Android et sur le téléphone.

## 1. Le module d'entrées du banc (`runtime/sc2_entrees_banc.c`)

`SC2_BANC_ENTREES=@<fichier>` (ou la liste en ligne) ; chaque événement appliqué est journalisé
`[INPUT] banc t=…s tour N`, et un bilan par segment sort à la fin du run. Sans la variable : aucun effet (le canon
reste identique à l'octet).

**Format 1** (instants fixes, en secondes ou en tours de pompe `f<n>`) : `DOWN`, `UP`, `CLIC`, `MOVE`, `ESC`, `KEY:vk`,
`KEYDOWN`/`KEYUP` (combinaisons), `QUIT` (arrêt volontaire), et le marqueur `QUAND:VICTOIRE` (le moteur a écrit sa
sauvegarde de victoire) qui fait partir la suite en instants relatifs `+s`.

**Format 2** (événementiel, sur-ensemble strict du format 1 — prouvé à l'identique sur les 16 scénarios existants par
le test hôte, qui embarque une copie de l'ancien analyseur) :

| élément | rôle |
|---|---|
| `QUAND:CARTE:<nom>` | une carte précise est lue dans les archives (rappel de la couche MPQ) |
| `QUAND:FICHIER:<motif> [ECRITURE]` | un `CreateFileW` réussi correspond au motif (glob, sans casse) |
| `QUAND:PRET` / `QUAND:CHARGEMENT` | fin / début d'un écran de chargement (masque des écrans d'interface du moteur) |
| `QUAND:VICTOIRE` / `QUAND:FINPARTIE` | victoire de campagne / écran de fin d'une partie d'escarmouche |
| `QUAND:FRAME:<n>`, `QUAND:STABLE:<s>`, `QUAND:GLUE:<m>` | nombre d'images présentées, aucune lecture d'archive depuis s secondes, écrans ouverts |
| `MAX:<s> SINON:SUITE\|QUIT\|SAUTE` | délai d'un déclencheur et conduite à tenir s'il échoit |
| `REF:LxH`, `x,y@ui` / `@scene` / `@lin` / `x%,y%` | coordonnées écrites pour un client de référence, ramenées au client réel (interface 2D en boîte 4:3, scène 3D à champ constant) |
| `RCLIC`, `DRAG`, `MOLETTE`, `TEXTE`, `WAIT`, `CAPTURE:<nom>` | clic droit, glisser, molette, saisie, jalon, capture nommée au Present suivant |
| `GARDE:<s>` | arrêt volontaire absolu, jamais annulé |

Plafonds : 1 024 événements, 64 segments. Le segment *k* n'est armé qu'au déclenchement du segment *k − 1* ; ce qui
restait du segment précédent est annulé et les touches maintenues sont relâchées. Résultat mesuré : le parcours « bar
→ console de mission → chargement de la mission suivante » passe de 170 s (instants fixes, et en échec en 1280×1024) à
77-82 s, dans les deux résolutions.

Règles de rédaction apprises : un bouton qui ne réagit qu'après survol exige `MOVE` puis `CLIC` ; un panneau qui
s'ouvre tard sous charge se ferme par des clics répétés sur une fenêtre de temps ; jamais de point-virgule dans un
commentaire (c'est le séparateur) ; chaque scénario dit en tête ce qu'il attend (`rc`, lignes du journal).

## 2. Les bancs (`scripts/`)

- `banc-mac.sh` : run natif (une mission, le canon « defaut » de 600 frames, ou un scénario) ; `banc-bar.sh` : entrée de
  campagne par le bar ; `campagne-banc.sh` : les 29 missions de la campagne, une par run, gagnées sans joueur ;
  `banc-escarmouche.sh`, `banc-options.sh` (matrice de préréglages graphiques), `marathon-bar.sh` + `marathon-scenario.py`
  (plusieurs missions **dans un même lancement**, depuis un plan) ; `progression-oracle.py` lit la progression écrite par
  le jeu après chaque victoire.
- Disque : `SC2_FRAMES=0` (aucune capture), compactage des runs en `empreintes.txt` + vignettes (`lib-banc.sh`), garde
  `df` en tête de chaque banc. Un run de mission passe de 1,3 Go à 27 Mo.
- Verrous partagés entre sessions parallèles (`verrou.sh` : compilation du lift, éditions de liens, banc, émulateur).
- Téléphone : `bar-fold.sh`, `fold-ab-auto.sh`, `seance-fold.sh` (séance complète, préréglages et réglages de
  l'utilisateur sauvegardés puis remis à l'identique) ; appareil par `SC2_ADB_SERIAL` ou par modèle (`lib-fold.sh`) ;
  `ADB_FAKE=1` rejoue tout contre des fixtures synthétiques.

## 3. Non-régression (`scripts/nonreg.sh`, `scripts/nonreg.py`)

Deux niveaux : **fumée** (canon, bar, trois missions en victoire ; ≈ 14 min au banc M4) et **complet** (canon, bar,
29 missions ; ≈ 1 h 40 à 2 h). Critères par contrôle, contre la référence de même nom :

| critère | exigence |
|---|---|
| `rc` | 0 |
| victoire | détectée (missions) ; cartes attendues lues (bar) |
| `MANQUE` | fonctions absentes du lift ≤ référence |
| « lumière » | dessins tombés sur le shader de repli ≤ référence |
| replis de texture | « sans données » : 0, toujours ; autre cause nouvelle : régression |
| arrêts | arrêts non volontaires (signal, DEVICE_LOST) ≤ référence |
| images | canon : exactement 600 ; runs minutés : ≥ 50 % de la référence |
| empreintes | canon : frames 1, 10, 100, 200, 300 identiques |
| images/s | jugées sur l'**agrégé** des missions (−10 %) ; par mission : indicatives |

Pourquoi l'agrégé : sur un banc partagé avec des agents, un même binaire varie de −9 % à +38 % d'une mission à
l'autre ; la mesure fine des images/s se fait à part (`banc-ab.sh` : paires entrelacées A/B, première paire de chauffe
écartée, machine calme). La référence est posée par une seule personne (`--maj-ref`), refusée si un contrôle n'est pas
PASS hors images/s. Chaque utilisateur pose **sa** référence (`NONREG_REF=<dossier>`) : elle contient des empreintes de
ses propres rendus et n'a pas sa place dans un dépôt.

Le juge a lui-même été testé : `SC2_ACTIVATION=0` (jeu laissé « en arrière-plan ») doit sortir RÉGRESSION (−61 %
d'images/s), un binaire sans SPIR-V aussi (21 M dessins de repli) ; `injecter-bugs-v22.sh` applique des mutants à une
copie du backend et exige que chacun soit tué par son test.

## 4. La porte rapide (`scripts/verifier.sh`, `tests-hote/`)

Avant tout banc : tests hôtes **sans le code lifté** (`tests-hote/`, CMake indépendant, -O0 : runtime écrit à la main,
en-têtes « pur calcul » d'Android et de l'hôte Mac ; 33 tests, dont un SKIP connu sur macOS), tests unitaires Python des
scripts, `bash -n`, fixtures `ADB_FAKE`, contrôle de la documentation des variables (`variables-sc2.sh verifier` contre
[VARIABLES.md](VARIABLES.md)). Environ 80 s. Sur ce dépôt :

```sh
cmake -S tests-hote -B build-tests -G Ninja && cmake --build build-tests && ctest --test-dir build-tests
bash android/test-javac.sh                       # tests Java du lanceur (écran titre, options, catalogue)
(cd scripts && python3 -m unittest test_nonreg test_marathon_scenario test_progression_oracle)
```

## 5. Ce que le banc a trouvé (exemples mesurés)

- Le jeu se croyait en arrière-plan (aucun `WM_ACTIVATEAPP` livré) et dormait 10 ms à chaque tour de boucle : trouvé
  par l'arbre d'appels de `sample` au banc, confirmé par `SC2_ATTENTES=1`, corrigé dans le module d'entrées — +60 %
  d'images/s sur le téléphone (35 → 56).
- Instanciation D3D9 ignorée (une instance dessinée sur N ≤ 516) : compteurs `[BILAN] [ETATS]` sur 9 runs, puis
  200 000 cas aléatoires contre une référence et 9 mutants tués ; les particules apparaissent.
- Sauvegardes vides (1 181 octets) : un `CreateDirectoryW` sur un chemin à séparateur final ; scénarios « sauver puis
  charger » comparés par captures.
- Un plantage intermittent (≈ 1 run sur 5, une seule mission) isolé par la non-régression complète : une texture lue
  vide menait D3DX à lire l'adresse invitée 0 ; D3DX refuse désormais une source nulle, comme d3dx9.
