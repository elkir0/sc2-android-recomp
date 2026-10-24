/* sc2_audio_android.h — sortie audio de la plate-forme Android (passe 7e).
 *
 * Consommateur de la sortie 5g (runtime/sc2_audio_sortie.h) : le mixeur du FMOD logiciel livre son
 * PCM16 stéréo sur le fil du jeu ; il est copié dans un anneau, que vide le rappel de données AAudio
 * (téléphone) ou un fil « pseudo-AAudio » au rythme réel (banc Linux/qemu, sans AAudio).
 * SC2_AUDIO_ANDROID=0 : rien n'est posé (comportement d'avant : pas de son).
 */
#ifndef SC2_AUDIO_ANDROID_H
#define SC2_AUDIO_ANDROID_H

/* À appeler AVANT sc2_poc_main (avant le premier System::update). 0 = sortie posée. */
int  sc2aa_demarrer(void);
/* Arrêt propre (idempotent) : consommateur retiré, flux fermé, bilan [AUDIO] Android imprimé. */
void sc2aa_arreter(void);
/* 21/10 : 1 = flux de sortie en pause (jeu suspendu en arrière-plan), 0 = repris. */
void sc2aa_pause(int pause);

#endif
