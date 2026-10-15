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
| `tools/d3d9-shaders/` | **D3D9 SM2/3 bytecode → SPIR-V offline translator**: decoder + translator + cache extractor + frame metric. 202/202 boot shaders validated by `spirv-val`. / **Traducteur hors-ligne bytecode D3D9 SM2/3 → SPIR-V** : décodeur, traducteur, extracteur de cache, métrique de frame. |
| `tools/android-porting/` | **Bionic porting kit**: user-space fibers (ucontext for Android's libc, arm64 asm) + configure-time `cpu.h` atomics patch for delta-arena mode. / **Kit de portage bionic** : fibres user-space (ucontext pour la libc Android, asm arm64) + patch configure-time des atomiques `cpu.h` en mode arène delta. |
| `tools/audio/` | **Audio import decoder & WAV proof checker**: `decode-msvc-audio-imports.py` decodes MSVC-decorated C++ import names (e.g. a whole FMOD Ex API) from a PE import table into a C dispatch table — stdcall stack words to pop, nature of every arg (value / double / in-pointer / out-scalar / out-object / string / callback); `wav-dump-stats.py` checks an audio dump WAV (duration, non-zero samples, peak, saturation, Shannon entropy per sample and per byte, RMS per second — a real sound is neither a flat line nor uniform noise). / **Décodeur d'imports audio & vérificateur de preuve WAV** : décode les noms d'imports C++ décorés MSVC (ex. toute une API FMOD Ex) d'une table d'imports PE en table C de dispatch — mots dépilés stdcall, nature de chaque argument ; vérifie un WAV d'audio (durée, échantillons non nuls, crête, saturation, entropie de Shannon par échantillon et par octet, RMS par seconde — un son réel n'est ni un aplat ni un bruit uniforme). |
| `docs/METHOD.md` | The end-to-end methodology: PE analysis → lift (CPU-struct model) → host runtime → headless Vulkan backend → per-function differential testing. / La méthodologie de bout en bout. |
| `.github/workflows/legal-scan.yml` | CI guard: rejects any commit containing binaries, assets or game-derived blobs. / Garde CI : rejette tout commit contenant binaires, assets ou blobs dérivés du jeu. |

## Status / État (2026-10)

- [x] Full ISO inventory & extraction pipeline (over your own copy) / inventaire ISO complet
- [x] 86,650 functions lifted to portable C (CPU-struct reentrant model) / fonctions liftées en C portable
- [x] Boot to the game's main loop on x86-64 and ARM64 (qemu + Apple Silicon native) / boot jusqu'à la boucle principale
- [x] Game data mounted (MPQ/StormLib), 194 real pixel shaders reachable / données du jeu montées
- [x] First real pixels: D3D9→Vulkan headless backend (Lavapipe) / premiers pixels réels
- [x] Per-function oracle: 42,000+ functions swept, 1,300+ certified byte-exact / oracle par fonction
- [x] Shaders SM2/3 → SPIR-V translation: 75,797 distinct bytecodes extracted, 202/202 boot shaders `spirv-val`-validated, 5,771 real draws — **title screen renders** (2,570 distinct 16×16 blocks on frame 10) / traduction des shaders : **l'écran-titre s'affiche**
- [x] Engine audio: the engine's sound API (FMOD Ex, C++ MSVC ABI) fully decoded from decorated import names — real system init, sounds created through the game's own file callbacks, Ogg Vorbis decode, 44.1 kHz mixing; verified by WAV dump (entropy > 0) with an exact revert switch / **audio du moteur** : API son du moteur décodée intégralement depuis les noms d'imports décorés — init réelle, sons créés via les rappels fichier du jeu, décodage Ogg Vorbis, mixage 44,1 kHz ; preuve par dump WAV (entropie > 0), commutateur de retour exact
- [x] Android APK chain (NDK r28, NativeActivity, delta-arena, fibers for bionic): reproducible signed APK built locally, qemu bench (fake ART heap) reaches the game loop rc=0 / chaîne APK Android : APK signé reproductible, banc qemu jusqu'à la boucle du jeu
- [ ] Playable on a Galaxy Z Fold 8 / jouable sur Galaxy Z Fold 8

## License

MIT for the tooling in this repository. The tooling is original work; it contains no content owned by Blizzard Entertainment. / MIT pour l'outillage de ce dépôt. L'outillage est un travail original ; il ne contient aucun contenu appartenant à Blizzard Entertainment.
