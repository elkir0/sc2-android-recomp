/* sc2_video.c — passe 7w3 : décodeur Ogg Theora des .ogv du jeu (cf. sc2_video.h).
 *
 * Source lue par blocs de 64 Kio (StormLib SFileReadFile, ou fread) → ogg_sync → pages du flux Theora
 * (les autres flux logiques, Vorbis, sont ignorés) → paquets → th_decode_packetin → th_ycbcr_buffer.
 * Mémoire : un bloc de 64 Kio + les tampons internes d'ogg/theora (trois plans de la vidéo) + une image
 * RGBA à la demande. Jamais le fichier entier.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <ogg/ogg.h>
#include <theora/theoradec.h>
#include "StormLib.h"
#include "sc2_video.h"

/* Fourni par sc2_runtime.c dans le binaire ; la définition faible sert aux bancs sans runtime. */
__attribute__((weak)) void sc2_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout);
}
void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define BLOC 65536

struct sc2v {
    HANDLE hmpq, hf;              /* source MPQ */
    FILE *f;                      /* source fichier hôte */
    char nom[256];
    ogg_sync_state oy;
    ogg_stream_state to; int to_ok;
    th_info ti; th_comment tc; th_setup_info *ts; th_dec_ctx *td;
    ogg_packet pend; int pend_ok;  /* 1er paquet de données, lu pendant les en-têtes */
    th_ycbcr_buffer yuv;
    int dispo, fin;
    uint32_t frames, dup, paquets;
    uint64_t octets;
};

static long lire_source(sc2v *v, char *buf, long n)
{
    if (v->f) return (long)fread(buf, 1, (size_t)n, v->f);
    DWORD lu = 0;
    if (!SFileReadFile(v->hf, buf, (DWORD)n, &lu, NULL) && SErrGetLastError() != ERROR_HANDLE_EOF) return -1;
    return (long)lu;
}
/* Un bloc de plus dans ogg_sync : 0 en fin de source. */
static int remplir(sc2v *v)
{
    char *b = ogg_sync_buffer(&v->oy, BLOC);
    long n = lire_source(v, b, BLOC);
    if (n <= 0) return 0;
    ogg_sync_wrote(&v->oy, n);
    v->octets += (uint64_t)n;
    return 1;
}
/* Page suivante : 1 si lue, 0 en fin. */
static int page(sc2v *v, ogg_page *og)
{
    for (;;) {
        int r = ogg_sync_pageout(&v->oy, og);
        if (r > 0) return 1;
        if (r == 0 && !remplir(v)) return 0;      /* r < 0 : resynchronisation, on continue */
    }
}

static int entetes(sc2v *v)
{
    ogg_sync_init(&v->oy);
    th_info_init(&v->ti); th_comment_init(&v->tc);
    ogg_page og; ogg_packet op;
    int nh = 0;                                     /* en-têtes Theora lus (3 attendus) */
    /* Pages BOS : repérer le flux Theora. */
    while (!v->to_ok && page(v, &og)) {
        if (!ogg_page_bos(&og)) break;
        ogg_stream_state s; ogg_stream_init(&s, ogg_page_serialno(&og));
        ogg_stream_pagein(&s, &og);
        if (ogg_stream_packetout(&s, &op) == 1 && th_decode_headerin(&v->ti, &v->tc, &v->ts, &op) > 0) {
            v->to = s; v->to_ok = 1; nh = 1;
        } else ogg_stream_clear(&s);
    }
    if (!v->to_ok) { sc2_log("[VIDEO] %s : aucun flux Theora", v->nom); return -1; }
    /* La page courante (og) peut appartenir au flux ou à un autre : on la donne si c'est la nôtre. */
    int page_en_main = 1;
    for (;;) {
        if (page_en_main) {
            if (ogg_page_serialno(&og) == v->to.serialno && !ogg_page_bos(&og)) ogg_stream_pagein(&v->to, &og);
            page_en_main = 0;
        }
        int r;
        while ((r = ogg_stream_packetout(&v->to, &op)) != 0) {
            if (r < 0) continue;
            int h = th_decode_headerin(&v->ti, &v->tc, &v->ts, &op);
            if (h > 0) { nh++; continue; }
            if (h == 0 && nh >= 3) {                /* 1er paquet de données : gardé pour sc2v_avancer */
                v->pend = op;
                v->pend.packet = malloc((size_t)op.bytes);
                memcpy(v->pend.packet, op.packet, (size_t)op.bytes);
                v->pend_ok = 1;
                goto fini;
            }
            sc2_log("[VIDEO] %s : en-tête Theora invalide (%d)", v->nom, h); return -1;
        }
        if (!page(v, &og)) { sc2_log("[VIDEO] %s : fin avant les en-têtes", v->nom); return -1; }
        page_en_main = 1;
    }
fini:
    v->td = th_decode_alloc(&v->ti, v->ts);
    if (!v->td) { sc2_log("[VIDEO] %s : th_decode_alloc a échoué", v->nom); return -1; }
    static const char *pf[] = { "4:2:0", "réservé", "4:2:2", "4:4:4" };
    sc2_log("[VIDEO] %s : Theora %u.%u.%u, image %ux%u (codée %ux%u, origine %u,%u), %.3f images/s, %s",
            v->nom, v->ti.version_major, v->ti.version_minor, v->ti.version_subminor,
            v->ti.pic_width, v->ti.pic_height, v->ti.frame_width, v->ti.frame_height, v->ti.pic_x, v->ti.pic_y,
            v->ti.fps_denominator ? (double)v->ti.fps_numerator / v->ti.fps_denominator : 0.0,
            pf[v->ti.pixel_fmt & 3]);
    return 0;
}

static sc2v *ouvrir_commun(sc2v *v)
{
    if (entetes(v) == 0) return v;
    sc2v_fermer(v);
    return NULL;
}

sc2v *sc2v_ouvrir_mpq(const char *archive, const char *nom)
{
    sc2v *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    snprintf(v->nom, sizeof v->nom, "%s", nom);
    char interne[256]; snprintf(interne, sizeof interne, "%s", nom);
    for (char *p = interne; *p; p++) if (*p == '/') *p = '\\';
    if (!SFileOpenArchive(archive, 0, STREAM_FLAG_READ_ONLY, &v->hmpq)) {
        sc2_log("[VIDEO] archive %s : ouverture impossible (%d)", archive, (int)SErrGetLastError());
        free(v); return NULL;
    }
    if (!SFileOpenFileEx(v->hmpq, interne, 0, &v->hf)) {
        sc2_log("[VIDEO] %s absent de %s (%d)", interne, archive, (int)SErrGetLastError());
        SFileCloseArchive(v->hmpq); free(v); return NULL;
    }
    return ouvrir_commun(v);
}

sc2v *sc2v_ouvrir_fichier(const char *chemin)
{
    sc2v *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    snprintf(v->nom, sizeof v->nom, "%s", chemin);
    if (!(v->f = fopen(chemin, "rb"))) { sc2_log("[VIDEO] %s : %s", chemin, strerror(errno)); free(v); return NULL; }
    return ouvrir_commun(v);
}

void sc2v_fermer(sc2v *v)
{
    if (!v) return;
    if (v->td) th_decode_free(v->td);
    if (v->ts) th_setup_free(v->ts);
    th_comment_clear(&v->tc); th_info_clear(&v->ti);
    if (v->to_ok) ogg_stream_clear(&v->to);
    ogg_sync_clear(&v->oy);
    if (v->pend_ok) free(v->pend.packet);
    if (v->hf) SFileCloseFile(v->hf);
    if (v->hmpq) SFileCloseArchive(v->hmpq);
    if (v->f) fclose(v->f);
    free(v);
}

void sc2v_dims(const sc2v *v, uint32_t *w, uint32_t *h) { *w = v->ti.pic_width; *h = v->ti.pic_height; }
double sc2v_fps(const sc2v *v)
{
    return v->ti.fps_denominator ? (double)v->ti.fps_numerator / v->ti.fps_denominator : 0.0;
}
uint32_t sc2v_frames(const sc2v *v) { return v->frames; }
uint64_t sc2v_octets(const sc2v *v) { return v->octets; }
int sc2v_frame_dispo(const sc2v *v) { return v->dispo; }

/* Paquet suivant du flux Theora : 1, ou 0 en fin. */
static int paquet(sc2v *v, ogg_packet *op)
{
    for (;;) {
        int r = ogg_stream_packetout(&v->to, op);
        if (r > 0) return 1;
        if (r < 0) continue;
        ogg_page og;
        do { if (!page(v, &og)) return 0; } while (ogg_page_serialno(&og) != v->to.serialno);
        ogg_stream_pagein(&v->to, &og);
    }
}

int sc2v_avancer(sc2v *v)
{
    if (v->fin) return 0;
    ogg_packet op;
    for (;;) {
        int depuis_pend = 0;
        if (v->pend_ok) { op = v->pend; depuis_pend = 1; }
        else if (!paquet(v, &op)) { v->fin = 1; return 0; }
        ogg_int64_t gp = 0;
        int r = th_decode_packetin(v->td, &op, &gp);
        if (depuis_pend) { free(v->pend.packet); v->pend_ok = 0; }
        v->paquets++;
        if (r == 0 || r == TH_DUPFRAME) {
            if (r == TH_DUPFRAME) v->dup++;
            if (th_decode_ycbcr_out(v->td, v->yuv) != 0) return -1;
            v->frames++; v->dispo = 1;
            return 1;
        }
        if (r == TH_EBADPACKET || r == TH_EFAULT) return -1;
        /* autres codes (en-tête inattendu, paquet vide) : paquet suivant */
    }
}

static inline uint8_t sat(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }

void sc2v_yuv_vers_rgba(int y, int u, int v, uint8_t *o)
{
    int c = 298 * (y - 16), d = u - 128, e = v - 128;
    o[0] = sat((c + 409 * e + 128) >> 8);
    o[1] = sat((c - 100 * d - 208 * e + 128) >> 8);
    o[2] = sat((c + 516 * d + 128) >> 8);
    o[3] = 255;
}

static int convertir(sc2v *v, uint8_t *dst, int bgra)
{
    if (!v->frames) return -1;
    int xd = v->ti.pixel_fmt != TH_PF_444, yd = v->ti.pixel_fmt == TH_PF_420;   /* 420 : 1,1 ; 422 : 1,0 ; 444 : 0,0 */
    uint32_t w = v->ti.pic_width, h = v->ti.pic_height, x0 = v->ti.pic_x, y0 = v->ti.pic_y;
    const th_img_plane *P = v->yuv;
    for (uint32_t y = 0; y < h; y++) {
        const unsigned char *ly = P[0].data + (ptrdiff_t)(y0 + y) * P[0].stride;
        const unsigned char *lu = P[1].data + (ptrdiff_t)((y0 + y) >> yd) * P[1].stride;
        const unsigned char *lv = P[2].data + (ptrdiff_t)((y0 + y) >> yd) * P[2].stride;
        uint8_t *o = dst + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            uint32_t X = x0 + x;
            sc2v_yuv_vers_rgba(ly[X], lu[X >> xd], lv[X >> xd], o);
            if (bgra) { uint8_t t = o[0]; o[0] = o[2]; o[2] = t; }
        }
    }
    v->dispo = 0;
    return 0;
}
int sc2v_lire_rgba(sc2v *v, uint8_t *rgba) { return convertir(v, rgba, 0); }
int sc2v_lire_bgra(sc2v *v, uint8_t *bgra) { return convertir(v, bgra, 1); }

/* ---- Banc dans un run : SC2_VIDEO=<archive>|<nom> -------------------------------------------------- */
static struct { char arc[512], nom[256]; uint32_t pas, max, frames, ecrites, dup; uint64_t octets;
                double ms, moy[3]; int fini, w, h; } B;

static int ecrire_ppm(const char *chemin, const uint8_t *rgba, uint32_t w, uint32_t h, double moy[3])
{
    FILE *f = fopen(chemin, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    double s[3] = { 0, 0, 0 };
    uint8_t *l = malloc((size_t)w * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) for (int c = 0; c < 3; c++) {
            uint8_t p = rgba[((size_t)y * w + x) * 4 + c]; l[x * 3 + c] = p; s[c] += p;
        }
        fwrite(l, 1, (size_t)w * 3, f);
    }
    free(l); fclose(f);
    for (int c = 0; c < 3; c++) moy[c] = s[c] / ((double)w * h);
    return 0;
}

static void banc_bilan(void)
{
    if (!B.arc[0]) return;
    sc2_log("[BILAN] [VIDEO] %s : %u frames décodées (%u dupliquées), %u écrites (1 sur %u), %llu octets lus en flux, "
            "%.1f ms de décodage (%.2f ms/frame)%s",
            B.nom, B.frames, B.dup, B.ecrites, B.pas, (unsigned long long)B.octets, B.ms,
            B.frames ? B.ms / B.frames : 0.0, B.fini ? "" : " — run terminé avant la fin du décodage");
}

static void *banc_fil(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);   /* fil hôte : passe après le jeu */
    sc2v *v = sc2v_ouvrir_mpq(B.arc, B.nom);
    if (!v) { B.fini = 1; return NULL; }
    uint32_t w, h; sc2v_dims(v, &w, &h); B.w = (int)w; B.h = (int)h;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    const char *dir = getenv("SC2_FRAMES_DIR"); if (!dir || !*dir) dir = "frames";
    mkdir(dir, 0755);                                  /* le fil peut précéder la 1re frame du jeu */
    /* Cadence de la vidéo (défaut) : la frame n part à t_depart + n / fps, comme à la lecture ;
     * SC2_VIDEO_TEMPS_REEL=0 : décodage au plus vite. */
    const char *tr = getenv("SC2_VIDEO_TEMPS_REEL");
    double fps = sc2v_fps(v), periode = (!tr || atoi(tr)) && fps > 0 ? 1.0 / fps : 0.0;
    struct timespec t0, t1, depart;
    clock_gettime(CLOCK_MONOTONIC, &depart);
    for (;;) {
        if (periode > 0) {
            double t = B.frames * periode;
            struct timespec q = { depart.tv_sec + (time_t)t, depart.tv_nsec + (long)((t - (double)(time_t)t) * 1e9) };
            if (q.tv_nsec >= 1000000000L) { q.tv_sec++; q.tv_nsec -= 1000000000L; }
#ifdef __APPLE__   /* banc natif macOS : pas de clock_nanosleep, attente relative jusqu'à la même échéance */
            for (;;) {
                struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n);
                long long ns = (long long)(q.tv_sec - n.tv_sec) * 1000000000LL + (q.tv_nsec - n.tv_nsec);
                if (ns <= 0) break;
                struct timespec d = { (time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL) };
                if (nanosleep(&d, NULL) == 0) break;
            }
#else
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &q, NULL) == EINTR) {}
#endif
        }
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = sc2v_avancer(v);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        B.ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        B.frames = sc2v_frames(v); B.octets = sc2v_octets(v); B.dup = v->dup;
        if (r <= 0) { if (r < 0) sc2_log("[VIDEO] erreur de décodage à la frame %u", B.frames + 1); break; }
        if ((B.frames - 1) % B.pas == 0) {
            char ch[1024]; snprintf(ch, sizeof ch, "%s/video_%05u.ppm", dir, B.frames);
            sc2v_lire_rgba(v, rgba);
            double m[3];
            if (ecrire_ppm(ch, rgba, w, h, m) == 0) {
                B.ecrites++;
                sc2_log("[VIDEO] frame %u → %s : moyenne RGB %.2f %.2f %.2f", B.frames, ch, m[0], m[1], m[2]);
            }
        }
        if (B.max && B.frames >= B.max) break;
    }
    free(rgba);
    sc2v_fermer(v);
    B.fini = 1;
    banc_bilan();
    return NULL;
}

/* Sortie du run avant la fin de la vidéo : bilan avec les compteurs du moment. */
static void banc_sortie(void) { if (!B.fini) banc_bilan(); }

__attribute__((constructor)) static void banc_init(void)
{
    const char *e = getenv("SC2_VIDEO");
    if (!e || !*e) return;
    const char *bar = strchr(e, '|');
    if (!bar) { sc2_log("[VIDEO] SC2_VIDEO attendu sous la forme <archive>|<nom interne>"); return; }
    snprintf(B.arc, sizeof B.arc, "%.*s", (int)(bar - e), e);
    snprintf(B.nom, sizeof B.nom, "%s", bar + 1);
    const char *p = getenv("SC2_VIDEO_PAS"), *m = getenv("SC2_VIDEO_MAX");
    B.pas = p && atoi(p) > 0 ? (uint32_t)atoi(p) : 10;
    B.max = m && atoi(m) > 0 ? (uint32_t)atoi(m) : 0;
    atexit(banc_sortie);
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, banc_fil, NULL) != 0) sc2_log("[VIDEO] fil de décodage non créé");
    else sc2_log("[VIDEO] fil de décodage lancé : %s | %s (1 frame sur %u écrite%s)", B.arc, B.nom, B.pas,
                 B.max ? ", arrêt borné" : "");
    pthread_attr_destroy(&a);
}
