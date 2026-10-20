/* mpq-extraire-8a.c — extrait des fichiers nommés d'une archive MPQ vers un dossier (passe 8a, StormLib).
 *   cc -O2 -I ~/tools/StormLib/src android/outils/mpq-extraire-8a.c ~/tools/StormLib/build/libstorm.a \
 *      -ltomcrypt -ltommath -lz -lbz2 -lstdc++ -o /tmp/mpq-extraire
 *   /tmp/mpq-extraire <archive> <dossier> <nom interne> [<nom interne> …]
 * Chaque fichier est écrit sous <dossier>/<dernier composant du nom> (les « \ » deviennent « _ » si deux noms
 * finissent pareil). Les données du jeu extraites restent HORS git. */
#include <StormLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage : %s <archive> <dossier> <nom> [<nom> …]\n", argv[0]); return 2; }
    HANDLE m = NULL;
    if (!SFileOpenArchive(argv[1], 0, STREAM_FLAG_READ_ONLY, &m)) { fprintf(stderr, "%s : ouverture impossible\n", argv[1]); return 1; }
    int rc = 0;
    for (int k = 3; k < argc; k++) {
        HANDLE f = NULL;
        if (!SFileOpenFileEx(m, argv[k], 0, &f)) { fprintf(stderr, "absent : %s\n", argv[k]); rc = 1; continue; }
        DWORD hi = 0, n = SFileGetFileSize(f, &hi);
        char *buf = malloc(n ? n : 1); DWORD lu = 0;
        if (!buf || !SFileReadFile(f, buf, n, &lu, NULL) || lu != n) { fprintf(stderr, "lecture : %s\n", argv[k]); rc = 1; }
        else {
            char nom[1024]; snprintf(nom, sizeof nom, "%s", argv[k]);
            for (char *p = nom; *p; p++) if (*p == '\\' || *p == '/') *p = '_';
            char chemin[2048]; snprintf(chemin, sizeof chemin, "%s/%s", argv[2], nom);
            FILE *o = fopen(chemin, "wb");
            if (o) { fwrite(buf, 1, n, o); fclose(o); printf("%u\t%s\n", (unsigned)n, chemin); } else rc = 1;
        }
        free(buf); SFileCloseFile(f);
    }
    SFileCloseArchive(m);
    return rc;
}
