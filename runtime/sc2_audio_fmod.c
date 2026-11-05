/* sc2_audio_fmod.c — FMOD Ex 4.28.08 implémenté pour le jeu (passe 5e).
 *
 * Étape A (ce qui rend l'API SÛRE) : chacun des 129 imports fmodex a un trampoline ; un
 * appel est compté (bilan [AUDIO]), journalisé (2 premiers), puis traité :
 *   - méthode implémentée (table g_impl) → son gestionnaire ;
 *   - sinon gestionnaire générique, guidé par la signature décodée du nom MSVC
 *     (sc2_audio_fmod_table.h) : chaque sortie scalaire reçoit 0, chaque sortie objet
 *     (Sound**, Channel**…) un objet FMOD neuf ; rend FMOD_OK.
 * Les objets FMOD sont des blocs de l'arène invitée : [0] 'FMOD', [4] genre, [8] index hôte.
 * Étape B (lecture réelle) : sc2_audio_mix.c (sons, canaux, mixage, WAV, sortie).
 */
#define _GNU_SOURCE
#include "sc2_audio.h"
#include "sc2_audio_int.h"
#include "sc2_audio_fmod_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A(i) (a[(i)])
#define FMOD_MAGIC 0x444F4D46u                 /* « FMOD » */

static uint64_t g_n[SC2_FMOD_NB];
static imp_def g_defs[SC2_FMOD_NB];
static int g_actif = -1;
typedef uint32_t (*fmod_fn)(CPU *c, const uint32_t *a);
static fmod_fn g_impl[SC2_FMOD_NB];

static int genre_de(const char *court)
{
    static const char *g[] = { "?", "System", "Sound", "Channel", "ChannelGroup", "DSP", "DSPConnection",
                               "Reverb", "SoundGroup", "Geometry", "SyncPoint" };
    for (int k = 1; k < (int)(sizeof g / sizeof g[0]); k++) {
        size_t n = strlen(g[k]);
        if (!strncmp(court, g[k], n) && (court[n] == ':' || court[n] == '*')) return k;
    }
    return 0;
}

uint32_t sc2_fmod_objet(int genre, int index)
{
    uint32_t o = sc2_alloc(16);
    wr32(o, FMOD_MAGIC); wr32(o + 4, (uint32_t)genre); wr32(o + 8, (uint32_t)index);
    return o;
}
int sc2_fmod_index(uint32_t o, int genre)
{
    if (!o || rd32(o) != FMOD_MAGIC || (genre && rd32(o + 4) != (uint32_t)genre)) return -1;
    return (int)rd32(o + 8);
}

/* genre de l'objet rendu par une sortie 'O' : d'après le type décodé du paramètre */
static int genre_sortie(const char *court, int rang)
{
    /* noms des types de sortie objet, dans l'ordre des paramètres 'O' de la signature */
    static const struct { const char *meth; int genre; } t[] = {
        { "System::createSound", 2 }, { "System::createStream", 2 }, { "System::playSound", 3 },
        { "System::playDSP", 3 }, { "System::createChannelGroup", 4 }, { "System::getMasterChannelGroup", 4 },
        { "System::createDSP", 5 }, { "System::createDSPByType", 5 }, { "System::addDSP", 6 },
        { "ChannelGroup::addDSP", 6 }, { "Channel::addDSP", 6 }, { "Channel::getSystemObject", 1 },
        { "Sound::getSubSound", 2 }, { "Sound::getSyncPoint", 10 }, { "System::createSoundGroup", 8 },
        { "System::getMasterSoundGroup", 8 }, { "System::createReverb", 7 }, { "System::createGeometry", 9 },
    };
    (void)rang;
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) if (!strcmp(court, t[k].meth)) return t[k].genre;
    return 0;
}

static uint32_t generique(int k, CPU *c, const uint32_t *a)
{
    const char *nat = SC2_FMOD[k].nature;
    int ai = 1, rang = 0;                      /* a[0] = this */
    for (const char *p = nat; *p; p++) {
        switch (*p) {
        case 'd': ai += 2; break;
        case 'o': if (A(ai)) wr32(A(ai), 0); ai++; break;
        case 'O': {
            int g = genre_sortie(SC2_FMOD[k].court, rang++);
            if (A(ai)) wr32(A(ai), sc2_fmod_objet(g ? g : genre_de(SC2_FMOD[k].court), -1));
            ai++; break;
        }
        default: ai++;
        }
    }
    (void)c;
    return 0;                                   /* FMOD_OK */
}

/* 24/10 : SC2_AUDIO=0 — FMOD « absent » : FMOD_System_Create échoue (FMOD_ERR_OUTPUT_INIT, sc2_iat_p4.c) mais le jeu
 * appelle quand même des méthodes (System::createDSP depuis 0x009E9420 : arrêt « NON IMPLÉMENTÉ » à 3 s). Toute méthode
 * rend alors FMOD_ERR_INVALID_HANDLE (36), sorties à 0 : ni objet, ni son. */
static uint32_t refus(int k, const uint32_t *a)
{
    int ai = 1;                                 /* a[0] = this */
    for (const char *p = SC2_FMOD[k].nature; *p; p++) {
        if (*p == 'd') { ai += 2; continue; }
        if ((*p == 'o' || *p == 'O') && A(ai)) wr32(A(ai), 0);
        ai++;
    }
    return 36;                                  /* FMOD_ERR_INVALID_HANDLE */
}

static uint32_t appel(int k, CPU *c, const uint32_t *a)
{
    if (!g_actif) { __atomic_add_fetch(&g_n[k], 1, __ATOMIC_RELAXED); return refus(k, a); }
    uint64_t n = __atomic_add_fetch(&g_n[k], 1, __ATOMIC_RELAXED);
    if (n <= 2) {
        char args[160] = ""; size_t o = 0;
        for (int j = 0; j < SC2_FMOD[k].mots && j < 9; j++)
            o += (size_t)snprintf(args + o, sizeof args - o, "%s0x%X", j ? ", " : "", a[j]);
        sc2_trace("[AUDIO] FMOD::%s(%s)  ← 0x%08X%s", SC2_FMOD[k].court, args, rd32(c->esp), n == 2 ? "  (suivants comptés)" : "");
    }
    return g_impl[k] ? g_impl[k](c, a) : generique(k, c, a);
}

#define TRAMPOLINE(k) static uint32_t t_##k(CPU *c, const uint32_t *a) { return appel(k, c, a); }
SC2_FMOD_INDICES(TRAMPOLINE)
#define ADRESSE(k) t_##k,
static const imp_fn g_tramp[SC2_FMOD_NB] = { SC2_FMOD_INDICES(ADRESSE) };

/* ---- fonctions libres --------------------------------------------------------- */
static uint32_t f_System_Create(CPU *c, const uint32_t *a)          /* (System**) */
{
    (void)c;
    /* Passe 7v : un index par System créé (le jeu en crée plusieurs : son principal, sondes, voix à
     * 8 000 Hz) — le mixeur ne suit que le System maître (sc2_audio_mix.c). */
    static int n;
    if (A(0)) wr32(A(0), sc2_fmod_objet(1, __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED)));
    sc2_audio_systeme_cree();
    return 0;
}
static uint32_t f_Memory_GetStats(CPU *c, const uint32_t *a)        /* (int*, int*, bool) */
{ (void)c; if (A(0)) wr32(A(0), 0); if (A(1)) wr32(A(1), 0); return 0; }
static uint32_t f_ok(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }

/* ---- System : description du « matériel » ---------------------------------------- */
static uint32_t m_getVersion(CPU *c, const uint32_t *a) { (void)c; if (A(1)) wr32(A(1), 0x00042808u); return 0; }
static uint32_t m_getNumDrivers(CPU *c, const uint32_t *a) { (void)c; if (A(1)) wr32(A(1), 1); return 0; }
static uint32_t m_getRecordNumDrivers(CPU *c, const uint32_t *a) { (void)c; if (A(1)) wr32(A(1), 0); return 0; }
static uint32_t m_getDriverInfoW(CPU *c, const uint32_t *a)          /* (id, wchar_t* nom, longueur, GUID*) */
{
    (void)c;
    static const char nom[] = "SC2 Partout - FMOD logiciel";
    if (A(2) && A(3) > 0) {
        uint32_t n = A(3) - 1 < sizeof nom - 1 ? A(3) - 1 : (uint32_t)sizeof nom - 1;
        for (uint32_t i = 0; i < n; i++) wr16(A(2) + 2 * i, (uint16_t)nom[i]);
        wr16(A(2) + 2 * n, 0);
    }
    if (A(4)) memset(GPTR(A(4)), 0, 16);
    return 0;
}
static uint32_t m_getDriverInfo(CPU *c, const uint32_t *a)           /* (id, char* nom, longueur, GUID*) */
{
    (void)c;
    static const char nom[] = "SC2 Partout - FMOD logiciel";
    if (A(2) && A(3) > 0) {
        uint32_t n = A(3) - 1 < sizeof nom - 1 ? A(3) - 1 : (uint32_t)sizeof nom - 1;
        memcpy(GPTR(A(2)), nom, n); wr8(A(2) + n, 0);
    }
    if (A(4)) memset(GPTR(A(4)), 0, 16);
    return 0;
}
static uint32_t m_getDriverCaps(CPU *c, const uint32_t *a)           /* (id, caps*, minfreq*, maxfreq*, speakermode*) */
{
    (void)c;
    /* Le jeu (0x009EB25B…0x009EB2B1) choisit son format de sortie dans les bits de capacité
     * FMOD_CAPS_OUTPUT_FORMAT_* (PCMFLOAT 0x80 … PCM8 0x08) : sans aucun, il renonce et
     * libère le System. On annonce PCM16 seul → format FMOD_SOUND_FORMAT_PCM16. */
    if (A(2)) wr32(A(2), 0x10u);
    if (A(3)) wr32(A(3), 8000);
    if (A(4)) wr32(A(4), 48000);
    if (A(5)) wr32(A(5), 2);                                         /* FMOD_SPEAKERMODE_STEREO */
    return 0;
}
static uint32_t m_getOutput(CPU *c, const uint32_t *a) { (void)c; if (A(1)) wr32(A(1), 6); return 0; }   /* DSOUND */
static uint32_t m_getDSPBufferSize(CPU *c, const uint32_t *a) { (void)c; if (A(1)) wr32(A(1), 1024); if (A(2)) wr32(A(2), 4); return 0; }
static uint32_t m_getStreamBufferSize(CPU *c, const uint32_t *a) { (void)c; if (A(1)) wr32(A(1), 16384); if (A(2)) wr32(A(2), 2); return 0; }
static uint32_t m_getSoftwareFormat(CPU *c, const uint32_t *a)       /* (rate*, format*, nbsorties*, maxentrées*, resampler*, bits*) */
{
    (void)c;
    /* Passe 7w7 : le format RÉEL du mixeur. Avant : 48 000 Hz constants (le mixeur tourne à la fréquence de
     * setSoftwareFormat, 44 100 Hz) et 0 canal de sortie. Le « MovieRecordingDSP » du jeu (0x009E7340) dimensionne
     * son anneau à fréquence × canaux puis divise par ce produit dans son rappel read (0x009E72A3) : 0 canal =
     * division par zéro au 1er bloc. setSoftwareFormat(…, numoutputchannels 0) = selon le mode haut-parleurs
     * (setSpeakerMode 2 = stéréo) → 2. */
    if (A(1)) wr32(A(1), (uint32_t)sc2_audio_freq());
    if (A(2)) wr32(A(2), 2);                                         /* FMOD_SOUND_FORMAT_PCM16 */
    if (A(3)) wr32(A(3), 2);
    if (A(4)) wr32(A(4), 6);
    if (A(5)) wr32(A(5), 1);                                         /* LINEAR */
    if (A(6)) wr32(A(6), 16);
    return 0;
}

/* ---- DSP (passe 7w6) ---------------------------------------------------------------
 * Le jeu crée ses propres DSP (System::createDSP + FMOD_DSP_DESCRIPTION 4.28 : name[32], version, channels,
 * create, release, reset, read, setposition, numparameters, paramdesc, setparameter, getparameter, config,
 * configwidth, configheight, userdata — 92 octets). Deux sortes, lues dans SC2.exe :
 *   - GÉNÉRATEUR 0x009E9280 (site 0x009E941B), joué par System::playDSP (site 0x009E9457) : son rappel read
 *     demande à sa source (vtbl[1]) longueur × canaux flottants et AVANCE sa position [userdata + 0xD0] ;
 *   - EFFET 0x009E7260 (site 0x009E73D6), posé par System::addDSP : recopie l'entrée vers la sortie, la range
 *     dans un anneau (0x0175AA60) et signale un événement passé un seuil.
 * Sans mixeur qui appelle read, ces positions restaient FIGÉES (hypothèse 7w4 : l'horloge vidéo suit le DSP).
 * Ici : registre des DSP, DSP::getUserData, rappels create/release, et sc2_dsp_lire / sc2_dsp_effets appelés
 * par le mixeur (sc2_audio_mix.c) par blocs de 1 024 frames (= getDSPBufferSize). SC2_TRACE_DSP=1 : traces. */
#define MAX_DSP 64
#define DSP_BLOC 1024
static struct {
    int vivant, actif, systeme_dsp, canaux, type;   /* type : -1 personnalisé, sinon FMOD_DSP_TYPE */
    uint32_t objet, etat, rappel_lire, rappel_creer, rappel_liberer, userdata, entree, sortie;
    char nom[33];
    uint64_t lectures, frames;
} g_dsp[MAX_DSP];
static int g_ndsp;
static uint64_t g_dsp_appels_lire, g_dsp_effets;
static int trace_dsp(void) { static int v = -1; if (v < 0) { const char *e = getenv("SC2_TRACE_DSP"); v = e && *e == '1'; } return v; }
/* SC2_DSP=0 : rappels read des DSP du jeu NON appelés (témoin A/B, comportement d'avant la 7w6) */
static int dsp_coupe(void) { static int v = -1; if (v < 0) { const char *e = getenv("SC2_DSP"); v = e && *e == '0';
                                                             if (v) sc2_log("[DSP] SC2_DSP=0 : rappels read des DSP du jeu coupés (témoin)"); } return v; }
int sc2_dsp_index(uint32_t o) { int i = sc2_fmod_index(o, 5); return i >= 0 && i < g_ndsp && g_dsp[i].vivant ? i : -1; }
static int dsp_nouveau(int type, uint32_t *objet)
{
    if (g_ndsp >= MAX_DSP) { *objet = sc2_fmod_objet(5, -1); return -1; }
    int i = g_ndsp++;
    memset(&g_dsp[i], 0, sizeof g_dsp[i]);
    g_dsp[i].vivant = 1; g_dsp[i].actif = 1; g_dsp[i].type = type;
    g_dsp[i].objet = *objet = sc2_fmod_objet(5, i);
    g_dsp[i].etat = sc2_alloc(16);                         /* FMOD_DSP_STATE : instance, plugindata, speakermask */
    wr32(g_dsp[i].etat, g_dsp[i].objet);
    wr16(g_dsp[i].etat + 8, 3);                            /* avant gauche + avant droit */
    return i;
}
static uint32_t m_createDSP(CPU *c, const uint32_t *a)                /* (FMOD_DSP_DESCRIPTION*, DSP**) */
{
    uint32_t d = A(1), o = 0;
    if (!d) return 37;                                                 /* FMOD_ERR_INVALID_PARAM */
    int i = dsp_nouveau(-1, &o);
    if (i >= 0) {
        memcpy(g_dsp[i].nom, GPTR(d), 32); g_dsp[i].nom[32] = 0;
        g_dsp[i].canaux = (int)rd32(d + 36);
        g_dsp[i].rappel_creer = rd32(d + 40); g_dsp[i].rappel_liberer = rd32(d + 44);
        g_dsp[i].rappel_lire = rd32(d + 52); g_dsp[i].userdata = rd32(d + 88);
        sc2_log("[DSP] createDSP n°%d « %s » : version %u, %d canaux, read 0x%08X, setposition 0x%08X, create 0x%08X, "
                "userdata 0x%08X (appelant 0x%08X) → DSP 0x%08X", i, g_dsp[i].nom, rd32(d + 32), g_dsp[i].canaux,
                g_dsp[i].rappel_lire, rd32(d + 56), g_dsp[i].rappel_creer, g_dsp[i].userdata, rd32(c->esp), o);
    }
    if (A(2)) wr32(A(2), o);
    if (i >= 0 && g_dsp[i].rappel_creer) { uint32_t e = g_dsp[i].etat; sc2_call_guest(c, g_dsp[i].rappel_creer, 1, &e, 0); }
    return 0;
}
static uint32_t m_createDSPByType(CPU *c, const uint32_t *a)          /* (FMOD_DSP_TYPE, DSP**) : effet intégré, neutre */
{
    uint32_t o = 0; int i = dsp_nouveau((int)A(1), &o);
    if (i >= 0) snprintf(g_dsp[i].nom, sizeof g_dsp[i].nom, "intégré type %u", A(1));
    if (trace_dsp()) sc2_log("[DSP] createDSPByType(type %u) → DSP 0x%08X n°%d (sans traitement) ← 0x%08X", A(1), o, i, rd32(c->esp));
    if (A(2)) wr32(A(2), o);
    return 0;
}
static uint32_t d_getUserData(CPU *c, const uint32_t *a)              /* (DSP*, void**) */
{
    (void)c; int i = sc2_dsp_index(A(0));
    if (A(1)) wr32(A(1), i >= 0 ? g_dsp[i].userdata : 0);
    return i >= 0 ? 0 : 36;                                            /* FMOD_ERR_INVALID_HANDLE */
}
static uint32_t d_setActive(CPU *c, const uint32_t *a)
{
    (void)c; int i = sc2_dsp_index(A(0));
    if (i >= 0) g_dsp[i].actif = A(1) & 0xFF;
    if (trace_dsp()) sc2_log("[DSP] DSP n°%d setActive(%u)", i, A(1) & 0xFF);
    return i >= 0 ? 0 : 36;
}
static uint32_t d_setParameter(CPU *c, const uint32_t *a)             /* (index, float) */
{
    (void)c; int i = sc2_dsp_index(A(0)); float v; memcpy(&v, &a[2], 4);
    if (trace_dsp()) sc2_log("[DSP] DSP n°%d (%s) setParameter(%u, %g)", i, i >= 0 ? g_dsp[i].nom : "?", A(1), v);
    return 0;
}
static uint32_t m_addDSP(CPU *c, const uint32_t *a)                   /* System::addDSP (DSP*, DSPConnection**) */
{
    (void)c; int i = sc2_dsp_index(A(1));
    if (i >= 0) g_dsp[i].systeme_dsp = 1;
    sc2_log("[DSP] System::addDSP DSP n°%d « %s » : effet sur le mixage final", i, i >= 0 ? g_dsp[i].nom : "?");
    if (A(2)) wr32(A(2), sc2_fmod_objet(6, i));
    return 0;
}
static uint32_t d_remove(CPU *c, const uint32_t *a)
{
    (void)c; int i = sc2_dsp_index(A(0));
    if (i >= 0) g_dsp[i].systeme_dsp = 0;
    if (trace_dsp()) sc2_log("[DSP] DSP n°%d remove", i);
    return 0;
}
static uint32_t d_release(CPU *c, const uint32_t *a)
{
    int i = sc2_dsp_index(A(0));
    if (trace_dsp()) sc2_log("[DSP] DSP n°%d release (%llu lectures, %llu frames)", i, i >= 0 ? (unsigned long long)g_dsp[i].lectures : 0ull,
                             i >= 0 ? (unsigned long long)g_dsp[i].frames : 0ull);
    if (i >= 0) {
        sc2_audio_dsp_oublier(i);                                      /* canaux playDSP de ce DSP arrêtés */
        if (g_dsp[i].rappel_liberer) { uint32_t e = g_dsp[i].etat; sc2_call_guest(c, g_dsp[i].rappel_liberer, 1, &e, 0); }
        g_dsp[i].vivant = 0;
    }
    return 0;
}
/* appel du rappel read invité sur n frames (≤ DSP_BLOC) : entrée = in (ou zéros), sortie lue dans out (n × canaux) */
static int dsp_appeler(CPU *c, int i, const float *in, int nch_in, uint32_t n, float *out, int *nch)
{
    int k = g_dsp[i].canaux > 0 && g_dsp[i].canaux <= 8 ? g_dsp[i].canaux : 2;
    if (!g_dsp[i].entree) { g_dsp[i].entree = sc2_alloc(DSP_BLOC * 8 * 4); g_dsp[i].sortie = sc2_alloc(DSP_BLOC * 8 * 4); }
    float *ge = (float *)GPTR(g_dsp[i].entree), *gs = (float *)GPTR(g_dsp[i].sortie);
    if (in) { k = nch_in; memcpy(ge, in, n * (uint32_t)k * 4); } else memset(ge, 0, n * (uint32_t)k * 4);
    memset(gs, 0, n * (uint32_t)k * 4);
    uint32_t args[6] = { g_dsp[i].etat, g_dsp[i].entree, g_dsp[i].sortie, n, (uint32_t)k, (uint32_t)k };
    uint32_t r = sc2_call_guest(c, g_dsp[i].rappel_lire, 6, args, 0);       /* F_CALLBACK = stdcall */
    g_dsp[i].lectures++; g_dsp[i].frames += n; g_dsp_appels_lire++;
    if (trace_dsp() && (g_dsp[i].lectures <= 6 || g_dsp[i].lectures % 2000 == 0)) {
        uint32_t ud = g_dsp[i].userdata;
        sc2_log("[DSP] read n°%d « %s » ×%llu : %u frames × %d canaux → FMOD_RESULT %u ; userdata 0x%08X [+0xD0] = %u, source [+0xD8] 0x%08X",
                i, g_dsp[i].nom, (unsigned long long)g_dsp[i].lectures, n, k, r, ud, ud ? rd32(ud + 0xD0) : 0, ud ? rd32(ud + 0xD8) : 0);
    }
    memcpy(out, gs, n * (uint32_t)k * 4); *nch = k;
    return (int)r;
}
/* générateur joué par playDSP : n frames stéréo ajoutées à acc (gains gauche / droit) */
void sc2_dsp_lire(CPU *c, int i, float *acc, uint32_t n, float gl, float gr)
{
    if (i < 0 || i >= g_ndsp || !g_dsp[i].vivant || !g_dsp[i].rappel_lire || !c || dsp_coupe()) return;
    static float tmp[DSP_BLOC * 8];
    for (uint32_t f = 0; f < n; f += DSP_BLOC) {
        uint32_t m = n - f < DSP_BLOC ? n - f : DSP_BLOC; int k;
        if (!g_dsp[i].actif) continue;
        dsp_appeler(c, i, NULL, 0, m, tmp, &k);
        for (uint32_t j = 0; j < m; j++) {
            float l = tmp[j * (uint32_t)k], r = k > 1 ? tmp[j * (uint32_t)k + 1] : l;
            acc[2 * (f + j)] += l * gl; acc[2 * (f + j) + 1] += r * gr;
        }
    }
}
/* effets System::addDSP sur le mixage final (stéréo entrelacé) */
void sc2_dsp_effets(CPU *c, float *acc, uint32_t n)
{
    if (!c || dsp_coupe()) return;
    static float tmp[DSP_BLOC * 8];
    for (int i = 0; i < g_ndsp; i++) {
        if (!g_dsp[i].vivant || !g_dsp[i].systeme_dsp || !g_dsp[i].actif || !g_dsp[i].rappel_lire) continue;
        for (uint32_t f = 0; f < n; f += DSP_BLOC) {
            uint32_t m = n - f < DSP_BLOC ? n - f : DSP_BLOC; int k;
            /* passe 7w7 : setActive(0) pris en compte au bloc SUIVANT (le MovieRecordingDSP se coupe lui-même dans son
             * read quand son anneau atteint le seuil) ; inactif = effet contourné, le mixage passe tel quel */
            if (!g_dsp[i].actif) break;
            dsp_appeler(c, i, acc + 2 * f, 2, m, tmp, &k);
            memcpy(acc + 2 * f, tmp, m * 2 * 4);
            g_dsp_effets++;
        }
    }
}
/* Passe 7w7 — diagnostic SC2_DSP_TEST=film[:<i/s>] : au 1er System::update du maître, le RUNTIME (pas le jeu) appelle
 * la fonction de démarrage d'enregistrement vidéo du jeu 0x009E7340 (ecx = images/s, défaut 30), appelée sinon depuis
 * 0x00BFB09B quand le joueur lance un enregistrement : elle crée le « MovieRecordingDSP » (read 0x009E7260), lit
 * getSoftwareFormat, alloue son anneau (0x0175AA60, fréquence × canaux flottants) et le pose par System::addDSP.
 * Le mixeur l'appelle ensuite sur le mixage final ; le bilan donne le compteur d'échantillons capturés (0x0175AC5C). */
static int g_test_film = -1;
void sc2_dsp_test(CPU *c)
{
    if (g_test_film >= 0 || !c) return;
    const char *e = getenv("SC2_DSP_TEST");
    g_test_film = 0;
    if (!e || strncmp(e, "film", 4)) return;
    uint32_t ips = e[4] == ':' ? (uint32_t)atoi(e + 5) : 30;
    uint32_t sauve[8] = { c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi };
    c->ecx = ips;
    uint32_t r = sc2_call_guest(c, 0x009E7340u, 0, NULL, 0);
    c->eax = sauve[0]; c->ecx = sauve[1]; c->edx = sauve[2]; c->ebx = sauve[3];
    c->esp = sauve[4]; c->ebp = sauve[5]; c->esi = sauve[6]; c->edi = sauve[7];
    g_test_film = 1;
    sc2_log("[DSP] DIAGNOSTIC SC2_DSP_TEST=film:%u : 0x009E7340 appelée par le RUNTIME → %u ; FMOD_RESULT [0x0175AA48] = %u, "
            "fréquence [0x0175AA4C] %u, canaux [0x0175AA50] %u, anneau [0x0175AA60] 0x%08X de %u échantillons, seuil [0x0175AA54] %u",
            ips, r, rd32(0x0175AA48u), rd32(0x0175AA4Cu), rd32(0x0175AA50u), rd32(0x0175AA60u), rd32(0x0175AA58u), rd32(0x0175AA54u));
}
uint64_t sc2_dsp_frames(int i) { return i >= 0 && i < g_ndsp ? g_dsp[i].frames : 0; }
static void dsp_bilan(void)
{
    printf("[BILAN] [DSP] %d DSP créés ; rappels read appelés %llu (dont effets %llu)\n", g_ndsp,
           (unsigned long long)g_dsp_appels_lire, (unsigned long long)g_dsp_effets);
    if (g_test_film == 1)
        printf("[BILAN] [DSP] diagnostic film : échantillons capturés par le MovieRecordingDSP [0x0175AC5C] %u (lus [0x0175AC58] %u), "
               "anneau %u échantillons à 0x%08X\n", rd32(0x0175AC5Cu), rd32(0x0175AC58u), rd32(0x0175AA58u), rd32(0x0175AA60u));
    for (int i = 0; i < g_ndsp; i++) {
        uint32_t ud = g_dsp[i].userdata;
        printf("[BILAN] [DSP]   n°%d « %s » %s read 0x%08X : %llu lectures, %llu frames ; userdata 0x%08X [+0xD0] = %u%s\n", i,
               g_dsp[i].nom, g_dsp[i].systeme_dsp ? "effet (addDSP)" : g_dsp[i].type >= 0 ? "intégré" : "générateur", g_dsp[i].rappel_lire,
               (unsigned long long)g_dsp[i].lectures, (unsigned long long)g_dsp[i].frames, ud, ud ? rd32(ud + 0xD0) : 0,
               g_dsp[i].vivant ? "" : " (libéré)");
    }
}

static const struct { const char *court; fmod_fn fn; } g_table_impl[] = {
    { "FMOD_System_Create", f_System_Create }, { "FMOD_Memory_GetStats", f_Memory_GetStats },
    { "FMOD_Memory_Initialize", f_ok },
    { "System::getVersion", m_getVersion }, { "System::getNumDrivers", m_getNumDrivers },
    { "System::getRecordNumDrivers", m_getRecordNumDrivers }, { "System::getDriverInfoW", m_getDriverInfoW },
    { "System::getDriverInfo", m_getDriverInfo },
    { "System::getDriverCaps", m_getDriverCaps }, { "System::getOutput", m_getOutput },
    { "System::getDSPBufferSize", m_getDSPBufferSize }, { "System::getStreamBufferSize", m_getStreamBufferSize },
    { "System::getSoftwareFormat", m_getSoftwareFormat },
    { "System::createDSP", m_createDSP }, { "System::createDSPByType", m_createDSPByType }, { "System::addDSP", m_addDSP },
    { "DSP::getUserData", d_getUserData }, { "DSP::setActive", d_setActive }, { "DSP::setParameter", d_setParameter },
    { "DSP::remove", d_remove }, { "DSP::release", d_release },
};

const imp_def *sc2_audio_find(const char *nom)
{
    if (g_actif < 0) {
        const char *e = getenv("SC2_AUDIO");
        g_actif = !(e && *e == '0');
        if (!g_actif) atexit(sc2_audio_bilan);   /* 24/10 : bilan des refus (sinon posé par le 1er System du runtime) */
        for (int k = 0; k < SC2_FMOD_NB; k++) {
            g_defs[k] = (imp_def){ SC2_FMOD[k].nom, SC2_FMOD[k].mots, STD, g_tramp[k] };
            for (unsigned j = 0; j < sizeof g_table_impl / sizeof g_table_impl[0]; j++)
                if (!strcmp(g_table_impl[j].court, SC2_FMOD[k].court)) g_impl[k] = g_table_impl[j].fn;
            for (unsigned j = 0; j < sc2_audio_nb_impl; j++)
                if (!strcmp(sc2_audio_impl[j].court, SC2_FMOD[k].court)) g_impl[k] = sc2_audio_impl[j].fn;
        }
    }
    for (int k = 0; k < SC2_FMOD_NB; k++) if (!strcmp(SC2_FMOD[k].nom, nom))
        return g_actif || strstr(SC2_FMOD[k].court, "::") ? &g_defs[k] : NULL;   /* SC2_AUDIO=0 : fonctions libres → p4 */
    return NULL;
}

void sc2_audio_bilan(void)
{
    if (!g_actif) {
        uint64_t r = 0; for (int k = 0; k < SC2_FMOD_NB; k++) r += g_n[k];
        printf("[BILAN] [AUDIO] SC2_AUDIO=0 : FMOD absent, %llu appel(s) de méthode refusé(s) (FMOD_ERR_INVALID_HANDLE)\n",
               (unsigned long long)r);
        return;
    }
    if (g_actif < 0) return;
    int ord[SC2_FMOD_NB], n = 0;
    for (int k = 0; k < SC2_FMOD_NB; k++) if (g_n[k]) ord[n++] = k;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_n[ord[j]] > g_n[ord[j - 1]]; j--) { int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
    printf("[BILAN] [AUDIO] FMOD Ex : %d méthodes distinctes appelées sur %d importées (%s = implémentée) :", n, SC2_FMOD_NB, "*");
    for (int i = 0; i < n; i++)
        printf("%s %s%s×%llu", i % 4 ? "," : "\n   ", SC2_FMOD[ord[i]].court, g_impl[ord[i]] ? "*" : "", (unsigned long long)g_n[ord[i]]);
    printf("\n");
    sc2_audio_bilan_mix();
    dsp_bilan();
}
