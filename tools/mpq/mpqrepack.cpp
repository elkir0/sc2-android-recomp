// mpqrepack — reconstruit une archive installée de SC2 à partir des entrées « Repack-MPQ »
// du Tome d'installation (ce que fait l'installeur Blizzard), via StormLib.
// Chantier SC2 Partout, passe 4. Lecture seule du Tome ; une archive MPQ v1 neuve en sortie.
//
// Usage : mpqrepack <tome-avec-prefixe> <prefixe-interne> <sortie.mpq>
//   ex. mpqrepack "mpqe-file:extraction/dvd-racine/Installer Tome 1.MPQE" \
//         'Repack-MPQ\fileset.base#Mods#Core.SC2Mod#Base.SC2Data\' Base.SC2Data
// Chaque fichier « <prefixe><chemin> » du Tome devient « <chemin> » dans l'archive,
// compressé zlib. Lecture par fichier entier (les fichiers d'archive font < 64 Mo).
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <strings.h>
#include "StormLib.h"

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <tome> <prefixe-interne> <sortie.mpq>\n", argv[0]); return 2; }
    const char* pref = argv[2];
    size_t lp = strlen(pref);
    HANDLE hTome = NULL;
    if (!SFileOpenArchive(argv[1], 0, MPQ_OPEN_READ_ONLY, &hTome)) {
        fprintf(stderr, "SFileOpenArchive(tome) echec, err=%u\n", (unsigned)SErrGetLastError()); return 1;
    }
    std::vector<std::string> noms;
    SFILE_FIND_DATA fd;
    HANDLE hf = SFileFindFirstFile(hTome, "*", &fd, NULL);
    if (hf) {
        do { if (strncasecmp(fd.cFileName, pref, lp) == 0) noms.push_back(fd.cFileName); }
        while (SFileFindNextFile(hf, &fd));
        SFileFindClose(hf);
    }
    if (noms.empty()) { fprintf(stderr, "aucune entree sous [%s]\n", pref); return 1; }
    DWORD maxf = 16; while (maxf < noms.size() * 2 + 16) maxf <<= 1;
    HANDLE hOut = NULL;
    remove(argv[3]);
    if (!SFileCreateArchive(argv[3], MPQ_CREATE_LISTFILE | MPQ_CREATE_ATTRIBUTES | MPQ_CREATE_ARCHIVE_V1, maxf, &hOut)) {
        fprintf(stderr, "SFileCreateArchive echec, err=%u\n", (unsigned)SErrGetLastError()); return 1;
    }
    unsigned long long tot = 0; unsigned ok = 0;
    std::vector<char> buf;
    for (const std::string& n : noms) {
        HANDLE hIn = NULL;
        if (!SFileOpenFileEx(hTome, n.c_str(), 0, &hIn)) { fprintf(stderr, "ouverture [%s] echec\n", n.c_str()); continue; }
        DWORD hi = 0, sz = SFileGetFileSize(hIn, &hi);
        buf.resize(sz ? sz : 1);
        DWORD lu = 0;
        if (sz && (!SFileReadFile(hIn, buf.data(), sz, &lu, NULL) || lu != sz)) {
            fprintf(stderr, "lecture [%s] echec\n", n.c_str()); SFileCloseFile(hIn); continue;
        }
        SFileCloseFile(hIn);
        const char* interne = n.c_str() + lp;
        HANDLE hNew = NULL;
        if (!SFileCreateFile(hOut, interne, 0, sz, 0, MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING, &hNew)) {
            fprintf(stderr, "creation [%s] echec, err=%u\n", interne, (unsigned)SErrGetLastError()); continue;
        }
        if (sz && !SFileWriteFile(hNew, buf.data(), sz, MPQ_COMPRESSION_ZLIB)) {
            fprintf(stderr, "ecriture [%s] echec, err=%u\n", interne, (unsigned)SErrGetLastError());
        }
        SFileFinishFile(hNew);
        tot += sz; ok++;
    }
    SFileCloseArchive(hOut);
    SFileCloseArchive(hTome);
    printf("%u/%zu fichiers, %llu octets -> %s\n", ok, noms.size(), tot, argv[3]);
    return ok == noms.size() ? 0 : 1;
}
