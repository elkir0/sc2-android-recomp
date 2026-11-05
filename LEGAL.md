# Legal Policy / Politique légale — v2

**EN — Summary.** This repository distributes **original work only**: tooling, a
from-scratch D3D9-on-Vulkan backend and host runtime for statically recompiled games,
measurement scripts, and methodology documentation. It does not distribute, and will
never distribute: (a) any game executable or binary, (b) any game asset (textures,
models, sounds, maps, videos), (c) any decompiled, lifted or recompiled representation
of the game's code, (d) any data file extracted from the game — including shader
bytecodes from the game's cache and any translation of them (SPIR-V or otherwise),
which are the publisher's compiled code and derivatives of it. All such derived
artifacts are produced **locally by each user, from their own legitimate copy**, and
remain on their machine. The project targets offline, single-player, personal use. It
does not circumvent any online service, matchmaking, or technical protection measure
governing access to online services. StarCraft and Blizzard Entertainment are
trademarks of Blizzard Entertainment, Inc.; this project is independent,
non-commercial, unaffiliated and unendorsed.

**FR — Résumé.** Ce dépôt ne distribue que du **travail original** : de l'outillage,
un backend D3D9-sur-Vulkan et un socle hôte écrits from scratch pour les jeux
recompilés statiquement, des scripts de mesure et de la documentation de méthode. Il
ne distribue pas et ne distribuera jamais : (a) aucun exécutable ou binaire du jeu,
(b) aucun asset du jeu (textures, modèles, sons, cartes, vidéos), (c) aucune
représentation décompilée, liftée ou recompilée du code du jeu, (d) aucun fichier de
données extrait du jeu — **y compris les bytecodes de shaders du cache du jeu et
toute traduction qu'on en ferait (SPIR-V ou autre)** : ce sont du code objet compilé
de l'éditeur et ses œuvres dérivées. Tous ces artefacts dérivés sont produits
**localement par chaque utilisateur, depuis sa propre copie légitime**, et restent
sur sa machine. Le projet cible l'usage hors-ligne, solo, personnel. Il ne contourne
aucun service en ligne, aucun matchmaking, aucune mesure technique de protection
régissant l'accès aux services en ligne. StarCraft et Blizzard Entertainment sont des
marques de Blizzard Entertainment, Inc. ; projet indépendant, non commercial, sans
affiliation ni aval.

## What the v2 policy clarifies / ce que la v2 clarifie

### 1. The runtime and backend are original work / le runtime est une œuvre originale

The D3D9-on-Vulkan backend, the fake-COM device, the host runtime (Win32/FMOD layers)
and the Android glue are **written from scratch for this project**. Reimplementing a
public, documented API (Direct3D 9) on another graphics API is the same activity as
DXVK, wined3d or ToGL. APIs and functional behaviour are not copyrightable (CJEU
*C-406/10 SAS Institute v WPL*; *Oracle v. Google*, USSC 2021); reverse engineering
for interoperability is permitted (EU Directive 2009/24/EC art. 6; French CPI
L122-6-1). An automated audit (blobs, decompiler-style identifiers, embedded data)
runs before anything is published.

### 2. Addresses and interface facts / adresses et faits d'interface

Some files contain hardcoded virtual addresses of a specific game build, and names of
internal symbols discovered by lawful reverse engineering of our own copy. **These are
uncopyrightable facts** (idea/expression dichotomy), needed for interoperability, and
valid only for the build they were measured on (documented per file). They are not,
and must not be confused with, game code or data. Bulk mechanical dumps of such facts
without pedagogical value (full trampoline lists, cache indexes) stay out of the
repository anyway.

### 3. Screenshots — limited, documented / captures d'écran — limitées, documentées

A **small curated set (≤ 12)** of screenshots captured from our own legitimate copy
is allowed under `docs/`, solely to document that the method works (the universal
practice of emulator and recompilation projects). Each is credited as such. Bulk
captures, asset rips and frame dumps remain excluded.

### 4. Absolutely excluded in all cases / exclu absolument dans tous les cas

Game executables; MPQ/CASC archives or any extracted file; shader bytecodes from the
game's shader cache; **translated/derived shaders (SPIR-V tables and packs built from
the game's bytecodes)**; the lifted/decompiled C of the game; built APKs or any
binary containing recompiled game code (APK, macOS `.app`, libraries); saves, banks or maps
derived from the game (including the private skirmish copies the runtime prepares locally); anything > 20 MB. The CI enforces what it can
detect (extensions, magic bytes, known hashes, size); the audit before each push
enforces the rest.

## Enforcement / Application

1. A CI workflow (`.github/workflows/legal-scan.yml`) inspects every commit and
   **fails** on: game-binary/assets extensions (incl. `.bin`, `.pak`, `.ogv`, `.swf`,
   `.fxa`), known game-hash literals, executables, files above 20 MB, images outside
   `docs/`/root or beyond the curated set, and obvious secrets.
2. Commits violating this policy are reverted on sight.
3. Takedown requests are honoured promptly: contact via GitHub issues on this
   repository.
4. Contributors must confirm, in their PR, that every contributed file is original
   work or MIT-compatible tooling.

## Third-party code and credits / code tiers et crédits

- [pcrecomp](https://github.com/sp00nznet/pcrecomp) (MIT) — the lifter this project
  builds on; `cpu.h`/`cpu_selftest.c` derive from it (attribution headers kept).
- `stb_vorbis` v1.22 (public domain / MIT) — vendored, licence kept in-file.
- [StormLib](https://github.com/ladislav-zezula/StormLib) — external dependency, never vendored.
- libogg / libtheora (© Xiph.Org, BSD-3) — built from official tarballs by scripts, never vendored.
- Liberation fonts (OFL) — licences included; `android/jni/sc2_barre_police.h` is a 1-bit raster of
  Liberation Sans Bold generated by `android/outils/police/police.swift` (same licence).
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) (Apache-2.0) — macOS host dependency (Homebrew,
  version pinned by sha256 in `scripts/paquet-mac.sh`), never vendored.
