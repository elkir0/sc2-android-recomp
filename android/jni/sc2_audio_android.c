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
#ifdef __APPLE__                                   /* M6 : AUHAL, écouteurs sur files privées, RMS du bilan */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <Block.h>
#include <dispatch/dispatch.h>
#include <math.h>
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
static void sortie_pause(int p)              /* 21/10 : jeu suspendu en arrière-plan → flux en pause, repris ensuite */
{
    pthread_mutex_lock(&g_mu);
    if (g_flux) { if (p) AAudioStream_requestPause(g_flux); else AAudioStream_requestStart(g_flux); }
    pthread_mutex_unlock(&g_mu);
}
static void sortie_fermer(void) { pthread_mutex_lock(&g_mu); fermer(); pthread_mutex_unlock(&g_mu); }
static void sortie_bilan(char *o, size_t cap)
{
    pthread_mutex_lock(&g_mu);
    int32_t xr = g_flux ? AAudioStream_getXRunCount(g_flux) : -1;
    pthread_mutex_unlock(&g_mu);
    snprintf(o, cap, "AAudio %d Hz, rafale %d, tampon %d frames (%.1f ms), xruns AAudio %d, reconnexions %d", g_freq_reelle,
             g_burst, g_tampon, g_freq_reelle ? 1000.0 * g_tampon / g_freq_reelle : 0.0, xr, atomic_load(&g_reconnexions));
}

#elif defined(__APPLE__)
/* ---- sortie : CoreAudio AUHAL (macOS, lot M6) --------------------------------------------- */
/* L'AUHAL suit le périphérique de sortie par défaut. Côté client (port d'entrée, élément 0) : PCM16 signé
 * entrelacé stéréo à g_freq (sc2_audio_sortie_frequence(), 44 100) — l'unité convertit elle-même vers la
 * fréquence propre du périphérique. Le rappel est LE consommateur (lui seul avance g_lu, purges comprises) ;
 * les écouteurs CoreAudio vivent sur une file privée, jamais la run loop principale (aucune au banc).
 * SC2_MAC_SON=muet : la vraie unité, volume à zéro. Jamais de clock_nanosleep (absent du SDK Apple). */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static AudioUnit g_unite;                    /* NULL tant que jamais ouverte (fermer est idempotent) */
static dispatch_queue_t g_file_ecoute;       /* la file privée des écouteurs */
static void (^g_bloc_periph)(UInt32, const AudioObjectPropertyAddress *);   /* périphérique par défaut */
static void (^g_bloc_marche)(UInt32, const AudioObjectPropertyAddress *);   /* marche du périphérique */
static AudioDeviceID g_ecoute_marche;        /* périphérique où l'écouteur de marche est posé */
static AudioDeviceID g_periph;               /* périphérique par défaut courant (mesuré) */
static double g_freq_periph;                 /* sa fréquence propre */
static UInt32 g_tampon_periph;               /* son tampon, en frames */
static int g_muet;                           /* SC2_MAC_SON=muet : unité réelle au volume nul */
static long g_latence_ms = 150;              /* SC2_MAC_SON_LATENCE_MS : seuil de purge de l'anneau */
static atomic_int g_marche_transitions;      /* transitions de marche vues (diagnostic, cf. g_bloc_marche) */
static _Atomic uint64_t g_purges, g_purgees, g_carres;   /* purges, frames purgées, Σv² des rappels (RMS) */
static atomic_int g_changements, g_redemarrages;

static int ouvrir(void);
static void fermer(void);

/* anneau, pur calcul — éprouvé tel quel par mac/tests/test_son_anneau.c (qui inclut ce fichier) */
static inline uint64_t ms_en_frames(long ms, int freq) { return (uint64_t)freq * (uint64_t)ms / 1000u; }

/* Frames à sauter pour ramener un remplissage « dispo » à la cible de 60 ms dès que le seuil de latence
 * (SC2_MAC_SON_LATENCE_MS) est dépassé ; 0 sinon : sous le seuil, aucune purge. */
static inline uint64_t anneau_saut(uint64_t dispo, uint64_t seuil, uint64_t cible)
{ return dispo > seuil ? dispo - cible : 0; }

/* Purge vers 60 ms. En régime permanent, appelée du rappel (lui seul avance g_lu) ; à la reprise d'une
 * pause, du fil du jeu — l'unité est alors ARRÊTÉE (AudioOutputUnitStop rendu), son rappel ne tourne plus :
 * avancer g_lu hors de lui est sans course (le producteur, lui, n'y gagne que de la place). */
static uint64_t purger(void)
{
    uint64_t r = atomic_load_explicit(&g_lu, memory_order_relaxed);
    uint64_t dispo = atomic_load_explicit(&g_ecrit, memory_order_acquire) - r;
    uint64_t saut = anneau_saut(dispo, ms_en_frames(g_latence_ms, g_freq), ms_en_frames(60, g_freq));
    if (saut) {
        atomic_fetch_add_explicit(&g_purgees, saut, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_purges, 1, memory_order_relaxed);
        atomic_store_explicit(&g_lu, r + saut, memory_order_release);
    }
    return saut;
}

/* LE rappel : purge, service, Σv² pour le RMS — ni verrou, ni allocation, aucune API blocante. */
static OSStatus rappel_mac(void *ctx, AudioUnitRenderActionFlags *drapeaux, const AudioTimeStamp *quand,
                           UInt32 nbus, UInt32 n, AudioBufferList *sorties)
{
    (void)ctx; (void)quand; (void)nbus;
    int16_t *out = (int16_t *)sorties->mBuffers[0].mData;
    purger();
    if (atomic_load_explicit(&g_ecrit, memory_order_acquire) == atomic_load_explicit(&g_lu, memory_order_relaxed))
        *drapeaux |= kAudioUnitRenderAction_OutputIsSilence;      /* rien à jouer : l'anneau est vide */
    consommer(out, n);
    uint64_t s2 = 0;                                              /* Σv² du tampon servi (RMS du bilan) */
    for (UInt32 j = 0; j < n * 2; j++) { int32_t v = out[j]; s2 += (uint64_t)(v * v); }
    atomic_fetch_add_explicit(&g_carres, s2, memory_order_relaxed);
    return noErr;
}

/* Mesure le périphérique par défaut (identifiant, fréquence propre, tampon) et trace « client →
 * périphérique » — à l'ouverture ET à chaque changement : l'AUHAL convertit 44 100 → N Hz. */
static void decrire(void)                          /* sous g_mu */
{
    AudioDeviceID id = 0; UInt32 t = sizeof id;
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    double f = 0; UInt32 tampon = 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &t, &id)) return;
    a.mSelector = kAudioDevicePropertyNominalSampleRate; t = sizeof f;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &t, &f) || !f) return;
    a.mSelector = kAudioDevicePropertyBufferFrameSize; t = sizeof tampon;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &t, &tampon) || !tampon) return;
    g_periph = id; g_freq_periph = f; g_tampon_periph = tampon;
    sc2_log("[AUDIO] Mac : CoreAudio AUHAL, client %d Hz → périphérique %.0f Hz, tampon %u frames (%.1f ms)",
            g_freq, g_freq_periph, g_tampon_periph, 1000.0 * g_tampon_periph / g_freq_periph);
}

/* Écouteur de marche posé sur le périphérique par défaut courant (ôtant celui d'avant). */
static void ecouter_marche(AudioDeviceID id)       /* sous g_mu */
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    if (!g_file_ecoute || !g_bloc_marche || g_ecoute_marche == id) return;
    if (g_ecoute_marche) AudioObjectRemovePropertyListenerBlock(g_ecoute_marche, &a, g_file_ecoute, g_bloc_marche);
    g_ecoute_marche = id;
    AudioObjectAddPropertyListenerBlock(id, &a, g_file_ecoute, g_bloc_marche);
}

/* Les deux écouteurs, sur la file privée. Celui du périphérique par défaut (objet système) stoppe,
 * reconfigure et redémarre l'unité — changements et redémarrages comptés au bilan. Celui de marche ne
 * compte que des transitions : d'après AudioHardware.h, ses notifications partent du contexte d'E/S et
 * peuvent donc être distribuées de façon SYNCHRONE — aucune lecture de propriété ici, juste un atomique. */
static int ecouteurs_poser(void)                   /* sous g_mu, une seule fois */
{
    if (g_file_ecoute) return 0;
    g_file_ecoute = dispatch_queue_create("sc2.son", DISPATCH_QUEUE_SERIAL);
    if (!g_file_ecoute) return -1;
    g_bloc_marche = Block_copy(^(UInt32 nb, const AudioObjectPropertyAddress *a)
    {
        (void)nb; (void)a;
        atomic_fetch_add(&g_marche_transitions, 1);
    });
    g_bloc_periph = Block_copy(^(UInt32 nb, const AudioObjectPropertyAddress *a)
    {
        (void)nb; (void)a;
        pthread_mutex_lock(&g_mu);
        if (g_unite && !atomic_load(&g_arrete)) {
            fermer();
            atomic_fetch_add(&g_changements, 1);
            if (!ouvrir()) atomic_fetch_add(&g_redemarrages, 1);
        }
        pthread_mutex_unlock(&g_mu);
    });
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    return AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, g_file_ecoute, g_bloc_periph) ? -1 : 0;
}

/* Retrait des écouteurs — HORS g_mu : un rappel en vol attend ce verrou, ne jamais l'y attendre. */
static void ecouteurs_oter(void)
{
    if (!g_file_ecoute) return;
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    if (g_bloc_periph) AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &a, g_file_ecoute,
                                                              g_bloc_periph);
    a.mSelector = kAudioDevicePropertyDeviceIsRunning;
    if (g_bloc_marche && g_ecoute_marche)
        AudioObjectRemovePropertyListenerBlock(g_ecoute_marche, &a, g_file_ecoute, g_bloc_marche);
    g_ecoute_marche = 0;    /* blocs et file jamais libérés : on ne les ôte qu'à la sortie du processus */
}

static void fermer(void)                           /* sous g_mu : idempotent */
{
    if (!g_unite) return;
    AudioOutputUnitStop(g_unite);
    AudioUnitUninitialize(g_unite);
    AudioComponentInstanceDispose(g_unite);
    g_unite = NULL;
}

static int ouvrir(void)                            /* sous g_mu : crée, démarre, décrit */
{
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, 0, 0, 0 };
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    AudioUnit u = NULL;
    OSStatus r;
    if (!c) { sc2_log("[AUDIO] Mac : composant AUHAL introuvable"); return -1; }
    if ((r = AudioComponentInstanceNew(c, &u))) { sc2_log("[AUDIO] Mac : AudioComponentInstanceNew → %d", (int)r); return -1; }
    AudioStreamBasicDescription client;            /* le côté client : PCM16 signé entrelacé stéréo */
    memset(&client, 0, sizeof client);
    client.mSampleRate = g_freq;                   /* l'unité convertit vers la fréquence du périphérique */
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    client.mBytesPerPacket = 4; client.mFramesPerPacket = 1;
    client.mBytesPerFrame = 4; client.mChannelsPerFrame = 2; client.mBitsPerChannel = 16;
    r = AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &client, sizeof client);
    if (!r) {
        AURenderCallbackStruct rappel = { rappel_mac, NULL };
        r = AudioUnitSetProperty(u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                 &rappel, sizeof rappel);
    }
    if (!r && g_muet) r = AudioUnitSetParameter(u, kHALOutputParam_Volume, kAudioUnitScope_Global, 0, 0.0f, 0);
    if (!r) r = AudioUnitInitialize(u);
    if (!r) r = AudioOutputUnitStart(u);
    if (r) {
        sc2_log("[AUDIO] Mac : ouverture AUHAL → %d", (int)r);
        AudioComponentInstanceDispose(u);
        return -1;
    }
    g_unite = u;
    decrire();
    ecouter_marche(g_periph);
    return 0;
}

static int sortie_ouvrir(void)
{
    const char *l = getenv("SC2_MAC_SON_LATENCE_MS");
    if (l && *l) { long v = strtol(l, NULL, 10); if (v >= 1 && v <= 10000) g_latence_ms = v; }
    const char *m = getenv("SC2_MAC_SON");
    g_muet = m && !strcmp(m, "muet");
    pthread_mutex_lock(&g_mu);
    if (ecouteurs_poser()) sc2_log("[AUDIO] Mac : écouteurs impossibles — le périphérique par défaut ne sera pas suivi");
    int r = ouvrir();
    pthread_mutex_unlock(&g_mu);
    return r;
}
static void sortie_pause(int p)                    /* suspendu : unité arrêtée ; reprise : purge PUIS départ */
{
    pthread_mutex_lock(&g_mu);
    if (g_unite) {
        if (p) AudioOutputUnitStop(g_unite);
        else { purger(); AudioOutputUnitStart(g_unite); }    /* avant Start : l'anneau redescend à 60 ms */
    }
    pthread_mutex_unlock(&g_mu);
}
static void sortie_fermer(void)
{
    ecouteurs_oter();                              /* hors g_mu (cf. ci-dessus), puis unité jetée sous g_mu */
    pthread_mutex_lock(&g_mu);
    fermer();
    pthread_mutex_unlock(&g_mu);
}
static void sortie_bilan(char *o, size_t cap)
{
    uint64_t j = atomic_load(&g_jouees);
    double rms = j ? 20.0 * log10(sqrt((double)atomic_load(&g_carres) / (2.0 * (double)j)) / 32768.0) : -120.0;
    snprintf(o, cap, "CoreAudio AUHAL : client %d Hz → périphérique %.0f Hz, tampon %u frames (%.1f ms), "
             "reçues %llu, jouées %llu, purgées %llu (purges %llu), débordements %llu, échantillons non nuls %llu, "
             "RMS du rappel %.1f dBFS, changements de périphérique %d, redémarrages %d", g_freq, g_freq_periph,
             g_tampon_periph, g_freq_periph ? 1000.0 * g_tampon_periph / g_freq_periph : 0.0,
             (unsigned long long)atomic_load(&g_recues), (unsigned long long)j,
             (unsigned long long)atomic_load(&g_purgees), (unsigned long long)atomic_load(&g_purges),
             (unsigned long long)atomic_load(&g_debord), (unsigned long long)atomic_load(&g_non_nuls), rms,
             atomic_load(&g_changements), atomic_load(&g_redemarrages));
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
static void sortie_pause(int p) { (void)p; }
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

void sc2aa_pause(int p)
{
    if (atomic_load(&g_pose) && !atomic_load(&g_arrete)) sortie_pause(p);
}

void sc2aa_arreter(void)
{
    if (!atomic_load(&g_pose) || atomic_exchange(&g_arrete, 1)) return;
    sc2_audio_sortie_definir(NULL, NULL);
#ifdef __APPLE__
    /* M6 : le bilan CoreAudio (périphérique, purges, RMS, redémarrages…) remplace la ligne Android. */
    char b[640]; sortie_bilan(b, sizeof b);
    sortie_fermer();
    printf("[BILAN] [AUDIO] Mac : %s\n", b);
    fflush(stdout);
#else
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
#endif
}
