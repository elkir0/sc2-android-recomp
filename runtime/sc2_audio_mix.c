/* sc2_audio_mix.c — sons, canaux, mixage et sorties du FMOD de recomp-sc2 (passe 5e).
 *
 * Ce que le jeu appelle pour jouer un son (FMOD Ex 4.28.08) devient réel :
 *   System::createSound  → octets lus par les RAPPELS FICHIER DU JEU (System::setFileSystem :
 *                          open/close/read/seek invités, appelés par sc2_call_guest), ou en
 *                          mémoire (FMOD_OPENMEMORY) ; décodage Ogg Vorbis (stb_vorbis : tous
 *                          les sons de SC2 sont des Vorbis, .ogg comme .wav_) — complet pour un
 *                          échantillon, au fil de l'eau pour un flux (FMOD_CREATESTREAM).
 *   System::playSound    → canal (volume, pause, sourdine, fréquence, boucles, groupe).
 *   System::update       → mixage logiciel stéréo au format logiciel demandé par le jeu
 *                          (setSoftwareFormat : 44 100 Hz), du temps écoulé depuis l'update
 *                          précédent (≤ 250 ms), vers : WAV (SC2_AUDIO_DUMP) et ALSA (dlopen
 *                          libasound, périphérique « default », si disponible).
 * Fin d'un son non bouclé : le rappel de canal du jeu (Channel::setCallback) reçoit END.
 * Diagnostic : SC2_AUDIO_TEST=<nom interne;…> — le RUNTIME (pas le jeu) charge ces sons par les
 * rappels fichier du jeu et les joue au 1er update : preuve de la chaîne quand l'écran courant
 * ne joue rien (écran d'attente Battle.net : aucun createSound).
 */
#define _GNU_SOURCE
#include "sc2_audio.h"
#include "sc2_audio_int.h"
#include "sc2_audio_sortie.h"

#define STB_VORBIS_HEADER_ONLY
#include "sc2_audio_stb_vorbis.c"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define A(i) (a[(i)])
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP                  /* glibc, bionic ; macOS : sans _NP */
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif
enum { G_SYSTEM = 1, G_SOUND, G_CHANNEL, G_GROUP };
enum { ERR_FILE_NOTFOUND = 23, ERR_FORMAT = 25, ERR_INVALID_HANDLE = 36, ERR_INVALID_PARAM = 37, ERR_MEMORY = 43 };
#define M_LOOP_NORMAL  0x00000002u
#define M_LOOP_BIDI    0x00000004u
#define M_CREATESTREAM 0x00000080u
#define M_OPENMEMORY   0x00000800u
#define M_OPENMEMORY_P 0x10000000u
#define M_OPENRAW      0x00001000u

typedef struct {
    int vivant, flux, nch, rate;
    uint32_t mode, objet;
    char nom[160];
    uint8_t *brut; uint32_t nbrut;     /* fichier Ogg (gardé pour un flux) */
    int16_t *pcm; uint32_t frames;     /* échantillon décodé */
    stb_vorbis *v;                     /* flux : décodeur */
    int boucles;                       /* -1 infini */
    uint32_t bdeb, bfin;               /* passe 7v : points de boucle en frames (bfin inclus ; 0/0 = tout le son) */
} son_t;

typedef struct {
    int vivant, gen, son, joue, pause, muet, groupe, boucles;
    int dsp;                           /* passe 7w6 : canal de System::playDSP → n° de DSP + 1 (0 : canal de son) */
    uint32_t objet, rappel, userdata, mode, systeme;
    float vol, freq, pan;              /* freq : Hz demandés (0 = fréquence du son) ; pan -1 (G) … 1 (D) */
    uint64_t debut, fin;               /* passe 7v : setDelay DSPCLOCK_START / _END (horloge DSP, 0 = aucun) */
    uint32_t bdeb, bfin;               /* passe 7v : points de boucle du canal (frames, bfin inclus) */
    double pos;                        /* position en frames du son */
    int16_t tampon[4096 * 2]; int tdeb, tlen;   /* flux : fenêtre décodée [tdeb, tdeb+tlen) */
    float p3[3], niv3d, occl, pan3d;   /* passe 7z : position 3D, set3DPanLevel (FMOD : 1), set3DOcclusion direct */
    int p3ok;
} canal_t;

typedef struct { int vivant, muet; float vol; uint32_t objet; } groupe_t;

#define MAX_SONS 4096
#define MAX_CANAUX 256
#define MAX_GROUPES 64
static struct {
    pthread_mutex_t mu;
    int init, freq, nb_init;
    uint32_t f_open, f_close, f_read, f_seek;
    son_t s[MAX_SONS]; int ns;
    canal_t c[MAX_CANAUX];
    groupe_t g[MAX_GROUPES]; int ng;
    struct timespec t0; int t0ok;
    FILE *wav; uint64_t wav_frames; char wav_nom[512];
    FILE *piste; uint64_t piste_seg;   /* passe 7z : SC2_AUDIO_PISTE — segments mixés (son, position, gains) */
    void *alsa; void *pcm;
    int (*snd_pcm_writei)(void *, const void *, unsigned long);
    int (*snd_pcm_recover)(void *, int, int);
    uint64_t frames_mixees, frames_non_nulles, sons_crees, sons_ko, lectures_jeu, octets_jeu, joues, fins, rappels;
    double crete;
    int test_fait;
    int fini;                          /* passe 5g : bilan en cours — plus aucun mixage */
    /* Passe 7v : le jeu crée PLUSIEURS System FMOD (son principal 0x009EAFAE ; sondes ; voix 0x00BE46AE à
     * setSoftwareFormat(8 000 Hz)). Un seul mixeur : il appartient au System « maître » (le 1er qui pose ses
     * rappels fichier, ou fait init avec des canaux) ; la fréquence, l'horloge DSP et update viennent de lui seul. */
    uint32_t maitre;
    uint64_t horloge;                  /* horloge DSP : frames mixées par le maître (getDSPClock, setDelay) */
    uint64_t upd_maitre, upd_autres, formats_ignores, retards, demarres_retard;
    uint64_t dsp_joues, frames_dsp;    /* passe 7w6 : System::playDSP, frames demandées aux générateurs */
    uint64_t fen_debut, fen_nn, fen_n; double fen_sq;   /* fenêtre [AUDIO-T] */
    /* passe 7z : 3D de base — auditeur 0 du maître, rappel d'atténuation du jeu, panoramique par position */
    int mode3d, droitier;              /* SC2_AUDIO_3D=0 : 3D ignorée (état 7v) ; FMOD_INIT_3D_RIGHTHANDED */
    float lpos[3], lfwd[3], lup[3]; int lok;
    uint32_t rolloff; float doppler, distf, roll_echelle;
    uint64_t n3d_attr, n3d_ecoute, n3d_blocs, n3d_rappels; float dernier_att;
} X = { .mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP, .freq = 44100 };

static int maitre(uint32_t sys, int choisir)
{
    if (!X.maitre && choisir && sc2_fmod_index(sys, G_SYSTEM) >= 0) {
        X.maitre = sys;
        sc2_log("[AUDIO] System maître du mixeur : 0x%08X (n°%d)", sys, sc2_fmod_index(sys, G_SYSTEM));
    }
    return !X.maitre || X.maitre == sys;
}

/* SC2_AUDIO_JOURNAL=<n> (diagnostic, 24/10) : journalise les n premiers événements de son (createSound avec nom, mode
 * et résultat ; playSound ; stop ; release ; fin de lecture) en « [AUDIO-J] », sans rien changer au comportement. */
static int journal_ok(void)
{
    static long reste = -2;
    if (reste == -2) { const char *e = getenv("SC2_AUDIO_JOURNAL"); reste = e && *e ? atol(e) : 0; if (reste < 0) reste = 0; }
    if (reste <= 0) return 0;
    reste--; return 1;
}
#define JOURNAL(...) do { if (journal_ok()) sc2_log("[AUDIO-J] " __VA_ARGS__); } while (0)

/* ---- sorties ------------------------------------------------------------------- */
static void wav_entete(FILE *f, uint64_t frames, int freq)
{
    uint32_t data = (uint32_t)(frames * 4), riff = 36 + data;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t v32 = 16; memcpy(h + 16, &v32, 4);
    uint16_t v16 = 1; memcpy(h + 20, &v16, 2); v16 = 2; memcpy(h + 22, &v16, 2);
    v32 = (uint32_t)freq; memcpy(h + 24, &v32, 4); v32 = (uint32_t)freq * 4; memcpy(h + 28, &v32, 4);
    v16 = 4; memcpy(h + 32, &v16, 2); v16 = 16; memcpy(h + 34, &v16, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fseek(f, 0, SEEK_END);
}
static void wav_fin(void)
{
    if (!X.wav) return;
    wav_entete(X.wav, X.wav_frames, X.freq);
    fclose(X.wav); X.wav = NULL;
}
static void sorties_ouvrir(void)
{
    const char *d = getenv("SC2_AUDIO_DUMP");
    if (d && *d && !X.wav) {
        X.wav = fopen(d, "wb");
        if (X.wav) { snprintf(X.wav_nom, sizeof X.wav_nom, "%s", d); wav_entete(X.wav, 0, X.freq); }
        sc2_log("[AUDIO] SC2_AUDIO_DUMP : %s %s (PCM16 stéréo %d Hz)", d, X.wav ? "ouvert" : "IMPOSSIBLE à ouvrir", X.freq);
    }
    /* Passe 7z : SC2_AUDIO_PISTE=<fichier> — une ligne par segment contigu mixé, pour reconstruire le WAV hors ligne
     * à partir des fichiers du jeu (android/outils/audio-piste-7z.c) :
     *   S <canal> <mix_début> <frames> <pos_début> <pas> <gain_g> <gain_d> <flux> <nom du son>
     *   D <canal> <mix_début> <frames> <gain_g> <gain_d>          (générateur DSP du jeu : pas de référence)
 *   P <canal> <horloge> <0|1> <nom> ; X <canal> <horloge> <nom>  (Channel::setPaused / Channel::stop du jeu)
     * mix_début = horloge DSP (frames mixées) ; la frame j du segment lit le son en pos_début + j × pas. */
    const char *pi = getenv("SC2_AUDIO_PISTE");
    if (pi && *pi && !X.piste) {
        X.piste = fopen(pi, "w");
        sc2_log("[AUDIO] SC2_AUDIO_PISTE : %s %s", pi, X.piste ? "ouvert" : "IMPOSSIBLE à ouvrir");
    }
    const char *e = getenv("SC2_AUDIO_SORTIE");
    if (e && *e == '0') return;
#ifdef __APPLE__
    /* M6 : macOS → CoreAudio côté hôte (android/jni/sc2_audio_android.c, AUHAL) ; le mixeur ne dlopen jamais
     * libasound ici. La sortie temps réel est posée par sc2aa_demarrer quand SC2_MAC_SON l'exige. */
    {
        const char *m = getenv("SC2_MAC_SON");
        if (m && (!strcmp(m, "1") || !strcmp(m, "muet"))) sc2_log("[AUDIO] sortie : CoreAudio (hôte Mac)");
    }
    return;
#endif
    X.alsa = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!X.alsa) { sc2_log("[AUDIO] sortie temps réel : libasound absente → WAV seul"); return; }
    int (*ouvrir)(void **, const char *, int, int) = (int (*)(void **, const char *, int, int))dlsym(X.alsa, "snd_pcm_open");
    int (*regler)(void *, int, int, unsigned, unsigned, int, unsigned) =
        (int (*)(void *, int, int, unsigned, unsigned, int, unsigned))dlsym(X.alsa, "snd_pcm_set_params");
    X.snd_pcm_writei = (int (*)(void *, const void *, unsigned long))dlsym(X.alsa, "snd_pcm_writei");
    X.snd_pcm_recover = (int (*)(void *, int, int))dlsym(X.alsa, "snd_pcm_recover");
    /* SND_PCM_STREAM_PLAYBACK 0, SND_PCM_NONBLOCK 1 ; S16_LE 2, accès RW_INTERLEAVED 3 ; 100 ms de latence */
    if (ouvrir && regler && X.snd_pcm_writei && !ouvrir(&X.pcm, "default", 0, 1) && !regler(X.pcm, 2, 3, 2, (unsigned)X.freq, 1, 100000))
        sc2_log("[AUDIO] sortie temps réel : ALSA « default » ouvert (%d Hz stéréo)", X.freq);
    else { X.pcm = NULL; sc2_log("[AUDIO] sortie temps réel : aucun périphérique ALSA utilisable → WAV seul"); }
}

/* ---- chargement par les rappels fichier du jeu ------------------------------------ */
static __thread uint32_t t_scratch;             /* bloc invité par thread : 4 mots + 64 Kio */
#define SCRATCH_DONNEES 65536u
static uint8_t *lire_par_le_jeu(CPU *c, const char *nom, uint32_t offset, uint32_t longueur, uint32_t *n)
{
    if (!X.f_open || !X.f_read) return NULL;
    if (!t_scratch) t_scratch = sc2_alloc(32 + SCRATCH_DONNEES + 512);
    uint32_t sz = t_scratch, hdl = t_scratch + 4, ud = t_scratch + 8, lu = t_scratch + 12, nomv = t_scratch + 16;
    uint32_t buf = t_scratch + 32 + 512;
    size_t ln = strlen(nom); if (ln > 500) ln = 500;
    memcpy(GPTR(nomv), nom, ln); wr8(nomv + (uint32_t)ln, 0);
    wr32(sz, 0); wr32(hdl, 0); wr32(ud, 0);
    uint32_t args[5] = { nomv, 0, sz, hdl, ud };
    uint32_t ro = sc2_call_guest(c, X.f_open, 5, args, 0);                  /* F_CALLBACK = stdcall */
    if (ro != 0) {
        static int nko; if (++nko <= 8) sc2_log("[AUDIO] rappel open du jeu (0x%08X) « %s » → FMOD_RESULT %u", X.f_open, nom, ro);
        return NULL;
    }
    uint32_t taille = rd32(sz), h = rd32(hdl), u = rd32(ud);
    if (offset && X.f_seek) { uint32_t s[3] = { h, offset, u }; sc2_call_guest(c, X.f_seek, 3, s, 0); }
    uint32_t total = longueur ? longueur : (taille > offset ? taille - offset : 0);
    uint8_t *out = malloc(total ? total : 1);
    uint32_t got = 0;
    while (out && got < total) {
        uint32_t q = total - got < SCRATCH_DONNEES ? total - got : SCRATCH_DONNEES;
        wr32(lu, 0);
        uint32_t r[5] = { h, buf, q, lu, u };
        uint32_t res = sc2_call_guest(c, X.f_read, 5, r, 0);
        uint32_t k = rd32(lu);
        if (k > q) k = q;
        memcpy(out + got, GPTR(buf), k); got += k;
        X.lectures_jeu++;
        if (!k || (res && res != 22)) break;                                   /* 22 : FMOD_ERR_FILE_EOF */
    }
    if (X.f_close) { uint32_t cl[2] = { h, u }; sc2_call_guest(c, X.f_close, 2, cl, 0); }
    X.octets_jeu += got;
    *n = got;
    return out;
}

uint8_t *sc2_audio_mpq_lire(const char *archive, const char *interne, uint32_t *n);   /* sc2_audio_mpq.c */

/* ---- sons ------------------------------------------------------------------------ */
static int son_creer(CPU *c, const char *nom_ou_donnees, uint32_t nom_va, uint32_t mode, uint32_t exinfo, uint32_t *objet)
{
    uint32_t longueur = 0, offset = 0;
    if (exinfo && rd32(exinfo) >= 12) { longueur = rd32(exinfo + 4); offset = rd32(exinfo + 8); }
    uint8_t *brut = NULL; uint32_t n = 0;
    char nom[160] = "";
    if (mode & (M_OPENMEMORY | M_OPENMEMORY_P)) {
        if (!longueur) return ERR_INVALID_PARAM;
        brut = malloc(longueur); n = longueur;
        if (brut) memcpy(brut, GPTR(nom_va + offset), longueur);
        snprintf(nom, sizeof nom, "<mémoire 0x%08X, %u o>", nom_va, longueur);
    } else {
        snprintf(nom, sizeof nom, "%s", nom_ou_donnees);
        const char *barre = strchr(nom_ou_donnees, '|');
        if (barre && X.test_fait) {                  /* DIAGNOSTIC seulement : « archive|nom interne » */
            char arc[512]; size_t l = (size_t)(barre - nom_ou_donnees) < sizeof arc - 1 ? (size_t)(barre - nom_ou_donnees) : sizeof arc - 1;
            memcpy(arc, nom_ou_donnees, l); arc[l] = 0;
            brut = sc2_audio_mpq_lire(arc, barre + 1, &n);
            snprintf(nom, sizeof nom, "%s", barre + 1);
        } else
            brut = lire_par_le_jeu(c, nom_ou_donnees, offset, longueur, &n);
        if (!brut) { X.sons_ko++; return ERR_FILE_NOTFOUND; }
    }
    if (mode & M_OPENRAW || n < 4 || memcmp(brut, "OggS", 4)) {
        X.sons_ko++;
        if (X.sons_ko <= 8) sc2_log("[AUDIO] createSound « %s » : format non géré (%u o, %02X%02X%02X%02X, mode 0x%X)", nom, n,
                                    n > 0 ? brut[0] : 0, n > 1 ? brut[1] : 0, n > 2 ? brut[2] : 0, n > 3 ? brut[3] : 0, mode);
        free(brut);
        return ERR_FORMAT;
    }
    int id = -1;
    for (int i = 0; i < X.ns; i++) if (!X.s[i].vivant) { id = i; break; }
    if (id < 0) { if (X.ns >= MAX_SONS) { free(brut); return ERR_MEMORY; } id = X.ns++; }
    son_t *s = &X.s[id];
    memset(s, 0, sizeof *s);
    snprintf(s->nom, sizeof s->nom, "%s", nom);
    s->mode = mode; s->boucles = -1;                 /* passe 7v : compte FMOD (-1 infini), effectif si mode LOOP_* */
    if (mode & M_CREATESTREAM) {
        int err = 0;
        s->v = stb_vorbis_open_memory(brut, (int)n, &err, NULL);
        if (!s->v) { free(brut); X.sons_ko++; return ERR_FORMAT; }
        stb_vorbis_info inf = stb_vorbis_get_info(s->v);
        s->flux = 1; s->brut = brut; s->nbrut = n; s->nch = inf.channels; s->rate = (int)inf.sample_rate;
        s->frames = stb_vorbis_stream_length_in_samples(s->v);
    } else {
        int nch = 0, rate = 0; short *pcm = NULL;
        int fr = stb_vorbis_decode_memory(brut, (int)n, &nch, &rate, &pcm);
        free(brut);
        if (fr <= 0 || !pcm) { X.sons_ko++; return ERR_FORMAT; }
        s->pcm = pcm; s->frames = (uint32_t)fr; s->nch = nch; s->rate = rate;
    }
    s->vivant = 1;
    s->objet = sc2_fmod_objet(G_SOUND, id);
    *objet = s->objet;
    X.sons_crees++;
    if (X.sons_crees <= 12 || (X.sons_crees & 63) == 0)
        sc2_trace("[AUDIO] createSound « %s » : Vorbis %d canal(aux) %d Hz, %u frames (%.2f s)%s, mode 0x%X — son n°%llu",
                s->nom, s->nch, s->rate, s->frames, s->rate ? (double)s->frames / s->rate : 0, s->flux ? ", FLUX" : "",
                mode, (unsigned long long)X.sons_crees);
    return 0;
}

static son_t *son_de(uint32_t o) { int i = sc2_fmod_index(o, G_SOUND); return i >= 0 && i < X.ns && X.s[i].vivant ? &X.s[i] : NULL; }
static canal_t *canal_de(uint32_t o)
{
    if (!o || rd32(o) != 0x444F4D46u || rd32(o + 4) != G_CHANNEL) return NULL;
    uint32_t v = rd32(o + 8); int i = (int)(v & 0xFFFF), gen = (int)(v >> 16);
    return i < MAX_CANAUX && X.c[i].vivant && X.c[i].gen == gen ? &X.c[i] : NULL;
}
static groupe_t *groupe_de(uint32_t o) { int i = sc2_fmod_index(o, G_GROUP); return i >= 0 && i < X.ng ? &X.g[i] : NULL; }

/* ---- mixage ------------------------------------------------------------------------ */
static int frame_flux(canal_t *ch, son_t *s, uint32_t f, float *l, float *r)
{
    while (f >= (uint32_t)(ch->tdeb + ch->tlen)) {
        ch->tdeb += ch->tlen;
        ch->tlen = stb_vorbis_get_samples_short_interleaved(s->v, s->nch, ch->tampon, (int)(sizeof ch->tampon / sizeof ch->tampon[0]));
        if (ch->tlen <= 0) { ch->tlen = 0; return 0; }
    }
    if (f < (uint32_t)ch->tdeb) return 0;                       /* pas de retour arrière dans un flux */
    const int16_t *p = ch->tampon + (size_t)(f - (uint32_t)ch->tdeb) * (size_t)s->nch;
    *l = p[0] / 32768.f; *r = (s->nch > 1 ? p[1] : p[0]) / 32768.f;
    return 1;
}
static void canal_fin(CPU *c, canal_t *ch)
{
    ch->joue = 0; X.fins++; JOURNAL("fin de lecture « %s »", ch->dsp ? "(DSP)" : X.s[ch->son].nom);
    if (ch->rappel && c) {                                       /* FMOD_CHANNEL_CALLBACKTYPE_END = 0 */
        uint32_t args[4] = { ch->objet, 0, 0, 0 };
        X.rappels++;
        sc2_call_guest(c, ch->rappel, 4, args, 0);
    }
}
/* Passe 7z : 3D de base d'un canal FMOD_3D (0x10). Atténuation = rappel du jeu (System::set3DRolloffCallback ;
 * celui de SC2, 0x009E4F10, est « fld1 ; ret 8 » : 1,0 toujours — le jeu atténue lui-même par setVolume), sinon
 * la loi inverse de FMOD (distance minimale 1). Panoramique : composante « droite » de la direction de la source dans
 * le repère de l'auditeur (droite = haut × avant en repère gaucher), mêlée au pan 2D selon set3DPanLevel ; source sur
 * l'auditeur = centre. FMOD_3D_HEADRELATIVE (0x40000) : position déjà relative à l'auditeur. Doppler, cône, spread
 * et réverbération : non rendus. */
static void canal_3d(CPU *c, canal_t *ch, float *gain, float *pan)
{
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = ch->p3[i] - ((ch->mode & 0x40000u) ? 0.f : X.lpos[i]);
    const float *f = X.lfwd, *u = X.lup;
    float d[3] = { u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0] };   /* haut × avant */
    if (ch->mode & 0x40000u) { d[0] = 1.f; d[1] = d[2] = 0.f; }
    else if (X.droitier) for (int i = 0; i < 3; i++) d[i] = -d[i];
    float nd = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), dist = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    float x = nd > 1e-6f && dist > 1e-6f ? (r[0] * d[0] + r[1] * d[1] + r[2] * d[2]) / (nd * dist) : 0.f;
    float att = 1.f;
    if (X.rolloff && c) {
        uint32_t args[2]; float dd = dist; args[0] = ch->objet; memcpy(&args[1], &dd, 4);
        pthread_mutex_unlock(&X.mu);
        int top = c->fpu_top;
        sc2_call_guest(c, X.rolloff, 2, args, 0);
        if (c->fpu_top != top) { att = (float)c->st[c->fpu_top]; c->fpu_top = (c->fpu_top + 1) & 7; }
        pthread_mutex_lock(&X.mu);
        X.n3d_rappels++;
    } else if (dist * X.roll_echelle > 1.f) att = 1.f / (1.f + X.roll_echelle * (dist - 1.f));
    if (!(att >= 0.f && att <= 1.f)) att = att > 1.f ? 1.f : 0.f;
    X.dernier_att = att;
    *gain *= att * (1.f - (ch->occl > 1.f ? 1.f : ch->occl < 0.f ? 0.f : ch->occl));
    float nv = ch->niv3d < 0.f ? 0.f : ch->niv3d > 1.f ? 1.f : ch->niv3d;
    *pan = (1.f - nv) * *pan + nv * x;
    ch->pan3d = *pan; X.n3d_blocs++;
}
static void mixer(CPU *c, uint32_t n)
{
    static float acc[2 * 11025]; static int16_t out[2 * 11025];
    if (X.fini) return;
    if (n > 11025) n = 11025;
    memset(acc, 0, sizeof(float) * 2 * n);
    for (int k = 0; k < MAX_CANAUX; k++) {
        canal_t *ch = &X.c[k];
        if (!ch->vivant || !ch->joue || ch->pause) continue;
        float g = ch->muet ? 0.f : ch->vol;
        if (ch->groupe >= 0 && ch->groupe < X.ng) g *= X.g[ch->groupe].muet ? 0.f : X.g[ch->groupe].vol;
        float pan = ch->pan;
        if (X.mode3d && (ch->mode & 0x10u) && !(ch->mode & 0x8u)) canal_3d(c, ch, &g, &pan);   /* passe 7z */
        float gl = g * (pan > 0 ? 1.f - pan : 1.f), gr = g * (pan < 0 ? 1.f + pan : 1.f);
        if (ch->dsp) {                                           /* passe 7w6 : générateur du jeu (rappel read invité) */
            /* le rappel du jeu prend ses propres verrous (source de la vidéo) : pas sous X.mu (inter-blocage avec
             * un fil du jeu qui appelle FMOD en les tenant) — X.mu n'est tenu qu'une fois ici (m_update) */
            int d = ch->dsp - 1;
            if (X.piste) { fprintf(X.piste, "D %d %llu %u %.9g %.9g\n", k, (unsigned long long)X.horloge, n, gl, gr); X.piste_seg++; }
            pthread_mutex_unlock(&X.mu); sc2_dsp_lire(c, d, acc, n, gl, gr); pthread_mutex_lock(&X.mu);
            X.frames_dsp += n;
            ch->pos = (double)sc2_dsp_frames(ch->dsp - 1);
            continue;
        }
        son_t *s = &X.s[ch->son];
        if (!s->vivant || !s->frames) { canal_fin(c, ch); continue; }
        double pas = (ch->freq > 0 ? ch->freq : (float)s->rate) / (double)X.freq;
        int boucle = (ch->mode & (M_LOOP_NORMAL | M_LOOP_BIDI)) != 0;
        uint32_t i0 = 0;
        if (ch->debut > X.horloge) {                             /* passe 7v : départ programmé (DSPCLOCK_START) */
            if (ch->debut >= X.horloge + n) continue;
            i0 = (uint32_t)(ch->debut - X.horloge); ch->debut = 0; X.demarres_retard++;
        } else if (ch->debut) { ch->debut = 0; X.demarres_retard++; }
        uint32_t sd = i0, i; double sp = ch->pos;                /* passe 7z : segment de piste en cours [sd, i) */
#define PISTE_SEG(fin) do { if (X.piste && (fin) > sd) { \
            fprintf(X.piste, "S %d %llu %u %.6f %.12g %.9g %.9g %d %s\n", k, (unsigned long long)(X.horloge + sd), (fin) - sd, \
                    sp, pas, gl, gr, s->flux, s->nom); X.piste_seg++; } } while (0)
        for (i = i0; i < n; i++) {
            if (ch->fin && X.horloge + i >= ch->fin) { canal_fin(c, ch); break; }   /* DSPCLOCK_END */
            uint32_t f = (uint32_t)ch->pos;
            /* passe 7v : boucle sur [bdeb, bfin] (points de boucle FMOD, bfin inclus) tant que le compte le permet,
             * puis lecture jusqu'à la fin du son (sémantique FMOD Ex) */
            uint32_t bf = ch->bfin && ch->bfin < s->frames ? ch->bfin : s->frames - 1, bd = ch->bdeb <= bf ? ch->bdeb : 0;
            if (boucle && ch->boucles != 0 && f > bf) {
                if (ch->boucles > 0) ch->boucles--;
                PISTE_SEG(i);
                ch->pos -= (double)(bf + 1 - bd); f = (uint32_t)ch->pos;
                sd = i; sp = ch->pos;
                if (s->flux) { stb_vorbis_seek(s->v, f); ch->tdeb = (int)f; ch->tlen = 0; }
            }
            if (f >= s->frames) { canal_fin(c, ch); break; }
            float l, r;
            if (s->flux) { if (!frame_flux(ch, s, f, &l, &r)) { canal_fin(c, ch); break; } }
            else {
                /* passe 7v : interpolation linéaire (fréquence du son ≠ mixage, ou setFrequency) */
                const int16_t *p = s->pcm + (size_t)f * (size_t)s->nch;
                l = p[0] / 32768.f; r = (s->nch > 1 ? p[1] : p[0]) / 32768.f;
                float t = (float)(ch->pos - f);
                if (t > 0 && f + 1 < s->frames) {
                    const int16_t *q = p + s->nch;
                    l += t * (q[0] / 32768.f - l); r += t * ((s->nch > 1 ? q[1] : q[0]) / 32768.f - r);
                }
            }
            acc[2 * i] += l * gl; acc[2 * i + 1] += r * gr;
            ch->pos += pas;
        }
        PISTE_SEG(i);
#undef PISTE_SEG
    }
    pthread_mutex_unlock(&X.mu); sc2_dsp_effets(c, acc, n); pthread_mutex_lock(&X.mu);   /* passe 7w6 : effets addDSP */
    for (uint32_t i = 0; i < 2 * n; i++) {
        float v = acc[i];
        if (fabsf(v) > X.crete) X.crete = fabsf(v);
        v = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
        out[i] = (int16_t)lrintf(v * 32767.f);
        if (out[i]) { X.frames_non_nulles++; X.fen_nn++; }
        X.fen_sq += (double)v * v;
    }
    X.frames_mixees += n; X.horloge += n; X.fen_n += 2 * n;
    if (X.horloge - X.fen_debut >= (uint64_t)X.freq * 10) {    /* passe 7v : activité par fenêtre de 10 s mixées */
        char noms[700] = ""; size_t o = 0; int actifs = 0;
        for (int k = 0; k < MAX_CANAUX; k++) {
            canal_t *ch = &X.c[k];
            if (!ch->vivant || !ch->joue || ch->pause) continue;
            actifs++;
            if (o < 560) {
                const char *b = strrchr(X.s[ch->son].nom, '\\'); b = b ? b + 1 : X.s[ch->son].nom;
                if (ch->dsp) b = "(générateur DSP du jeu)";
                float gg = ch->groupe >= 0 && ch->groupe < X.ng ? (X.g[ch->groupe].muet ? 0.f : X.g[ch->groupe].vol) : 1.f;
                o += (size_t)snprintf(noms + o, sizeof noms - o, "%s%s [vol %.2f×%.2f, %.0f Hz, %.1f s]%s", o ? ", " : "", b,
                                      ch->muet ? 0.f : ch->vol, gg, ch->freq > 0 ? ch->freq : (float)X.s[ch->son].rate,
                                      X.s[ch->son].rate ? ch->pos / X.s[ch->son].rate : 0.0, ch->debut ? " (programmé)" : "");
            }
        }
        double rms = X.fen_n ? sqrt(X.fen_sq / (double)X.fen_n) : 0;
        sc2_trace("[AUDIO-T] %.0f s mixées : %llu échantillons non nuls sur %llu (%.1f %%), RMS %.1f dBFS ; %d canal(aux) en lecture%s%s",
                (double)X.horloge / X.freq, (unsigned long long)X.fen_nn, (unsigned long long)X.fen_n,
                X.fen_n ? 100.0 * (double)X.fen_nn / (double)X.fen_n : 0.0, rms > 0 ? 20 * log10(rms) : -999.0,
                actifs, actifs ? " : " : "", noms);
        X.fen_debut = X.horloge; X.fen_nn = X.fen_n = 0; X.fen_sq = 0;
        if (X.wav) { wav_entete(X.wav, X.wav_frames, X.freq); fflush(X.wav); }   /* WAV valide même si le run est tué */
        if (X.piste) fflush(X.piste);
    }
    sc2_audio_sortie_emettre(out, n, X.freq);          /* passe 5g : consommateur éventuel, mêmes octets que le WAV */
    if (X.wav) { fwrite(out, 4, n, X.wav); X.wav_frames += n; }
    if (X.pcm) {
        int r = X.snd_pcm_writei(X.pcm, out, n);
        if (r < 0 && X.snd_pcm_recover) X.snd_pcm_recover(X.pcm, r, 1);
    }
}

/* ---- méthodes FMOD réelles ------------------------------------------------------------ */
static uint32_t m_setFileSystem(CPU *c, const uint32_t *a)   /* (open, close, read, seek, blockalign) */
{
    (void)c; pthread_mutex_lock(&X.mu);
    maitre(A(0), 1);
    X.f_open = A(1); X.f_close = A(2); X.f_read = A(3); X.f_seek = A(4);
    pthread_mutex_unlock(&X.mu);
    return 0;
}
static uint32_t m_setSoftwareFormat(CPU *c, const uint32_t *a)   /* (rate, format, nbsorties, maxentrées, resampler) */
{
    (void)c; pthread_mutex_lock(&X.mu);
    if (!maitre(A(0), 0) || X.init) {                 /* passe 7v : System secondaire (voix 8 000 Hz) : sans effet */
        if (++X.formats_ignores <= 4)
            sc2_log("[AUDIO] setSoftwareFormat(%u Hz) du System 0x%08X ignoré : le mixeur reste à %d Hz (maître 0x%08X)", A(1), A(0), X.freq, X.maitre);
    } else if (A(1) >= 8000 && A(1) <= 192000) X.freq = (int)A(1);
    pthread_mutex_unlock(&X.mu);
    return 0;
}
static void jouer_tests(CPU *c);
static uint32_t m_init(CPU *c, const uint32_t *a)                /* (maxcanaux, flags, extradriverdata) */
{
    (void)c; pthread_mutex_lock(&X.mu);
    if (A(1) > 0) maitre(A(0), 1);
    if (!X.init && maitre(A(0), 0) && A(1) > 0) {
        X.init = 1;
        for (int k = 0; k < MAX_GROUPES; k++) X.g[k].vol = 1.f;
        const char *e3 = getenv("SC2_AUDIO_3D");                  /* passe 7z */
        X.mode3d = !(e3 && *e3 == '0'); X.droitier = (A(2) & 0x4u) != 0;
        X.lfwd[2] = 1.f; X.lup[1] = 1.f; X.distf = X.roll_echelle = X.doppler = 1.f;
        sc2_log("[AUDIO] 3D : %s (repère %s)", X.mode3d ? "panoramique par position (passe 7z ; SC2_AUDIO_3D=0 pour l'ignorer)"
                                                        : "IGNORÉE (SC2_AUDIO_3D=0)", X.droitier ? "droitier" : "gaucher, FMOD par défaut");
        sorties_ouvrir();
        const char *ec = getenv("SC2_AUDIO_COMPTEUR");
        if (ec && *ec == '1') { sc2_audio_compteur_installer(); sc2_log("[AUDIO] SC2_AUDIO_COMPTEUR=1 : consommateur de banc posé (sc2_audio_sortie)"); }
        sc2_log("[AUDIO] System::init(%u canaux, flags 0x%X) : mixage logiciel stéréo %d Hz", A(1), A(2), X.freq);
    }
    X.nb_init++;
    pthread_mutex_unlock(&X.mu);
    return 0;
}
static uint32_t m_createSound(CPU *c, const uint32_t *a)        /* (nom|données, mode, exinfo*, Sound**) */
{
    pthread_mutex_lock(&X.mu);
    uint32_t o = 0;
    const char *nom = (A(2) & (M_OPENMEMORY | M_OPENMEMORY_P)) ? "" : (const char *)GPTR(A(1));
    int r = A(1) ? son_creer(c, nom, A(1), A(2), A(3), &o) : ERR_INVALID_PARAM;
    if (A(4)) wr32(A(4), r ? 0 : o);
    JOURNAL("createSound « %s » mode 0x%X → %d (Sound 0x%08X, System 0x%08X)", (A(2) & (M_OPENMEMORY | M_OPENMEMORY_P)) ? "<mémoire>" : nom,
            A(2), r, r ? 0 : o, A(0));
    pthread_mutex_unlock(&X.mu);
    return (uint32_t)r;
}
static uint32_t jouer(CPU *c, uint32_t sys, uint32_t index, uint32_t son, int pause, uint32_t *objet)
{
    (void)c;
    son_t *s = son_de(son);
    if (!s) return ERR_INVALID_HANDLE;
    int k = -1;
    for (int i = 0; i < MAX_CANAUX; i++) if (!X.c[i].vivant || !X.c[i].joue) { k = i; break; }
    if (k < 0) return 10;                                        /* FMOD_ERR_CHANNEL_ALLOC */
    canal_t *ch = &X.c[k];
    int gen = (ch->gen + 1) & 0x7FFF;
    memset(ch, 0, sizeof *ch);
    ch->vivant = 1; ch->gen = gen; ch->son = sc2_fmod_index(son, G_SOUND); ch->joue = 1; ch->pause = pause;
    ch->vol = 1.f; ch->groupe = -1; ch->boucles = s->boucles; ch->mode = s->mode; ch->systeme = sys;
    ch->bdeb = s->bdeb; ch->bfin = s->bfin;
    ch->niv3d = 1.f;                                             /* passe 7z : 3D pan level FMOD par défaut */
    if (s->flux) { stb_vorbis_seek_start(s->v); }
    ch->objet = sc2_fmod_objet(G_CHANNEL, k | (gen << 16));
    *objet = ch->objet;
    X.joues++;
    if (X.joues <= 12) sc2_trace("[AUDIO] playSound « %s » (index %d, %s) → canal %d", s->nom, (int)index, pause ? "en pause" : "lecture", k);
    return 0;
}
static uint32_t m_playSound(CPU *c, const uint32_t *a)          /* (index, Sound*, paused, Channel**) */
{
    pthread_mutex_lock(&X.mu);
    uint32_t o = 0, r = jouer(c, A(0), A(1), A(2), A(3) & 0xFF, &o);
    if (A(4)) wr32(A(4), r ? 0 : o);
    { son_t *sj = son_de(A(2)); JOURNAL("playSound « %s » %s → %u (canal 0x%08X)", sj ? sj->nom : "?", (A(3) & 0xFF) ? "en pause" : "lecture", r, r ? 0 : o); }
    pthread_mutex_unlock(&X.mu);
    return r;
}
/* Passe 7w6 : System::playDSP (index, DSP*, paused, Channel**) — un canal comme les autres (pause, volume, groupe,
 * stop, isPlaying…) dont les échantillons viennent du rappel read du DSP (sc2_dsp_lire). */
static uint32_t m_playDSP(CPU *c, const uint32_t *a)
{
    (void)c; pthread_mutex_lock(&X.mu);
    int d = sc2_dsp_index(A(2)), k = -1;
    uint32_t r = 0;
    if (d < 0) r = ERR_INVALID_HANDLE;
    else {
        for (int i = 0; i < MAX_CANAUX; i++) if (!X.c[i].vivant || !X.c[i].joue) { k = i; break; }
        if (k < 0) r = 10;                                       /* FMOD_ERR_CHANNEL_ALLOC */
    }
    if (!r) {
        canal_t *ch = &X.c[k];
        int gen = (ch->gen + 1) & 0x7FFF;
        memset(ch, 0, sizeof *ch);
        ch->vivant = 1; ch->gen = gen; ch->dsp = d + 1; ch->joue = 1; ch->pause = A(3) & 0xFF;
        ch->vol = 1.f; ch->groupe = -1; ch->systeme = A(0);
        ch->objet = sc2_fmod_objet(G_CHANNEL, k | (gen << 16));
        X.dsp_joues++;
        sc2_log("[DSP] playDSP DSP n°%d (index %d, %s) → canal %d (Channel 0x%08X)", d, (int)A(1), ch->pause ? "en pause" : "lecture",
                k, ch->objet);
    }
    if (A(4)) wr32(A(4), r ? 0 : X.c[k].objet);
    pthread_mutex_unlock(&X.mu);
    return r;
}
int sc2_audio_freq(void) { return X.freq; }
void sc2_audio_dsp_oublier(int d)
{
    pthread_mutex_lock(&X.mu);
    for (int k = 0; k < MAX_CANAUX; k++) if (X.c[k].vivant && X.c[k].dsp == d + 1) X.c[k].joue = 0;
    pthread_mutex_unlock(&X.mu);
}
static void jouer_tests(CPU *c)
{
    const char *t = getenv("SC2_AUDIO_TEST");
    if (!t || !*t || X.test_fait) return;
    X.test_fait = 1;
    char liste[2048]; snprintf(liste, sizeof liste, "%s", t);
    for (char *p = strtok(liste, ";"); p; p = strtok(NULL, ";")) {
        uint32_t so = 0, ch = 0;
        int r = son_creer(c, p, 0, 0, 0, &so);
        if (!r) r = (int)jouer(c, X.maitre, 0xFFFFFFFFu, so, 0, &ch);
        sc2_log("[AUDIO] DIAGNOSTIC SC2_AUDIO_TEST : « %s » chargé %s et joué par le RUNTIME (pas par le jeu) → %s", p,
                strchr(p, '|') ? "DIRECTEMENT dans l'archive (StormLib)" : "par les rappels fichier DU JEU", r ? "ÉCHEC" : "en lecture");
    }
}
static uint32_t m_update(CPU *c, const uint32_t *a)
{
    pthread_mutex_lock(&X.mu);
    if (!maitre(A(0), 0)) X.upd_autres++;             /* passe 7v : seul le maître fait avancer le mixage */
    else if (X.init) {
        X.upd_maitre++;
        jouer_tests(c);
        sc2_dsp_test(c);                                         /* passe 7w7 : diagnostic SC2_DSP_TEST=film */
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        if (!X.t0ok) { X.t0 = t; X.t0ok = 1; }
        double dt = (double)(t.tv_sec - X.t0.tv_sec) + (double)(t.tv_nsec - X.t0.tv_nsec) * 1e-9;
        X.t0 = t;
        uint32_t n = (uint32_t)(dt * X.freq);
        if (n > (uint32_t)X.freq / 4) n = (uint32_t)X.freq / 4;
        /* Passe 5g : SC2_AUDIO_PAS=<frames> — horloge DÉTERMINISTE, n frames par System::update (banc
         * reproductible au md5 près ; 735 = 44 100 / 60). Absent : temps réel (passe 5e, défaut). */
        static int pas = -1;
        if (pas < 0) { const char *e = getenv("SC2_AUDIO_PAS"); pas = e ? atoi(e) : 0; if (pas < 0 || pas > 11025) pas = 0;
                       if (pas) sc2_log("[AUDIO] SC2_AUDIO_PAS=%d : horloge déterministe (%d frames par System::update)", pas, pas); }
        if (pas) n = (uint32_t)pas;
        if (n) mixer(c, n);
    }
    pthread_mutex_unlock(&X.mu);
    return 0;
}

/* Sound */
static uint32_t s_release(CPU *c, const uint32_t *a)
{
    (void)c; pthread_mutex_lock(&X.mu);
    son_t *s = son_de(A(0));
    if (s) {
        int id = (int)(s - X.s);
        JOURNAL("Sound::release « %s »", s->nom);
        for (int k = 0; k < MAX_CANAUX; k++) if (X.c[k].vivant && X.c[k].son == id) X.c[k].joue = 0;
        free(s->pcm); if (s->v) stb_vorbis_close(s->v); free(s->brut);
        memset(s, 0, sizeof *s);
    }
    pthread_mutex_unlock(&X.mu);
    return s ? 0 : ERR_INVALID_HANDLE;
}
static uint32_t en_unite(son_t *s, double frames, uint32_t unite)
{
    if (unite == 1) return (uint32_t)(frames * 1000.0 / (s->rate ? s->rate : 1));      /* MS */
    if (unite == 4) return (uint32_t)(frames * 2 * s->nch);                              /* PCMBYTES */
    return (uint32_t)frames;                                                             /* PCM */
}
static uint32_t s_getLength(CPU *c, const uint32_t *a)
{ (void)c; son_t *s = son_de(A(0)); if (!s) return ERR_INVALID_HANDLE; if (A(1)) wr32(A(1), en_unite(s, s->frames, A(2))); return 0; }
static uint32_t s_getDefaults(CPU *c, const uint32_t *a)        /* (freq*, vol*, pan*, prio*) */
{
    (void)c; son_t *s = son_de(A(0)); if (!s) return ERR_INVALID_HANDLE;
    float f = (float)s->rate, v = 1.f, p = 0.f;
    if (A(1)) memcpy(GPTR(A(1)), &f, 4);
    if (A(2)) memcpy(GPTR(A(2)), &v, 4);
    if (A(3)) memcpy(GPTR(A(3)), &p, 4);
    if (A(4)) wr32(A(4), 128);
    return 0;
}
static uint32_t s_getFormat(CPU *c, const uint32_t *a)          /* (type*, format*, canaux*, bits*) */
{
    (void)c; son_t *s = son_de(A(0)); if (!s) return ERR_INVALID_HANDLE;
    if (A(1)) wr32(A(1), 14); if (A(2)) wr32(A(2), 2); if (A(3)) wr32(A(3), (uint32_t)s->nch); if (A(4)) wr32(A(4), 16);
    return 0;
}
/* Passe 7m : les sorties `bool*` (PA_N) font UN octet. Écrire 4 octets débordait sur la pile du jeu :
 * 0x009E5D10 passe &bool en [ebp-1] à Channel::isPlaying → les 3 octets bas de l'ebp sauvegardé mis à 0
 * (0x08E8D5A8 → 0x08000000) → SIGSEGV en 0x00A40300 ([ebp-4] = 0x07FFFFFC) dès l'ouverture du menu. */
static void ecrire_bool(uint32_t va, int v) { if (va) wr8(va, (uint8_t)(v != 0)); }
static uint32_t s_getOpenState(CPU *c, const uint32_t *a)    /* (état*, pourcentage*, bool* famine) */
{ (void)c; if (A(1)) wr32(A(1), 0); if (A(2)) wr32(A(2), 100); ecrire_bool(A(3), 0); return son_de(A(0)) ? 0 : ERR_INVALID_HANDLE; }
static uint32_t s_getMode(CPU *c, const uint32_t *a) { (void)c; son_t *s = son_de(A(0)); if (A(1)) wr32(A(1), s ? s->mode : 0); return s ? 0 : ERR_INVALID_HANDLE; }
static uint32_t s_setLoopCount(CPU *c, const uint32_t *a) { (void)c; son_t *s = son_de(A(0)); if (s) s->boucles = (int)A(1); return s ? 0 : ERR_INVALID_HANDLE; }
static double en_frames(son_t *s, uint32_t v, uint32_t unite)
{
    if (unite == 1) return v * (double)s->rate / 1000.0;                                 /* MS */
    if (unite == 4) return v / (2.0 * (s->nch ? s->nch : 1));                           /* PCMBYTES */
    return (double)v;                                                                    /* PCM */
}
static uint32_t s_setLoopPoints(CPU *c, const uint32_t *a)      /* (début, unité, fin, unité) */
{ (void)c; son_t *s = son_de(A(0)); if (!s) return ERR_INVALID_HANDLE; s->bdeb = (uint32_t)en_frames(s, A(1), A(2)); s->bfin = (uint32_t)en_frames(s, A(3), A(4)); return 0; }
static uint32_t s_getLoopPoints(CPU *c, const uint32_t *a)
{
    (void)c; son_t *s = son_de(A(0)); if (!s) return ERR_INVALID_HANDLE;
    uint32_t bf = s->bfin && s->bfin < s->frames ? s->bfin : s->frames ? s->frames - 1 : 0;
    if (A(1)) wr32(A(1), en_unite(s, s->bdeb, A(2)));
    if (A(3)) wr32(A(3), en_unite(s, bf, A(4)));
    return 0;
}
static uint32_t s_getLoopCount(CPU *c, const uint32_t *a) { (void)c; son_t *s = son_de(A(0)); if (A(1)) wr32(A(1), s ? (uint32_t)s->boucles : 0); return s ? 0 : ERR_INVALID_HANDLE; }

/* Channel */
#define CANAL canal_t *ch = canal_de(A(0)); if (!ch) return ERR_INVALID_HANDLE
static float flt(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static void ecrire_flt(uint32_t va, float f) { if (va) memcpy(GPTR(va), &f, 4); }
static uint32_t c_setPaused(CPU *c, const uint32_t *a)
{
    (void)c; CANAL; int p = A(1) & 0xFF;
    if (X.piste && p != ch->pause)                               /* passe 7z : P <canal> <horloge> <0|1> <son> */
        fprintf(X.piste, "P %d %llu %d %s\n", (int)(ch - X.c), (unsigned long long)X.horloge, p, ch->dsp ? "(DSP)" : X.s[ch->son].nom);
    ch->pause = p; return 0;
}
static uint32_t c_getPaused(CPU *c, const uint32_t *a) { (void)c; CANAL; ecrire_bool(A(1), ch->pause); return 0; }
static uint32_t c_setVolume(CPU *c, const uint32_t *a) { (void)c; CANAL; float v = flt(A(1)); ch->vol = v < 0 ? 0 : v; return 0; }
static uint32_t c_getVolume(CPU *c, const uint32_t *a) { (void)c; CANAL; ecrire_flt(A(1), ch->vol); return 0; }
static uint32_t c_setMute(CPU *c, const uint32_t *a) { (void)c; CANAL; ch->muet = A(1) & 0xFF; return 0; }
static uint32_t c_getMute(CPU *c, const uint32_t *a) { (void)c; CANAL; ecrire_bool(A(1), ch->muet); return 0; }
static uint32_t c_setFrequency(CPU *c, const uint32_t *a) { (void)c; CANAL; ch->freq = flt(A(1)); return 0; }
static uint32_t c_getFrequency(CPU *c, const uint32_t *a)
{ (void)c; CANAL; ecrire_flt(A(1), ch->freq > 0 ? ch->freq : ch->dsp ? (float)X.freq : (float)X.s[ch->son].rate); return 0; }
static uint32_t c_stop(CPU *c, const uint32_t *a)
{
    (void)c; pthread_mutex_lock(&X.mu); canal_t *ch = canal_de(A(0));
    if (ch && X.piste && ch->joue)                               /* passe 7z : X <canal> <horloge> <son> */
        fprintf(X.piste, "X %d %llu %s\n", (int)(ch - X.c), (unsigned long long)X.horloge, ch->dsp ? "(DSP)" : X.s[ch->son].nom);
    if (ch) JOURNAL("Channel::stop « %s »%s", ch->dsp ? "(DSP)" : X.s[ch->son].nom, ch->joue ? "" : " (déjà arrêté)");
    if (ch) ch->joue = 0;
    pthread_mutex_unlock(&X.mu); return ch ? 0 : ERR_INVALID_HANDLE;
}
static uint32_t c_isPlaying(CPU *c, const uint32_t *a) { (void)c; CANAL; ecrire_bool(A(1), ch->joue); return 0; }
static uint32_t c_setLoopCount(CPU *c, const uint32_t *a) { (void)c; CANAL; ch->boucles = (int)A(1); return 0; }
static uint32_t c_getLoopCount(CPU *c, const uint32_t *a) { (void)c; CANAL; if (A(1)) wr32(A(1), (uint32_t)ch->boucles); return 0; }
static uint32_t c_setLoopPoints(CPU *c, const uint32_t *a)      /* (début, unité, fin, unité) */
{ (void)c; CANAL; son_t *s = &X.s[ch->son]; ch->bdeb = (uint32_t)en_frames(s, A(1), A(2)); ch->bfin = (uint32_t)en_frames(s, A(3), A(4)); return 0; }
static uint32_t c_getLoopPoints(CPU *c, const uint32_t *a)
{
    (void)c; CANAL; son_t *s = &X.s[ch->son];
    uint32_t bf = ch->bfin && ch->bfin < s->frames ? ch->bfin : s->frames ? s->frames - 1 : 0;
    if (A(1)) wr32(A(1), en_unite(s, ch->bdeb, A(2)));
    if (A(3)) wr32(A(3), en_unite(s, bf, A(4)));
    return 0;
}
static uint32_t c_setPosition(CPU *c, const uint32_t *a)
{
    (void)c; CANAL;
    if (ch->dsp) return 0;                                       /* passe 7w6 : canal DSP (pas de son à déplacer) */
    son_t *s = &X.s[ch->son];
    double f = A(2) == 1 ? A(1) * (double)s->rate / 1000.0 : A(2) == 4 ? A(1) / (2.0 * s->nch) : (double)A(1);
    if (s->flux) { stb_vorbis_seek(s->v, (unsigned)f); ch->tdeb = (int)f; ch->tlen = 0; }
    ch->pos = f;
    return 0;
}
static uint32_t c_getPosition(CPU *c, const uint32_t *a)
{
    (void)c; CANAL;
    if (ch->dsp) {                                               /* passe 7w6 : frames demandées au générateur */
        double f = ch->pos;
        if (A(1)) wr32(A(1), A(2) == 1 ? (uint32_t)(f * 1000.0 / X.freq) : A(2) == 4 ? (uint32_t)(f * 8) : (uint32_t)f);
        return 0;
    }
    if (A(1)) wr32(A(1), en_unite(&X.s[ch->son], ch->pos, A(2)));
    return 0;
}
static uint32_t c_setChannelGroup(CPU *c, const uint32_t *a) { (void)c; CANAL; ch->groupe = sc2_fmod_index(A(1), G_GROUP); return 0; }
static uint32_t c_setUserData(CPU *c, const uint32_t *a) { (void)c; CANAL; ch->userdata = A(1); return 0; }
static uint32_t c_getUserData(CPU *c, const uint32_t *a) { (void)c; CANAL; if (A(1)) wr32(A(1), ch->userdata); return 0; }
static uint32_t c_setCallback(CPU *c, const uint32_t *a) { (void)c; CANAL; ch->rappel = A(1); return 0; }
static uint32_t c_getSystemObject(CPU *c, const uint32_t *a)
{ (void)c; canal_t *ch = canal_de(A(0)); if (A(1)) wr32(A(1), ch && ch->systeme ? ch->systeme : X.maitre); return 0; }
/* passe 7v : modes de boucle, panoramique, audibilité, horloge DSP et départs/arrêts programmés */
#define M_LOOP_OFF 0x00000001u
static uint32_t c_setMode(CPU *c, const uint32_t *a)
{
    (void)c; CANAL; uint32_t m = A(1);
    if (m & (M_LOOP_OFF | M_LOOP_NORMAL | M_LOOP_BIDI)) ch->mode &= ~(M_LOOP_OFF | M_LOOP_NORMAL | M_LOOP_BIDI);
    ch->mode |= m;
    if (m & 0x18u) ch->mode = (ch->mode & ~0x18u) | (m & 0x18u);                       /* 2D / 3D */
    return 0;
}
static uint32_t c_getMode(CPU *c, const uint32_t *a) { (void)c; CANAL; if (A(1)) wr32(A(1), ch->mode); return 0; }
static uint32_t c_setPan(CPU *c, const uint32_t *a) { (void)c; CANAL; float p = flt(A(1)); ch->pan = p < -1 ? -1 : p > 1 ? 1 : p; return 0; }
static uint32_t c_getPan(CPU *c, const uint32_t *a) { (void)c; CANAL; ecrire_flt(A(1), ch->pan); return 0; }
static uint32_t c_getAudibility(CPU *c, const uint32_t *a)
{
    (void)c; CANAL; float g = ch->muet ? 0.f : ch->vol;
    if (ch->groupe >= 0 && ch->groupe < X.ng) g *= X.g[ch->groupe].muet ? 0.f : X.g[ch->groupe].vol;
    ecrire_flt(A(1), g);
    return 0;
}
/* passe 7z : 3D (voir canal_3d) */
static void lire_vec(uint32_t va, float *v) { if (va) memcpy(v, GPTR(va), 12); }
static uint32_t c_set3DAttributes(CPU *c, const uint32_t *a)    /* (pos*, vel*) */
{
    (void)c; CANAL; if (A(1)) { lire_vec(A(1), ch->p3); ch->p3ok = 1; }
    if (++X.n3d_attr <= 8)
        sc2_trace("[AUDIO] set3DAttributes « %s » : (%.1f, %.1f, %.1f), mode 0x%X ; auditeur (%.1f, %.1f, %.1f)",
                ch->dsp ? "(DSP)" : X.s[ch->son].nom, ch->p3[0], ch->p3[1], ch->p3[2], ch->mode, X.lpos[0], X.lpos[1], X.lpos[2]);
    return 0;
}
static uint32_t c_set3DPanLevel(CPU *c, const uint32_t *a) { (void)c; CANAL; ch->niv3d = flt(A(1)); return 0; }
static uint32_t c_get3DPanLevel(CPU *c, const uint32_t *a) { (void)c; CANAL; ecrire_flt(A(1), ch->niv3d); return 0; }
static uint32_t c_set3DOcclusion(CPU *c, const uint32_t *a) { (void)c; CANAL; ch->occl = flt(A(1)); return 0; }
static uint32_t c_get3DOcclusion(CPU *c, const uint32_t *a) { (void)c; CANAL; ecrire_flt(A(1), ch->occl); ecrire_flt(A(2), 0.f); return 0; }
static uint32_t m_set3DListenerAttributes(CPU *c, const uint32_t *a)   /* (auditeur, pos*, vel*, avant*, haut*) */
{
    (void)c; pthread_mutex_lock(&X.mu);
    if (maitre(A(0), 0) && A(1) == 0) {
        lire_vec(A(2), X.lpos); lire_vec(A(4), X.lfwd); lire_vec(A(5), X.lup); X.lok = 1;
        if (++X.n3d_ecoute <= 4)
            sc2_trace("[AUDIO] set3DListenerAttributes : pos (%.1f, %.1f, %.1f), avant (%.2f, %.2f, %.2f), haut (%.2f, %.2f, %.2f)",
                    X.lpos[0], X.lpos[1], X.lpos[2], X.lfwd[0], X.lfwd[1], X.lfwd[2], X.lup[0], X.lup[1], X.lup[2]);
    }
    pthread_mutex_unlock(&X.mu);
    return 0;
}
static uint32_t m_set3DRolloffCallback(CPU *c, const uint32_t *a)
{
    (void)c; pthread_mutex_lock(&X.mu);
    if (maitre(A(0), 0)) { X.rolloff = A(1); sc2_log("[AUDIO] set3DRolloffCallback : rappel d'atténuation du jeu 0x%08X", A(1)); }
    pthread_mutex_unlock(&X.mu);
    return 0;
}
static uint32_t m_set3DSettings(CPU *c, const uint32_t *a)      /* (doppler, facteur de distance, échelle d'atténuation) */
{
    (void)c; pthread_mutex_lock(&X.mu);
    if (maitre(A(0), 0)) { X.doppler = flt(A(1)); X.distf = flt(A(2)); X.roll_echelle = flt(A(3)); }
    pthread_mutex_unlock(&X.mu);
    return 0;
}
static uint32_t m_get3DSettings(CPU *c, const uint32_t *a)
{ (void)c; ecrire_flt(A(1), X.doppler); ecrire_flt(A(2), X.distf); ecrire_flt(A(3), X.roll_echelle); return 0; }
static uint32_t m_getDSPClock(CPU *c, const uint32_t *a)        /* (hi*, lo*) */
{
    (void)c; pthread_mutex_lock(&X.mu);
    if (A(1)) wr32(A(1), (uint32_t)(X.horloge >> 32));
    if (A(2)) wr32(A(2), (uint32_t)X.horloge);
    pthread_mutex_unlock(&X.mu);
    return 0;
}
/* FMOD_DELAYTYPE (4.28) : 0 END_MS, 1 DSPCLOCK_START, 2 DSPCLOCK_END, 3 DSPCLOCK_PAUSE */
static uint32_t c_setDelay(CPU *c, const uint32_t *a)           /* (type, hi, lo) */
{
    (void)c; pthread_mutex_lock(&X.mu);
    canal_t *ch = canal_de(A(0));
    if (ch) {
        uint64_t v = ((uint64_t)A(2) << 32) | A(3);
        if (A(1) == 1) ch->debut = v; else if (A(1) == 2) ch->fin = v;
        if (++X.retards <= 6)
            sc2_trace("[AUDIO] setDelay « %s » : %s à l'horloge DSP %llu (maintenant %llu, %+.2f s)", X.s[ch->son].nom,
                    A(1) == 1 ? "départ" : A(1) == 2 ? "arrêt" : "autre", (unsigned long long)v, (unsigned long long)X.horloge,
                    ((double)v - (double)X.horloge) / X.freq);
    }
    pthread_mutex_unlock(&X.mu);
    return ch ? 0 : ERR_INVALID_HANDLE;
}
static uint32_t c_getDelay(CPU *c, const uint32_t *a)           /* (type, hi*, lo*) */
{
    (void)c; CANAL; uint64_t v = A(1) == 1 ? ch->debut : A(1) == 2 ? ch->fin : 0;
    if (A(2)) wr32(A(2), (uint32_t)(v >> 32));
    if (A(3)) wr32(A(3), (uint32_t)v);
    return 0;
}

/* ChannelGroup */
static uint32_t nouveau_groupe(void)
{
    if (X.ng >= MAX_GROUPES) return sc2_fmod_objet(G_GROUP, -1);
    groupe_t *g = &X.g[X.ng]; g->vivant = 1; g->vol = 1.f; g->objet = sc2_fmod_objet(G_GROUP, X.ng); X.ng++;
    return g->objet;
}
static uint32_t m_getMasterChannelGroup(CPU *c, const uint32_t *a)
{
    (void)c; pthread_mutex_lock(&X.mu);
    static uint32_t maitre; if (!maitre) maitre = nouveau_groupe();
    if (A(1)) wr32(A(1), maitre);
    pthread_mutex_unlock(&X.mu);
    return 0;
}
static uint32_t m_createChannelGroup(CPU *c, const uint32_t *a)
{ (void)c; pthread_mutex_lock(&X.mu); uint32_t o = nouveau_groupe(); if (A(2)) wr32(A(2), o); pthread_mutex_unlock(&X.mu); return 0; }
static uint32_t g_setVolume(CPU *c, const uint32_t *a) { (void)c; groupe_t *g = groupe_de(A(0)); if (g) { float v = flt(A(1)); g->vol = v < 0 ? 0 : v; } return 0; }
static uint32_t g_getVolume(CPU *c, const uint32_t *a) { (void)c; groupe_t *g = groupe_de(A(0)); ecrire_flt(A(1), g ? g->vol : 1.f); return 0; }
static uint32_t g_setMute(CPU *c, const uint32_t *a) { (void)c; groupe_t *g = groupe_de(A(0)); if (g) g->muet = A(1) & 0xFF; return 0; }

const sc2_audio_impl_t sc2_audio_impl[] = {
    { "System::setFileSystem", m_setFileSystem }, { "System::setSoftwareFormat", m_setSoftwareFormat },
    { "System::init", m_init }, { "System::createSound", m_createSound }, { "System::playSound", m_playSound },
    { "System::update", m_update }, { "System::playDSP", m_playDSP }, { "System::getMasterChannelGroup", m_getMasterChannelGroup },
    { "System::createChannelGroup", m_createChannelGroup },
    { "Sound::release", s_release }, { "Sound::getLength", s_getLength }, { "Sound::getDefaults", s_getDefaults },
    { "Sound::getFormat", s_getFormat }, { "Sound::getOpenState", s_getOpenState }, { "Sound::getMode", s_getMode },
    { "Sound::setLoopCount", s_setLoopCount }, { "Sound::getLoopCount", s_getLoopCount },
    { "Channel::setPaused", c_setPaused }, { "Channel::getPaused", c_getPaused }, { "Channel::setVolume", c_setVolume },
    { "Channel::getVolume", c_getVolume }, { "Channel::setMute", c_setMute }, { "Channel::getMute", c_getMute },
    { "Channel::setFrequency", c_setFrequency }, { "Channel::getFrequency", c_getFrequency }, { "Channel::stop", c_stop },
    { "Channel::isPlaying", c_isPlaying }, { "Channel::setLoopCount", c_setLoopCount }, { "Channel::getLoopCount", c_getLoopCount },
    { "Channel::setPosition", c_setPosition }, { "Channel::getPosition", c_getPosition },
    { "Channel::setChannelGroup", c_setChannelGroup }, { "Channel::setUserData", c_setUserData },
    { "Channel::getUserData", c_getUserData }, { "Channel::setCallback", c_setCallback },
    { "Channel::getSystemObject", c_getSystemObject },
    { "Channel::setMode", c_setMode }, { "Channel::getMode", c_getMode }, { "Channel::setPan", c_setPan },
    { "Channel::getPan", c_getPan }, { "Channel::getAudibility", c_getAudibility }, { "System::getDSPClock", m_getDSPClock },
    { "Channel::setDelay", c_setDelay }, { "Channel::getDelay", c_getDelay },
    { "Channel::setLoopPoints", c_setLoopPoints }, { "Channel::getLoopPoints", c_getLoopPoints },
    { "Sound::setLoopPoints", s_setLoopPoints }, { "Sound::getLoopPoints", s_getLoopPoints },
    { "Channel::set3DAttributes", c_set3DAttributes }, { "Channel::set3DPanLevel", c_set3DPanLevel },
    { "Channel::get3DPanLevel", c_get3DPanLevel }, { "Channel::set3DOcclusion", c_set3DOcclusion },
    { "Channel::get3DOcclusion", c_get3DOcclusion }, { "System::set3DListenerAttributes", m_set3DListenerAttributes },
    { "System::set3DRolloffCallback", m_set3DRolloffCallback }, { "System::set3DSettings", m_set3DSettings },
    { "System::get3DSettings", m_get3DSettings },
    { "ChannelGroup::setVolume", g_setVolume }, { "ChannelGroup::getVolume", g_getVolume }, { "ChannelGroup::setMute", g_setMute },
};
const unsigned sc2_audio_nb_impl = sizeof sc2_audio_impl / sizeof sc2_audio_impl[0];

static int g_cree;
void sc2_audio_systeme_cree(void)
{
    if (g_cree++) return;
    atexit(sc2_audio_bilan);
    sc2_log("[AUDIO] FMOD_System_Create → System FMOD logiciel (recomp-sc2, passe 5e)");
}

void sc2_audio_bilan_mix(void)
{
    pthread_mutex_lock(&X.mu);
    X.fini = 1;
    if (sc2_audio_compteur_installe()) sc2_audio_compteur_bilan();
    printf("[BILAN] [AUDIO] mixage : %llu frames (%.1f s à %d Hz), %llu échantillons non nuls, crête %.3f ; sons créés %llu "
           "(échecs %llu), lectures par les rappels fichier du jeu %llu (%llu o) ; playSound %llu, fins %llu, rappels END %llu\n",
           (unsigned long long)X.frames_mixees, (double)X.frames_mixees / X.freq, X.freq, (unsigned long long)X.frames_non_nulles,
           X.crete, (unsigned long long)X.sons_crees, (unsigned long long)X.sons_ko, (unsigned long long)X.lectures_jeu,
           (unsigned long long)X.octets_jeu, (unsigned long long)X.joues, (unsigned long long)X.fins, (unsigned long long)X.rappels);
    printf("[BILAN] [AUDIO] System maître 0x%08X : update %llu (autres System : %llu, sans effet) ; setSoftwareFormat ignorés %llu ; "
           "horloge DSP %llu frames ; setDelay %llu, départs programmés tenus %llu\n", X.maitre, (unsigned long long)X.upd_maitre,
           (unsigned long long)X.upd_autres, (unsigned long long)X.formats_ignores, (unsigned long long)X.horloge,
           (unsigned long long)X.retards, (unsigned long long)X.demarres_retard);
    printf("[BILAN] [AUDIO] playDSP %llu ; frames demandées aux générateurs DSP du jeu %llu\n", (unsigned long long)X.dsp_joues,
           (unsigned long long)X.frames_dsp);
    if (X.wav) {
        uint64_t f = X.wav_frames;
        wav_fin();
        printf("[BILAN] [AUDIO] WAV %s : %llu frames, %llu octets\n", X.wav_nom, (unsigned long long)f, (unsigned long long)(44 + f * 4));
    }
    printf("[BILAN] [AUDIO] 3D %s : set3DAttributes %llu, set3DListenerAttributes (maître) %llu, blocs mixés en 3D %llu, "
           "rappel d'atténuation 0x%08X appelé %llu fois (dernier gain %.3f)\n", X.mode3d ? "active" : "IGNORÉE",
           (unsigned long long)X.n3d_attr, (unsigned long long)X.n3d_ecoute, (unsigned long long)X.n3d_blocs, X.rolloff,
           (unsigned long long)X.n3d_rappels, X.dernier_att);
    if (X.piste) {                                                  /* passe 7z */
        fclose(X.piste); X.piste = NULL;
        printf("[BILAN] [AUDIO] piste (SC2_AUDIO_PISTE) : %llu segments\n", (unsigned long long)X.piste_seg);
    }
    pthread_mutex_unlock(&X.mu);
}
