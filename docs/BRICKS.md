# Build bricks / Briques de build

*Detailed usage of the self-contained build bricks (moved from the README).* / *Usage détaillé des briques autonomes (déplacé du README).*

## Bricks / Briques

### `scripts/theora-android.sh` — Ogg Theora decoder for Android aarch64

**EN:** Builds the Theora **decoder** and libogg as static aarch64 libraries with the Android NDK (no assembly,
`-O2 -fPIC`), from the official Xiph.Org release tarballs, which every user downloads themselves (nothing third-party
is vendored here). Two runs produce identical sha256 (deterministic `llvm-ar`, `-ffile-prefix-map`).
**FR :** Compile le **décodeur** Theora et libogg en bibliothèques statiques aarch64 avec le NDK Android (sans
assembleur, `-O2 -fPIC`), depuis les archives officielles Xiph.Org que chaque utilisateur télécharge lui-même (aucune
source tierce embarquée ici). Deux exécutions donnent les mêmes sha256.

```sh
ANDROID_NDK_HOME=/path/to/android-ndk-r28c ./scripts/theora-android.sh
# → $OUT/lib/libtheoradec.a, $OUT/lib/libogg.a, $OUT/include/{ogg,theora}/, $OUT/EMPREINTES.txt (sha256)
```

| Variable | Default / Défaut | Role / Rôle |
|---|---|---|
| `ANDROID_NDK_HOME` (or `ANDROID_NDK`) | — (required / requis) | NDK root; tested with r28c (28.2.13676358) |
| `ANDROID_API` | `26` | API level of the `aarch64-linux-android<API>-clang` target |
| `THEORA_SRC` | `${XDG_CACHE_HOME:-~/.cache}/theora-android` | downloads + unpacked sources / téléchargements + sources |
| `OUT` | `$THEORA_SRC/android-arm64` | output prefix / préfixe de sortie |

Official sources, checked before use / sources officielles, vérifiées avant usage:

| Tarball | sha256 |
|---|---|
| `https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz` | `c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705` |
| `https://downloads.xiph.org/releases/theora/libtheora-1.1.1.tar.xz` | `f36da409947aa2b3dcc6af0a8c2e3144bc19db2ed547d64e9171c59c66561c61` |

Link order / ordre de lien : `libtheoradec.a` then `libogg.a` (the decoder calls the ogg bit-packer).
In CMake: `target_include_directories(app PRIVATE $OUT/include)` +
`target_link_libraries(app PRIVATE $OUT/lib/libtheoradec.a $OUT/lib/libogg.a)`.

**Licences:** libogg and libtheora are © Xiph.Org Foundation under BSD-style (3-clause) licences — keep their
copyright notices (`COPYING` in each tarball) when you redistribute a binary that links them. This repository only
ships the build script (MIT). / libogg et libtheora sont © Xiph.Org Foundation sous licences de type BSD (3 clauses)
— conservez leurs mentions (`COPYING` de chaque archive) si vous redistribuez un binaire qui les lie. Ce dépôt ne
fournit que le script de build (MIT).

### `scripts/video-transcode.sh` — H.264 video brick / brique vidéo H.264

**EN:** Software Theora decoding of full-screen cinematics is expensive on a phone CPU. This script transcodes,
**locally and only from your own legitimate copy**, every `.ogv` it finds into an H.264 Baseline `.mp4` that any
Android hardware AVC decoder accepts (no B-frames, `yuv420p`, level 3.1, one key frame per second, `faststart` index).
Video only: the engine keeps reading sound and timing from the original `.ogv`; the `.mp4` replaces the picture alone,
frame *n* of the engine = picture *n* of the `.mp4` (constant frame rate, duplicated Theora packets kept). The script
checks that the picture count equals the Theora packet count and warns otherwise. Originals are never modified;
up-to-date outputs are skipped.
**FR :** Le décodage logiciel Theora des cinématiques plein écran coûte cher au CPU d'un téléphone. Ce script
transcode, **localement et uniquement depuis votre propre copie légitime**, chaque `.ogv` trouvé en `.mp4` H.264
Baseline accepté par tout décodeur AVC matériel Android. Image seule : le moteur continue de lire le son et l'horloge
dans le `.ogv` d'origine ; frame *n* du moteur = image *n* du `.mp4`. Les originaux ne sont jamais modifiés.

```sh
./scripts/video-transcode.sh /path/to/your/install            # .mp4 next to each .ogv / à côté de chaque .ogv
SRC_DIR=/path/to/your/install OUT_DIR=/path/to/mp4-tree ./scripts/video-transcode.sh   # parallel tree / arbre parallèle
```

| Variable | Default / Défaut | Role / Rôle |
|---|---|---|
| `SRC_DIR` | — | source folder when no argument is given (recursive `.ogv` scan) / dossier source sans argument |
| `OUT_DIR` | — (sibling `.mp4`) | parallel output tree, same relative paths / arbre de sortie parallèle |
| `FFMPEG`, `FFPROBE` | `ffmpeg`, `ffprobe` from `PATH` | binaries to use; ffmpeg must have `libx264` / binaires ; libx264 requis |
| `CRF`, `PRESET` | `20`, `slow` | libx264 quality / speed |
| `GOP` | source fps (1 s) | max distance between key frames / distance max entre images clés |
| `AUDIO` | — | `aac` adds the audio track (not needed by the engine) / ajoute la piste son |
| `FORCE` | `0` | `1` redoes up-to-date outputs / refait les sorties à jour |

**Runtime switch `SC2_VIDEO_AMC=1` / interrupteur d'exécution :** off by default (the engine's own Theora decoding
runs unchanged). When set, each time the engine opens a cinematic the Android runtime looks for a `.mp4` with the same
relative path (case-insensitive), or the same base name, under `SC2_VIDEO_AMC_DIR` — default: the install root — and
decodes its pictures with the platform hardware decoder (`AMediaCodec`, NDK); sound and timing still come from the
`.ogv`. So either keep the sibling `.mp4` files in the install, or point `SC2_VIDEO_AMC_DIR` at your `OUT_DIR` tree.
No `.mp4`, an unsupported stream format or a decoder that fails to open → the cinematic plays through the engine's
software Theora path; a decoder failure during playback keeps the last picture (the game never stops). On non-Android
hosts the switch only logs that a `.mp4` was found. / désactivé par défaut ; s'il vaut `1`, le runtime Android cherche,
à chaque ouverture de cinématique, un `.mp4` de même chemin relatif (casse ignorée) ou de même nom sous
`SC2_VIDEO_AMC_DIR` (défaut : racine d'installation) et en décode les images avec le décodeur matériel ; le son et
l'horloge restent ceux du `.ogv`. Absent ou non ouvrable → repli sur le décodage logiciel Theora du moteur ; un échec
en cours de lecture garde la dernière image.

**Licences:** the script only transforms files you already own, on your machine; **no game asset is distributed**,
here or in any APK built from this toolkit — the `.mp4` files are derived from your copy and must stay with it.
ffmpeg/libx264 are third-party tools you install yourself (libx264 is GPL; the script only invokes it, it does not
ship it). / le script ne fait que transformer vos propres fichiers, localement ; **aucun asset n'est distribué** — les
`.mp4` dérivent de votre copie et doivent y rester. ffmpeg/libx264 sont installés par vous (libx264 est GPL ; le script
l'appelle, il ne le fournit pas).

