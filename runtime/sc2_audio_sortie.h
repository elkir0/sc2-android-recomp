/* sc2_audio_sortie.h — consommateur des échantillons mixés par le FMOD de recomp-sc2 (passe 5g).
 *
 * Le mixage (sc2_audio_mix.c, System::update du jeu) produit du PCM 16 bits signé, STÉRÉO
 * ENTRELACÉ (G, D, G, D…), à la fréquence logicielle demandée par le jeu (44 100 Hz au boot).
 * Un consommateur — sortie AAudio/OpenSL d'Android, banc de mesure, enregistreur — s'abonne ici :
 * il reçoit EXACTEMENT les octets écrits dans le WAV de SC2_AUDIO_DUMP (pas de second mixage),
 * dans l'ordre, sur le thread du jeu qui appelle System::update, verrou du mixeur tenu.
 * Le rappel ne doit donc pas bloquer : une sortie temps réel copie dans sa propre file.
 * Aucun consommateur posé : comportement de la passe 5e inchangé (WAV si SC2_AUDIO_DUMP, sinon rien).
 */
#ifndef RECOMP_SC2_AUDIO_SORTIE_H
#define RECOMP_SC2_AUDIO_SORTIE_H
#include <stddef.h>
#include <stdint.h>

typedef void (*sc2_audio_sortie_t)(const int16_t *pcm, size_t n_frames, void *ctx);

/* Pose (ou retire, cb = NULL) le consommateur. Un seul à la fois ; appelable à tout moment. */
void sc2_audio_sortie_definir(sc2_audio_sortie_t cb, void *ctx);
/* Fréquence des frames livrées (Hz) ; 2 canaux, int16. */
int  sc2_audio_sortie_frequence(void);

/* Interne au mixeur (sc2_audio_mix.c) : livre n frames au consommateur s'il y en a un. */
void sc2_audio_sortie_emettre(const int16_t *pcm, size_t n_frames, int frequence);

/* Consommateur de banc (sc2_audio_sortie_compteur.c) : compte frames, échantillons non nuls,
 * crête et empreinte FNV-1a 64 des octets reçus. SC2_AUDIO_COMPTEUR=1 l'installe à System::init ;
 * son bilan est imprimé par le bilan du mixeur, verrou tenu, mixage arrêté — avant la fermeture du
 * WAV : les deux décomptes portent sur les mêmes frames. */
void sc2_audio_compteur_installer(void);
int  sc2_audio_compteur_installe(void);
void sc2_audio_compteur_bilan(void);

#endif
