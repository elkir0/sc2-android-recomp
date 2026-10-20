# StarCraft II sur Android — la recompilation statique outillée

**[English version 🇬🇧](README.md)**

[![legal-scan](https://github.com/elkir0/sc2-android-recomp/actions/workflows/legal-scan.yml/badge.svg)](https://github.com/elkir0/sc2-android-recomp/actions/workflows/legal-scan.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Aucun contenu du jeu](https://img.shields.io/badge/contains-no%20game%20code%20or%20assets-green)

**D'un exécutable Windows x86-32 de 15,7 Mo à un APK Android signé et jouable avec un
renderer Vulkan natif — par recompilation statique assistée par IA.** Une semaine de
travail a mené StarCraft II: Wings of Liberty (2010) de « ça boote » à « la première
mission de campagne se joue de bout en bout à 28 FPS sur téléphone ». Ce dépôt est la
méthode et la chaîne d'outils complètes, validées de bout en bout, pour faire pareil
avec **votre** jeu de l'ère D3D9.

> **Il ne contient aucun code ni aucun asset du jeu — et n'en contiendra jamais.**
> Tout ce qui dérive du jeu est généré localement, depuis votre propre copie
> légitime, par les outils publiés ici. Un scanner CI l'impose à chaque commit.
> Hors-ligne / solo uniquement. StarCraft® et Blizzard Entertainment® sont des marques
> de Blizzard Entertainment, Inc. Projet indépendant, non commercial, sans affiliation.
> Détails : [LEGAL.md](LEGAL.md).

## Est-ce que ça marche ? (octobre 2026)

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
| Première mission de campagne | **jouée en entier sur un Galaxy Z Fold 8** (clavier+souris BT, HUD vivant, victoire) |
| Performance en jeu actif | **28,1 images/s** (4,1 au premier build jouable — ×6,9), GPU jamais limitant |
| Audio | bande-son + voix de mission reconstruites **bit-exact** |
| Vidéo | cinématique d'intro décodée **en temps réel sur l'appareil** (le décodeur Theora du jeu, lifté) |
| Shaders traduits | 2 856 sur les 75 797 du cache du jeu (croît — voir la feuille de route) |

## État du projet

| Sous-système | État | Notes |
|---|---|---|
| Recompilation statique (x86-32 → C portable) | ✅ fait | modèle CPU-struct réentrant ; patches génériques du lifter publiés (atomiques, MMX, voies SSE) |
| Renderer Vulkan (D3D9 → Vulkan) | ✅ cœur complet | écran-titre, menus, scènes 3D, terrain, HUD, minicarte, unités ; ombres opt-in ; MRT et sRGB restants |
| Audio (FMOD Ex → mixeur logiciel → AAudio) | ✅ | reconstruction bit-exact, preuve par entropie WAV |
| Vidéo (Theora) | ✅ | temps réel sur appareil ; voie H.264 matérielle optionnelle |
| Entrées | ✅ | tactile → souris Win32 ; clavier/souris BT ; DeX |
| Empaquetage Android | ✅ | APK signé reproductible (NDK r28, NativeActivity) |
| Performance | 🟡 en cours | 28,1 FPS, limitée par le code lifté (~19 ms d'une frame de ~35 ms) |
| Couverture shaders | 🟡 en cours | 2 856 / 75 797 ; générateur et chargeur du pack complet en chantier |
| Progression de campagne | 🔴 suivant | sauvegardes effacées au lancement ; la persistance est le prochain chantier |
| Campagne au-delà de la mission 1 | 🟡 | la mission 2 charge ; parcours automatique de toutes les missions planifié |

Le journal daté complet (35+ jalons, avec la mesure derrière chaque affirmation) :
[docs/MILESTONES.md](docs/MILESTONES.md).

## Feuille de route

| # | Jalon | Pourquoi c'est important |
|---|---|---|
| 1 | **Pack SPIR-V complet** — traduire les 75 797 shaders du cache en un pack mmap (~2 Go, format dans [docs/PACK-SPIRV.md](docs/PACK-SPIRV.md)) | toute scène inédite perd des dessins (cibles de rendu périmées) ; ça clôt la couverture shaders pour de bon |
| 2 | **Persistance des sauvegardes et de la progression** | le `C:\` émulé est effacé au lancement — la progression ne survit pas à un redémarrage |
| 3 | **Parcours automatique de la campagne** — chaque mission jouée au banc par code de triche | transforme « est-ce que la mission N marche ? » en tableau de fonctions manquantes, par mission |
| 4 | **Vraie entrée par le menu de campagne** (sans `-run` forcé) | le parcours normal du joueur, plein écran |
| 5 | **Leviers CPU restants** — recopie par plage des blocs de constantes (seulement ce que lit le shader), SSE en locaux | les derniers ~6 Mo/frame de `memmove` et le chemin vers 60 FPS |

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
| `runtime/` | le **backend D3D9-sur-Vulkan et le socle hôte** (œuvre originale) : device COM factice dans l'arène invitée, backend Vulkan sans état invité, présentation, FMOD Ex logiciel, vidéo Theora/AMediaCodec |
| `tools/difftest/` | l'**oracle par fonction** : fonction liftée vs référence matérielle Unicorn, chaque registre/drapeau/octet comparé |
| `tools/d3d9-shaders/` | **bytecode SM2/3 → SPIR-V** : décodeur, traducteur, extracteur de cache de shaders, métrique de frame |
| `tools/pcrecomp-*/` | patches génériques du lifter : atomiques, MMX, `movhps`/`movhpd`, `movhlps`/`movlhps`, mémoire disasm32 |
| `tools/lift/` | closure incrémentale, élimination des drapeaux morts, inventaire MMX, garde de relift |
| `tools/mpq/`, `tools/audio/` | extraction/repack MPQ à taille vérifiée ; décodeur d'imports MSVC décorés ; vérificateur de preuve WAV |
| `scripts/` | mesure et optimisation : banc natif (`banc-mac.sh`), A/B/A sur appareil (`fold-ab*`), banc cinématique, transforms du lift (locaux, appels directs) |
| `android/` | le portage : glue NativeActivity, fibres bionic, présentateur swapchain, chaîne APK reproductible, bancs de validation |
| `interfaces/` | faits d'interface générés (géométrie PE, IAT 755 slots) + compteurs du lift |

## Documentation

| Doc | Contenu |
|---|---|
| [docs/D3D9-PORTING-GUIDE.md](docs/D3D9-PORTING-GUIDE.md) 🇬🇧 | **la recette pour porter votre propre jeu D3D9** |
| [docs/RENDU-D3D9-VERS-VULKAN.md](docs/RENDU-D3D9-VERS-VULKAN.md) | la méthode complète de traduction D3D9→Vulkan (modèle d'état, descripteurs, textures, shaders, ombres, présentation, transferts, 15 décisions contre-intuitives) |
| [docs/RETROSPECTIVE-RECOMPILATION.md](docs/RETROSPECTIVE-RECOMPILATION.md) | la rétrospective d'ingénierie en 7 jours : jalons, 14 difficultés majeures, discipline de mesure, impasses mesurées |
| [docs/METHOD.md](docs/METHOD.md) 🇬🇧 | le pipeline de bout en bout en une page |
| [docs/MILESTONES.md](docs/MILESTONES.md) | le journal daté complet des jalons |
| [docs/BRICKS.md](docs/BRICKS.md) 🇬🇧 | briques de build : cross-compilation Theora, transcodage vidéo local |
| [docs/PACK-SPIRV.md](docs/PACK-SPIRV.md) | le format du pack SPIR-V |
| [docs/BANC-MAC.md](docs/BANC-MAC.md) | la méthode du banc natif |

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
