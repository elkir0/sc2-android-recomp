/* sc2_barre.h — clavier de raccourcis dans la bande libre de l'écran (21/10).
 *
 * Sur un écran dont le rapport diffère de celui du jeu (Fold intérieur 2504x2256 : 1280x1024 → 2504x2003, 253 px de
 * libre), l'image du jeu est calée en HAUT (bande verticale libre) ou à GAUCHE (bande horizontale libre) et toute la
 * place libre reçoit une barre de touches : groupes de contrôle, F1/F2, Tab, Ctrl/Maj/Alt (verrouillés pour la touche
 * suivante), ordres A/S/H/P/M, Espace, F10, Échap, Entrée, et une page « lettres » (ABC). Pur calcul, sans Android ni
 * Vulkan : disposition, détection d'une touche, effet d'un appui (suite de VK Win32) et dessin RGBA. La glue Android
 * (entrées) et sc2_vk_presente.c (dessin dans la swapchain) s'en servent ; testé sur l'hôte (tests/test_barre.c).
 */
#ifndef SC2_BARRE_H
#define SC2_BARRE_H

#include <stdint.h>
#include <string.h>
#include "sc2_barre_police.h"

#define BARRE_PART_MIN 0.08          /* bande libre ≥ 8 % de la dimension : barre possible */

enum { BT_TOUCHE, BT_MODIF, BT_PAGE };
enum { BM_CTRL = 1, BM_MAJ = 2, BM_ALT = 4 };
typedef struct { const char *lib; uint16_t vk; uint8_t genre, arg; } barre_def;
typedef struct { int n; barre_def t[13]; } barre_rang;
typedef struct { int nr; barre_rang r[3]; } barre_page;

#define T(l, v) { l, v, BT_TOUCHE, 0 }
static const barre_page barre_pages[2] = {
    { 2, { { 13, { T("1", '1'), T("2", '2'), T("3", '3'), T("4", '4'), T("5", '5'), T("6", '6'), T("7", '7'),
                   T("8", '8'), T("9", '9'), T("0", '0'), T("F1", 0x70), T("F2", 0x71), T("TAB", 0x09) } },
               { 13, { { "CTRL", 0x11, BT_MODIF, BM_CTRL }, { "MAJ", 0x10, BT_MODIF, BM_MAJ }, { "ALT", 0x12, BT_MODIF, BM_ALT },
                       T("A", 'A'), T("S", 'S'), T("H", 'H'), T("P", 'P'), T("M", 'M'), T("ESP", 0x20), T("F10", 0x79),
                       T("ECHAP", 0x1B), T("ENTR", 0x0D), { "ABC", 0, BT_PAGE, 1 } } } } },
    { 3, { { 10, { T("Q", 'Q'), T("W", 'W'), T("E", 'E'), T("R", 'R'), T("T", 'T'), T("Y", 'Y'), T("U", 'U'),
                   T("I", 'I'), T("O", 'O'), T("P", 'P') } },
               { 10, { T("A", 'A'), T("S", 'S'), T("D", 'D'), T("F", 'F'), T("G", 'G'), T("H", 'H'), T("J", 'J'),
                       T("K", 'K'), T("L", 'L'), T("ENTR", 0x0D) } },
               { 10, { { "CTRL", 0x11, BT_MODIF, BM_CTRL }, { "MAJ", 0x10, BT_MODIF, BM_MAJ }, T("Z", 'Z'), T("X", 'X'),
                       T("C", 'C'), T("V", 'V'), T("B", 'B'), T("N", 'N'), T("M", 'M'), { "123", 0, BT_PAGE, 0 } } } } },
};
#undef T

typedef struct { int32_t x, y, w, h; } barre_rect;
typedef struct {
    int active, verticale;       /* barre affichée ; verticale = bande à DROITE (sinon en bas) */
    barre_rect jeu, barre;       /* en pixels de swapchain */
} barre_geo;

/* Image sw×sh dans dw×dh, rapport conservé : centrée sans barre ; avec barre voulue et assez de place, calée en haut
 * (bande en bas) ou à gauche (bande à droite). Même calcul que sc2_geo_ajuster pour la taille. */
static inline barre_geo barre_disposer(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, int voulue)
{
    barre_geo g = { 0 };
    uint32_t rw, rh;
    if ((uint64_t)dw * sh >= (uint64_t)dh * sw) { rh = dh; rw = (uint32_t)((uint64_t)sw * dh / sh); }
    else { rw = dw; rh = (uint32_t)((uint64_t)sh * dw / sw); }
    g.jeu.w = (int32_t)rw; g.jeu.h = (int32_t)rh;
    g.jeu.x = (int32_t)(dw - rw) / 2; g.jeu.y = (int32_t)(dh - rh) / 2;
    if (!voulue) return g;
    if (dh - rh >= (uint32_t)(dh * BARRE_PART_MIN)) {
        g.active = 1; g.jeu.y = 0;
        g.barre = (barre_rect){ 0, (int32_t)rh, (int32_t)dw, (int32_t)(dh - rh) };
    } else if (dw - rw >= (uint32_t)(dw * BARRE_PART_MIN)) {
        g.active = 1; g.verticale = 1; g.jeu.x = 0;
        g.barre = (barre_rect){ (int32_t)rw, 0, (int32_t)(dw - rw), (int32_t)dh };
    }
    return g;
}

/* Rectangles des touches de la page (relatifs à la barre) ; rend leur nombre, idx[i] = rang * 16 + colonne.
 * Bande en bas : un rang par ligne, largeur de touche commune (rang le plus long), rangs centrés. Bande à droite :
 * touches à la suite, en grille de 2 ou 3 colonnes. */
static inline int barre_touches(const barre_geo *g, int page, barre_rect *r, int *idx, int max)
{
    const barre_page *p = &barre_pages[page & 1];
    int n = 0, maxc = 0;
    for (int i = 0; i < p->nr; i++) if (p->r[i].n > maxc) maxc = p->r[i].n;
    if (!g->active || !maxc) return 0;
    if (!g->verticale) {
        int32_t kw = g->barre.w / maxc, kh = g->barre.h / p->nr;
        for (int i = 0; i < p->nr; i++) {
            int32_t x0 = (g->barre.w - kw * p->r[i].n) / 2;
            for (int j = 0; j < p->r[i].n && n < max; j++, n++) {
                r[n] = (barre_rect){ x0 + kw * j, kh * i, kw, kh }; idx[n] = i * 16 + j;
            }
        }
    } else {
        int total = 0; for (int i = 0; i < p->nr; i++) total += p->r[i].n;
        int cols = g->barre.w >= 3 * 120 ? 3 : 2, rangs = (total + cols - 1) / cols;
        int32_t kw = g->barre.w / cols, kh = g->barre.h / (rangs ? rangs : 1);
        for (int i = 0; i < p->nr; i++)
            for (int j = 0; j < p->r[i].n && n < max; j++, n++) {
                r[n] = (barre_rect){ kw * (n % cols), kh * (n / cols), kw, kh }; idx[n] = i * 16 + j;
            }
    }
    return n;
}
static inline const barre_def *barre_def_de(int page, int idx) { return &barre_pages[page & 1].r[idx / 16].t[idx % 16]; }

/* Touche sous le point (x, y) en pixels de swapchain : idx (rang * 16 + colonne), -1 hors barre ou entre deux touches. */
static inline int barre_toucher(const barre_geo *g, int page, int32_t x, int32_t y)
{
    barre_rect r[40]; int idx[40];
    int n = barre_touches(g, page, r, idx, 40);
    x -= g->barre.x; y -= g->barre.y;
    for (int i = 0; i < n; i++)
        if (x >= r[i].x && x < r[i].x + r[i].w && y >= r[i].y && y < r[i].y + r[i].h) return idx[i];
    return -1;
}
static inline int barre_dedans(const barre_geo *g, int32_t x, int32_t y)
{
    return g->active && x >= g->barre.x && x < g->barre.x + g->barre.w && y >= g->barre.y && y < g->barre.y + g->barre.h;
}

/* Appui sur une touche : met l'état à jour et rend la suite de VK à envoyer (vk[i], bas[i]) ; modificateurs verrouillés
 * pour UNE touche (Ctrl+3 = assigner le groupe 3), relâchés après elle. */
typedef struct { int page; uint32_t modifs; unsigned version; } barre_etat;
static inline int barre_appuyer(barre_etat *e, const barre_def *d, uint16_t *vk, uint8_t *bas, int max)
{
    static const uint16_t mvk[3] = { 0x11, 0x10, 0x12 };
    int n = 0;
    if (d->genre == BT_PAGE) { e->page = d->arg; e->version++; return 0; }
    if (d->genre == BT_MODIF) { e->modifs ^= d->arg; e->version++; return 0; }
    for (int m = 0; m < 3; m++) if ((e->modifs >> m & 1) && n < max) { vk[n] = mvk[m]; bas[n++] = 1; }
    if (n + 2 <= max) { vk[n] = d->vk; bas[n++] = 1; vk[n] = d->vk; bas[n++] = 0; }
    for (int m = 2; m >= 0; m--) if ((e->modifs >> m & 1) && n < max) { vk[n] = mvk[m]; bas[n++] = 0; }
    if (e->modifs) { e->modifs = 0; e->version++; }
    return n;
}

/* Dessin de la barre dans px (w×h pixels de 4 octets, ligne = w), en RGBA (bgr = 0) ou BGRA (bgr = 1). */
static inline void barre_remplir(uint8_t *px, int32_t w, int32_t h, barre_rect r, uint8_t cr, uint8_t cg, uint8_t cb, int bgr)
{
    for (int32_t y = r.y < 0 ? 0 : r.y; y < r.y + r.h && y < h; y++)
        for (int32_t x = r.x < 0 ? 0 : r.x; x < r.x + r.w && x < w; x++) {
            uint8_t *p = px + 4 * ((size_t)y * (size_t)w + (size_t)x);
            p[0] = bgr ? cb : cr; p[1] = cg; p[2] = bgr ? cr : cb; p[3] = 255;
        }
}
static inline void barre_texte(uint8_t *px, int32_t w, int32_t h, barre_rect k, const char *s, int bgr)
{
    int len = (int)strlen(s);
    const int avance = 11;                                   /* chasse utile d'un glyphe de 14 px */
    int e1 = (int)(k.h * 0.75 / BARRE_POLICE_H), e2 = (int)(k.w * 0.8 / (len * avance));
    int e = e1 < e2 ? e1 : e2; if (e < 1) e = 1;
    int32_t x0 = k.x + (k.w - len * avance * e) / 2 - 2 * e, y0 = k.y + (k.h - BARRE_POLICE_H * e) / 2 + 2 * e;
    for (int c = 0; c < len; c++) {
        int g = (unsigned char)s[c] - 32; if (g < 0 || g >= 64) continue;
        for (int ly = 0; ly < BARRE_POLICE_H; ly++)
            for (int lx = 0; lx < BARRE_POLICE_L; lx++)
                if (barre_police[g][ly] & (0x8000u >> lx))
                    barre_remplir(px, w, h, (barre_rect){ x0 + (c * avance + lx) * e, y0 + ly * e, e, e }, 232, 236, 240, bgr);
    }
}
static inline void barre_dessiner(uint8_t *px, const barre_geo *g, const barre_etat *e, int bgr)
{
    int32_t w = g->barre.w, h = g->barre.h;
    barre_remplir(px, w, h, (barre_rect){ 0, 0, w, h }, 12, 13, 16, bgr);
    barre_rect r[40]; int idx[40];
    int n = barre_touches(g, e->page, r, idx, 40);
    for (int i = 0; i < n; i++) {
        const barre_def *d = barre_def_de(e->page, idx[i]);
        int32_t m = r[i].w < r[i].h ? r[i].w / 14 : r[i].h / 14; if (m < 2) m = 2;
        barre_rect k = { r[i].x + m, r[i].y + m, r[i].w - 2 * m, r[i].h - 2 * m };
        int actif = d->genre == BT_MODIF && (e->modifs & d->arg);
        barre_remplir(px, w, h, k, 70, 76, 90, bgr);                                  /* bord */
        barre_rect f = { k.x + 2, k.y + 2, k.w - 4, k.h - 4 };
        if (actif) barre_remplir(px, w, h, f, 30, 110, 190, bgr);
        else if (d->genre == BT_PAGE) barre_remplir(px, w, h, f, 50, 40, 70, bgr);
        else barre_remplir(px, w, h, f, 34, 37, 45, bgr);
        barre_texte(px, w, h, f, d->lib, bgr);
    }
}

#endif
