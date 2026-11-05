/* video_amc.c — passe 9s : cinématiques H.264 décodées par le matériel (AMediaCodec), cf. video_amc.h.
 *
 * Accroches --wrap=L_<va> (android/CMakeLists.txt) : la table de dispatch du runtime référence chaque L_<va> depuis
 * sc2_runtime.c, l'éditeur de liens envoie donc TOUS les appels lifté → lifté de ces fonctions (dispatch(c, va),
 * SC2_SAUT compris) sur __wrap_L_<va> ; l'original reste __real_L_<va>. Convention des fonctions visées (code LTCG de
 * SC2.exe, relevé à la main : android/TRACE-PASSE9S.md §1) : arguments dans ecx/edx (+ pile), retour eax, `ret` simple
 * sauf mention ; un remplacement émule donc `c->esp += 4` comme le `ret` lifté.
 *
 * Mode matériel d'une session (état theora_state du jeu ↔ .mp4 frère) :
 *   - packetin : ogg_packet.bytes mis à 0 le temps de l'appel → branche « image dupliquée » du vrai décodeur
 *     (granulepos et compteur d'images tenus par la libtheora elle-même, rendu 0), rien n'est décodé ;
 *   - YUVout : image n = trame du granulepos courant (le jeu y recopie le granulepos du paquet ogg) ; le décodeur
 *     matériel est amené à l'image n (recherche arrière ou saut avant au besoin) et ses plans sont recopiés dans
 *     des plans INVITÉS (sc2_alloc) rendus au jeu sous la forme exacte de la libtheora (pas négatif, ligne du haut) ;
 *   - un échec du décodeur matériel en cours de lecture laisse la dernière image (jamais d'arrêt du jeu).
 */
#define _GNU_SOURCE
#include "sc2_lifted.h"
#include "cpu.h"
#include "sc2_host.h"
#include "video_amc.h"
#include "sc2_input.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImageReader.h>
#endif

/* ---- fonctions liftées interceptées (SC2.exe build 15405) ------------------------------------------------------ */
void __real_L_00C73940(CPU *c);   /* ouverture vidéo du jeu (thiscall, [esp+4] = nom, ret 0xC) */
void __real_L_01093B70(CPU *c);   /* theora_decode_init */
void __real_L_01093C60(CPU *c);   /* theora_decode_packetin */
void __real_L_01093CB0(CPU *c);   /* theora_decode_YUVout */
void __real_L_01093810(CPU *c);   /* theora_clear */

/* theora_info (API historique, 0x68 octets) et theora_state */
enum { TI_W = 0x00, TI_H = 0x04, TI_FW = 0x08, TI_FH = 0x0C, TI_OX = 0x10, TI_OY = 0x14, TI_FPSN = 0x18, TI_FPSD = 0x1C,
       TI_VMAJ = 0x38, TI_VMIN = 0x39, TI_VSUB = 0x3A, TI_KFF = 0x4C, TI_PIXFMT = 0x64 };
enum { TS_INFO = 0x00, TS_GRANPOS = 0x08 };
enum { OP_BYTES = 0x04 };

/* ---- sessions ------------------------------------------------------------------------------------------------- */
typedef struct session session;
typedef struct {
    const char *nom;
    int  (*ouvrir)(session *s, const char *mp4);          /* 0 si l'image 0 est décodable */
    int  (*image)(session *s, int64_t n, uint8_t *y, int ys, uint8_t *u, uint8_t *v, int uvs);   /* 0 : plans écrits */
    void (*fermer)(session *s);
} moteur;

struct session {
    uint32_t etat, info;              /* VA invitées */
    char nom[256];
    uint32_t w, h, pw, ph, px, py;    /* tampon Theora (multiple de 16), image utile, décalage depuis le haut */
    uint32_t fpsn, fpsd;
    int shift, biais;
    int materiel;                     /* 1 : images du moteur ci-dessous, Theora non décodé */
    const moteur *m;
    void *priv;
    uint32_t plans;                   /* Y | Cb | Cr invités, image de haut en bas */
    int64_t n_ecrite;                 /* image présente dans les plans (-1 : aucune) */
    double duree_s;                   /* durée du .mp4 (0 : inconnue) */
    double secours_s;
    int sautable;
    atomic_int saut;                  /* sauts demandés (Échap injectée) */
    atomic_llong saut_ns;             /* dernier saut : la sortie de secours relance toutes les 5 s */
    struct timespec t0;
    atomic_llong dernier_paquet_ns;
    uint64_t paquets, images, echecs, ns_image, ns_max;
};

#define NSESS 8
static session *g_s[NSESS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_actif = -1, g_saut_actif, g_saut_min = 1024;
static atomic_uint g_ouvertes, g_materielles, g_sauts, g_secours;
static __thread char t_nom[256];      /* nom passé à l'ouverture vidéo en cours sur ce fil */

static int64_t maintenant_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }

static int actif(void)
{
    if (g_actif < 0) {
        const char *e = getenv("SC2_VIDEO_AMC");
        g_actif = e && !strcmp(e, "1");
        if (g_actif) {
            const char *s = getenv("SC2_VIDEO_SAUT"), *m = getenv("SC2_VIDEO_SAUT_MIN");
            g_saut_actif = !(s && !strcmp(s, "0"));
            if (m && atoi(m) > 0) g_saut_min = atoi(m);
            sc2_log("[VIDEO-AMC] actif : .mp4 frère → décodage matériel%s (saut Échap/toucher %s, plein écran ≥ %d px)",
                    getenv("SC2_VIDEO_AMC_MIRE") && !strcmp(getenv("SC2_VIDEO_AMC_MIRE"), "1") ? " [MIRE de banc]" : "",
                    g_saut_actif ? "oui" : "non", g_saut_min);
        }
    }
    return g_actif;
}

static session *trouver(uint32_t etat)
{
    session *r = NULL;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < NSESS; i++) if (g_s[i] && g_s[i]->etat == etat) { r = g_s[i]; break; }
    pthread_mutex_unlock(&g_mu);
    return r;
}

static void fermer(session *s, const char *pourquoi)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < NSESS; i++) if (g_s[i] == s) g_s[i] = NULL;
    pthread_mutex_unlock(&g_mu);
    if (s->m && s->m->fermer) s->m->fermer(s);
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    double dt = (t.tv_sec - s->t0.tv_sec) + (t.tv_nsec - s->t0.tv_nsec) / 1e9;
    sc2_log("[VIDEO-AMC] fin %s (%s) : %s, %llu paquets, %llu images matérielles (moy %.2f ms, max %.2f ms), %llu échecs, %.1f s%s",
            s->nom, pourquoi, s->materiel ? s->m->nom : "Theora lifté", (unsigned long long)s->paquets,
            (unsigned long long)s->images, s->images ? s->ns_image / 1e6 / s->images : 0.0, s->ns_max / 1e6,
            (unsigned long long)s->echecs, dt, atomic_load(&s->saut) ? " — après saut (Échap)" : "");
    free(s);
}

/* ---- moteur « mire » (banc x86 : valide l'accroche sans .mp4 ni AMediaCodec) ------------------------------------ */
static int mire_ouvrir(session *s, const char *mp4) { (void)mp4; s->duree_s = 0; return 0; }
static int mire_image(session *s, int64_t n, uint8_t *y, int ys, uint8_t *u, uint8_t *v, int uvs)
{
    uint32_t w = s->pw, h = s->ph, barre = (uint32_t)((n * 8) % (w ? w : 1));
    for (uint32_t r = 0; r < h; r++) {
        uint8_t *l = y + (ptrdiff_t)r * ys;
        for (uint32_t x = 0; x < w; x++) l[x] = (x >= barre && x < barre + 16) ? 235 : (uint8_t)(16 + (x * 200) / (w ? w : 1));
    }
    for (uint32_t r = 0; r < h / 2; r++) {
        memset(u + (ptrdiff_t)r * uvs, (int)(64 + (r * 128) / (h / 2 ? h / 2 : 1)), w / 2);
        memset(v + (ptrdiff_t)r * uvs, (int)(192 - (n % 128)), w / 2);
    }
    return 0;
}
static const moteur MIRE = { "mire", mire_ouvrir, mire_image, NULL };

/* ---- moteur AMediaCodec (Android) --------------------------------------------------------------------------- */
#ifdef __ANDROID__
/* Deux voies de sortie du décodeur :
 *   - LECTEUR (par défaut) : surface d'un AImageReader YUV_420_888 (lecture CPU), chaque image rendue arrive avec ses
 *     3 plans décrits (pas de ligne, pas de pixel, recadrage) — voie documentée pour lire sur CPU les images d'un
 *     décodeur matériel, quel que soit son format interne (NV12, tuilé fabricant…) ;
 *   - TAMPON (repli si le lecteur n'est pas disponible) : ByteBuffer, I420 (19) ou semi-planaire (21 et autres),
 *     disposition déduite de « stride » / « slice-height » / « crop-* » du format de sortie. */
typedef struct {
    int fd;
    AMediaExtractor *ex;
    AMediaCodec *co;
    AImageReader *lecteur;
    int32_t w, h, pas, haut, couleur, cg, ch, cd, cb;
    int64_t n_cour;                   /* dernière image sortie du décodeur (-1 : aucune) */
    int fin_entree, fin_sortie;
    uint8_t *der;                     /* dernière image sortie, I420 compact pw × ph (rejouée si besoin) */
} amc;

static void amc_format(session *s, amc *a)
{
    AMediaFormat *f = AMediaCodec_getOutputFormat(a->co);
    if (!f) return;
    int32_t v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &v)) a->w = v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &v)) a->h = v;
    a->pas = a->w; a->haut = a->h;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v > 0) a->pas = v;
    if (AMediaFormat_getInt32(f, "slice-height", &v) && v > 0) a->haut = v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v)) a->couleur = v;
    a->cg = 0; a->ch = 0; a->cd = a->w - 1; a->cb = a->h - 1;
    AMediaFormat_getInt32(f, "crop-left", &a->cg); AMediaFormat_getInt32(f, "crop-top", &a->ch);
    AMediaFormat_getInt32(f, "crop-right", &a->cd); AMediaFormat_getInt32(f, "crop-bottom", &a->cb);
    sc2_log("[VIDEO-AMC] %s : sortie %dx%d pas %d hauteur %d couleur %d (0x%x) recadrage %d,%d-%d,%d (%s)",
            s->nom, a->w, a->h, a->pas, a->haut, a->couleur, (unsigned)a->couleur, a->cg, a->ch, a->cd, a->cb,
            a->lecteur ? "voie lecteur YUV_420_888" : "voie tampon");
    AMediaFormat_delete(f);
}

/* Voie TAMPON → I420 compact (a->der). 19 = I420 planaire ; 21 et formats fabricant (Qualcomm 0x7FA30C04…) = NV12. */
static int amc_copier_tampon(session *s, amc *a, const uint8_t *b, size_t taille)
{
    uint32_t w = s->pw, h = s->ph, pas = (uint32_t)a->pas, haut = (uint32_t)a->haut;
    const uint8_t *Y = b + (size_t)a->ch * pas + a->cg, *C = b + (size_t)pas * haut;
    if ((size_t)pas * haut + (size_t)pas * (haut / 2) > taille) return -1;
    uint8_t *dy = a->der, *du = dy + w * h, *dv = du + (w / 2) * (h / 2);
    for (uint32_t r = 0; r < h; r++) memcpy(dy + r * w, Y + (size_t)r * pas, w);
    if (a->couleur == 19) {
        const uint8_t *U = C + (size_t)(a->ch / 2) * (pas / 2) + a->cg / 2, *V = U + (size_t)(pas / 2) * (haut / 2);
        for (uint32_t r = 0; r < h / 2; r++) { memcpy(du + r * (w / 2), U + (size_t)r * (pas / 2), w / 2); memcpy(dv + r * (w / 2), V + (size_t)r * (pas / 2), w / 2); }
    } else {
        const uint8_t *UV = C + (size_t)(a->ch / 2) * pas + (a->cg & ~1);
        for (uint32_t r = 0; r < h / 2; r++) {
            const uint8_t *l = UV + (size_t)r * pas;
            for (uint32_t x = 0; x < w / 2; x++) { du[r * (w / 2) + x] = l[2 * x]; dv[r * (w / 2) + x] = l[2 * x + 1]; }
        }
    }
    return 0;
}

/* Voie LECTEUR : attend (≤ 40 ms) l'image d'horodatage pts_us rendue dans l'AImageReader → I420 compact (a->der). */
static int amc_copier_lecteur(session *s, amc *a, int64_t pts_us)
{
    int64_t fin = maintenant_ns() + 40000000LL;
    AImage *im = NULL;
    for (;;) {
        AImage *x = NULL;
        if (AImageReader_acquireLatestImage(a->lecteur, &x) == AMEDIA_OK && x) {
            int64_t ts = 0; AImage_getTimestamp(x, &ts);
            if (ts / 1000 >= pts_us) { im = x; break; }        /* image plus ancienne restée en file : jetée */
            AImage_delete(x);
        }
        if (maintenant_ns() >= fin) return -1;
        usleep(500);
    }
    uint32_t w = s->pw, h = s->ph;
    AImageCropRect cr = { 0, 0, (int32_t)w, (int32_t)h };
    AImage_getCropRect(im, &cr);
    uint8_t *dst[3] = { a->der, a->der + w * h, a->der + w * h + (w / 2) * (h / 2) };
    int ok = 0;
    for (int p = 0; p < 3; p++) {
        uint8_t *d = NULL; int len = 0, rs = 0, ps = 0;
        if (AImage_getPlaneData(im, p, &d, &len) != AMEDIA_OK || AImage_getPlaneRowStride(im, p, &rs) != AMEDIA_OK
            || AImage_getPlanePixelStride(im, p, &ps) != AMEDIA_OK || !d) { ok = -1; break; }
        uint32_t pw = p ? w / 2 : w, ph = p ? h / 2 : h, x0 = (uint32_t)(p ? cr.left / 2 : cr.left), y0 = (uint32_t)(p ? cr.top / 2 : cr.top);
        if ((size_t)(y0 + ph - 1) * rs + (size_t)(x0 + pw - 1) * ps >= (size_t)len) { ok = -1; break; }
        for (uint32_t r = 0; r < ph; r++) {
            const uint8_t *l = d + (size_t)(y0 + r) * rs + (size_t)x0 * ps;
            uint8_t *o = dst[p] + r * pw;
            if (ps == 1) memcpy(o, l, pw); else for (uint32_t x = 0; x < pw; x++) o[x] = l[(size_t)x * ps];
        }
    }
    AImage_delete(im);
    return ok;
}

/* Amène le décodeur à l'image n (horodatage n·fpsd/fpsn) : alimente l'entrée, vide la sortie ; 0 si a->der = n. */
static int amc_vers(session *s, amc *a, int64_t n, int64_t delai_ns)
{
    int64_t cible = n * 1000000LL * s->fpsd / s->fpsn, demi = 500000LL * s->fpsd / s->fpsn;
    if (n < a->n_cour || n > a->n_cour + 4 * (int64_t)(s->fpsn / (s->fpsd ? s->fpsd : 1) + 1)) {   /* arrière / loin devant */
        AMediaExtractor_seekTo(a->ex, cible, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
        AMediaCodec_flush(a->co);
        a->fin_entree = a->fin_sortie = 0; a->n_cour = -1;
    }
    int64_t fin = maintenant_ns() + delai_ns;
    while (maintenant_ns() < fin) {
        if (!a->fin_entree) {
            ssize_t i = AMediaCodec_dequeueInputBuffer(a->co, 0);
            if (i >= 0) {
                size_t cap; uint8_t *buf = AMediaCodec_getInputBuffer(a->co, (size_t)i, &cap);
                ssize_t t = buf ? AMediaExtractor_readSampleData(a->ex, buf, cap) : -1;
                if (t < 0) { AMediaCodec_queueInputBuffer(a->co, (size_t)i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); a->fin_entree = 1; }
                else { AMediaCodec_queueInputBuffer(a->co, (size_t)i, 0, (size_t)t, AMediaExtractor_getSampleTime(a->ex), 0); AMediaExtractor_advance(a->ex); }
            }
        }
        AMediaCodecBufferInfo inf;
        ssize_t o = AMediaCodec_dequeueOutputBuffer(a->co, &inf, 2000);
        if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { amc_format(s, a); continue; }
        if (o < 0) { if (a->fin_sortie) return -1; continue; }
        int fin_flux = (inf.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        int64_t m = (inf.presentationTimeUs * s->fpsn + 500000LL * s->fpsd) / (1000000LL * s->fpsd);   /* n° d'image */
        int bonne = inf.size > 0 && inf.presentationTimeUs >= cible - demi, copie = -1;
        if (bonne && a->lecteur) {
            AMediaCodec_releaseOutputBuffer(a->co, (size_t)o, true);          /* rendue dans l'AImageReader */
            copie = amc_copier_lecteur(s, a, inf.presentationTimeUs);
        } else {
            if (bonne) { size_t cap; uint8_t *b = AMediaCodec_getOutputBuffer(a->co, (size_t)o, &cap); if (b) copie = amc_copier_tampon(s, a, b + inf.offset, (size_t)inf.size); }
            AMediaCodec_releaseOutputBuffer(a->co, (size_t)o, false);
        }
        if (inf.size > 0) a->n_cour = m;
        if (fin_flux) a->fin_sortie = 1;
        if (bonne) return copie;
        if (fin_flux) return -1;
    }
    return -1;
}

static void amc_fermer(session *s)
{
    amc *a = s->priv; if (!a) return;
    if (a->co) { AMediaCodec_stop(a->co); AMediaCodec_delete(a->co); }
    if (a->lecteur) AImageReader_delete(a->lecteur);
    if (a->ex) AMediaExtractor_delete(a->ex);
    if (a->fd >= 0) close(a->fd);
    free(a->der); free(a); s->priv = NULL;
}

/* Décodeur configuré et démarré, voie lecteur (surface AImageReader) ou tampon ; 0 si l'image 0 sort en 1 s. */
static int amc_demarrer(session *s, amc *a, const char *mp4, const char *mime, AMediaFormat *f, int lecteur)
{
    ANativeWindow *fen = NULL;
    if (lecteur) {
        if (AImageReader_new(a->w, a->h, AIMAGE_FORMAT_YUV_420_888, 3, &a->lecteur) != AMEDIA_OK
            || AImageReader_getWindow(a->lecteur, &fen) != AMEDIA_OK) {
            if (a->lecteur) AImageReader_delete(a->lecteur);
            a->lecteur = NULL;
            return -1;
        }
    } else AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F420888);   /* COLOR_FormatYUV420Flexible */
    a->co = AMediaCodec_createDecoderByType(mime);
    media_status_t r = a->co ? AMediaCodec_configure(a->co, f, fen, NULL, 0) : AMEDIA_ERROR_UNKNOWN;
    if (r == AMEDIA_OK) r = AMediaCodec_start(a->co);
    if (r == AMEDIA_OK && !amc_vers(s, a, 0, 1000000000LL)) return 0;
    sc2_log("[VIDEO-AMC] %s : voie %s en échec (décodeur %s, état %d)", mp4, lecteur ? "lecteur" : "tampon", mime, (int)r);
    if (a->co) { AMediaCodec_stop(a->co); AMediaCodec_delete(a->co); a->co = NULL; }
    if (a->lecteur) { AImageReader_delete(a->lecteur); a->lecteur = NULL; }
    AMediaExtractor_seekTo(a->ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
    a->fin_entree = a->fin_sortie = 0; a->n_cour = -1;
    return -1;
}

static int amc_ouvrir(session *s, const char *mp4)
{
    amc *a = calloc(1, sizeof *a); if (!a) return -1;
    s->priv = a; a->fd = -1; a->n_cour = -1;
    a->der = calloc(1, (size_t)s->pw * s->ph * 3 / 2);
    struct stat st;
    AMediaFormat *f = NULL; const char *mime = NULL;
    if (!a->der || (a->fd = open(mp4, O_RDONLY | O_CLOEXEC)) < 0 || fstat(a->fd, &st)) goto ko;
    a->ex = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(a->ex, a->fd, 0, st.st_size) != AMEDIA_OK) goto ko;
    for (size_t i = 0, n = AMediaExtractor_getTrackCount(a->ex); i < n; i++) {
        AMediaFormat *g = AMediaExtractor_getTrackFormat(a->ex, i);
        if (g && AMediaFormat_getString(g, AMEDIAFORMAT_KEY_MIME, &mime) && !strncmp(mime, "video/", 6)) { AMediaExtractor_selectTrack(a->ex, i); f = g; break; }
        if (g) AMediaFormat_delete(g);
    }
    if (!f) { sc2_log("[VIDEO-AMC] %s : aucune piste vidéo", mp4); goto ko; }
    int32_t w = 0, h = 0; int64_t d = 0;
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w); AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
    if (AMediaFormat_getInt64(f, AMEDIAFORMAT_KEY_DURATION, &d)) s->duree_s = d / 1e6;
    if ((uint32_t)w != s->pw || (uint32_t)h != s->ph) { sc2_log("[VIDEO-AMC] %s : %dx%d ≠ image Theora %ux%u — refusé", mp4, w, h, s->pw, s->ph); goto ko; }
    a->w = w; a->h = h; a->pas = w; a->haut = h; a->couleur = 21; a->cd = w - 1; a->cb = h - 1;
    const char *v = getenv("SC2_VIDEO_AMC_VOIE");      /* banc : « tampon » force la voie ByteBuffer */
    if ((v && !strcmp(v, "tampon")) || amc_demarrer(s, a, mp4, mime, f, 1))
        if (amc_demarrer(s, a, mp4, mime, f, 0)) goto ko;
    sc2_log("[VIDEO-AMC] %s : décodeur %s démarré, voie %s, %.1f s", mp4, mime, a->lecteur ? "lecteur YUV_420_888" : "tampon", s->duree_s);
    AMediaFormat_delete(f);
    return 0;
ko:
    if (f) AMediaFormat_delete(f);
    amc_fermer(s);
    return -1;
}

static int amc_image(session *s, int64_t n, uint8_t *y, int ys, uint8_t *u, uint8_t *v, int uvs)
{
    amc *a = s->priv;
    int r = n != a->n_cour ? amc_vers(s, a, n, 50000000LL) : 0;    /* 50 ms max : sinon l'image d'avant reste */
    uint32_t w = s->pw, h = s->ph;
    const uint8_t *dy = a->der, *du = dy + w * h, *dv = du + (w / 2) * (h / 2);
    for (uint32_t i = 0; i < h; i++) memcpy(y + (ptrdiff_t)i * ys, dy + i * w, w);
    for (uint32_t i = 0; i < h / 2; i++) { memcpy(u + (ptrdiff_t)i * uvs, du + i * (w / 2), w / 2); memcpy(v + (ptrdiff_t)i * uvs, dv + i * (w / 2), w / 2); }
    return r;
}
static const moteur AMC = { "AMediaCodec", amc_ouvrir, amc_image, amc_fermer };
#endif

/* ---- recherche du .mp4 frère -------------------------------------------------------------------------------- */
static int existe(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode) && st.st_size > 0; }

/* <dir>/<rel> sans tenir compte de la casse, composant par composant (le jeu écrit « Cinematic_BetterMarine.ogv »,
 * l'archive liste « cinematic_bettermarine.ogv ») ; 0 et chemin réel dans o si trouvé. */
static int chercher_casse(const char *dir, const char *rel, char *o, size_t cap)
{
    char reste[256]; snprintf(reste, sizeof reste, "%s", rel);
    snprintf(o, cap, "%s", dir);
    char *sv = NULL;
    for (char *c = strtok_r(reste, "/", &sv); c; c = strtok_r(NULL, "/", &sv)) {
        DIR *d = opendir(o); if (!d) return -1;
        struct dirent *e; int vu = 0; size_t l = strlen(o);
        while ((e = readdir(d))) if (!strcasecmp(e->d_name, c)) { snprintf(o + l, cap - l, "/%s", e->d_name); vu = 1; break; }
        closedir(d);
        if (!vu) return -1;
    }
    return existe(o) ? 0 : -1;
}

static int mp4_frere(const char *nom, char *o, size_t cap)
{
    const char *dir = getenv("SC2_VIDEO_AMC_DIR");
    if (!dir || !*dir) dir = getenv("SC2_INSTALL");
    if (!dir || !*dir) dir = getenv("SC2_ROOT");
    o[0] = 0;
    if (!dir || !*dir || !*nom) return -1;
    char rel[256]; size_t i = 0;
    for (const char *p = nom; *p && i + 5 < sizeof rel; p++) rel[i++] = *p == '\\' ? '/' : *p;
    rel[i] = 0;
    char *pt = strrchr(rel, '.'), *sl = strrchr(rel, '/');
    if (pt && (!sl || pt > sl)) *pt = 0;
    const char *base = strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel;
    char r2[264], b2[264];
    snprintf(r2, sizeof r2, "%s.mp4", rel); snprintf(b2, sizeof b2, "%s.mp4", base);
    snprintf(o, cap, "%s/%s", dir, r2);
    if (existe(o) || !chercher_casse(dir, r2, o, cap)) return 0;
    snprintf(o, cap, "%s/%s", dir, b2);
    if (existe(o) || !chercher_casse(dir, b2, o, cap)) return 0;
    o[0] = 0;
    return -1;
}

static int ilog(uint32_t v) { int r = 0; while (v) { r++; v >>= 1; } return r; }

static void ouvrir_session(uint32_t etat, uint32_t info, const char *nom)
{
    session *ancien = trouver(etat);
    if (ancien) fermer(ancien, "état réinitialisé");
    session *s = calloc(1, sizeof *s);
    if (!s) return;
    s->etat = etat; s->info = info; s->n_ecrite = -1;
    snprintf(s->nom, sizeof s->nom, "%s", *nom ? nom : "(nom inconnu)");
    s->w = rd32(info + TI_W); s->h = rd32(info + TI_H);
    s->pw = rd32(info + TI_FW); s->ph = rd32(info + TI_FH); s->px = rd32(info + TI_OX);
    uint32_t oy = rd32(info + TI_OY);
    s->py = s->h >= s->ph + oy ? s->h - s->ph - oy : 0;      /* offset_y Theora compté depuis le bas */
    s->fpsn = rd32(info + TI_FPSN); s->fpsd = rd32(info + TI_FPSD);
    uint32_t kff = rd32(info + TI_KFF);
    s->shift = kff > 1 ? ilog(kff - 1) : 0;
    s->biais = (rd8(info + TI_VMAJ) << 16 | rd8(info + TI_VMIN) << 8 | rd8(info + TI_VSUB)) >= 0x030201;
    s->sautable = s->pw >= (uint32_t)g_saut_min;
    clock_gettime(CLOCK_MONOTONIC, &s->t0);
    atomic_store(&s->dernier_paquet_ns, 0);
    const char *se = getenv("SC2_VIDEO_SECOURS");
    s->secours_s = se ? atof(se) : 0;

    int ok_fmt = rd32(info + TI_PIXFMT) == 0 && s->fpsn && s->fpsd && s->pw && s->ph && !(s->pw & 1) && !(s->ph & 1)
                 && s->px + s->pw <= s->w && s->py + s->ph <= s->h;
    const char *mire = getenv("SC2_VIDEO_AMC_MIRE");
    char mp4[512] = "";
    const moteur *m = NULL;
    if (ok_fmt && mire && !strcmp(mire, "1")) m = &MIRE;
#ifdef __ANDROID__
    else if (ok_fmt && !mp4_frere(s->nom, mp4, sizeof mp4)) m = &AMC;
#else
    else if (ok_fmt && !mp4_frere(s->nom, mp4, sizeof mp4))
        sc2_log("[VIDEO-AMC] %s : %s présent, mais pas d'AMediaCodec sur cette plate-forme → Theora", s->nom, mp4);
#endif
    if (m && !m->ouvrir(s, mp4)) {
        s->plans = sc2_alloc(s->w * s->h + 2 * (s->w / 2) * (s->h / 2));
        if (s->plans) {
            s->m = m; s->materiel = 1;
            uint8_t *p = GPTR(s->plans);
            memset(p, 16, s->w * s->h); memset(p + s->w * s->h, 128, 2 * (s->w / 2) * (s->h / 2));   /* noir vidéo */
            if (!se && s->duree_s > 0) s->secours_s = 2 * s->duree_s + 30;
            atomic_fetch_add(&g_materielles, 1);
        } else if (m->fermer) m->fermer(s);
    }
    pthread_mutex_lock(&g_mu);
    int pose = 0;
    for (int i = 0; i < NSESS && !pose; i++) if (!g_s[i]) { g_s[i] = s; pose = 1; }
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_ouvertes, 1);
    sc2_log("[VIDEO-AMC] ouverture %s : tampon %ux%u, image %ux%u à (%u,%u), %u/%u i/s, shift %d → %s%s%s%s",
            s->nom, s->w, s->h, s->pw, s->ph, s->px, s->py, s->fpsn, s->fpsd, s->shift,
            s->materiel ? s->m->nom : "Theora lifté (inchangé)", *mp4 ? " " : "", mp4,
            ok_fmt ? "" : " [format non pris en charge]");
    if (!pose) { if (s->m && s->m->fermer) s->m->fermer(s); free(s); }   /* table pleine : Theora pour celle-ci */
}

/* ---- accroches ----------------------------------------------------------------------------------------------- */
void __wrap_L_00C73940(CPU *c)
{
    if (!actif()) { __real_L_00C73940(c); return; }
    /* [esp+4] = chaîne du jeu {longueur, drapeaux, données} : données en ligne à +8, ou pointeur à +8 si drapeaux & 4
     * (lecture faite par 0x00C73940 elle-même, cf. TRACE-PASSE9S §1) */
    uint32_t va = rd32(STACK_BASE(c) + c->esp + 4), n = va ? rd32(va) : 0;
    uint32_t p = va && (rd32(va + 4) & 4) ? rd32(va + 8) : va + 8;
    size_t i = 0;
    if (va && p && n < 4096) for (; i < n && i + 1 < sizeof t_nom; i++) { char ch = (char)rd8(p + (uint32_t)i); if (!ch) break; t_nom[i] = ch; }
    t_nom[i] = 0;
    __real_L_00C73940(c);
    t_nom[0] = 0;
}

void __wrap_L_01093B70(CPU *c)
{
    if (!actif()) { __real_L_01093B70(c); return; }
    uint32_t etat = c->ecx, info = c->edx;
    __real_L_01093B70(c);
    if (c->eax == 0 && etat && info) ouvrir_session(etat, info, t_nom);
}

/* Saut = le geste natif du jeu : Échap (VK_ESCAPE enfoncée puis relâchée) par l'API d'entrées du runtime. Banc x86 :
 * le jeu ferme la cinématique dès le WM_KEYDOWN (theora_clear au tour de pompe suivant, TRACE-PASSE9S §4). Épuiser
 * les paquets ne suffit PAS : la cinématique suit l'horloge de son son et resterait figée jusqu'au bout. */
static int sauter(session *s, const char *pourquoi)
{
    int64_t t = maintenant_ns(), d = atomic_load(&s->saut_ns);
    if (d && t - d < 5000000000LL) return 0;
    atomic_store(&s->saut_ns, t);
    if (!sc2_input_actif()) { sc2_log("[VIDEO-AMC] saut de %s (%s) impossible : entrées du runtime inactives", s->nom, pourquoi); return 0; }
    sc2_input_touche(0x1B, 1);
    sc2_input_touche(0x1B, 0);
    atomic_fetch_add(&s->saut, 1);
    atomic_fetch_add(&g_sauts, 1);
    sc2_log("[VIDEO-AMC] saut de %s (%s) : VK_ESCAPE injectée", s->nom, pourquoi);
    return 1;
}

void __wrap_L_01093C60(CPU *c)
{
    session *s = actif() ? trouver(c->ecx) : NULL;
    if (!s) { __real_L_01093C60(c); return; }
    s->paquets++;
    atomic_store(&s->dernier_paquet_ns, maintenant_ns());
    if (s->paquets % (10u * (s->fpsn / (s->fpsd ? s->fpsd : 1) + 1)) == 0) {   /* ~toutes les 10 s de vidéo */
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        sc2_log("[VIDEO-AMC] %s : %llu paquets = %.1f s de vidéo en %.1f s réelles (%s)", s->nom, (unsigned long long)s->paquets,
                (double)s->paquets * s->fpsd / s->fpsn, (t.tv_sec - s->t0.tv_sec) + (t.tv_nsec - s->t0.tv_nsec) / 1e9,
                s->materiel ? s->m->nom : "Theora lifté");
    }
    if (s->secours_s > 0) {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - s->t0.tv_sec) + (t.tv_nsec - s->t0.tv_nsec) / 1e9 > s->secours_s && sauter(s, "sortie de secours SC2_VIDEO_SECOURS"))
            atomic_fetch_add(&g_secours, 1);
    }
    if (!s->materiel) { __real_L_01093C60(c); return; }
    uint32_t op = c->edx, octets = rd32(op + OP_BYTES);
    wr32(op + OP_BYTES, 0);                    /* branche « image dupliquée » : granulepos tenu, rien décodé */
    __real_L_01093C60(c);
    wr32(op + OP_BYTES, octets);
}

void __wrap_L_01093CB0(CPU *c)
{
    session *s = actif() ? trouver(c->ecx) : NULL;
    if (!s || !s->materiel) { __real_L_01093CB0(c); return; }
    uint32_t yb = c->edx;
    int64_t gp = (int64_t)((uint64_t)rd32(s->etat + TS_GRANPOS + 4) << 32 | rd32(s->etat + TS_GRANPOS));
    int64_t n = 0;
    if (gp >= 0) { int64_t i = gp >> s->shift; n = i + (gp - (i << s->shift)) - s->biais; if (n < 0) n = 0; }
    uint32_t W = s->w, H = s->h, w2 = W / 2, h2 = H / 2;
    uint32_t Y = s->plans, U = Y + W * H, V = U + w2 * h2;
    /* forme exacte de la libtheora : pointeur sur la ligne du HAUT de l'image, pas négatif (stockage bas → haut) */
    uint32_t y0 = Y + (H - 1) * W, u0 = U + (h2 - 1) * w2, v0 = V + (h2 - 1) * w2;
    if (n != s->n_ecrite) {
        int64_t t = maintenant_ns();
        uint8_t *py = (uint8_t *)GPTR(y0) - (ptrdiff_t)s->py * W + s->px;
        uint8_t *pu = (uint8_t *)GPTR(u0) - (ptrdiff_t)(s->py / 2) * w2 + s->px / 2;
        uint8_t *pv = (uint8_t *)GPTR(v0) - (ptrdiff_t)(s->py / 2) * w2 + s->px / 2;
        if (s->m->image(s, n, py, -(int)W, pu, pv, -(int)w2)) s->echecs++;
        uint64_t d = (uint64_t)(maintenant_ns() - t);
        s->images++; s->ns_image += d; if (d > s->ns_max) s->ns_max = d;
        s->n_ecrite = n;
    }
    wr32(yb + 0x00, W); wr32(yb + 0x04, H); wr32(yb + 0x08, (uint32_t)-(int32_t)W);
    wr32(yb + 0x0C, w2); wr32(yb + 0x10, h2); wr32(yb + 0x14, (uint32_t)-(int32_t)w2);
    wr32(yb + 0x18, y0); wr32(yb + 0x1C, u0); wr32(yb + 0x20, v0);
    c->eax = 0; c->esp += 4;                   /* ret du vrai YUVout (rend 0) */
}

void __wrap_L_01093810(CPU *c)
{
    session *s = actif() ? trouver(c->ecx) : NULL;
    if (s) fermer(s, "theora_clear");
    __real_L_01093810(c);
}

/* ---- API plate-forme ------------------------------------------------------------------------------------------ */
static session *plein_ecran_en_lecture(void)
{
    session *r = NULL;
    int64_t t = maintenant_ns();
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < NSESS; i++) {
        session *s = g_s[i];
        if (s && s->sautable && t - atomic_load(&s->dernier_paquet_ns) < 500000000LL) { r = s; break; }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

int video_amc_en_lecture(void) { return g_actif == 1 && plein_ecran_en_lecture() != NULL; }

int video_amc_entree(void)
{
    if (g_actif != 1 || !g_saut_actif) return 0;
    session *s = plein_ecran_en_lecture();
    return s ? sauter(s, "toucher") : 0;
}

void video_amc_bilan(char *o, size_t cap)
{
    snprintf(o, cap, "[VIDEO-AMC] %s : %u vidéo(s) ouvertes, %u en décodage matériel, %u saut(s) dont %u de secours",
             g_actif == 1 ? "actif" : "inactif", atomic_load(&g_ouvertes), atomic_load(&g_materielles),
             atomic_load(&g_sauts), atomic_load(&g_secours));
}
