/* sc2_audio_mpq.c — lecture directe d'un son dans une archive MPQ hôte (DIAGNOSTIC, passe 5e).
 *
 * Sert SC2_AUDIO_TEST quand le nom est de la forme « <archive hôte>|<nom interne> » : à l'écran
 * d'attente Battle.net, le système de fichiers du JEU ne cherche que dans Core.SC2Mod (aucun son :
 * 1 120 DDS, 1 OGV) ; Liberty.SC2Mod/base.SC2Assets (913 .ogg + 1 516 .wav_, tous Vorbis) n'entre
 * dans sa recherche qu'avec le contenu de jeu. Fichier séparé : StormLib.h apporte ses propres
 * HANDLE/DWORD, incompatibles avec win32_compat.h (même raison que sc2_mpq.c, passe 6a).
 */
#include <StormLib.h>
#include <stdint.h>
#include <stdlib.h>

uint8_t *sc2_audio_mpq_lire(const char *archive, const char *interne, uint32_t *n)
{
    HANDLE m = NULL, f = NULL;
    uint8_t *out = NULL;
    *n = 0;
    if (!SFileOpenArchive(archive, 0, STREAM_FLAG_READ_ONLY, &m)) return NULL;
    if (SFileOpenFileEx(m, interne, 0, &f)) {
        DWORD taille = SFileGetFileSize(f, NULL), lu = 0;
        if (taille != SFILE_INVALID_SIZE && (out = malloc(taille ? taille : 1)) != NULL) {
            SFileReadFile(f, out, taille, &lu, NULL);    /* FALSE avec lu = taille à la fin : normal */
            *n = lu;
        }
        SFileCloseFile(f);
    }
    SFileCloseArchive(m);
    return out;
}
