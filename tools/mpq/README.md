# MPQ extraction tools (StormLib) — and the partial-block EOF trap

**FR :** Deux petits outils C++ pour extraire (`mpqget`) et lister (`mpqlist`) une archive MPQ,
via [StormLib](https://github.com/ladislav-zezula/StormLib). Le point intéressant est le **piège
documenté** : `SFileReadFile` rend **FALSE + `ERROR_HANDLE_EOF` alors même qu'il a lu des octets**
(`nb > 0`) sur la dernière lecture partielle d'un fichier — comportement voulu de StormLib
(SFileReadFile.cpp : *« If the read operation succeeded, but not full number of bytes was read,
set the last error to ERROR_HANDLE_EOF »*). La boucle naïve
`while (SFileReadFile(...) && nb > 0)` jette donc le dernier bloc partiel :

- tout fichier **non multiple** du tampon (64 KiB) sort tronqué au multiple inférieur ;
- tout fichier **plus petit** que le tampon sort **vide** (0 octet) ;
- les multiples **exacts** du tampon sortent intacts — d'où un défaut discret qui peut survivre
  des semaines (vécu : des extractions fausses pendant une journée entière, le source était
  corrigé mais le binaire déployé jamais recompilé).

`mpqget` écrit **tous** les octets lus, ne s'arrête qu'à la fin réelle, toute autre erreur est
fatale, et le total doit égaler la taille du catalogue (`rc 0` ⟺ extraction complète, sinon
`rc 1` et la sortie est supprimée). `mpqlist` lit le `(listfile)` en entier (même correctif),
retire les `CR` du CRLF, et ajoute `--tailles` (taille du catalogue par entrée — pratique pour
préparer des bancs de contrôle d'extraction).

**EN :** Two small C++ tools to extract (`mpqget`) and list (`mpqlist`) an MPQ archive, via
[StormLib](https://github.com/ladislav-zezula/StormLib). The interesting part is the **documented
trap**: `SFileReadFile` returns **FALSE + `ERROR_HANDLE_EOF` even though it did read bytes**
(`nb > 0`) on a file's final partial read — intended StormLib behaviour. A naive
`while (SFileReadFile(...) && nb > 0)` loop therefore drops the last partial block: any file
**not a multiple** of the 64 KiB buffer comes out truncated to the lower multiple; anything
**smaller** than the buffer comes out **empty**; **exact** multiples come out intact — a quiet
defect that can survive for weeks (lived: a full day of wrong extractions from a fixed source
whose deployed binary had never been recompiled).

`mpqget` always writes every byte read, stops only at the true end, treats any other error as
fatal, and requires the total to equal the catalog size (`rc 0` ⟺ complete extraction, else
`rc 1` with the output removed). `mpqlist` reads the whole `(listfile)` (same fix), strips CRLF,
and adds `--tailles` (catalog size per entry — handy to prepare extraction control benches).

## Build (dependency: StormLib)

```sh
g++ -O2 -Wall -o mpqget  mpqget.cpp  -I <StormLib>/src -L <StormLib>/build -lstorm -ltomcrypt -ltommath -lz -lbz2
g++ -O2 -Wall -o mpqlist mpqlist.cpp -I <StormLib>/src -L <StormLib>/build -lstorm -ltomcrypt -ltommath -lz -lbz2
```

Recompile your deployed binaries after touching these sources — the trap above survived a day in
a stale binary. / Recompilez les binaires déployés après toute modification — le piège ci-dessus
a survécu une journée dans un binaire périmé.

## Usage

```sh
mpqget  <archive> <internal-name> <destination>   # rc 0 ⟺ extracted bytes == catalog size
mpqlist <archive> [--tailles]                     # names only, or "<size>\t<name>" lines
```
