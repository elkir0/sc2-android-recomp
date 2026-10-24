# StarCraft II sur Android — la recompilation statique outillée

**[English version 🇬🇧](README.md)**

[![legal-scan](https://github.com/elkir0/sc2-android-recomp/actions/workflows/legal-scan.yml/badge.svg)](https://github.com/elkir0/sc2-android-recomp/actions/workflows/legal-scan.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Aucun contenu du jeu](https://img.shields.io/badge/contains-no%20game%20code%20or%20assets-green)

**D'un exécutable Windows x86-32 de 15,7 Mo à un APK Android signé et jouable avec un
renderer Vulkan natif — par recompilation statique assistée par IA.** Une semaine de
travail a mené StarCraft II: Wings of Liberty (2010) de « ça boote » à « la première
mission de campagne se joue de bout en bout sur téléphone » ; la deuxième en a fait un
jeu utilisable : écran titre, sauvegardes, campagne entière pilotée par un banc
automatique, escarmouche hors ligne contre l'IA, 56 à 61 images/s sur l'appareil, et un
hôte macOS natif. Ce dépôt est la méthode et la chaîne d'outils complètes, validées de
bout en bout, pour faire pareil avec **votre** jeu de l'ère D3D9.

> **Il ne contient aucun code ni aucun asset du jeu — et n'en contiendra jamais.**
> Tout ce qui dérive du jeu est généré localement, depuis votre propre copie
> légitime, par les outils publiés ici. Un scanner CI l'impose à chaque commit.
> Hors-ligne / solo uniquement. StarCraft® et Blizzard Entertainment® sont des marques
> de Blizzard Entertainment, Inc. Projet indépendant, non commercial, sans affiliation.
> Détails : [LEGAL.md](LEGAL.md).

## Est-ce que ça marche ? (24 octobre 2026)

**Le jeu recompilé, en train de tourner sur un Galaxy Z Fold 8, sur l'appareil lui-même** —
sélection au rectangle, sauts de caméra par la minicarte, puis le geste « home » vers
l'écran d'accueil Android :

![Gameplay sur un Galaxy Z Fold 8 : mission 1, sélections et déplacements de caméra, puis retour à l'écran d'accueil Android](docs/fold8-gameplay.gif)

| | | |
|---|---|---|
| ![Menu campagne](docs/menu-glue-single-f1000-campagne.png)<br/>Menu de campagne, Vulkan | ![Menu + scène 3D](docs/menu-echap-f2500-scene-sans-ui.png)<br/>Menu et scène 3D | ![Chargement mission](docs/launch-f3600-liberation-day-chargee.png)<br/>Chargement de mission sur appareil |
| ![Sélection de mission](docs/mission-selection-f2400-liberation-day.png)<br/>Console de sélection de mission | ![Cinématique](docs/ok-normal-f1700-tstory01-raynor.png)<br/>Cinématique en moteur | ![Dashboard moteur](docs/tumor-b-f1500-mainui-dashboard.png)<br/>UI moteur pilotée depuis le runtime |

*Captures réalisées depuis notre propre copie légitime, à seule fin de documentation.*

| L'essentiel en chiffres | |
|---|---|
| Fonctions liftées en C portable | **86 653** (420 Mo de C, compilent en x86-64 Linux, Apple Silicon, aarch64 Android) |
| Certifiées contre un émulateur de référence matérielle | **76 650** fonctions, **0 vraie divergence** |
| Campagne | **29 missions sur 29 gagnées** par le banc automatique (run de référence du 23/10 : sans joueur, 0 plantage, 0 fonction manquante) ; **6 missions de l'acte 1 d'affilée dans un seul lancement** depuis que la mémoire invitée est rendue entre les missions |
| Performance en jeu actif | **56 à 61 images/s sur un Galaxy Z Fold 8** — médiane au plafond de la synchro à 60 Hz (28,1 le 20/10, 4,1 au premier build jouable) |
| Shaders | **les 75 797** shaders du cache du jeu traduits en un pack SPIR-V local — construit sur votre machine depuis votre copie, jamais distribué |
| Sauvegardes | sauvegarde depuis le menu de jeu, chargement **à l'état sauvé**, ou démarrage direct sur une sauvegarde (banc et émulateur Android) |
| Escarmouche | **hors ligne, contre l'IA**, sur les 63 cartes mêlée du cache de l'installation elle-même — l'IA Facile bâtit sa base, attaque à 9 min et gagne à 10 (banc) ; tourne aussi sur l'émulateur Android |
| Audio | bande-son + voix de mission reconstruites **bit-exact** ; les ≈ 3 000 sons `.wav` des archives rebâties rétablis (hub : 40 → 219 sons) |
| Hôtes | Android (lanceur en écran titre : Continuer, Charger, Escarmouche, Missions, Options) et **macOS** (app Cocoa fenêtrée, Metal par MoltenVK, CoreAudio, clavier/souris/trackpad, menus du lanceur) |

## État du projet

| Sous-système | État | Notes |
|---|---|---|
| Recompilation statique (x86-32 → C portable) | ✅ fait | modèle CPU-struct réentrant ; patches génériques du lifter publiés (atomiques, MMX, voies SSE) |
| Renderer Vulkan (D3D9 → Vulkan) | ✅ cœur complet | menus, scènes 3D, terrain, HUD, unités, ombres (aussi sur l'Adreno 840), stencil, **instanciation** (particules) ; caps explicites ([docs/CAPS.md](docs/CAPS.md)) ; MRT, plan de découpe et portrait de l'Ultra restants |
| Couverture shaders | ✅ | pack complet (75 797) généré localement ; 0 dessin sur le shader de repli dans les 29 missions |
| Audio (FMOD Ex → mixeur logiciel → AAudio / CoreAudio) | ✅ | reconstruction bit-exact ; alias `.wav` servis par le runtime |
| Vidéo (Theora) | ✅ | temps réel sur appareil ; voie H.264 matérielle optionnelle |
| Entrées | ✅ | tactile (1 doigt = clic gauche / sélection au cadre, 2 doigts = clic droit), barre de raccourcis dans la bande noire, clavier/souris BT ; macOS : événements Cocoa, AZERTY |
| Campagne | ✅ banc · 🟡 appareil | progression persistante ; 29/29 missions gagnées au banc ; 6 d'affilée en un lancement ; un achat à l'armurerie fonctionne dans l'interface (format des nombres d'ICU implémenté : crédits et coûts s'affichent) |
| Sauvegardes | ✅ banc + émulateur | sauver / charger / démarrer sur une sauvegarde ; à confirmer sur le téléphone |
| Escarmouche contre l'IA (hors ligne) | ✅ banc + émulateur | détection de fin de partie pour le banc ; à essayer sur le téléphone |
| Écran titre (Android) | ✅ émulateur | écrans intérieur et extérieur du pliable ; sur le téléphone ensuite |
| Hôte macOS | ✅ fenêtré | même binaire que le banc (images de référence identiques) ; séance en main (son, AZERTY, trackpad, plein écran) à faire |
| Performance | ✅ | 56 à 61 images/s sur le Fold, plafonnées par la synchro ; le coût restant est le code lifté |
| Longues sessions | 🟡 | l'arène invitée grossit encore d'≈ 100 Mo par mission après la passe de libération : ≈ 10 missions par lancement |

Le journal daté complet (35+ jalons, avec la mesure derrière chaque affirmation) :
[docs/MILESTONES.md](docs/MILESTONES.md).

## Feuille de route

| # | Jalon | Pourquoi c'est important |
|---|---|---|
| 1 | **Séance sur le téléphone** avec le dernier build : écran titre, sauvegarde/chargement, escarmouche, endurance de 20-30 min | tout ce qui précède est prouvé au banc et sur l'émulateur Android ; l'appareil reste le juge |
| 2 | **Fidélité du rendu** : cibles de rendu multiples (une 2e cible est liée sur 23 à 39 % des dessins), le seul plan de découpe utilisé par le jeu, la passe d'ombre sans cible couleur, le portrait gris en Ultra | les dernières différences visibles, trouvées par des compteurs d'états plutôt qu'à l'œil |
| 3 | **Campagne entière en un lancement** — rendre la croissance restante par mission | 6 missions d'affilée aujourd'hui, ≈ 10 par lancement avant l'épuisement de l'arène |
| 4 | **À-coups de chargement** — construire l'index des archives hors du verrou global (0,85 à 2,4 s par grosse archive) | chargements et transitions plus fluides |
| 5 | **Finitions macOS** : plein écran, premier lancement par le Finder | un build de bureau confiable à un testeur (en privé : il contient du code recompilé) |

## Portez VOTRE jeu D3D9 — la recette

Tout ce qui précède se généralise. La recette pas à pas complète — arbre de décision,
inventaire, analyse statique, lift, socle hôte, le device D3D9, la traduction des
shaders, la présentation, l'oracle de correction, les leviers de performance *dans
l'ordre qui a payé*, calibration d'effort et liste des pièges — est dans
**[docs/D3D9-PORTING-GUIDE.md](docs/D3D9-PORTING-GUIDE.md)** (EN). En résumé :

1. Hachez et extrayez votre propre copie ; élaguez en A/B pour trouver le jeu de données minimal.
2. Ghidra headless : carte de boot + la table d'imports comme contrat d'API.
3. Liftez avec pcrecomp (modèle CPU-struct) ; appliquez les patches génériques publiés.
4. Socle hôte : fibres, arène invitée, IAT tracé — **refuser, ne jamais mentir**.
5. Device D3D9 en trois étages ; prouvez le plumbing avec un triangle dégénéré d'abord.
6. Traduisez le cache de shaders du jeu hors-ligne (décodeur + traducteur ici) ;
   validez chaque SPIR-V ; hachez le bytecode en FNV partout.
7. Présentez headless → banc natif → appareil ; discipline SUBOPTIMAL ; deux frames en vol.
8. Difftestez chaque fonction contre un émulateur de référence ; empreintes de frames ; A/B/A.
9. Optimisez dans l'ordre mesuré : lift -O2, dispatch O(1), plus de TLS, fences, anneau, locaux.

## Ce qu'il y a dans ce dépôt

| Chemin | Ce que c'est |
|---|---|
| `runtime/` | le **backend D3D9-sur-Vulkan et le socle hôte** (œuvre originale) : device COM factice dans l'arène invitée, backend Vulkan sans état invité (instanciation, stencil, libération différée des ressources), présentation, FMOD Ex logiciel, vidéo Theora/AMediaCodec, sauvegardes et documents, préparation de l'escarmouche hors ligne, format des nombres d'ICU, module d'entrées du banc ; `runtime/tests/` = ses tests hôtes |
| `tests-hote/` | la **porte rapide** : un projet CMake indépendant qui compile et lance 33 tests hôtes **sans le code lifté** |
| `mac/` | l'**hôte macOS** : fenêtre et cycle de vie Cocoa, surface Metal par MoltenVK, CoreAudio, clavier/souris, rejeu d'événements, menus du lanceur, configuration et contrôle de l'installation |
| `android/` | le portage : glue NativeActivity, fibres bionic, présentateur swapchain, gestes et barre de raccourcis, lanceur en écran titre (Java), chaîne APK reproductible, bancs de validation |
| `scripts/` | bancs et mesure : banc natif, campagne et marathon, **scénarios événementiels** (`scripts/entrees/`), **non-régression** (`nonreg.sh`), A/B (`banc-ab.sh`, `fold-ab*`), séances sur le téléphone, préréglages graphiques, transforms du lift |
| `tools/difftest/` | l'**oracle par fonction** : fonction liftée vs référence matérielle Unicorn, chaque registre/drapeau/octet comparé |
| `tools/d3d9-shaders/` | **bytecode SM2/3 → SPIR-V** : décodeur, traducteur, extracteur de cache de shaders, métrique de frame |
| `tools/pcrecomp-*/` | patches génériques du lifter : atomiques, MMX, `movhps`/`movhpd`, `movhlps`/`movlhps`, mémoire disasm32 |
| `tools/lift/` | closure incrémentale, élimination des drapeaux morts, inventaire MMX, garde de relift |
| `tools/mpq/`, `tools/audio/` | extraction/repack MPQ à taille vérifiée ; décodeur d'imports MSVC décorés ; vérificateur de preuve WAV |
| `interfaces/` | faits d'interface générés (géométrie PE, IAT 755 slots) + compteurs du lift |

Scripts et tests gardent par endroits les chemins de l'arbre de travail privé (`recomp-sc2/runtime` y est `runtime/`
ici, `recomp-sc2/tests` est `runtime/tests/`) ; tout ce qu'ils lisent du jeu vient de votre installation.

## Documentation

| Doc | Contenu |
|---|---|
| [docs/D3D9-PORTING-GUIDE.md](docs/D3D9-PORTING-GUIDE.md) 🇬🇧 | **la recette pour porter votre propre jeu D3D9** |
| [docs/RENDU-D3D9-VERS-VULKAN.md](docs/RENDU-D3D9-VERS-VULKAN.md) | la méthode complète de traduction D3D9→Vulkan (modèle d'état, descripteurs, textures, shaders, ombres, présentation, transferts, 15 décisions contre-intuitives) |
| [docs/RETROSPECTIVE-RECOMPILATION.md](docs/RETROSPECTIVE-RECOMPILATION.md) | la rétrospective d'ingénierie en 7 jours : jalons, 14 difficultés majeures, discipline de mesure, impasses mesurées |
| [docs/METHOD.md](docs/METHOD.md) 🇬🇧 | le pipeline de bout en bout en une page |
| [docs/BANC-SCENARIOS.md](docs/BANC-SCENARIOS.md) | **scénarios de banc événementiels et non-régression à deux niveaux** (fumée ≈ 14 min, complet ≈ 2 h) : format, déclencheurs, critères, comment le juge est lui-même testé |
| [docs/MILESTONES.md](docs/MILESTONES.md) | le journal daté complet des jalons |
| [docs/BRICKS.md](docs/BRICKS.md) 🇬🇧 | briques de build : cross-compilation Theora, transcodage vidéo local |
| [docs/PACK-SPIRV.md](docs/PACK-SPIRV.md) | le format du pack SPIR-V |
| [docs/BANC-MAC.md](docs/BANC-MAC.md) | la méthode du banc natif (et l'app macOS) |
| [docs/VARIABLES.md](docs/VARIABLES.md) | toutes les variables d'environnement `SC2_*` lues par le code (contrôlées par `scripts/variables-sc2.sh`) |
| [docs/CAPS.md](docs/CAPS.md) | les caps D3D9 annoncées par le device factice, les profils GPU, et les compteurs d'états qui ont décidé du travail de rendu |

## Veille des précédents

Nous suivons le sort des projets comparables à celui-ci. L'analogue le plus proche —
une recompilation statique d'un shooter de 2010 ([bo1z](http://vel.gg/bo1z)) — a été
retiré (**HTTP 451**) en octobre 2026 : il distribuait du code et des binaires dérivés
du jeu. Ce projet suit la politique inverse : méthode et outillage original seulement,
**tout ce qui dérive du jeu reste sur la machine de l'utilisateur**
([LEGAL.md](LEGAL.md)). Observer l'écosystème fait partie de la méthode — si ses règles
changent, cette politique changera avec elles.

## Licence et aspect légal

MIT pour l'outillage de ce dépôt — travail original, aucun contenu Blizzard. Bâti sur
[pcrecomp](https://github.com/sp00nznet/pcrecomp) (MIT) ; voir [LEGAL.md](LEGAL.md)
pour la politique complète (ce qui est publié, ce qui ne le sera jamais, crédits
tiers) et la garde CI qui l'impose.
