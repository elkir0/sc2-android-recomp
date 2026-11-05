// mpqlist — liste le contenu d'une archive MPQ/MPQE via StormLib
// Chantier SC2 Partout (Reverso-SC2). Usage : mpqlist <archive> [--tailles]
// --tailles : chaque ligne devient « <taille du catalogue MPQ>\t<nom> » (SFileGetFileSize ; « - » si
// l'entrée du listfile n'existe pas dans l'archive). Sans option : noms seuls, comme avant.
// Lecture du (listfile) interne en accumulate borné (256 Mo max, règle streaming ;
// troncature au plafond = exit 3 BRUYANT, jamais silencieuse),
// fallback sur l'énumération SFileFind*.
// Construction (binaire HORS git, dans tools/ — à refaire après toute modification de ce source :
// le défaut du dernier bloc partiel a survécu une journée dans un tools/mpqlist jamais recompilé) :
//   g++ -O2 -Wall -o tools/mpqlist mpqlist.cpp -I ~/tools/StormLib/src -L ~/tools/StormLib/build -lstorm -ltomcrypt -ltommath -lz -lbz2
#include <cstdio>
#include <cstring>
#include <string>
#include "StormLib.h"

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <archive> [--tailles]\n", argv[0]); return 2; }
    bool tailles = argc > 2 && !strcmp(argv[2], "--tailles");
    HANDLE hMpq = NULL;
    if (!SFileOpenArchive(argv[1], 0, MPQ_OPEN_READ_ONLY, &hMpq)) {
        fprintf(stderr, "SFileOpenArchive echec, err=%u\n", (unsigned)SErrGetLastError());
        return 1;
    }
    printf("# archive ouverte : %s\n", argv[1]);

    HANDLE hList = NULL;
    if (SFileOpenFileEx(hMpq, "(listfile)", 0, &hList)) {
        std::string acc; char buf[65536]; DWORD nb = 0;
        // Même défaut que l'ancien mpqget : « while (SFileReadFile(...) && nb > 0) » abandonnait le
        // dernier bloc PARTIEL (SFileReadFile rend FALSE + ERROR_HANDLE_EOF avec nb > 0) — un grand
        // listfile de 809 851 octets n'était lu que sur 786 432 (12 × 64 Kio) : entrées de la fin perdues.
        const size_t PLAFOND_LISTFILE = 256u*1024*1024; // 256 Mo — le listfile d'un gros MPQ d'installer dépasse 4 Mio : l'ancien plafond 2 Mio tronquait le listing à MOITIÉ sans aucun signal.
        bool eof_listfile = false;
        while (acc.size() < PLAFOND_LISTFILE) {
            bool ok = SFileReadFile(hList, buf, sizeof(buf), &nb, NULL);
            acc.append(buf, nb);
            if (!ok || nb == 0) { eof_listfile = true; break; }
        }
        if (!eof_listfile) {
            fprintf(stderr, "# ATTENTION : (listfile) TRONQUÉ — plafond %zu octets atteint sans EOF ; listing incomplet REFUSÉ (exit 3)\n", PLAFOND_LISTFILE);
            SFileCloseFile(hList);
            SFileCloseArchive(hMpq);
            return 3;
        }
        SFileCloseFile(hList);
        printf("# (listfile) : %zu octets\n", acc.size());
        size_t debut = 0, lignes = 0;
        while (debut < acc.size()) {
            size_t fin = acc.find('\n', debut);
            if (fin == std::string::npos) fin = acc.size();
            if (fin > debut && acc[fin - 1] == '\r') acc[fin - 1] = '\0';
            if (fin > debut) {
                acc[fin] = '\0';
                const char *nom = acc.c_str() + debut;
                if (tailles) {
                    HANDLE hf = NULL;
                    if (*nom && SFileOpenFileEx(hMpq, nom, 0, &hf)) {
                        DWORD haut = 0, bas = SFileGetFileSize(hf, &haut);
                        printf("%llu\t%s\n", ((unsigned long long)haut << 32) | bas, nom);
                        SFileCloseFile(hf);
                    } else printf("-\t%s\n", nom);
                } else printf("%s\n", nom);
                lignes++;
            }
            debut = fin + 1;
        }
        printf("# %zu entrees listees\n", lignes);
    } else {
        printf("# pas de (listfile) lisible, err=%u — enumeration\n", (unsigned)SErrGetLastError());
        SFILE_FIND_DATA fd; memset(&fd, 0, sizeof(fd));
        HANDLE hFind = SFileFindFirstFile(hMpq, "*", &fd, NULL);
        if (hFind) {
            int n = 0;
            do { if (tailles) printf("%u\t%s\n", fd.dwFileSize, fd.cFileName); else printf("%s\n", fd.cFileName); n++; }
            while (SFileFindNextFile(hFind, &fd));
            SFileFindClose(hFind);
            printf("# %d entrees\n", n);
        } else {
            printf("# enumeration impossible, err=%u\n", (unsigned)SErrGetLastError());
        }
    }
    SFileCloseArchive(hMpq);
    return 0;
}
