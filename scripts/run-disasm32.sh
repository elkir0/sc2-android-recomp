#!/usr/bin/env bash
# Catalogue disasm32 (pcrecomp) sur SC2.exe — SC2 Partout
# Descente récursive pure ; sortie JSON. Règles : ulimit 8 Go, free -h avant/après.
set -uo pipefail
ulimit -v 8000000
DEPOT="$HOME/code/reverse/sc2-partout"
LOG="$DEPOT/extraction/pcrecomp/disasm32.log"
{
echo "=== $(date -Is) disasm32 sur SC2.exe ==="
free -h | head -2
cd ~/tools/pcrecomp
time "$DEPOT/.venv/bin/python" tools/disasm/disasm32.py \
  "$DEPOT/extraction/moteur/Win32/Base15405/SC2.exe" \
  --min-size 64 --output "$DEPOT/extraction/pcrecomp/SC2-catalogue-disasm32.json"
RC=$?
echo "rc=$RC"
ls -la "$DEPOT/extraction/pcrecomp/SC2-catalogue-disasm32.json" 2>/dev/null
free -h | head -2
echo "=== $(date -Is) fin ==="
} >> "$LOG" 2>&1
exit $RC
