# Rétrospective — sept jours pour recompiler StarCraft II vers Android

*Du 14 au 20 octobre 2026 : recompilation statique assistée par IA d'un moteur x86-32
de 2010 (15,7 Mo, 86 653 fonctions découvertes) vers ARM64 natif + Vulkan, jusqu'à une
mission de campagne jouée en entier sur un Galaxy Z Fold 8. Ce document est le récit de
méthode : jalons, difficultés, façon de mesurer, impasses. Il complète
[RENDU-D3D9-VERS-VULKAN.md](RENDU-D3D9-VERS-VULKAN.md) (le détail du renderer) et
[METHOD.md](METHOD.md) (le pipeline de bout en bout). Tout est advenu sur notre propre
copie légitime du jeu, hors-ligne, sans distribution d'aucun dérivé.*

## 1. Le résultat, d'abord

| Au 20/10 au soir | État |
|---|---|
| Mission 1 de campagne (TRaynor01) | **jouée en entier jusqu'à la victoire**, clavier+souris BT, HUD complet, cinématique du bar propre après fix shaders |
| FPS en jeu actif (Fold 8) | **28,1** (premier build jouable : 4,1 — ×6,9) ; GPU jamais limitant |
| Vidéo d'intro | décodée **en temps réel sur le téléphone** par la libtheora *liftée* (24,9 img/s pour 24) |
| Audio | bande-son et voix de mission reconstruites **bit-exact** (100 % des tranches, voix au frame près) |
| Oracle de correction | **76 650 fonctions** testées contre un émulateur matériel, 0 vraie divergence |
| Reste ouvert | progression effacée à chaque lancement ; 2 856/75 797 shaders traduits (pack complet en chantier) ; blocs de constantes ≈ 6 Mo/frame de `memmove` |

## 2. Jalons (chronologie vérifiée)

### 2.1 Fonctionnalité / rendu

| Date | Passe | Débloqué | Preuve |
|---|---|---|---|
| 14/10 | 3 | le code lifté s'exécute | — |
| 14/10 | 4 | contrôle « lanceur » passé, boucle de rendu x86-64 | — |
| 14/10 | 5c | premiers pixels (Clear) | 194 `CreatePixelShader`, 30 VS |
| 15/10 | 5d/7a | écran-titre + chaîne APK | — |
| 15/10 | 5e | audio (FMOD logiciel) | — |
| 15/10 | 7b | premier test Android (émulateur M4) | — |
| 15/10 | 7f | **écran-titre affiché par Vulkan** | 1 création de swapchain (avant : 4 809) |
| 15/10 | 7h | chemin direct vers la mission 1 | mission chargée |
| 15/10 | 7l | texte UI + textures réelles | atlas de glyphes lisible |
| 15/10 | 7n | **scène 3D du menu** | captures animées f150/f300 |
| 15/10 | 7q-7u | terrain 3D, premier écran de partie | HUD complet |
| 16/10 | 7w | **minicarte + sprites d'unités** | 39 blocs distincts, A/B |
| 16/10 | 7t/7v | **ombres** (sampler de profondeur) | 300 k échantillonnages D32/frame |
| 16/10 | 7w6 | cinématique du menu décodée | 767 466 échantillons non nuls |
| 16/10 | 7p3 | mission rendue en 3D **sur Android** | 2 114 frames, 0 SIGSEGV |
| 16/10 | 9a | validé sur le vrai Fold 8 | APK 262 Mo, 0 refus Adreno |
| 17/10 | — | écran de chargement officiel sur l'écran du téléphone | — |
| 17/10 | 9n | The Lost Viking joué de bout en bout | 1er « jeu dans le jeu » complet |
| 17/10 | 9q/9s | **mission EN JEU** | relift + difftest 30/30 |
| 17-18/10 | 9w/9z | **gameplay complet** mission 2 : construction, gaz, combat | 0 MANQUE sur 4×4000 frames |
| 18/10 | 10a/10l/10n | diagnostic du mur « menu Battle.net » ; UI Scaleform minimale rendue | 262 447 draws |
| 20/10 | 10t | **vidéo en temps réel** + **cinématique du bar propre** | 18 387 → 278 draws « lumière » |

### 2.2 Performance (Fold, mission 1, jeu actif, A/B/A)

| Version | Levier principal | FPS | CPU jeu (ms/frame) |
|---|---|---:|---:|
| v8 | -O0 (constat initial) | 4,1 | 236 |
| v9 | lift compilé -O2 | 7,8 | 134 |
| v10 | dispatch O(1), PLT réduit | 9,8 | 101-106 |
| v11a | appels directs **seuls** — perd 15 % | 7,7 | 122 |
| v12f | état par CPU (fin du TLS), caches | 12,9 | 49-61 |
| v13 | **2 frames en vol** | 21,3 (scène pause) / 12,5 actif | 45,8 |
| v14 | anneau 256 Mo/emplacement | 22,5 | 44,1 |
| v15 | TLS froids `noinline`, cache pipelines persistant | 19,9-24,0 | 40,4 |
| v15rladE | registres locaux + appels directs | 25,0 | 31,8 |
| **v16** | + pile x87 locale + cache volatils | **28,1** | 34,5-36,7 |

## 3. Les difficultés qui ont coûté le plus (symptôme → mesure → cause → correctif)

1. **Écran noir de la scène 3D du menu.** L'UI rendait, la 3D non. Cause racine :
   *un caractère* — le lifter testait le mauvais caractère du mnémonique `movhps`
   (voie haute SSE traitée comme la voie basse) ; tout produit de matrices 4×4
   dégénérait (lignes a,b,a,b) et la 3D s'effondrait en noir pendant que la 2D
   marchait. Correctif d'une ligne dans le lifter + relift. Leçon : quand 2D et 3D
   divergent, soupçonner les maths matricielles du lift, pas le renderer.
2. **Écran noir sur émulateur = boucle audio.** Le moteur tournait (frames
   advances) mais les captures étaient noires et identiques. Le logcat a tranché :
   l'init audio échouait et **re-bouclait 4 495 fois**, entrelacée de 4 493
   recréations de swapchain. Lier notre FMOD logiciel → 0 itération, premiers pixels.
   Leçon : un moteur qui n'a pas son son n'a pas son écran.
3. **SUBOPTIMAL à chaque frame.** 1 412 créations de swapchain pour 1 400
   présentations : le compositeur ne latche jamais un buffer d'une swapchain déjà
   détruite. Discipline « recréer seulement si la taille change » → 1 création au
   total. (Détail dans le doc rendu §7.1.)
4. **Minicarte noire, sans erreur.** Deux causes indépendantes : la couche D3DX ne
   voyait que du DDS (la minicarte est un TGA) ; et `GetRenderTargetData` était un
   no-op — le moteur compose les sprites d'unités depuis ses cibles relues.
5. **Le verre de la cinématique du bar.** 57 % de draws sautés faute de shaders
   traduits ; une passe sautée ne redessine pas sa cible, et le verre réfractait les
   restes d'une scène précédente — *visible sur Adreno, invisible au banc Mac*
   (mémoire recyclée vs mémoire à zéro). Six hypothèses écartées une à une avant la
   cause. Correctif : capture ciblée de 355 bytecodes. Leçons : (a) tester sur le
   matériel tôt ; (b) un compteur de repli vaut dix captures.
6. **La pile x87 en mémoire.** Après les registres locaux, 51 % du temps du code
   lifté restait dans des fonctions à pile x87 lue/écrite à indice dynamique dans la
   structure CPU. Transformation « pile relative au sommet » (rotations PUSH/POP,
   masque 10 bits) : −11 % de temps lifté, accès mémoire flottants −75 % sur la
   fonction test, 0 écart en build de contrôle (50 638 contrôles).
7. **Le TLS du dispatch.** La version « appels directs » *perdait* 15 % sur le Fold
   alors qu'elle gagnait 21 % au banc x86. `simpleperf` : le résolveur TLS dynamique
   prenait 7,4 % — dans une `.so` chargée par `dlopen`, *chaque* accès `__thread`
   passe par le résolveur. Correctif en deux temps : état porté par le pointeur CPU
   (v12), chemins froids marqués `noinline, cold` (v15). Leçon : un banc d'archi
   différent ment sur les coûts mémoire.
8. **L'anneau qui débordait en milieu de frame.** Le backend semblait saturer le GPU
   (14 ms/frame). Les timestamps GPU ont acquitté le GPU (< 40 % d'occupation) et
   accusé 1,93 soumission/frame + 9,2 ms d'attente de fence : la moitié de l'anneau
   de 64 Mo débordait, forçant une soumission qui attendait la frame précédente.
   256 Mo/emplacement → 1,07 soumission, +4 FPS.
9. **Le débordement d'arène des runs longs.** Mort à 27 min : 1,8 Go de débordement.
   Journal d'arène par appelant → deux causes : fragmentation par les gros
   `VirtualAlloc` (531 Mo d'étendues libres inutilisables) et une fuite d'objets
   surface (~260/s, 320 866 blocs jamais libérés). Cache de surfaces + arène à deux
   bouts + meilleur ajustement + quarantaine → même run à 40 000 frames : 390/768 Mo,
   0 débordement, empreintes inchangées.
10. **Le faux profil « threads à 100 % dans la libc ».** Artefact d'`ITIMER_PROF`
    (un tick entier porté au thread réveillé à l'échéance). Recoupement par
    `/proc/<pid>/task/<tid>/schedstat` : 0,1-0,4 % de CPU réel. Leçon : toujours
    recouper un profileur samplé avec le compteur du noyau.
11. **La concurrence qui « tenait par accident ».** Avec les registres invités en
    variables locales, plus aucun store dans la structure CPU dans les boucles → le
    compilateur pouvait hisser des accès invités hors boucle (légal en C, faux en
    pratique) : gel à la 35ᵉ frame puis SIGSEGV. Correctif : barrière en tête de
    boucle + accès mémoire invités volatils (+0,3 % d'instructions). Leçon : un
    modèle CPU « explicite » devient *implicite* dès qu'on optimise — réintroduire
    les barrières là où le compilateur gagne des droits.
12. **Les fibres macOS vs `backtrace()` de Metal.** SIGSEGV dans le backtrace de
    création du device : Metal remonte la chaîne de cadres, qui plonge au-delà du
    haut de pile de la fibre `makecontext`. Correctif : 64 octets nuls au-dessus du
    pointeur de pile initial.
13. **Le menu principal « mort ».** Diagnostic final : le menu WoL est le dashboard
    Scaleform **du client Battle.net absent du DVD** — pas un trou du lift (0 MANQUE,
    99 089 draws réels). Contournement : chargement direct de mission (`-run`) et,
    plus tard, re-création d'un UI minimale en pilotant les propres classes UI du
    moteur. Leçon : quand tout est « vert » et que ça ne marche pas, l'explication
    peut être *du contenu absent*, pas un bug.
14. **Les mesures qui mentent.** Empreinte f10 dépendante d'une **course de threads**
    (prouvé par épinglage CPU) ; queue de frames avançant en **temps mur** ; une passe
    « négative » due à 3 artefacts de jugement. D'où le protocole ci-dessous.

## 4. Mesurer juste (le protocole, duplicable)

- **A/B/A systématique** : la référence rejouée *après* la candidate — le contenu
  d'une partie pèse ±15 ms de CPU ; runs entrelacés ≥ 3 paires ; thermique contrôlé.
- **Empreintes de frames** (hachage des pixels) comme oracle de non-régression, sur
  des frames choisies pour leur stabilité (f1, f100-f300) — jamais la queue.
- **Oracle par fonction** (difftest) : chaque fonction liftée exécutée avec un état
  aléatoire seedé, rejouée dans **Unicorn** comme référence matérielle, comparaison
  registres/drapeaux/octets écrits. **76 650 fonctions** balayées ; les 3 985 écarts
  initiaux étaient des artefacts du harnais (cadre de retour simulé), 0 vraie
  divergence après correctifs. C'est lui qui a certifié chaque patch du lifter.
- **Trois bancs, trois rôles** : devbox x86 (Lavapipe — seule la métrique CPU/frame
  compte), **banc Mac M4 natif** (même arch/compilateur que la cible ; build 6 min,
  mission 90 s), le Fold comme juge final. La méthode « banc d'abord, matériel en
  dernier » a divisé le coût d'itération par dix.
- **Sur l'appareil** : `simpleperf` au fil du jeu symbolisé côté Mac, timestamps GPU
  Vulkan par passe, compteurs structurels (soumissions/Present, attentes de fence,
  draws réels vs repli, surfaces vivantes).
- **Vérifier les passes par décomptes indépendants** (grep) et non par leurs seuls
  invariants — c'est un décompte qui a trahi un bug de la transform registres-locaux ;
  build de contrôle dynamique + injection de fautes pour la vivacité des drapeaux.

## 5. Les impasses (autant de temps gagné pour la suite)

- **Baisser la résolution de rendu : aucun effet** (960×540 = même FPS que 720p) —
  la frame était limitée CPU.
- **Appels directs seuls : perdants sur le Fold** (−15 %, +31 Mo de code, pression
  cache) — gagnants seulement *après* les registres locaux. L'ordre des
  transformations compte autant que les transformations.
- **Cache de tampons statiques sans garde : contre-productif sur Adreno** (le moteur
  régénère plus que prévu) — réactivé avec garde de volatilité.
- **Décodage vidéo matériel (NEON, H.264/MediaCodec)** : rendu inutile — la
  libtheora liftée décode en temps réel une fois le lift optimisé.
- **Vivacité interprocédurale des drapeaux** : correcte (prouvée par injection de
  fautes) mais gain dynamique non mesurable → OFF par défaut.
- **`non_volatile_as_local` de XenonRecomp** : non transposable (conventions privées
  MSVC /GL, prologues SEH lisant l'EBP de l'appelant).
- **Rétablir Battle.net.mpq seul : sans effet** (nécessaire, pas suffisant).
- **Hypothèse « encodeur BSN »** pour le chargement des films : fausse — c'était
  l'init générique d'un flux de bits ; le vrai chemin part d'une chaîne du jeu.

## 6. L'organisation (ce qui a tenu sept jours à ce rythme)

Deux sessions d'agents lourdes en parallèle maximum par machine ; un **bus de
coordination écrit** entre sessions ; un cron de supervision qui se réveille quand
*rien n'avance* (la stabilité silencieuse est un blocage) ; chaque jalon publié avec
preuve (empreinte, compteur, mesure) ; la règle « l'oracle avant le volume » ; jeter
le mauvais code plutôt que le réparer. Et une séparation nette : le superviseur
conçoit, relit ligne à ligne et mesure ; les workers écrivent dans des worktrees.

## 7. Ce qui reste (feuille de route ouverte)

1. **Pack SPIR-V complet** (75 797 bytecodes → ~2 Go mmap) : chaque motif de refus du
   traducteur est un ticket identifié.
2. **Persistance des sauvegardes** (le « C:\ » émulé est effacé au démarrage).
3. **Parcours automatique de la campagne** au banc (code de triche + entrées
   scriptées) pour cartographier les fonctions manquantes mission par mission.
4. CPU : blocs de constantes (ne recopier que la plage lue par le shader), SSE en
   local, entrée par le vrai menu de campagne.
