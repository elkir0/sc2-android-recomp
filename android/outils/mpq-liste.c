/* mpq-liste.c — liste le contenu d'archives MPQ (passe 7y) : « taille<TAB>archive<TAB>nom interne » par fichier,
 * d'après la (listfile) de l'archive (StormLib). Outil générique, aucune constante du jeu.
 *   cc -O2 -I ~/tools/StormLib/src android/outils/mpq-liste.c <libstorm.a> -lz -lbz2 -lstdc++ -o /tmp/mpq-liste
 *   /tmp/mpq-liste <archive> [<archive> …] */
#include <StormLib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    int rc = 0;
    for (int k = 1; k < argc; k++) {
        HANDLE m = NULL;
        if (!SFileOpenArchive(argv[k], 0, STREAM_FLAG_READ_ONLY, &m)) { fprintf(stderr, "%s : ouverture impossible\n", argv[k]); rc = 1; continue; }
        SFILE_FIND_DATA d;
        HANDLE f = SFileFindFirstFile(m, "*", &d, NULL);
        unsigned long n = 0;
        if (f) {
            do { printf("%u\t%s\t%s\n", (unsigned)d.dwFileSize, argv[k], d.cFileName); n++; } while (SFileFindNextFile(f, &d));
            SFileFindClose(f);
        }
        if (!n) fprintf(stderr, "%s : aucune entrée listée (pas de (listfile) ?)\n", argv[k]);
        SFileCloseArchive(m);
    }
    return rc;
}
