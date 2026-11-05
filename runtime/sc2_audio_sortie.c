/* sc2_audio_sortie.c — abonnement d'un consommateur aux échantillons mixés (passe 5g). */
#include "sc2_audio_sortie.h"

#include <pthread.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static sc2_audio_sortie_t g_cb;
static void *g_ctx;
static int g_freq = 44100;

void sc2_audio_sortie_definir(sc2_audio_sortie_t cb, void *ctx)
{
    pthread_mutex_lock(&g_mu);
    g_cb = cb; g_ctx = ctx;
    pthread_mutex_unlock(&g_mu);
}

int sc2_audio_sortie_frequence(void) { return g_freq; }

void sc2_audio_sortie_emettre(const int16_t *pcm, size_t n_frames, int frequence)
{
    pthread_mutex_lock(&g_mu);
    g_freq = frequence;
    sc2_audio_sortie_t cb = g_cb; void *ctx = g_ctx;
    pthread_mutex_unlock(&g_mu);
    if (cb && n_frames) cb(pcm, n_frames, ctx);
}
