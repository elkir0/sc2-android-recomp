/* test-geo.c — test hôte de android/jni/sc2_geo.h (passe 7o).
 *
 *   cc -O2 -Wall -Wextra -I android/jni android/outils/test-geo.c -lm -o /tmp/test-geo && /tmp/test-geo
 *
 * Pour chaque fenêtre (émulateur M4, Fold 8 intérieur/extérieur, portrait/paysage) et chaque chemin
 * (swapchain, copie CPU à tampon) :
 *   1. aller-retour EXHAUSTIF : pour tout pixel client (x, y), le pixel fenêtre qui en contient le
 *      centre (vers_ecran) revient à (x, y) par vers_jeu — dès que l'image est agrandie (s ≥ 1) ;
 *   2. cohérence avec le rendu : le pixel fenêtre dont le centre tombe dans la colonne/ligne client c
 *      (échantillonnage du blit / compositeur) revient à c, pour TOUS les pixels fenêtre de l'image ;
 *   3. bandes noires : un toucher hors de l'image est signalé (rendu 0) et ramené au bord ;
 *   4. le cas du contrat : image 1280x720 en (160, 0) d'un tampon 1600x720 ;
 *   5. SC2_ECRAN=auto. */
#include <stdio.h>
#include <stdlib.h>
#include "sc2_geo.h"

static int g_ko;
#define VERIF(c, ...) do { if (!(c)) { g_ko++; if (g_ko < 20) { printf("  ÉCHEC : "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* Ce que le compositeur / le blit affiche au pixel fenêtre f (centre f+0,5) : la colonne client. */
static long echantillon(double x0, double s, int32_t f) { return (long)floor((f + 0.5 - x0) / s); }

/* vers_jeu d'AVANT la 7o (sc2_android_main.c) : échelle uniforme déduite de la fenêtre, troncature. */
static void ancien(uint32_t jw, uint32_t jh, uint32_t fw, uint32_t fh, float fx, float fy, int32_t *x, int32_t *y)
{
    double e = (double)fw / jw < (double)fh / jh ? (double)fw / jw : (double)fh / jh;
    double ox = (fw - jw * e) / 2, oy = (fh - jh * e) / 2;
    *x = (int32_t)((fx - ox) / e); *y = (int32_t)((fy - oy) / e);
    if (*x < 0) *x = 0;
    if (*x >= (int32_t)jw) *x = (int32_t)jw - 1;
    if (*y < 0) *y = 0;
    if (*y >= (int32_t)jh) *y = (int32_t)jh - 1;
}

static void tester(const char *nom, sc2_geo g, uint32_t fw, uint32_t fh)
{
    long n = 0, bandes = 0;
    int agrandie = g.sx >= 1.0 && g.sy >= 1.0;
    if (agrandie)
        for (int32_t y = 0; y < (int32_t)g.jh; y++)
            for (int32_t x = 0; x < (int32_t)g.jw; x++) {
                int32_t fx, fy, cx, cy;
                sc2_geo_vers_ecran(&g, x, y, &fx, &fy);
                int d = sc2_geo_vers_jeu(&g, fx, fy, &cx, &cy);
                VERIF(d && cx == x && cy == y, "%s : client (%d,%d) → écran (%d,%d) → (%d,%d)", nom, x, y, fx, fy, cx, cy);
                n++;
            }
    for (int32_t f = 0; f < (int32_t)fw; f++) {          /* axe x : rendu vs inverse, toute la largeur */
        int32_t cx, cy; long e = echantillon(g.x0, g.sx, f);
        int d = sc2_geo_vers_jeu(&g, f, g.y0 + g.sy * g.jh / 2, &cx, &cy);
        if (e < 0 || e >= (long)g.jw) { VERIF(!d, "%s : x=%d sur bande noire non signalé", nom, f); bandes++; }
        else VERIF(d && cx == e, "%s : x=%d affiche la colonne %ld, toucher → %d", nom, f, e, cx);
    }
    for (int32_t f = 0; f < (int32_t)fh; f++) {
        int32_t cx, cy; long e = echantillon(g.y0, g.sy, f);
        int d = sc2_geo_vers_jeu(&g, g.x0 + g.sx * g.jw / 2, f, &cx, &cy);
        if (e < 0 || e >= (long)g.jh) { VERIF(!d, "%s : y=%d sur bande noire non signalé", nom, f); bandes++; }
        else VERIF(d && cy == e, "%s : y=%d affiche la ligne %ld, toucher → %d", nom, f, e, cy);
    }
    /* l'ancienne conversion : pixels fenêtre de l'image dont le clic tombait sur un AUTRE pixel client */
    long faux = 0, total = 0, ecart_max = 0;
    for (int32_t fy = 0; fy < (int32_t)fh; fy += 3)
        for (int32_t fx = 0; fx < (int32_t)fw; fx += 3) {
            long ex = echantillon(g.x0, g.sx, fx), ey = echantillon(g.y0, g.sy, fy);
            if (ex < 0 || ey < 0 || ex >= (long)g.jw || ey >= (long)g.jh) continue;
            int32_t ax, ay; ancien(g.jw, g.jh, fw, fh, fx, fy, &ax, &ay);
            total++;
            long d = labs(ax - ex) > labs(ay - ey) ? labs(ax - ex) : labs(ay - ey);
            if (d) { faux++; if (d > ecart_max) ecart_max = d; }
        }
    int32_t bx, by; sc2_geo_vers_ecran(&g, 767, 301, &bx, &by);
    printf("  %-44s ancienne conversion : %.1f %% des pixels de l'image sur le mauvais pixel client (écart max %ld)\n",
           "", total ? 100.0 * faux / total : 0.0, ecart_max);
    printf("  %-44s image en (%.1f, %.1f) ×(%.4f, %.4f) ; %ld pixels aller-retour%s ; %ld pixels de bande ; "
           "bouton client (767,301) = adb shell input tap %d %d\n", nom, g.x0, g.y0, g.sx, g.sy, n,
           agrandie ? "" : " (réduite : non exhaustif)", bandes, bx, by);
}

int main(void)
{
    static const struct { const char *nom; uint32_t fw, fh; } F[] = {
        { "émulateur M4 paysage 2400x1080", 2400, 1080 }, { "émulateur M4 portrait 1080x2400", 1080, 2400 },
        { "Fold 8 intérieur 2176x1812", 2176, 1812 },    { "Fold 8 extérieur 2316x904", 2316, 904 },
        { "banc X 2316x1920", 2316, 1920 },              { "1600x720 (tampon = fenêtre)", 1600, 720 },
    };
    const uint32_t jw = 1280, jh = 720;
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) {
        uint32_t fw = F[i].fw, fh = F[i].fh;
        char n[96];
        snprintf(n, sizeof n, "%s swapchain", F[i].nom);
        tester(n, sc2_geo_swapchain(jw, jh, fw, fh, fw, fh), fw, fh);
        /* tampon de la copie CPU : même calcul que geometrie() de sc2_android_main.c */
        int32_t bw, bh;
        if ((int64_t)fw * jh >= (int64_t)fh * jw) { bh = jh; bw = (int32_t)((int64_t)jh * fw / fh); }
        else { bw = jw; bh = (int32_t)((int64_t)jw * fh / fw); }
        if (bw < (int32_t)jw) bw = jw;
        if (bh < (int32_t)jh) bh = jh;
        snprintf(n, sizeof n, "%s CPU (tampon %dx%d)", F[i].nom, bw, bh);
        tester(n, sc2_geo_tampon(jw, jh, bw, bh, (bw - (int32_t)jw) / 2, (bh - (int32_t)jh) / 2, fw, fh), fw, fh);
    }
    /* le cas du contrat : 1280x720 en (160, 0) de 1600x720, tampon affiché 1:1 */
    sc2_geo t = sc2_geo_tampon(1280, 720, 1600, 720, 160, 0, 1600, 720);
    int32_t x, y;
    VERIF(sc2_geo_vers_jeu(&t, 160, 0, &x, &y) && x == 0 && y == 0, "tampon 1600 : (160,0) → (%d,%d)", x, y);
    VERIF(sc2_geo_vers_jeu(&t, 1439, 719, &x, &y) && x == 1279 && y == 719, "tampon 1600 : (1439,719) → (%d,%d)", x, y);
    VERIF(!sc2_geo_vers_jeu(&t, 159, 300, &x, &y) && x == 0, "tampon 1600 : (159,300) bande gauche → (%d,%d)", x, y);
    VERIF(!sc2_geo_vers_jeu(&t, 1440, 300, &x, &y) && x == 1279, "tampon 1600 : (1440,300) bande droite → (%d,%d)", x, y);
    printf("  tampon 1600x720, image en (160,0) : (160,0)→(0,0), (1439,719)→(1279,719), 159 et 1440 = bandes\n");
    static const struct { uint32_t fw, fh, w, h; } A[] = {
        { 2176, 1812, 1280, 1024 }, { 2316, 904, 1280, 720 }, { 2400, 1080, 1280, 720 }, { 1080, 2400, 1280, 720 },
        { 1920, 1200, 1280, 800 }, { 2048, 1536, 1280, 960 },
    };
    for (unsigned i = 0; i < sizeof A / sizeof A[0]; i++) {
        uint32_t w, h; sc2_geo_mode_auto(A[i].fw, A[i].fh, &w, &h);
        VERIF(w == A[i].w && h == A[i].h, "auto %ux%u → %ux%u (attendu %ux%u)", A[i].fw, A[i].fh, w, h, A[i].w, A[i].h);
        printf("  SC2_ECRAN=auto : fenêtre %ux%u → %ux%u\n", A[i].fw, A[i].fh, w, h);
    }
    printf("%s (%d échec(s))\n", g_ko ? "ÉCHEC" : "OK", g_ko);
    return g_ko != 0;
}
