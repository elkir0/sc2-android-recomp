# Banc natif macOS (MacBook M4, MoltenVK)

Mis en place le 19/10/2026 (branche `claude/20261019-registres-locaux`). Le MacBook exécute `recomp-sc2` en natif
(arm64, Apple clang, Vulkan via MoltenVK), sans la devbox. C'est le banc le plus proche du Fold (même architecture,
clang, 31 registres) et le plus rapide : un build complet du lift en -O2 prend ≈ 6 min, une mission de 2 500 frames
≈ 90 s (≈ 33 images/s), contre 30 min et 12 min sur la devbox.

## Prérequis

| élément | où | note |
|---|---|---|
| données du jeu | `~/sc2-partout/extraction` (9,9 Go, hors git) | copie complète de la devbox ; `Battle.net.MPQ` et `.mpq` (liens durs sur la devbox) n'y font qu'un, le volume étant insensible à la casse |
| Vulkan | `brew install vulkan-headers molten-vk` ; `brew pin molten-vk` (1.4.2) | le runtime charge `/opt/homebrew/lib/libMoltenVK.dylib` directement, sans loader ; l'épinglage fige les empreintes |
| StormLib | `~/tools/StormLib` | |
| libogg 1.3.5, libtheora 1.1.1 | `~/tools/theora/libogg-1.3.5`, `libtheora-1.1.1` (sources, copiées de la devbox) | lecteur vidéo de la devbox (`sc2_video.c`) ; absentes, le build se fait sans |
| lift et table SPIR-V | `recomp-sc2/src/lifted/recomp_*.c`, `src/shaders/sc2_shaders_spv.h` (générés, hors git) | dans un worktree : liens **durs** (la garde du lift ignore les liens symboliques) ; sans la table SPIR-V, le runtime compile sans shaders (écran noir, 0 dessin réel) |
| polices | `android/assets/polices` (Liberation, dans le dépôt) | `SC2_POLICES` ; la devbox les prend dans `/usr/share/fonts` |

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

## Empreintes de référence (run « défaut », 600 frames)

| frame | devbox (Lavapipe) | Mac (MoltenVK) |
|---|---|---|
| 1 | `be01318` | `be01318` |
| 10 | `d3cd40` ou `2ef6c21d` (course connue) | `470ebae2` (canon) ou `b5f7db71` (variante plus rapide) : même écran de chargement, à un autre stade |
| 100-300 | `ae1ff947` | `ae1ff947` |

## Ce qu'il a fallu corriger dans le runtime (jamais exécuté nativement depuis la 7u)

1. **Zone invitée alignée sur 4 Go** (`zone_hote_init`, `sc2_runtime.c`). Les stubs rendent des pointeurs hôtes par
   `GVA_OF(p) = (uint32_t)p` (`memchr`, `strchr`, `strstr`…), exact seulement si les 32 bits bas de l'adresse hôte
   sont la VA invitée, ce qu'Android garantissait déjà (`sc2_android_hooks.c`). Avec une zone prise au hasard, le
   décodeur Ogg du jeu recevait un `memchr` faux et le lanceur plantait à la 40e frame (NULL + 0xF dans
   `L_0107F9F0`). Le décodeur Ogg du jeu ne tourne ni sur Android (vidéo interceptée par MediaCodec) ni sur la devbox
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


Voir `TRACE-PASSE10S.md` §6.5 (registres locaux : code lifté −42 % sur le Mac).
