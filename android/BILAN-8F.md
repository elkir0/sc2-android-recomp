# BILAN 8F — APK de référence main8d (relift 7w8 + présentation 8d)

## Artefact

| | |
|---|---|
| fichier | `artifacts/sc2-partout-20261016-1805-main8d.apk` (hors git) |
| sha256 | `c0a9672a9c30f14229921a9ba9f7211e626f19e53240d9b57cee8f9f229c5e1f` |
| taille | 46 208 073 octets (45 Mo) |
| variante | `SC2_APK_SWAPCHAIN=1` (présentation par swapchain, recette 7r2), signée debug |
| construit | 2026-10-16, 17:36:02Z → 17:55:43Z, `JOBS=2`, `nice -n 10`, `ulimit -v 8000000` sur le build seul |

## Provenance git exacte

- Worktree NEUF `.claude/worktrees/sc2-passe8f`, branche `hermes/20261016-passe8f-apk-8d`, depuis main **0066641**. Ce
  main contient la fusion 8d **bd0c0f8**, la fusion 7w8 0d54d84 et la fusion 7p3 613fa2d.
- Arbre construit = **b80c0a9** : main 0066641 plus la garde de lift sur la lib dans `scripts/build-apk.sh`. Seul ce
  script diffère de main, et le build a tourné avec ce contenu exact.
- Entrées générées, hors git, copiées du checkout principal sans rien modifier de suivi sous `recomp-sc2/` :
  - `recomp-sc2/src/lifted/` (`rsync -a`, sans `--delete`) : 132 fichiers `recomp_*.c`, relift post-7w8 du 16/10 avec
    30 147 `SC2_SAUT` dans les sources ;
  - `recomp-sc2/src/shaders/sc2_shaders_spv.h` : table SPIR-V `be59b5c1d41fb4d6…`, la même que l'APK main 7w8p3 de
    13:14Z. Sans elle, le build ne dessine que la « lumière ».

## Contrôles

- `scripts/valider-apk.sh` : **PASS (10 contrôles)** — paquet `fr.sc2partout`, minSdk 26, native-code arm64-v8a,
  NativeActivity, lib présente, ELF AArch64, `ANativeActivity_onCreate` et `android_main` exportés, signature vérifiée,
  zipalign. Résultat : BUILD_RC=0, VALIDER_RC=0.
- Garde de lift, sources (2629625) : `SC2_SAUT` présent dans 132 fichiers.
- Garde de lift, **lib (nouvelle, b80c0a9)** : les racines 7j/7q `L_010CAB80` et `L_0129DC30` sont définies dans
  `libSC2Partout.so`, qui compte **86 653 fonctions liftées** (= le relift de référence).

## `libSC2Partout.so`

| | taille | sha256 (16) |
|---|---|---|
| non strippée (`android/build-vk/`) | 272 922 896 o | `8d7eee6807af1806` |
| dans l'APK (strip `--strip-unneeded`) | 175 219 072 o | `35ce7c87e039d631` |

`grep -c SC2_SAUT` sur la lib donne **2**. Ce compte **ne prouve rien sur le lift** : `SC2_SAUT` est une macro
(`sc2_lifted.h`), donc la lib n'en garde que les deux chaînes de journal du runtime (`[LIFT]` et `[BILAN] [PILE]`),
présentes quel que soit le lift. D'où la garde de la lib sur les symboles des racines 7j/7q, plus un test négatif de sa
logique : un jeu de symboles sans `L_0129DC30` est refusé.

## Non fait (hors zone)

Aucun adb, aucun émulateur (le banc M4 est réservé à la supervision), aucun push. Rien de modifié sous `recomp-sc2/src/*`
dans git.
