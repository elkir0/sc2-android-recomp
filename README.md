# StarCraft II for Android — Static Recompilation Toolkit

**FR :** Ce dépôt fournit les **outils et la méthode** pour recompiler statiquement un moteur de jeu x86-32 vers ARM64 natif (Android / Apple Silicon), en s'appuyant sur la vague de recompilation statique assistée par IA de 2026 (pcrecomp). La démonstration cible est StarCraft II (Wings of Liberty, 2010) sur Android — mais **ce dépôt ne contient AUCUN code du jeu ni aucun asset**.

**EN:** This repository provides the **tooling and methodology** to statically recompile an x86-32 game engine to native ARM64 (Android / Apple Silicon), building on the 2026 AI-assisted static recompilation wave (pcrecomp). The showcase target is StarCraft II (Wings of Liberty, 2010) on Android — but **this repository contains NO game code and NO assets**.

## ⚖️ Legal / Aspect légal — read this first / lisez ceci d'abord

- **You MUST own a legitimate copy of the game. / Vous DEVEZ posséder une copie légitime du jeu.**
- All derived code (the lifted C, the recompiled runtime, extracted data) is **generated locally on your machine, from your own copy**. Nothing derived is distributed here. / Tout code dérivé est **généré localement sur votre machine, depuis votre propre copie**. Rien de dérivé n'est distribué ici.
- No assets, no binaries, no data files from the game will ever be committed. A CI scanner enforces this on every commit. / Aucun asset, binaire ou fichier de données du jeu ne sera jamais commité. Un scanner CI l'impose à chaque commit.
- Offline / single-player only. This project does not touch online services, matchmaking or any protection of access. / Hors-ligne / solo uniquement. Ce projet ne touche ni aux services en ligne, ni au matchmaking, ni à aucune protection d'accès.
- StarCraft® and Blizzard Entertainment® are trademarks of Blizzard Entertainment, Inc. This is an independent, non-commercial, personal-use interoperability project. Not affiliated, not endorsed. / StarCraft® et Blizzard Entertainment® sont des marques de Blizzard Entertainment, Inc. Projet indépendant, non commercial, d'interopérabilité à usage personnel. Sans affiliation ni aval.

See / voir [LEGAL.md](LEGAL.md).

## What's here / Ce qu'il y a ici

| Path | Description (EN/FR) |
|---|---|
| `tools/difftest/` | **Per-function oracle**: runs any lifted function with an injected CPU/memory state on your host, replays the original bytes in Unicorn as the hardware reference, and diffs every register, flag and written byte. / **Oracle par fonction** : exécute toute fonction liftée avec un état CPU/mémoire injecté, rejoue les octets originaux dans Unicorn comme référence matérielle, et compare chaque registre, flag et octet écrit. |
| `docs/METHOD.md` | The end-to-end methodology: PE analysis → lift (CPU-struct model) → host runtime → headless Vulkan backend → per-function differential testing. / La méthodologie de bout en bout. |
| `.github/workflows/legal-scan.yml` | CI guard: rejects any commit containing binaries, assets or game-derived blobs. / Garde CI : rejette tout commit contenant binaires, assets ou blobs dérivés du jeu. |

## Status / État (2026-10)

- [x] Full ISO inventory & extraction pipeline (over your own copy) / inventaire ISO complet
- [x] 86,650 functions lifted to portable C (CPU-struct reentrant model) / fonctions liftées en C portable
- [x] Boot to the game's main loop on x86-64 and ARM64 (qemu + Apple Silicon native) / boot jusqu'à la boucle principale
- [x] Game data mounted (MPQ/StormLib), 194 real pixel shaders reachable / données du jeu montées
- [x] First real pixels: D3D9→Vulkan headless backend (Lavapipe) / premiers pixels réels
- [x] Per-function oracle: 42,000+ functions swept, 1,300+ certified byte-exact / oracle par fonction
- [ ] Shaders SM2/3 → SPIR-V translation (in progress) / traduction des shaders
- [ ] Android APK chain (NDK r28, NativeActivity) / chaîne APK Android
- [ ] Playable on a Galaxy Z Fold 8 / jouable sur Galaxy Z Fold 8

## License

MIT for the tooling in this repository. The tooling is original work; it contains no content owned by Blizzard Entertainment. / MIT pour l'outillage de ce dépôt. L'outillage est un travail original ; il ne contient aucun contenu appartenant à Blizzard Entertainment.
