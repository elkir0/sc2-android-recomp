# Banc natif macOS (MacBook M4, MoltenVK)

Mis en place le 19/10/2026 (branche `claude/20261019-registres-locaux`). Le MacBook exécute `recomp-sc2` en natif
(arm64, Apple clang, Vulkan via MoltenVK), sans le serveur Linux. C'est le banc le plus proche du Fold (même architecture,
clang, 31 registres) et le plus rapide : un build complet du lift en -O2 prend ≈ 6 min, une mission de 2 500 frames
≈ 90 s (≈ 33 images/s), contre 30 min et 12 min sur le serveur Linux.

## Prérequis

| élément | où | note |
|---|---|---|
| données du jeu | `~/sc2-partout/extraction` (9,9 Go, hors git) | copie complète du serveur Linux ; `Battle.net.MPQ` et `.mpq` (liens durs sur le serveur Linux) n'y font qu'un, le volume étant insensible à la casse |
| Vulkan | `brew install vulkan-headers molten-vk` ; `brew pin molten-vk` (1.4.2) | le runtime charge `/opt/homebrew/lib/libMoltenVK.dylib` directement, sans loader ; l'épinglage fige les empreintes |
| StormLib | `~/tools/StormLib` | |
| libogg 1.3.5, libtheora 1.1.1 | `~/tools/theora/libogg-1.3.5`, `libtheora-1.1.1` (sources, copiées du serveur Linux) | lecteur vidéo du serveur Linux (`sc2_video.c`) ; absentes, le build se fait sans |
| lift et table SPIR-V | `recomp-sc2/src/lifted/recomp_*.c`, `src/shaders/sc2_shaders_spv.h` (générés, hors git) | dans un worktree : liens **durs** (la garde du lift ignore les liens symboliques) ; sans la table SPIR-V, le runtime compile sans shaders (écran noir, 0 dessin réel) |
| polices | `android/assets/polices` (Liberation, dans le dépôt) | `SC2_POLICES` ; le serveur Linux les prend dans `/usr/share/fonts` |

## Construire et lancer

```sh
cmake -S recomp-sc2 -B recomp-sc2/build-mac-O2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      "-DSC2_LIFTED_OPT=-O2;-fno-strict-aliasing;-fwrapv;-g0;-fno-stack-protector"   # + -DSC2_REGISTRES_LOCAUX=ON…
ninja -C recomp-sc2/build-mac-O2 recomp-sc2
scripts/banc-mac.sh defaut  recomp-sc2/build-mac-O2/recomp-sc2 /tmp/mac-defaut            # 600 frames, empreintes
scripts/banc-mac.sh mission recomp-sc2/build-mac-O2/recomp-sc2 /tmp/mac-mission 2500 40 40 # TRaynor01 + profil
```

Le profil de mission vient de `sample` (échantillonneur de macOS, 1 ms, tous les fils), analysé par
`scripts/profil/banc-mac-profil.py` : temps propre par fonction tiré de l'arbre d'appels (pas de seuil), attentes du
noyau exclues, ms de CPU par frame sur la fenêtre (dates des PPM). `sample` suspend le processus à chaque
échantillon : les images/s de la fenêtre en pâtissent, les comparaisons A/B restent valables à méthode égale.

Progression et sauvegardes d'un run à l'autre : le banc repart d'un `sc2-c` neuf (`rm -rf` au début et à la
fin de `scripts/banc-mac.sh`), la progression de campagne et les sauvegardes seraient perdues. Poser
`SC2_DOCUMENTS` sur un dossier hors du dossier du run — `SC2_DOCUMENTS=/tmp/mac-progression
scripts/banc-mac.sh mission recomp-sc2/build-mac-O2/recomp-sc2 /tmp/mac-mission 2500 40 40` — et, au
démarrage du runtime, `Banks/` (banques XML, ex. `TCampaign.SC2Bank`) et `Saves/` de
`C:\Users\sc2\Documents\StarCraft II\` y sont reliés par liens symboliques ; un `sc2-c` préexistant est
d'abord fusionné dans le persistant (mtime : le plus récent gagne). Sans la variable, rien ne change.
C'est le même mécanisme qu'Android, où le défaut est `/sdcard/SC2Partout/StarCraft II` (passe 10t,
`recomp-sc2/runtime/sc2_documents.c`, test `test_documents`).

## Binaire unique : banc et app (V3, 22/10)

Depuis le lot M0 de la vague 3, `recomp-sc2` est **aussi** l'app macOS : un seul exécutable, au même chemin, et un seul
lien. Le mode se décide dans `main()` (`mac/sc2_mac_main.m`) :
- `SC2_FENETRE` absente ou `0` : **banc historique**. `sc2_poc_main` (le `main` de `recomp-sc2/src/main.c`, renommé par
  `mac/mac.cmake`) tourne sur le fil principal, l'`exit(code)` du runtime est conservé et rien d'AppKit n'est appelé.
  Seule différence avec le binaire d'avant : chaque Present traverse le crochet de la couche plate-forme d'Android
  (`android/jni/sc2_android_hooks.c`, compilée en place), qui ne fait que mesurer et passer la main tant que la swapchain
  est inactive. Tous les scripts de banc et le canon restent inchangés.
- `SC2_FENETRE=1` : **app** (hôte Cocoa du lot M7 ; d'ici là, `[MAC] mode fenêtre absent de ce binaire`, code 3).
  `LSEnvironment` de l'Info.plist la pose pour tout lancement par le Finder ou par `open`.

Ce qui est lié en plus sur Mac (`mac/mac.cmake`) : la couche plate-forme, le présentateur `android/jni/sc2_vk_presente.c`
et la mesure `sc2_trace_present.c`, en place ; un second objet de `runtime/sc2_d3d9v.c` dont quatre noms sont renommés
par définitions de cible (`d3dv_present`, `d3dv_cible`, `dlsym`, `dladdr` : les `--wrap` d'Android, que ld64 n'a pas) ;
l'hôte `mac/*.c` et `mac/*.m` (GLOB : un lot ajoute un fichier, pas une ligne) ; AppKit, QuartzCore, Metal, CoreGraphics,
AudioToolbox, CoreAudio. Les objets du lift et le cache `SC2_LIFTED_PREBUILT` ne changent pas. `-DSC2_APP_MAC=OFF`
redonne le binaire du banc d'avant la V3 (sans hôte ni crochet).

Au banc, deux options de l'hôte : `SC2_SWAPCHAIN=1` pose une surface sans écran à la taille de `SC2_ECRAN` (présentateur
de M5) ; `SC2_TRACE_PRESENT=1` donne les lignes `[PRESENT]` et leur bilan. Le son ne démarre qu'avec `SC2_MAC_SON=1` ou
`muet` (lot M6).

Paquet de développement :

```sh
ninja -C recomp-sc2/build-mac-O2radx recomp-sc2 sc2partout-app      # cible hors de ALL, sans lsregister
cmp recomp-sc2/build-mac-O2radx/recomp-sc2 recomp-sc2/build-mac-O2radx/SC2Partout.app/Contents/MacOS/recomp-sc2
```

`SC2Partout.app` est assemblé autour d'un **clone APFS** (`cp -c`) du binaire : même contenu, aucun second lien.
`Contents/Resources` pointe vers les polices et `inits-stricts.txt` du dépôt ; `Contents/Frameworks` reste vide (MoltenVK
de Homebrew). Une exécution directe de `SC2Partout.app/Contents/MacOS/recomp-sc2` reste le banc (`LSEnvironment` ne vaut
que pour LaunchServices) : le canon y donne les mêmes empreintes. Le paquet distribuable (MoltenVK embarqué, signature)
est l'affaire de `scripts/paquet-mac.sh` (lot M9), jamais dans git.

Paquet distribuable (lot M9) :

```sh
scripts/paquet-mac.sh recomp-sc2/build-mac-O2radx --dest ~/code/sc2-apk/mac-essai   # refus si hors ~/code/sc2-apk/
codesign --verify --deep --strict ~/code/sc2-apk/mac-essai/SC2Partout.app
plutil -lint ~/code/sc2-apk/mac-essai/SC2Partout.app/Contents/Info.plist
```

Le paquet est PRIVÉ (code recompilé du jeu) : jamais dans le dépôt, jamais hors de `~/code/sc2-apk/`. Refus nommés
(code ≠ 0) : 65 destination hors `~/code/sc2-apk/` (après normalisation des liens), 66 lift `-O0`
(`options.SC2_LIFTED_OPT` du manifeste de `SC2_LIFTED_PREBUILT`), 67 `sc2_shaders_spv.h` absent (SPIR-V non compilé),
68 sha256 de MoltenVK ≠ la référence (sauf `--mvk <fichier> <sha256>` pour une autre version assumée), 69 autre.
Assemblage : copies RÉELLES de l'exécutable (clone APFS), des 6 polices Liberation et de `inits-stricts.txt`, plus
`Contents/Frameworks/libMoltenVK.dylib` (realpath Homebrew, sha contrôlé avant). Signature ad hoc
`codesign --force --deep -s -` (décision D8 : sans identité développeur ni notarisation — pour les Mac du mainteneur),
puis `--verify --deep --strict` et `plutil -lint`. `SC2Partout.app.sha256`, fichier VOISIN du paquet, donne les
sha256 de l'exécutable et de la dylib **calculés après la signature** : `codesign --force` re-signe l'exécutable,
une somme d'avant serait périmée (c'est pourquoi il n'y a plus de somme DANS le paquet, et pourquoi `cmp` du binaire
du banc avec celui du paquet diffère toujours sur les octets de signature). `shasum -a 256 -c
SC2Partout.app.sha256` depuis le dossier copié en USB vérifie la copie ; `xattr -dr com.apple.quarantine
SC2Partout.app` après une copie par Finder/USB (la quarantaine casse le chargement des frameworks).

## Empreintes de référence (run « défaut », 600 frames)

## Empreintes de référence (run « défaut », 600 frames)

| frame | serveur Linux (Lavapipe) | Mac (MoltenVK) |
|---|---|---|
| 1 | `be01318` | `be01318` |
| 10 | `d3cd40` ou `2ef6c21d` (course connue) | `470ebae2` (canon) ou `b5f7db71` (variante plus rapide) : même écran de chargement, à un autre stade |
| 100-300 | `ae1ff947` | `ae1ff947` |

## Ce qu'il a fallu corriger dans le runtime (jamais exécuté nativement depuis la 7u)

1. **Zone invitée alignée sur 4 Go** (`zone_hote_init`, `sc2_runtime.c`). Les stubs rendent des pointeurs hôtes par
   `GVA_OF(p) = (uint32_t)p` (`memchr`, `strchr`, `strstr`…), exact seulement si les 32 bits bas de l'adresse hôte
   sont la VA invitée, ce qu'Android garantissait déjà (`sc2_android_hooks.c`). Avec une zone prise au hasard, le
   décodeur Ogg du jeu recevait un `memchr` faux et le lanceur plantait à la 40e frame (NULL + 0xF dans
   `L_0107F9F0`). Le décodeur Ogg du jeu ne tourne ni sur Android (vidéo interceptée par MediaCodec) ni sur le serveur Linux
   avec un delta non nul : seul le Mac réunissait les deux.
2. `GetCurrentThreadId` : `pthread_threadid_np` (petit entier) au lieu de `pthread_self()` tronqué, un pointeur à bit
   de poids fort qu'un identifiant de fil Windows n'a jamais.
3. `sc2_video.c` : pas de `clock_nanosleep` sous macOS (attente relative jusqu'à la même échéance).
4. Polices : `SC2_POLICES` vers celles du dépôt (`scripts/banc-mac.sh`).
5. **Fibres** (20/10) : Metal appelle `backtrace()` à la création des files de commandes (`vkCreateDevice`) ; la chaîne
   des cadres de `_ctx_start` pointait sur le haut de la pile de la fibre, lu hors du mapping (SIGSEGV dans
   `__thread_stack_pcs`), dès que la pile hôte était assez courte : avec les appels directs. 64 octets nuls laissés
   au-dessus du sp initial (`fiber_ctx_init`, `win32_compat.c`). TRACE-PASSE10T.md §3.

Diagnostic optionnel ajouté au passage : `SC2_FS_TRI=nom|inverse` trie les énumérations de dossiers (sinon l'ordre de
`readdir` : hachage sous ext4, tri sous APFS comme NTFS).

## Mesures

`banc-mac-profil.py` ne reconnaît que l'image nommée `recomp-sc2` : pour profiler une copie renommée d'un binaire
(`recomp-sc2.ref`), renommer l'image dans `sample.txt` avant l'analyse, sinon tout tombe dans « autres bibliothèques ».

## Parcours automatique de la campagne (20/10)

`scripts/campagne-banc.sh <binaire> <sortie> [carte …]` enchaîne les 29 missions de Wings of Liberty sans joueur
(sans carte : les 29 dans l'ordre). Chaque run reprend les variables de `banc-mac.sh` + `SC2_ARGS`, `SC2_FRAMES_PAS=300`,
`SC2_SHADERS_DUMP`, et `SC2_BANC_ENTREES=@scripts/entrees/victoire-generique.txt` : aide fermée, code de triche
`whatisbestinlife` tapé dans le chat (60/120/180 s). La victoire est DÉTECTÉE par le module d'entrées — la sauvegarde
`…Victory.SC2Save` que le jeu écrit dans `$SC2_ROOT/Users/sc2/Documents/StarCraft II/Saves/Unsaved/`, avec un mtime
postérieur au démarrage du run (les victoires des runs précédents ne comptent pas ; le dossier `Saves` peut être un
lien vers un stockage persistant). Le scénario s'y coupe en deux (`QUAND:VICTOIRE`) : les éléments d'avant gardent
leurs instants absolus et les restants sont ANNULÉS à la détection — fini les frappes de secours qui gênaient le clic
sur « Continue » — puis la suite part en relatif (`+6:CLIC:998,674` … `+120:QUIT` ; action `QUIT` = `sc2_stop(0)`,
aussi utilisable en absolu comme garde-fou `300:QUIT`). Garde-fous : `300:QUIT` sans victoire, kill murale après
8 min par mission. Sortie : `<sortie>/<carte>/{run.log,resume.txt,frames/,shaders/}` (+ `planche.png`, 10 frames
tuilées, si ffmpeg est installé) et le tableau Markdown de `scripts/campagne-rapport.py` (recopié dans
`<sortie>/rapport.md`) : rc, victoire et instant, durée, frames présentées, draws réels/lumière, adresses MANQUE et
occurrences, arrêts non volontaires, shaders capturés. Tests : `tests/test_entrees_banc.c` (analyse, détection,
annulation) et `scripts/test_campagne_rapport.py`.

Voir `TRACE-PASSE10S.md` §6.5 (registres locaux : code lifté −42 % sur le Mac).

## Escarmouche (V4.10, 23/10)

Partie mêlée hors ligne contre l'IA du jeu, sur l'une des 63 cartes du cache Battle.net de l'installation
(variables `SC2_ESCARMOUCHE*` : `docs/VARIABLES.md`, « Escarmouche contre l'IA »). Le runtime prépare une copie privée
de la carte sous `$SC2_ROOT` au démarrage et le lanceur simulé la lance en mode 2 ; rien n'est écrit dans
l'installation.

```sh
B=recomp-sc2/build-mac-O2radx/recomp-sc2; X=~/sc2-partout/extraction/install
env SC2_INSTALL=$X SC2_ROOT=/tmp/esc/sc2-c SC2_ESCARMOUCHE=liste $B        # 63 cartes (n°, nom, joueurs), code 0
env SC2_INSTALL=$X SC2_ROOT=/tmp/esc/sc2-c SC2_ESCARMOUCHE=verifier $B     # les 63 préparées et relues, sans jouer
SC2_ESCARMOUCHE=SteppesofWar SC2_ESCARMOUCHE_RACE=terran SC2_ESCARMOUCHE_RACE_IA=zerg SC2_ESCARMOUCHE_IA=facile \
SC2_ESCARMOUCHE_VISION=1 scripts/banc-escarmouche.sh $B <sortie> scripts/entrees/escarmouche-5min.txt 600
```

`scripts/banc-escarmouche.sh <binaire> <sortie> <scénario> [alarme s]` prend le verrou banc, lance le jeu avec le
lanceur simulé, `SC2_FRAMES=0` (seules les `CAPTURE:<nom>` du scénario, en PNG sans perte) et
`SC2_BANC_ENTREES=@<scénario>`, puis résume dans `<sortie>/resume.txt` : lignes `[ESCARMOUCHE]`, montée de
`Maps\Escarmouche\<Nom>.SC2Map`, MANQUE, « lumière », QUIT, segments du banc d'entrées, banques écrites. Scénarios
(`scripts/entrees/`, client 1280×720, horloge depuis `QUAND:PRET`, la fin du chargement) :

| scénario | partie | ce qu'il montre |
|---|---|---|
| `escarmouche-5min.txt` | Steppes of War, `VISION=1` | captures des deux coins de la minicarte à 5 min (départs tirés au sort : l'un est la base de l'IA), QUIT |
| `escarmouche-4j.txt` | Lost Temple, 2 IA, `VISION=1` | les quatre coins à 3 min, QUIT |
| `escarmouche-longue.txt` | Steppes of War, `FIN=1` | une capture par minute de la base du joueur (inactif), puis l'écran de fin dès que la banque `EscarmoucheFin*` est écrite (`QUAND:FICHIER`), au plus 21 min |

Fin de partie : la mêlée n'écrit pas de `…Victory.SC2Save` (`QUAND:VICTOIRE` ne s'applique pas) ;
`SC2_ESCARMOUCHE_FIN=1` ajoute au script de la carte deux déclencheurs `TriggerAddEventPlayerLeft` qui écrivent
`EscarmoucheFinVictoire` / `EscarmoucheFinDefaite`, et `EscarmoucheDebut` au début, déclarées dans un `BankList.xml`
ajouté à la carte. NON OPÉRANT au 23/10 : aucune banque écrite (script compilé, partie normale), si bien que la
partie longue s'arrête sur son `MAX` (1260 s), écran de fin capturé par les captures minute. Le mode 2 est celui de
« Tester le document » de l'éditeur : la fenêtre de fin a la forme de la campagne (« Defeat! » : Load, Restart, Score
Screen).

Mesuré le 23/10 (binaire de la branche claude/20261023-escarmouche, sorties hors git) : canon inchangé ; Steppes of
War, Terran contre IA Zerg Facile : à 5 min la base de l'IA (Hatchery sur le creep, Spawning Pool, Extractor, autres
bâtiments, drones au minerai, roaches et zerglings), rc 0, 0 MANQUE, 0 « lumière », QUIT propre ; joueur inactif :
roaches et zerglings dans sa base à 9 min, « Defeat! » et « Player 2 was victorious! » à 10 min. Pièges : le premier
clic sur la minicarte après des minutes sans entrée ne déplace pas la caméra (les scénarios doublent chaque clic) ; un
« ; » dans un commentaire de scénario coupe la ligne (élément rejeté).

Lanceur Mac (lot M9) : les menus de l'app (« Partie », « Missions » — les 33 cartes de `CARTES_ARCHIVE`, copie figée
dans `mac/sc2_catalogue_mac.c` —, « Réglages ») écrivent le même `sc2.env` que le lanceur Android — `SC2_ESCARMOUCHE=<nom>`,
`SC2_ESCARMOUCHE_RACE`, `SC2_ESCARMOUCHE_RACE_IA`, `SC2_ESCARMOUCHE_IA` (et `SC2_RUN_SANS_BNET=1`, déjà le défaut
du Mac), sans `SC2_ARGS` pour l'escarmouche (un `-run` l'emporterait) ; une mission de campagne écrit
`SC2_ARGS=-run Maps/Campaign/<carte>.SC2Map`. Chaque choix fusionne dans le `sc2.env` du support (les autres clés de
l'utilisateur restent) puis relance l'app par `NSWorkspace` : l'ancienne instance sort proprement (`sc2m_quitter`), la
nouvelle attend au verrou d'instance et journalise `[MAC] instance précédente terminée après x s`. Les clés de lancement
(`SC2_ARGS`, `SC2_ESCARMOUCHE*`, `SC2_SAUVEGARDE`, le filtre `SC2_MAC_LANCEUR_CHOIX`) ne sont jamais héritées par
l'instance relancée. Essai sans main : `SC2_MAC_LANCEUR_CHOIX=menu|carte:<nom>|escarmouche:<carte>:<race>:<raceIA>:<diff>`
joué après `SC2_MAC_LANCEUR_DELAI_S` s (défaut 5).
