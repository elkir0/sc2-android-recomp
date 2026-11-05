#!/usr/bin/env python3
# Listing du contenu d'une archive MPQ/MPQE — SC2 Partout (Reverso-SC2)
# Usage : python3 scripts/liste-mpq.py <archive.mpqe> [nb-lignes]
# Règles mémoire (incident n°3) : exécuter sous ulimit -v 8000000 ; lecture par
# blocs via mpyq ; free -h avant/après dans la sortie.
import sys

def main():
    path = sys.argv[1]
    limite = int(sys.argv[2]) if len(sys.argv) > 2 else 80
    from mpyq import MPQArchive
    print(f"# listing {path}")
    a = MPQArchive(path)
    try:
        a.print_headers()
    except Exception as e:
        print(f"(print_headers indisponible : {e})")
    files = getattr(a, "files", None)
    if files is None:
        print("(pas d'attribut files)")
        return
    try:
        noms = sorted(files)
    except TypeError:
        noms = sorted(getattr(f, "filename", str(f)) for f in files)
    print(f"# entrées connues : {len(noms)}")
    for n in noms[:limite]:
        print(n)
    if len(noms) > limite:
        print(f"... (+{len(noms) - limite} autres)")

if __name__ == "__main__":
    main()
