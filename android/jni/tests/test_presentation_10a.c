/* test_presentation_10a.c — banc hôte de la copie CPU de l'activité (passe 10a), sans téléphone.
 *
 * sc2_android_main.c est inclus TEL QUEL ; l'API ANativeWindow est simulée (fenêtre factice : taille, format,
 * géométrie, verrou, tampon avec garde de 64 octets). Chaque tampon posté imprime une empreinte FNV-1a de ses pixels
 * visibles et l'état de la garde. Le même fichier se compile contre le jni d'AVANT (main df85db1) et d'APRÈS (10a) :
 * les empreintes des scénarios 1 et 2 (cas nominaux) doivent être IDENTIQUES — comportement invariant de la copie CPU.
 *
 *   1  nominal RGBA : INIT, 3 frames, TERM, 2 frames sans fenêtre, INIT d'une 2e fenêtre (portrait), 3 frames
 *   2  fenêtre de taille 0 pendant 2 frames, puis normale : géométrie reposée à l'occasion suivante
 *   3  verrou refusé jusqu'à ce que la géométrie soit reposée (H1-verrou)
 *   4  setBuffersGeometry refusé, fenêtre restée au format par défaut d'une NativeActivity (RGB_565)
 *   5  (APRÈS seulement, -DAPRES) bascule swapchain → copie CPU : sc2a_geo_invalider() repose la géométrie
 *
 * Construction (NDK r28, exécutable statique, qemu-aarch64) — <jni> = android/jni d'avant ou d'après :
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang --target=aarch64-linux-android26 -static -O1 \
 *     -I<jni> -I<jni>/compat -Irecomp-sc2/runtime -I$NDK/sources/android/native_app_glue [-DAPRES] \
 *     android/jni/tests/test_presentation_10a.c -Wl,--unresolved-symbols=ignore-all -o t && qemu-aarch64 ./t
 * (les symboles non résolus — glue, AAsset, hooks, runtime — ne sont jamais appelés par ces scénarios)
 */
#include "sc2_android_main.c"
#include <stdarg.h>

struct ANativeWindow {
    int32_t w, h, fmt, bw, bh;      /* fenêtre ; format et taille de tampon posés par setBuffersGeometry */
    int zero;                       /* getWidth/getHeight rendent 0 */
    int geo_ko;                     /* setBuffersGeometry refusé */
    int refus_jusqua_geo;           /* verrou refusé jusqu'à la PROCHAINE pose de géométrie */
    unsigned geo_appels, posts;
    uint8_t *mem; int32_t lw, lh, ls, opp;
};
#define GARDE 64

static int opp_de(int32_t f) { return f == 4 ? 2 : f == 3 ? 3 : 4; }
int32_t ANativeWindow_getWidth(ANativeWindow *w) { return w->zero ? 0 : w->w; }
int32_t ANativeWindow_getHeight(ANativeWindow *w) { return w->zero ? 0 : w->h; }
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t bw, int32_t bh, int32_t f)
{
    w->geo_appels++;
    printf("  [fen] setBuffersGeometry(%dx%d, format %d) n°%u%s\n", bw, bh, f, w->geo_appels, w->geo_ko ? " → REFUSÉ -22" : "");
    if (w->geo_ko) return -22;
    w->bw = bw; w->bh = bh; w->fmt = f;
    if (w->refus_jusqua_geo && w->geo_appels >= 2) w->refus_jusqua_geo = 0;
    return 0;
}
int32_t ANativeWindow_lock(ANativeWindow *w, ANativeWindow_Buffer *b, ARect *r)
{
    (void)r;
    if (w->refus_jusqua_geo) return -22;
    w->lw = w->bw ? w->bw : w->w; w->lh = w->bh ? w->bh : w->h;
    int32_t f = w->fmt ? w->fmt : 4;                     /* défaut NativeActivity : RGB_565 */
    w->opp = opp_de(f); w->ls = w->lw + 16;              /* stride ≠ largeur */
    size_t n = (size_t)w->ls * w->lh * w->opp;
    free(w->mem); w->mem = malloc(n + GARDE);
    memset(w->mem, 0x11, n); memset(w->mem + n, 0xA5, GARDE);
    b->width = w->lw; b->height = w->lh; b->stride = w->ls; b->format = f; b->bits = w->mem;
    return 0;
}
int32_t ANativeWindow_unlockAndPost(ANativeWindow *w)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int32_t y = 0; y < w->lh; y++)
        for (int32_t x = 0; x < w->lw * w->opp; x++) { h ^= w->mem[(size_t)y * w->ls * w->opp + x]; h *= 0x100000001b3ull; }
    size_t n = (size_t)w->ls * w->lh * w->opp; int garde_ok = 1;
    for (int i = 0; i < GARDE; i++) if (w->mem[n + i] != 0xA5) garde_ok = 0;
    w->posts++;
    printf("  POST n°%u %dx%d %d o/px empreinte %016llx garde %s\n", w->posts, w->lw, w->lh, w->opp,
           (unsigned long long)h, garde_ok ? "intacte" : "ÉCRASÉE (débordement)");
    return 0;
}
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    (void)prio; (void)tag; va_list a; va_start(a, fmt);
    printf("  log: "); vprintf(fmt, a); printf("\n"); va_end(a); return 1;
}
int __android_log_write(int prio, const char *tag, const char *t) { (void)prio; (void)tag; printf("  log: %s\n", t); return 1; }

static uint8_t *g_img;
static void frames(unsigned *k, int n)
{
    for (int i = 0; i < n; i++, (*k)++) {
        for (uint32_t y = 0; y < g_jh; y++) for (uint32_t x = 0; x < g_jw; x++) {
            uint8_t *p = g_img + ((size_t)y * g_jw + x) * 4;
            p[0] = (uint8_t)(x * 7 + *k); p[1] = (uint8_t)(y * 13); p[2] = (uint8_t)(x ^ y); p[3] = 0;
        }
        sc2a_frame(*k, g_img, g_jw, g_jh);
    }
}
static void init(struct android_app *app, ANativeWindow *w, const char *quoi)
{
    printf("== %s\n", quoi); app->window = w; cmd(app, APP_CMD_INIT_WINDOW);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setenv("SC2_FPS_PERIODE", "0", 1);
    atomic_store(&g_jeu_lance, 1);                       /* lancer_jeu() inerte : pas de fil du jeu */
    g_config_ok = 0; g_jw = 1280; g_jh = 720;
    g_img = malloc((size_t)g_jw * g_jh * 4);
    struct android_app app; memset(&app, 0, sizeof app);
    unsigned k = 1;

    ANativeWindow f1 = { .w = 2176, .h = 1812 }, f2 = { .w = 1080, .h = 2092 };
    init(&app, &f1, "scénario 1 : nominal RGBA (Fold intérieur)"); frames(&k, 3);
    printf("== TERM_WINDOW\n"); cmd(&app, APP_CMD_TERM_WINDOW); app.window = NULL; frames(&k, 2);
    init(&app, &f2, "scénario 1 : fenêtre revenue (portrait)"); frames(&k, 3);

    ANativeWindow f3 = { .w = 2316, .h = 904, .zero = 1 };
    init(&app, &f3, "scénario 2 : fenêtre de taille 0 pendant 2 frames"); frames(&k, 2); f3.zero = 0; frames(&k, 2);

    ANativeWindow f4 = { .w = 2176, .h = 1812, .refus_jusqua_geo = 1 };
    init(&app, &f4, "scénario 3 : verrou refusé jusqu'à une nouvelle géométrie (H1-verrou)"); frames(&k, 6);

#ifdef APRES
    printf("== scénario 5 : bascule swapchain → copie CPU (sc2a_geo_invalider)\n");
    unsigned g = f4.geo_appels; sc2a_geo_invalider(); frames(&k, 2);
    printf("  géométrie reposée après la bascule : %s\n", f4.geo_appels > g ? "OUI" : "NON");
#endif

    ANativeWindow f5 = { .w = 2176, .h = 1812, .geo_ko = 1 };
    init(&app, &f5, "scénario 4 : setBuffersGeometry refusé, tampon resté en RGB_565"); frames(&k, 2);

    printf("== bilan : tampons postés %llu, échecs de verrou %llu, géométries vides %llu, fenêtres perdues %llu, frames affichées %u\n",
           (unsigned long long)g_postes, (unsigned long long)g_echecs_verrou, (unsigned long long)g_geo_vide,
           (unsigned long long)g_fen_perdues, atomic_load(&g_frames_affichees));
#ifdef APRES
    printf("== bilan 10a : géométries reposées %llu, tampons d'un autre format %llu\n",
           (unsigned long long)g_reposes, (unsigned long long)g_format_autre);
#endif
    return 0;
}
