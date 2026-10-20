/* sc2_audio.h — périphérique audio de recomp-sc2 : FMOD Ex 4.28.08 vu par le jeu (passe 5e).
 *
 * SC2.exe joue tout son son par fmodex_4_28_08.dll (129 imports, API C++ stdcall ;
 * inventaire : scripts/extraire-audio-imports.py). Ici, une IMPLÉMENTATION de cette API :
 * objets FMOD invités, méthodes comptées (bilan [AUDIO]), décodage des sons du jeu
 * (WAV PCM, Ogg Vorbis), mixage logiciel stéréo 48 kHz à chaque System::update.
 *
 *   SC2_AUDIO=0             : comportement des passes 4-5d (FMOD_System_Create échoue)
 *   SC2_AUDIO_DUMP=<f.wav>  : écrit le mixage en WAV (banc vérifiable, sans carte son)
 *   SC2_AUDIO_SORTIE=0      : pas de sortie temps réel (sinon PulseAudio par dlopen, si présent)
 */
#ifndef RECOMP_SC2_AUDIO_H
#define RECOMP_SC2_AUDIO_H
#include "sc2_iat.h"

const imp_def *sc2_audio_find(const char *nom);   /* NULL : import non audio, ou audio désactivé */
void sc2_audio_bilan(void);

#endif
