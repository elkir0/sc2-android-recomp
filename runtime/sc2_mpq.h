/* sc2_mpq.h — passe 6a : observation des archives MPQ que le moteur monte, via StormLib.
 *
 * Storm n'est PAS un import de SC2.exe (aucun Storm.dll dans l'IAT, cf. sc2_imports.h) : il est
 * lié en statique dans l'exécutable et lit ses archives par CreateFileW / ReadFile (overlapped).
 * Le runtime ne peut donc pas « remplacer » SFileOpenArchive ; il SERT les octets (couche [FS]
 * de sc2_iat.c) et, à côté, ouvre la même archive avec StormLib (lecture seule) pour traduire
 * chaque lecture brute du moteur en nom de fichier interne : traces [MPQ].
 *
 * Fichier séparé : StormLib.h apporte ses propres HANDLE/DWORD/WINAPI, incompatibles avec
 * win32_compat.h — l'interface ci-dessous n'utilise que des types C.
 */
#ifndef RECOMP_SC2_MPQ_H
#define RECOMP_SC2_MPQ_H

#include <stdint.h>

/* CreateFileW a ouvert `hote` (chemin hôte) pour le handle invité `fh` : si c'est une archive
 * MPQ (signature « MPQ\x1A » ou en-tête utilisateur « MPQ\x1B »), StormLib l'ouvre et l'indexe. */
void sc2_mpq_ouvert(uint32_t fh, const char *hote, const char *win);
/* ReadFile du moteur sur `fh` : [off, off+n) → fichiers internes touchés (1re lecture tracée). 24/10 : `tampon` (les n
 * octets lus, hôte) reçoit la table de hachage augmentée des alias « X.wav » → « X.wav_ » là où il la recouvre ; à
 * appeler AVANT de rendre la lecture au jeu (avant le signal de l'OVERLAPPED). */
void sc2_mpq_lu(uint32_t fh, uint64_t off, void *tampon, uint32_t n);
void sc2_mpq_ferme(uint32_t fh);
/* Bilan de fin de run : par archive, fichiers internes distincts lus et octets. */
void sc2_mpq_bilan(void);
/* L3 (24/10) : `rel` (« /Maps/…/X.SC2Map », relatif à l'installation `install`) absent du disque mais dossier d'une
 * archive Campaigns/<c>.SC2Campaign/<a>.SC2Maps → archive MPQ synthétique construite une fois sous `cache`, chemin hôte
 * écrit dans `o`, rend 1 (ligne [MPQ] « carte de campagne servie … »). Rend 0 si la carte n'est trouvée nulle part. */
#include <stddef.h>
int sc2_mpq_carte_campagne(const char *install, const char *rel, const char *cache, char *o, size_t cap, uint32_t appelant);

#endif
