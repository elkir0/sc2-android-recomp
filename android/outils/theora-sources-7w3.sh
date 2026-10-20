#!/usr/bin/env bash
# Passe 7w3 — sources OFFICIELLES de libogg 1.3.5 et libtheora 1.1.1 (Xiph.Org, licences BSD) dans
# ~/tools/theora (hors dépôt, comme StormLib). recomp-sc2/CMakeLists.txt les compile en statique
# (décodeur seul, sans assembleur). Idempotent ; vérifie les sha256.
set -euo pipefail
D="${THEORA_DIR:-$HOME/tools/theora}"
mkdir -p "$D/dl"; cd "$D"
prendre() {   # url sha256
  local f="dl/$(basename "$1")"
  [ -f "$f" ] || curl -fL -o "$f" "$1"
  echo "$2  $f" | sha256sum -c - >/dev/null || { echo "sha256 inattendu : $f"; exit 1; }
  tar xf "$f"
}
prendre https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz \
        c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705
prendre https://downloads.xiph.org/releases/theora/libtheora-1.1.1.tar.xz \
        f36da409947aa2b3dcc6af0a8c2e3144bc19db2ed547d64e9171c59c66561c61
echo "sources prêtes dans $D : $(ls -d "$D"/lib*/ | xargs -n1 basename | tr '\n' ' ')"
