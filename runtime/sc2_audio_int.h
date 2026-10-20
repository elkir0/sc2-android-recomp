/* sc2_audio_int.h — interne au périphérique audio (sc2_audio_fmod.c ↔ sc2_audio_mix.c), passe 5e. */
#ifndef RECOMP_SC2_AUDIO_INT_H
#define RECOMP_SC2_AUDIO_INT_H
#include "sc2_host.h"

#define SC2_AUDIO_FREQ 48000

/* objets FMOD invités : [0] « FMOD », [4] genre (1 System, 2 Sound, 3 Channel, 4 ChannelGroup,
 * 5 DSP, 6 DSPConnection, 7 Reverb, 8 SoundGroup, 9 Geometry, 10 SyncPoint), [8] index hôte */
uint32_t sc2_fmod_objet(int genre, int index);
int      sc2_fmod_index(uint32_t objet, int genre);    /* -1 : pas un objet FMOD (de ce genre) */

typedef uint32_t (*sc2_audio_fn)(CPU *c, const uint32_t *a);
typedef struct { const char *court; sc2_audio_fn fn; } sc2_audio_impl_t;
extern const sc2_audio_impl_t sc2_audio_impl[];         /* méthodes réelles (sc2_audio_mix.c) */
extern const unsigned sc2_audio_nb_impl;

void sc2_audio_systeme_cree(void);
/* passe 7w6 : DSP du jeu (sc2_audio_fmod.c) ↔ mixeur (sc2_audio_mix.c) */
int      sc2_dsp_index(uint32_t objet);                 /* -1 : pas un DSP vivant */
void     sc2_dsp_lire(CPU *c, int dsp, float *acc, uint32_t n, float gl, float gr);   /* générateur playDSP → acc */
void     sc2_dsp_effets(CPU *c, float *acc, uint32_t n);                             /* effets System::addDSP */
uint64_t sc2_dsp_frames(int dsp);
void     sc2_audio_dsp_oublier(int dsp);                /* DSP::release : ses canaux s'arrêtent */
/* passe 7w7 */
int      sc2_audio_freq(void);                          /* fréquence RÉELLE du mixeur (setSoftwareFormat du maître) */
void     sc2_dsp_test(CPU *c);                          /* SC2_DSP_TEST=film[:i/s] : enregistrement vidéo du jeu (0x009E7340) */
void sc2_audio_bilan_mix(void);
#endif
