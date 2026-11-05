/* sc2_audio_sortie_compteur.c — consommateur de BANC des échantillons mixés (passe 5g).
 *
 * Preuve sans WAV : compte les frames reçues, les échantillons non nuls, la crête, et calcule
 * l'empreinte FNV-1a 64 des octets reçus — à comparer à celle de la zone « data » du WAV
 * de SC2_AUDIO_DUMP quand les deux sont actifs (mêmes octets attendus). SC2_AUDIO_COMPTEUR=1.
 */
#include "sc2_audio_sortie.h"

#include <stdio.h>

static struct { uint64_t frames, non_nuls, appels, fnv; int crete, freq; } C = { .fnv = 0xCBF29CE484222325ull };

static void compteur(const int16_t *pcm, size_t n, void *ctx)
{
    (void)ctx;
    const uint8_t *o = (const uint8_t *)pcm;
    for (size_t i = 0; i < 4 * n; i++) { C.fnv ^= o[i]; C.fnv *= 0x100000001B3ull; }
    for (size_t i = 0; i < 2 * n; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v) C.non_nuls++;
        if (v > C.crete) C.crete = v;
    }
    C.frames += n; C.appels++;
}

void sc2_audio_compteur_bilan(void)
{
    int f = sc2_audio_sortie_frequence();
    printf("[BILAN] [AUDIO] consommateur compteur : %llu frames reçues (%.2f s à %d Hz) en %llu livraisons, "
           "%llu échantillons non nuls, crête %d, FNV-1a 64 des octets 0x%016llx\n",
           (unsigned long long)C.frames, f ? (double)C.frames / f : 0.0, f, (unsigned long long)C.appels,
           (unsigned long long)C.non_nuls, C.crete, (unsigned long long)C.fnv);
    fflush(stdout);
}

static int g_installe;
int sc2_audio_compteur_installe(void) { return g_installe; }
void sc2_audio_compteur_installer(void)
{
    if (g_installe++) return;
    sc2_audio_sortie_definir(compteur, NULL);
}
