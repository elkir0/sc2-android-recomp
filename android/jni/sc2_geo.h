/* sc2_geo.h — géométrie de présentation Android (passe 7o) : où l'image du jeu tombe dans la fenêtre,
 * et la conversion INVERSE exacte d'un toucher (pixels fenêtre) en pixels CLIENT du jeu (back buffer).
 *
 * Deux chemins de présentation, deux géométries (sc2_android_main.c choisit celle du dernier Present) :
 *   - swapchain (7b) : sc2_vk_presente.c blitte le back buffer jw×jh dans l'image de swapchain dw×dh
 *     (= fenêtre, en général) sur le rectangle entier (ox, oy, rw, rh) de sc2_geo_ajuster ; si la surface
 *     n'a pas la taille de la fenêtre, le compositeur étire dw×dh → fw×fh ;
 *   - copie CPU (7a) : l'image est copiée 1:1 en (ox, oy) d'un TAMPON bw×bh (ex. 1280x720 en (160, 0) de
 *     1600x720) que le compositeur étire ensuite à fw×fh — échelle non uniforme quand bw/bh, entiers,
 *     n'ont pas exactement le rapport de la fenêtre (Fold : 2176x1812 → tampon 1280x1065).
 * Dans les deux cas : client = (écran − x0) / s, axe par axe. Un toucher en (fx, fy) désigne le CENTRE
 * du pixel fenêtre (fx + 0,5) : c'est ce que voit un filtre de mise à l'échelle (VK_FILTER_LINEAR, le
 * compositeur), donc le pixel client sous le doigt.
 * Pur calcul, sans Android : testé sur l'hôte (android/outils/test-geo.c). */
#ifndef SC2_GEO_H
#define SC2_GEO_H
#include <math.h>
#include <stdint.h>

typedef struct {
    double x0, y0;          /* coin haut-gauche de l'image du jeu, en pixels fenêtre */
    double sx, sy;          /* pixels fenêtre par pixel client */
    uint32_t jw, jh;        /* back buffer du jeu */
    int ok;
} sc2_geo;

/* Rectangle entier, rapport conservé, centré : EXACTEMENT celui du blit de sc2_vk_presente.c. */
static inline void sc2_geo_ajuster(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                                   int32_t *ox, int32_t *oy, uint32_t *rw, uint32_t *rh)
{
    if ((uint64_t)dw * sh >= (uint64_t)dh * sw) { *rh = dh; *rw = (uint32_t)((uint64_t)sw * dh / sh); }
    else { *rw = dw; *rh = (uint32_t)((uint64_t)sh * dw / sw); }
    *ox = (int32_t)(dw - *rw) / 2; *oy = (int32_t)(dh - *rh) / 2;
}

/* Swapchain dw×dh (étirée à fw×fh par le compositeur si elles diffèrent). */
/* 21/10 : rectangle du blit DONNÉ (image calée en haut ou à gauche quand la barre de raccourcis occupe la bande). */
static inline sc2_geo sc2_geo_swapchain_rect(uint32_t jw, uint32_t jh, int32_t ox, int32_t oy, uint32_t rw, uint32_t rh,
                                             uint32_t dw, uint32_t dh, uint32_t fw, uint32_t fh)
{
    sc2_geo g = { 0 };
    if (!jw || !jh || !dw || !dh || !fw || !fh || !rw || !rh) return g;
    double ex = (double)fw / dw, ey = (double)fh / dh;
    g.x0 = ox * ex; g.y0 = oy * ey;
    g.sx = (double)rw / jw * ex; g.sy = (double)rh / jh * ey;
    g.jw = jw; g.jh = jh; g.ok = 1;
    return g;
}
static inline sc2_geo sc2_geo_swapchain(uint32_t jw, uint32_t jh, uint32_t dw, uint32_t dh, uint32_t fw, uint32_t fh)
{
    sc2_geo g = { 0 };
    if (!jw || !jh || !dw || !dh || !fw || !fh) return g;
    int32_t ox, oy; uint32_t rw, rh;
    sc2_geo_ajuster(jw, jh, dw, dh, &ox, &oy, &rw, &rh);
    double ex = (double)fw / dw, ey = (double)fh / dh;
    g.x0 = ox * ex; g.y0 = oy * ey;
    g.sx = (double)rw / jw * ex; g.sy = (double)rh / jh * ey;
    g.jw = jw; g.jh = jh; g.ok = 1;
    return g;
}

/* Copie CPU : image jw×jh posée en (ox, oy) d'un tampon bw×bh, étiré à fw×fh. */
static inline sc2_geo sc2_geo_tampon(uint32_t jw, uint32_t jh, uint32_t bw, uint32_t bh, int32_t ox, int32_t oy,
                                     uint32_t fw, uint32_t fh)
{
    sc2_geo g = { 0 };
    if (!jw || !jh || !bw || !bh || !fw || !fh) return g;
    double ex = (double)fw / bw, ey = (double)fh / bh;
    g.x0 = ox * ex; g.y0 = oy * ey; g.sx = ex; g.sy = ey;
    g.jw = jw; g.jh = jh; g.ok = 1;
    return g;
}

/* Toucher (pixels fenêtre, flottants d'AMotionEvent) → pixel client, borné à l'image. Rend 1 si le
 * doigt est DANS l'image, 0 s'il est sur une bande noire (le point est alors ramené au bord). */
static inline int sc2_geo_vers_jeu(const sc2_geo *g, double fx, double fy, int32_t *x, int32_t *y)
{
    double cx = floor((fx + 0.5 - g->x0) / g->sx), cy = floor((fy + 0.5 - g->y0) / g->sy);
    int dedans = cx >= 0 && cy >= 0 && cx < g->jw && cy < g->jh;
    if (cx < 0) cx = 0;
    if (cx > g->jw - 1.0) cx = g->jw - 1.0;
    if (cy < 0) cy = 0;
    if (cy > g->jh - 1.0) cy = g->jh - 1.0;
    *x = (int32_t)cx; *y = (int32_t)cy;
    return dedans;
}

/* Pixel client → pixel fenêtre qui en contient le centre (aide « adb shell input tap », tests). */
static inline void sc2_geo_vers_ecran(const sc2_geo *g, int32_t x, int32_t y, int32_t *fx, int32_t *fy)
{
    *fx = (int32_t)floor(g->x0 + (x + 0.5) * g->sx);
    *fy = (int32_t)floor(g->y0 + (y + 0.5) * g->sy);
}

/* SC2_ECRAN=auto : le mode d'affichage standard du jeu dont le rapport est le plus proche de celui de
 * la fenêtre (bandes noires minimales sans résolution exotique). Fold 8 intérieur 2176x1812 (1,20) →
 * 1280x1024 ; extérieur 2316x904 et émulateur 2400x1080 → 1280x720. */
static inline void sc2_geo_mode_auto(uint32_t fw, uint32_t fh, uint32_t *w, uint32_t *h)
{
    static const uint32_t modes[][2] = { { 1280, 720 }, { 1280, 800 }, { 1280, 960 }, { 1280, 1024 }, { 1024, 768 } };
    double r = fh ? (double)fw / fh : 16.0 / 9.0, meilleur = 1e9;
    if (r < 1.0) r = 1.0 / r;                       /* portrait : le jeu reste en paysage */
    *w = 1280; *h = 720;
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        double e = fabs(log(((double)modes[i][0] / modes[i][1]) / r));
        if (e < meilleur - 1e-9) { meilleur = e; *w = modes[i][0]; *h = modes[i][1]; }
    }
}

#endif
