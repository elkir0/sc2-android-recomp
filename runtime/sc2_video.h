/* sc2_video.h — passe 7w3 : décodeur vidéo Ogg Theora (.ogv des briefings et cinématiques).
 *
 * Lecture EN FLUX depuis une archive MPQ du jeu (StormLib, lecture seule, blocs de 64 Kio : jamais le
 * fichier entier en mémoire) ou depuis un fichier hôte ; décodage libtheora (theoradec, LGPL, sources
 * séparées dans ~/tools/theora, cf. android/TRACE-PASSE7W3.md) ; conversion YUV → RGBA (BT.601, plage
 * vidéo 16–235, entiers) sur la zone d'image utile.
 *
 * API en C pur (types C seulement, comme sc2_mpq.h : StormLib apporte ses propres HANDLE/DWORD). Un
 * lecteur est un objet opaque : plusieurs vidéos peuvent être ouvertes, un lecteur n'est utilisé que
 * par un fil à la fois.
 *
 * Banc dans un run (aucun effet sans la variable) :
 *   SC2_VIDEO=<archive hôte>|<nom interne>   au chargement du binaire, un fil hôte décode la vidéo
 *                                           pendant le run ([VIDEO], bilan à la sortie)
 *   SC2_VIDEO_PAS=N      une frame sur N convertie en RGBA et écrite (défaut 10 ; toutes les frames
 *                        sont DÉCODÉES : Theora prédit chaque frame de la précédente)
 *   SC2_VIDEO_MAX=M      arrêt après M frames décodées (défaut : toute la vidéo)
 *   SC2_VIDEO_TEMPS_REEL=0  décodage au plus vite (défaut : à la cadence de la vidéo, frame n à t0 + n/fps)
 *   SC2_FRAMES_DIR=<dir> PPM écrits en <dir>/video_NNNNN.ppm
 */
#ifndef RECOMP_SC2_VIDEO_H
#define RECOMP_SC2_VIDEO_H

#include <stdint.h>

typedef struct sc2v sc2v;

/* Ouvre <nom> (chemin interne, « \ » ou « / ») dans l'archive MPQ hôte <archive>, lit les en-têtes
 * Theora. NULL si échec (raison journalisée). */
sc2v *sc2v_ouvrir_mpq(const char *archive, const char *nom);
/* Même chose depuis un fichier .ogv hôte (tests). */
sc2v *sc2v_ouvrir_fichier(const char *chemin);
void  sc2v_fermer(sc2v *v);

/* Taille de l'image utile (zone pic de Theora), cadence en images/s. */
void   sc2v_dims(const sc2v *v, uint32_t *w, uint32_t *h);
double sc2v_fps(const sc2v *v);

/* Décode la frame suivante : 1 si une frame est prête, 0 en fin de flux, -1 sur erreur. */
int sc2v_avancer(sc2v *v);
/* 1 si une frame décodée n'a pas encore été lue par sc2v_lire_rgba. */
int sc2v_frame_dispo(const sc2v *v);
/* Convertit la dernière frame décodée en RGBA (w*h*4 octets, alpha 255). 0 si fait. */
int sc2v_lire_rgba(sc2v *v, uint8_t *rgba);
/* Variante BGRA (ordre mémoire de D3DFMT_A8R8G8B8, = 21) pour le téléversement en texture. */
int sc2v_lire_bgra(sc2v *v, uint8_t *bgra);

/* Compteurs : frames décodées, paquets lus, octets lus dans la source. */
uint32_t sc2v_frames(const sc2v *v);
uint64_t sc2v_octets(const sc2v *v);

/* Vidéo YUV synthétique → RGBA (tests de la conversion seule). */
void sc2v_yuv_vers_rgba(int y, int u, int v, uint8_t *rgba);

#endif
