// mpqget — extrait un fichier d'une archive MPQ/MPQE via StormLib
// Chantier SC2 Partout (Reverso-SC2). Usage : mpqget <archive-avec-prefixe> <nom-interne> <destination>
// Streaming par tampon 64 Ko (règle incident n°3 : jamais de lecture complète en mémoire).
// rc 0 : octets extraits = taille du catalogue MPQ ; sinon rc 1 et aucune sortie (fichier partiel supprimé).
// Construction (binaire HORS git, dans tools/ — à refaire après toute modification de ce source :
// le défaut du dernier bloc partiel a survécu une journée dans un tools/mpqget jamais recompilé) :
//   g++ -O2 -Wall -o tools/mpqget mpqget.cpp -I ~/tools/StormLib/src -L ~/tools/StormLib/build -lstorm -ltomcrypt -ltommath -lz -lbz2
#include <cstdio>
#include <cstring>
#include "StormLib.h"

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <archive> <nom-interne-mpq> <destination>\n", argv[0]); return 2; }
    HANDLE hMpq = NULL;
    if (!SFileOpenArchive(argv[1], 0, MPQ_OPEN_READ_ONLY, &hMpq)) {
        fprintf(stderr, "SFileOpenArchive echec, err=%u\n", (unsigned)SErrGetLastError());
        return 1;
    }
    HANDLE hFile = NULL;
    if (!SFileOpenFileEx(hMpq, argv[2], 0, &hFile)) {
        fprintf(stderr, "SFileOpenFileEx echec [%s], err=%u\n", argv[2], (unsigned)SErrGetLastError());
        SFileCloseArchive(hMpq);
        return 1;
    }
    FILE* out = fopen(argv[3], "wb");
    if (!out) { fprintf(stderr, "fopen echec [%s]\n", argv[3]); SFileCloseFile(hFile); SFileCloseArchive(hMpq); return 1; }
    char buf[65536]; DWORD nb = 0; unsigned long long total = 0;
    DWORD haut = 0, bas = SFileGetFileSize(hFile, &haut);
    unsigned long long attendu = ((unsigned long long)haut << 32) | bas;   /* taille du catalogue MPQ */
    // SFileReadFile rend FALSE + ERROR_HANDLE_EOF quand il lit MOINS que demandé, nb > 0 COMPRIS
    // (StormLib SFileReadFile.cpp : « If the read operation succeeded, but not full number of bytes
    // was read, set the last error to ERROR_HANDLE_EOF »). L'ancienne boucle
    // « while (SFileReadFile(...) && nb > 0) » sortait donc AVANT d'écrire le dernier bloc partiel :
    // un fichier de 130 743 octets n'en rendait que 65 536 ; plus petit que le tampon → 0 octet
    // (le binaire tools/mpqget en était resté là : source corrigé, binaire jamais recompilé).
    // Ici : on écrit TOUJOURS les nb octets lus, on ne s'arrête qu'à la fin réelle (nb = 0, ou
    // ERROR_HANDLE_EOF), toute autre erreur est fatale, et le total doit égaler la taille du catalogue.
    int erreur = 0;
    for (;;) {
        bool ok = SFileReadFile(hFile, buf, sizeof(buf), &nb, NULL);
        DWORD err = ok ? ERROR_SUCCESS : SErrGetLastError();
        if (nb && fwrite(buf, 1, nb, out) != nb) { fprintf(stderr, "fwrite echec\n"); erreur = 1; break; }
        total += nb;
        if (!ok && err != ERROR_HANDLE_EOF) { fprintf(stderr, "SFileReadFile echec apres %llu octets, err=%u\n", total, (unsigned)err); erreur = 1; break; }
        if (!ok || nb == 0) break;
    }
    fclose(out);
    SFileCloseFile(hFile);
    SFileCloseArchive(hMpq);
    if (!erreur && total != attendu) {
        fprintf(stderr, "taille incoherente : %llu octets extraits, %llu annonces par le catalogue\n", total, attendu);
        erreur = 1;
    }
    if (erreur) { remove(argv[3]); return 1; }
    printf("%llu octets extraits -> %s (catalogue : %llu)\n", total, argv[3], attendu);
    return 0;
}
