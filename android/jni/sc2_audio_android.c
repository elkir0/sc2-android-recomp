/* sc2_audio_android.c — sortie audio Android par AAudio, consommateur du rappel 5g (passe 7e).
 *
 *   fil du jeu (System::update, verrou du mixeur tenu)          rappel AAudio (fil temps réel)
 *     sc2_audio_sortie → produire() ──► anneau SPSC ──► consommer() ──► tampon du flux
 *
 * Anneau : un producteur (le fil du jeu), un consommateur (le rappel AAudio) ; indices 64 bits
 * atomiques, aucun verrou, aucune allocation ni attente dans les deux rappels. Capacité 32 768
 * frames stéréo (743 ms à 44,1 kHz, ≥ les 250 ms maximum d'un bloc du mixeur).
 *   - plein : les frames qui ne tiennent pas sont jetées et comptées (« débordements ») ;
 *   - vide : silence, compté en « sous-alimentation » une fois le flux amorcé (1re frame reçue).
 * AAudio : 44 100 Hz (sc2_audio_sortie_frequence()), 2 canaux, PCM16, LowLatency, partagé. Sur
 * déconnexion (casque, Bluetooth, changement de périphérique), le rappel d'erreur lance un fil qui
 * ferme et rouvre le flux — jamais dans le rappel lui-même (exigence AAudio).
 * Banc (pas d'AAudio hors Android) : un fil tire des rafales de 441 frames toutes les 10 ms au
 * rythme réel, par le MÊME consommer() — l'anneau et ses compteurs sont éprouvés tels quels.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

#include "sc2_audio_sortie.h"
#include "sc2_audio_android.h"

void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define CAP 32768u                                   /* frames stéréo, puissance de 2 */

static int16_t g_anneau[CAP * 2];
static _Atomic uint64_t g_ecrit, g_lu;               /* frames écrites / lues depuis le début */
static int g_freq;                                   /* fréquence du flux ouvert */
static atomic_int g_pose, g_amorce, g_arrete;
static _Atomic uint64_t g_recues, g_livraisons, g_debord, g_jouees, g_silence, g_sous_alim, g_non_nuls,
                        g_rappels, g_remplissage_cumul;

/* ---- anneau ------------------------------------------------------------------------------- */

static void produire(const int16_t *pcm, size_t n, void *ctx)     /* fil du jeu, verrou du mixeur tenu */
{
    (void)ctx;
    atomic_fetch_add_explicit(&g_recues, n, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_livraisons, 1, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&g_ecrit, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&g_lu, memory_order_acquire);
    uint64_t libre = CAP - (w - r);
    if (n > libre) { atomic_fetch_add_explicit(&g_debord, n - libre, memory_order_relaxed); n = (size_t)libre; }
    uint32_t i = (uint32_t)(w & (CAP - 1)), k = (uint32_t)(n < CAP - i ? n : CAP - i);
    memcpy(&g_anneau[i * 2], pcm, (size_t)k * 4);
    if (n > k) memcpy(&g_anneau[0], pcm + k * 2, (n - k) * 4);
    atomic_store_explicit(&g_ecrit, w + n, memory_order_release);
    if (n) atomic_store_explicit(&g_amorce, 1, memory_order_relaxed);
}

static void consommer(int16_t *out, uint32_t n)                    /* rappel temps réel */
{
    uint64_t r = atomic_load_explicit(&g_lu, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&g_ecrit, memory_order_acquire);
    uint64_t dispo = w - r;
    atomic_fetch_add_explicit(&g_rappels, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_remplissage_cumul, dispo, memory_order_relaxed);
    uint32_t m = (uint32_t)(dispo < n ? dispo : n);
    uint32_t i = (uint32_t)(r & (CAP - 1)), k = m < CAP - i ? m : CAP - i;
    memcpy(out, &g_anneau[i * 2], (size_t)k * 4);
    if (m > k) memcpy(out + k * 2, &g_anneau[0], (size_t)(m - k) * 4);
    atomic_store_explicit(&g_lu, r + m, memory_order_release);
    uint64_t nz = 0;
    for (uint32_t j = 0; j < m * 2; j++) nz += out[j] != 0;
    atomic_fetch_add_explicit(&g_non_nuls, nz, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_jouees, m, memory_order_relaxed);
    if (m < n) {
        memset(out + m * 2, 0, (size_t)(n - m) * 4);
        if (atomic_load_explicit(&g_amorce, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&g_silence, n - m, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_sous_alim, 1, memory_order_relaxed);
        }
    }
}

/* ---- sortie : AAudio (Android) ------------------------------------------------------------ */

#ifdef __ANDROID__
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static AAudioStream *g_flux;
static atomic_int g_reconnexions, g_reconnexion_en_cours;
static int32_t g_burst, g_tampon, g_capacite, g_mode_perf, g_freq_reelle;

static aaudio_data_callback_result_t rappel_donnees(AAudioStream *s, void *ctx, void *audio, int32_t n)
{
    (void)s; (void)ctx;
    consommer((int16_t *)audio, (uint32_t)n);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void *fil_reconnexion(void *p);
static void rappel_erreur(AAudioStream *s, void *ctx, aaudio_result_t err)
{
    (void)s; (void)ctx;
    /* Pas de close() ici (interdit dans un rappel AAudio) : un fil s'en charge. */
    if (err == AAUDIO_ERROR_DISCONNECTED && !atomic_load(&g_arrete) && !atomic_exchange(&g_reconnexion_en_cours, 1)) {
        pthread_t t;
        if (pthread_create(&t, NULL, fil_reconnexion, NULL)) atomic_store(&g_reconnexion_en_cours, 0);
        else pthread_detach(t);
    }
}

static int ouvrir(void)                              /* sous g_mu */
{
    AAudioStreamBuilder *b;
    aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) { sc2_log("[AUDIO] Android : AAudio_createStreamBuilder → %s", AAudio_convertResultToText(r)); return -1; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, g_freq);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(b, rappel_donnees, NULL);
    AAudioStreamBuilder_setErrorCallback(b, rappel_erreur, NULL);
    r = AAudioStreamBuilder_openStream(b, &g_flux);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { sc2_log("[AUDIO] Android : openStream → %s", AAudio_convertResultToText(r)); g_flux = NULL; return -1; }
    g_burst = AAudioStream_getFramesPerBurst(g_flux);
    g_capacite = AAudioStream_getBufferCapacityInFrames(g_flux);
    /* 2 rafales de tampon : la latence basse recommandée par AAudio (réajustable si sous-alimentations) */
    AAudioStream_setBufferSizeInFrames(g_flux, g_burst * 2);
    g_tampon = AAudioStream_getBufferSizeInFrames(g_flux);
    g_mode_perf = AAudioStream_getPerformanceMode(g_flux);
    g_freq_reelle = AAudioStream_getSampleRate(g_flux);
    r = AAudioStream_requestStart(g_flux);
    if (r != AAUDIO_OK) {
        sc2_log("[AUDIO] Android : requestStart → %s", AAudio_convertResultToText(r));
        AAudioStream_close(g_flux); g_flux = NULL; return -1;
    }
    sc2_log("[AUDIO] Android : flux AAudio ouvert — %d Hz (demandé %d), 2 canaux PCM16, %s, rafale %d frames, "
            "tampon %d frames (capacité %d) → latence estimée %.1f ms + anneau", g_freq_reelle, g_freq,
            g_mode_perf == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "LowLatency" : "mode de performance NON LowLatency",
            g_burst, g_tampon, g_capacite, g_freq_reelle ? 1000.0 * g_tampon / g_freq_reelle : 0.0);
    return 0;
}

static void fermer(void)                             /* sous g_mu */
{
    if (!g_flux) return;
    AAudioStream_requestStop(g_flux);
    AAudioStream_close(g_flux);
    g_flux = NULL;
}

static void *fil_reconnexion(void *p)
{
    (void)p;
    pthread_mutex_lock(&g_mu);
    if (!atomic_load(&g_arrete)) {
        fermer();
        int n = atomic_fetch_add(&g_reconnexions, 1) + 1;
        sc2_log("[AUDIO] Android : flux déconnecté (changement de périphérique) — réouverture n°%d : %s", n,
                ouvrir() ? "ÉCHEC" : "OK");
    }
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_reconnexion_en_cours, 0);
    return NULL;
}

static int sortie_ouvrir(void) { pthread_mutex_lock(&g_mu); int r = ouvrir(); pthread_mutex_unlock(&g_mu); return r; }
static void sortie_fermer(void) { pthread_mutex_lock(&g_mu); fermer(); pthread_mutex_unlock(&g_mu); }
static void sortie_bilan(char *o, size_t cap)
{
    pthread_mutex_lock(&g_mu);
    int32_t xr = g_flux ? AAudioStream_getXRunCount(g_flux) : -1;
    pthread_mutex_unlock(&g_mu);
    snprintf(o, cap, "AAudio %d Hz, rafale %d, tampon %d frames (%.1f ms), xruns AAudio %d, reconnexions %d", g_freq_reelle,
             g_burst, g_tampon, g_freq_reelle ? 1000.0 * g_tampon / g_freq_reelle : 0.0, xr, atomic_load(&g_reconnexions));
}

#else
/* ---- sortie : banc (pas d'AAudio) — pseudo-rappel au rythme réel ------------------------- */
#define RAFALE 441                                   /* 10 ms à 44,1 kHz */
static pthread_t g_fil; static int g_fil_ok;
static void *fil_banc(void *p)
{
    (void)p;
    static int16_t tampon[RAFALE * 2];
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    long pas = (long)(1e9 * RAFALE / g_freq);
    while (!atomic_load(&g_arrete)) {
        consommer(tampon, RAFALE);
        t.tv_nsec += pas; while (t.tv_nsec >= 1000000000L) { t.tv_nsec -= 1000000000L; t.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    }
    return NULL;
}
static int sortie_ouvrir(void)
{
    g_fil_ok = !pthread_create(&g_fil, NULL, fil_banc, NULL);
    sc2_log("[AUDIO] Android (banc) : pas d'AAudio hors Android — pseudo-rappel au rythme réel, rafale %d frames "
            "(%.1f ms) à %d Hz, même anneau et même consommateur que l'APK : %s", RAFALE, 1000.0 * RAFALE / g_freq, g_freq,
            g_fil_ok ? "démarré" : "ÉCHEC");
    return g_fil_ok ? 0 : -1;
}
static void sortie_fermer(void) { if (g_fil_ok) { pthread_join(g_fil, NULL); g_fil_ok = 0; } }
static void sortie_bilan(char *o, size_t cap)
{ snprintf(o, cap, "banc : pseudo-rappel %d Hz, rafale %d frames (%.1f ms)", g_freq, RAFALE, 1000.0 * RAFALE / g_freq); }
#endif

/* ---- démarrage / arrêt ---------------------------------------------------------------------- */

int sc2aa_demarrer(void)
{
    const char *e = getenv("SC2_AUDIO_ANDROID");
    if (e && *e == '0') { sc2_log("[AUDIO] Android : SC2_AUDIO_ANDROID=0 — aucune sortie posée (pas de son)"); return -1; }
    if (atomic_exchange(&g_pose, 1)) return 0;
    g_freq = sc2_audio_sortie_frequence();
    /* Consommateur posé AVANT le premier System::update : aucune frame mixée n'est perdue. */
    sc2_audio_sortie_definir(produire, NULL);
    sc2_log("[AUDIO] Android : consommateur posé sur la sortie 5g (anneau %u frames = %.0f ms à %d Hz)", CAP,
            1000.0 * CAP / g_freq, g_freq);
    if (sortie_ouvrir()) sc2_log("[AUDIO] Android : sortie indisponible — l'anneau se remplit puis déborde (compté), le jeu continue");
    atexit(sc2aa_arreter);
    return 0;
}

void sc2aa_arreter(void)
{
    if (!atomic_load(&g_pose) || atomic_exchange(&g_arrete, 1)) return;
    sc2_audio_sortie_definir(NULL, NULL);
    char b[256]; sortie_bilan(b, sizeof b);
    sortie_fermer();
    uint64_t rap = atomic_load(&g_rappels), rem = atomic_load(&g_remplissage_cumul);
    double f = g_freq ? (double)g_freq : 44100.0;
    printf("[BILAN] [AUDIO] Android : %s ; reçues %llu frames (%.2f s) en %llu livraisons, jouées %llu (%llu échantillons non nuls), "
           "silence %llu frames en %llu sous-alimentations (après amorçage), débordements %llu, remplissage moyen de l'anneau "
           "%.1f ms sur %llu rappels%s\n", b,
           (unsigned long long)atomic_load(&g_recues), atomic_load(&g_recues) / f, (unsigned long long)atomic_load(&g_livraisons),
           (unsigned long long)atomic_load(&g_jouees), (unsigned long long)atomic_load(&g_non_nuls),
           (unsigned long long)atomic_load(&g_silence), (unsigned long long)atomic_load(&g_sous_alim),
           (unsigned long long)atomic_load(&g_debord), rap ? 1000.0 * (double)rem / (double)rap / f : 0.0,
           (unsigned long long)rap, sc2_audio_sortie_frequence() != g_freq ? " ; ⚠ fréquence du mixeur ≠ flux" : "");
    fflush(stdout);
}
