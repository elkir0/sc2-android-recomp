/* test_geo.c — géométrie de présentation Android (android/jni/sc2_geo.h), barre de raccourcis (sc2_barre.h) et
 * recentrage, sur l'hôte. Reprend android/outils/test-geo.c (passe 7o) et l'étend au 21/10 (V0.2, prérequis de V3.4) :
 *   cc -std=gnu17 -O0 -Wall -Wextra -I android/jni android/jni/tests/test_geo.c -lm -o /tmp/test_geo && /tmp/test_geo
 * (ou ctest dans build-tests : tests-hote/CMakeLists.txt). Code de sortie 0 : tout conforme.
 *
 * A. Passe 7o, pour chaque fenêtre (émulateur M4, Fold 8 intérieur/extérieur, portrait/paysage) et chaque chemin
 *    (swapchain, copie CPU à tampon) :
 *    1. aller-retour EXHAUSTIF : pour tout pixel client (x, y), le pixel fenêtre qui en contient le centre
 *       (sc2_geo_vers_ecran) revient à (x, y) par sc2_geo_vers_jeu — dès que l'image est agrandie (s ≥ 1) ;
 *    2. cohérence avec le rendu : le pixel fenêtre dont le centre tombe dans la colonne/ligne client c (échantillonnage
 *       du blit / compositeur) revient à c, pour TOUS les pixels fenêtre de l'image ;
 *    3. bandes noires : un toucher hors de l'image est signalé (rendu 0) et ramené au bord ;
 *    4. le cas du contrat : image 1280x720 en (160, 0) d'un tampon 1600x720 ; 5. SC2_ECRAN=auto.
 * B. Barre de raccourcis : l'image est calée en HAUT (bande libre en bas) ou à GAUCHE (bande à droite) ; geo_courante
 *    de sc2_android_main.c inverse alors le rectangle de barre_disposer par sc2_geo_swapchain_rect :
 *    1. aller-retour exhaustif et bandes comme en A ; 2. partition de l'écran : chaque pixel fenêtre est SOIT dans
 *       l'image (vers_jeu rend 1), SOIT dans la barre (barre_dedans, coordonnées de swapchain de barre_entree), jamais
 *       les deux, jamais aucun ; 3. dernière ligne (colonne) de l'image → dernier pixel client, la suivante → barre.
 * C. Recentrage (périphérique physique → sc2v_barre_voulue(0) : barre masquée, image recentrée) :
 *    1. rectangle de barre_disposer(voulue = 0) = celui de sc2_geo_swapchain, à l'identique (x0, y0, sx, sy), pour
 *       toutes les fenêtres : les deux calculs de rectangle ne divergent pas ;
 *    2. aller-retour exhaustif sur l'image recentrée ; 3. chaque pixel client se décale EXACTEMENT de la demi-bande
 *       (pixels entiers) entre barre et recentrage ; 4. un toucher dans l'ancienne barre tombe dans l'image, un toucher
 *       en haut de l'image d'avant tombe sur la nouvelle bande noire (signalé, ramené au bord) ;
 *    5. sans bande libre suffisante (16:9 sur 16:9), barre voulue ou non : même géométrie. */
#include <stdio.h>
#include <stdlib.h>
#include "sc2_geo.h"
#include "sc2_barre.h"

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
    VERIF(g.ok, "%s : géométrie refusée", nom);
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
    printf("  %-52s image en (%.1f, %.1f) ×(%.4f, %.4f) ; %ld pixels aller-retour%s ; %ld pixels de bande ; "
           "ancienne conversion fausse sur %.1f %% (écart max %ld) ; bouton client (767,301) = adb shell input tap %d %d\n",
           nom, g.x0, g.y0, g.sx, g.sy, n, agrandie ? "" : " (réduite : non exhaustif)", bandes,
           total ? 100.0 * faux / total : 0.0, ecart_max, bx, by);
}

/* ---- B et C : barre de raccourcis et recentrage ------------------------------------------------------------------ */

/* Rectangle du blit de sc2_vk_presente.c (barre_disposer) → géométrie inverse de geo_courante (sc2_android_main.c). */
static sc2_geo geo_barre(uint32_t jw, uint32_t jh, uint32_t dw, uint32_t dh, uint32_t fw, uint32_t fh, int voulue,
                         barre_geo *bg)
{
    *bg = barre_disposer(jw, jh, dw, dh, voulue);
    return sc2_geo_swapchain_rect(jw, jh, bg->jeu.x, bg->jeu.y, (uint32_t)bg->jeu.w, (uint32_t)bg->jeu.h, dw, dh, fw, fh);
}
/* Toucher fenêtre → pixel de swapchain : même troncature que barre_entree (sc2_android_main.c). */
static int32_t vers_sc(float f, uint32_t d, uint32_t fd) { return (int32_t)(f * (float)d / (float)fd); }

static int meme_geo(sc2_geo a, sc2_geo b)
{
    return a.ok == b.ok && a.jw == b.jw && a.jh == b.jh && a.x0 == b.x0 && a.y0 == b.y0 && a.sx == b.sx && a.sy == b.sy;
}

/* B.2 et B.3 : partition image / barre le long de l'axe libre (ligne ou colonne médiane de l'image). */
static void partition(const char *nom, sc2_geo g, const barre_geo *bg, uint32_t dw, uint32_t dh, uint32_t fw, uint32_t fh)
{
    long dans_image = 0, dans_barre = 0;
    int vert = bg->verticale;
    uint32_t n = vert ? fw : fh;
    for (uint32_t f = 0; f < n; f++) {
        double fx = vert ? f : g.x0 + g.sx * g.jw / 2, fy = vert ? g.y0 + g.sy * g.jh / 2 : f;
        int32_t cx, cy;
        int img = sc2_geo_vers_jeu(&g, fx, fy, &cx, &cy);
        int bar = barre_dedans(bg, vers_sc((float)fx, dw, fw), vers_sc((float)fy, dh, fh));
        VERIF(img != bar, "%s : %s=%u %s", nom, vert ? "x" : "y", f,
              img ? "à la fois dans l'image et dans la barre" : "ni dans l'image ni dans la barre");
        dans_image += img; dans_barre += bar;
    }
    /* dernière ligne (colonne) de l'image → dernier pixel client ; la suivante → barre, ramenée au bord */
    int32_t cx, cy, k = vert ? bg->jeu.w : bg->jeu.h;          /* dw == fw : pixels de swapchain = pixels fenêtre */
    if (dw == fw && dh == fh) {
        int d1 = vert ? sc2_geo_vers_jeu(&g, k - 1, g.y0 + 1, &cx, &cy) : sc2_geo_vers_jeu(&g, g.x0 + 1, k - 1, &cx, &cy);
        VERIF(d1 && (vert ? cx == (int32_t)g.jw - 1 : cy == (int32_t)g.jh - 1),
              "%s : dernier pixel de l'image (%d) → client %d", nom, k - 1, vert ? cx : cy);
        int d2 = vert ? sc2_geo_vers_jeu(&g, k, g.y0 + 1, &cx, &cy) : sc2_geo_vers_jeu(&g, g.x0 + 1, k, &cx, &cy);
        VERIF(!d2 && (vert ? cx == (int32_t)g.jw - 1 : cy == (int32_t)g.jh - 1)
                  && barre_dedans(bg, vert ? k : (int32_t)(g.x0 + 1), vert ? (int32_t)(g.y0 + 1) : k),
              "%s : 1er pixel de la barre (%d) : dans l'image ou hors barre", nom, k);
    }
    printf("  %-52s partition le long de %s : %ld pixels image + %ld pixels barre = %u\n", nom, vert ? "x" : "y",
           dans_image, dans_barre, n);
}

/* B : barre affichée ; C : la même fenêtre après recentrage. */
static void barre_et_recentrage(const char *nom, uint32_t jw, uint32_t jh, uint32_t dw, uint32_t dh, uint32_t fw, uint32_t fh)
{
    char n[128];
    barre_geo bb, bc;
    sc2_geo gb = geo_barre(jw, jh, dw, dh, fw, fh, 1, &bb), gc = geo_barre(jw, jh, dw, dh, fw, fh, 0, &bc);
    VERIF(bb.active, "%s : barre attendue (bande libre %u px)", nom, bb.verticale ? dw - (uint32_t)bb.jeu.w : dh - (uint32_t)bb.jeu.h);
    VERIF(bb.verticale ? gb.x0 == 0 && bb.barre.x == bb.jeu.w : gb.y0 == 0 && bb.barre.y == bb.jeu.h,
          "%s : image non calée %s (image en %.1f, barre en %d)", nom, bb.verticale ? "à gauche" : "en haut",
          bb.verticale ? gb.x0 : gb.y0, bb.verticale ? bb.barre.x : bb.barre.y);
    snprintf(n, sizeof n, "%s, barre", nom);
    tester(n, gb, fw, fh);
    partition(n, gb, &bb, dw, dh, fw, fh);

    /* C.1 : recentrage = la géométrie centrée de sc2_geo_swapchain, à l'identique */
    VERIF(!bc.active, "%s : barre encore active après recentrage", nom);
    VERIF(meme_geo(gc, sc2_geo_swapchain(jw, jh, dw, dh, fw, fh)),
          "%s : recentrage (%.3f, %.3f) ≠ sc2_geo_swapchain", nom, gc.x0, gc.y0);
    snprintf(n, sizeof n, "%s, recentrée", nom);
    tester(n, gc, fw, fh);
    /* C.3 : décalage entier et uniforme de la demi-bande, axe libre seulement */
    int32_t demi = bb.verticale ? bc.jeu.x : bc.jeu.y, d_min = 1 << 30, d_max = -(1 << 30), autre = 0;
    VERIF(bb.verticale ? bc.jeu.x == (int32_t)(dw - (uint32_t)bc.jeu.w) / 2 : bc.jeu.y == (int32_t)(dh - (uint32_t)bc.jeu.h) / 2,
          "%s : image recentrée en %d, pas au milieu de la bande", nom, demi);
    for (int32_t c = 0; c < (int32_t)(bb.verticale ? jw : jh); c++) {
        int32_t ax, ay, bx, by;
        sc2_geo_vers_ecran(&gb, bb.verticale ? c : 0, bb.verticale ? 0 : c, &ax, &ay);
        sc2_geo_vers_ecran(&gc, bb.verticale ? c : 0, bb.verticale ? 0 : c, &bx, &by);
        int32_t d = bb.verticale ? bx - ax : by - ay;
        if (d < d_min) d_min = d;
        if (d > d_max) d_max = d;
        if (bb.verticale ? by != ay : bx != ax) autre++;
    }
    int32_t attendu = (int32_t)(demi * (double)fw / dw + 0.5);
    if (!bb.verticale) attendu = (int32_t)(demi * (double)fh / dh + 0.5);
    VERIF(dw != fw || dh != fh || (d_min == attendu && d_max == attendu),
          "%s : décalage du recentrage de %d à %d px (attendu %d partout)", nom, d_min, d_max, attendu);
    VERIF(!autre, "%s : %d pixel(s) client(s) déplacés sur l'autre axe par le recentrage", nom, autre);
    /* C.4 : un toucher dans le premier quart de l'ancienne barre tombe dans l'image recentrée (la barre fait deux
     * demi-bandes : son milieu est le bord exact de l'image recentrée) ; en haut (à gauche) de l'image d'avant, il
     * tombe sur la nouvelle bande noire, ramené au bord */
    int32_t cx, cy, qx, qy;
    double quart = bb.verticale ? (bb.barre.x + bb.barre.w / 4.0) * fw / dw : (bb.barre.y + bb.barre.h / 4.0) * fh / dh;
    double travers = bb.verticale ? gc.y0 + gc.sy * jh / 2 : gc.x0 + gc.sx * jw / 2;
    int dedans = bb.verticale ? sc2_geo_vers_jeu(&gc, quart, travers, &qx, &qy) : sc2_geo_vers_jeu(&gc, travers, quart, &qx, &qy);
    int dans_barre = barre_dedans(&bb, vers_sc((float)(bb.verticale ? quart : travers), dw, fw),
                                  vers_sc((float)(bb.verticale ? travers : quart), dh, fh));
    VERIF(dedans && dans_barre, "%s : toucher en %.0f : dans la barre %d, dans l'image recentrée %d", nom, quart, dans_barre, dedans);
    int32_t gx, gy;
    int avant = bb.verticale ? sc2_geo_vers_jeu(&gb, 1, travers, &gx, &gy) : sc2_geo_vers_jeu(&gb, travers, 1, &gx, &gy);
    int apres = bb.verticale ? sc2_geo_vers_jeu(&gc, 1, travers, &cx, &cy) : sc2_geo_vers_jeu(&gc, travers, 1, &cx, &cy);
    VERIF(avant && !apres && (bb.verticale ? cx == 0 : cy == 0),
          "%s : toucher au bord %s : avant %d, après %d (ramené à %d)", nom, bb.verticale ? "gauche" : "haut", avant, apres,
          bb.verticale ? cx : cy);
    printf("  %-52s recentrage : image en %d (demi-bande), décalage %d px ; toucher en %.0f : barre → client (%d, %d)\n",
           nom, demi, d_min, quart, qx, qy);
}

int main(void)
{
    static const struct { const char *nom; uint32_t fw, fh; } F[] = {
        { "émulateur M4 paysage 2400x1080", 2400, 1080 }, { "émulateur M4 portrait 1080x2400", 1080, 2400 },
        { "Fold 8 intérieur 2176x1812", 2176, 1812 },    { "Fold 8 extérieur 2316x904", 2316, 904 },
        { "banc X 2316x1920", 2316, 1920 },              { "1600x720 (tampon = fenêtre)", 1600, 720 },
        { "Fold intérieur 2504x2256", 2504, 2256 },     { "écran extérieur 2520x1080", 2520, 1080 },
    };
    printf("A. sc2_geo : swapchain et copie CPU (passe 7o)\n");
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

    printf("B/C. barre de raccourcis (image calée, bande libre) et recentrage (périphérique physique)\n");
    /* C.1 sur toutes les fenêtres et les deux modes du jeu : barre_disposer(voulue = 0) ≡ sc2_geo_ajuster */
    static const uint32_t J[][2] = { { 1280, 720 }, { 1280, 1024 } };
    for (unsigned j = 0; j < 2; j++)
        for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) {
            barre_geo bg;
            sc2_geo a = geo_barre(J[j][0], J[j][1], F[i].fw, F[i].fh, F[i].fw, F[i].fh, 0, &bg);
            VERIF(meme_geo(a, sc2_geo_swapchain(J[j][0], J[j][1], F[i].fw, F[i].fh, F[i].fw, F[i].fh)),
                  "%s, jeu %ux%u : rectangle de barre_disposer ≠ sc2_geo_ajuster", F[i].nom, J[j][0], J[j][1]);
        }
    /* Fold intérieur (jeu 1280x1024 → image 2504x2003, bande de 253 px en bas) ; écran extérieur 2520x1080 (jeu 1280x720
     * → image 1920x1080, bande de 600 px à droite) ; Fold 8 intérieur 2176x1812 en 1280x720 (bande de 588 px en bas) ;
     * surface de swapchain deux fois plus petite que la fenêtre, étirée par le compositeur. */
    barre_et_recentrage("Fold intérieur 2504x2256, jeu 1280x1024", 1280, 1024, 2504, 2256, 2504, 2256);
    barre_et_recentrage("écran extérieur 2520x1080, jeu 1280x720", 1280, 720, 2520, 1080, 2520, 1080);
    barre_et_recentrage("Fold 8 intérieur 2176x1812, jeu 1280x720", 1280, 720, 2176, 1812, 2176, 1812);
    barre_et_recentrage("surface 1252x1128 étirée à 2504x2256, jeu 1280x1024", 1280, 1024, 1252, 1128, 2504, 2256);
    /* C.5 : 16:9 sur 16:9, pas de bande libre : barre voulue ou non, même géométrie centrée */
    barre_geo b1, b0;
    sc2_geo g1 = geo_barre(1280, 720, 1920, 1080, 1920, 1080, 1, &b1), g0 = geo_barre(1280, 720, 1920, 1080, 1920, 1080, 0, &b0);
    VERIF(!b1.active && meme_geo(g1, g0), "16:9 sur 16:9 : la barre voulue change la géométrie (active %d)", b1.active);
    printf("  16:9 sur 16:9 : barre %s, géométrie identique avec ou sans barre voulue\n", b1.active ? "ACTIVE" : "absente");

    printf("%s (%d échec(s))\n", g_ko ? "ÉCHEC" : "OK", g_ko);
    return g_ko != 0;
}
