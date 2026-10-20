/* sc2_d3dx_surface.h — passe 9h : cœur de D3DXLoadSurfaceFromMemory (copie + conversion de format), sans CPU invité.
 *
 * En-tête seul, inclus par sc2_d3d9.c (glue de l'import) et par le test hôte tests/test_d3dx_surface.c : aucun
 * fichier de build à toucher. Tout travaille sur des pointeurs HÔTE déjà bornés :
 *   - sc2x_etendue_source() calcule, depuis le rectangle et le pitch source, la plage d'octets à lire
 *     (début, taille) — la glue la copie de la mémoire invitée par une lecture qui ne peut pas fauter ;
 *   - sc2x_copier() n'accède qu'à ces [0, n) octets source et qu'aux lignes du rectangle destination.
 * Chemins :
 *   - BRUT : même format source et destination, sans clé de couleur → copie ligne à ligne (ou ligne de blocs DXTn) ;
 *   - CONVERSION : source → RGBA8 (d3dv_decoder_rgba, les décodeurs du backend), clé de couleur, puis encodage dans
 *     le format destination (formats non compressés courants). DXTn en destination n'est pas encodé : SC2X_NON_GERE.
 * Pas de mise à l'échelle : rectangles de tailles différentes → la partie commune (coin haut-gauche), notée. */
#ifndef SC2_D3DX_SURFACE_H
#define SC2_D3DX_SURFACE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int d3dv_decoder_rgba(uint32_t fmt, const void *src, uint32_t w, uint32_t h, uint8_t *rgba);   /* sc2_d3d9v.c */

typedef struct { int32_t l, t, r, b; } sc2x_rect;            /* RECT Win32 : gauche, haut, droite, bas (exclus) */
enum { SC2X_OK = 0, SC2X_INVALIDE = 1, SC2X_NON_GERE = 2 };
#define SC2X_MAX_COTE 16384

static uint32_t sc2x_bloc(uint32_t f)                         /* octets par bloc 4×4, 0 = non compressé */
{
    return f == 0x31545844u ? 8u : (f >= 0x32545844u && f <= 0x35545844u) ? 16u : 0u;
}
static uint32_t sc2x_bpp(uint32_t f)                          /* octets par pixel, 0 = inconnu */
{
    switch (f) {
    case 20: return 3;
    case 21: case 22: case 31: case 32: case 33: case 34: case 35: return 4;
    case 23: case 24: case 25: case 26: case 29: case 30: case 51: case 60: case 111: return 2;   /* 60 : V8U8 */
    case 27: case 28: case 50: return 1;
    case 36: case 112: case 113: case 115: return 8;
    case 114: return 4;
    case 116: return 16;
    default: return 0;
    }
}
static int sc2x_rect_ok(const sc2x_rect *r)
{
    return r->l >= 0 && r->t >= 0 && r->l < r->r && r->t < r->b && r->r - r->l <= SC2X_MAX_COTE && r->b - r->t <= SC2X_MAX_COTE;
}
/* Géométrie d'un rectangle dans un format : octets utiles par ligne (de pixels ou de blocs), nombre de lignes,
 * décalage d'octets du coin dans une ligne, ligne de départ. -1 : format inconnu ou rectangle DXTn non aligné. */
static int sc2x_geo(uint32_t f, const sc2x_rect *r, uint32_t *octets, uint32_t *lignes, uint32_t *col, uint32_t *lig0)
{
    uint32_t w = (uint32_t)(r->r - r->l), h = (uint32_t)(r->b - r->t), b = sc2x_bloc(f), p = sc2x_bpp(f);
    if (b) {
        if ((r->l & 3) || (r->t & 3)) return -1;
        *octets = (w + 3) / 4 * b; *lignes = (h + 3) / 4; *col = (uint32_t)r->l / 4 * b; *lig0 = (uint32_t)r->t / 4;
        return 0;
    }
    if (!p) return -1;
    *octets = w * p; *lignes = h; *col = (uint32_t)r->l * p; *lig0 = (uint32_t)r->t;
    return 0;
}
/* Plage source à lire (relative à pSrcMemory) : [*debut, *debut + *taille). SC2X_INVALIDE si le pitch ne contient pas
 * la ligne du rectangle (pitch incohérent) ou si le rectangle est invalide. */
static int sc2x_etendue_source(uint32_t f, uint32_t pitch, const sc2x_rect *r, uint64_t *debut, uint64_t *taille)
{
    uint32_t oct, lig, col, l0;
    if (!sc2x_rect_ok(r) || sc2x_geo(f, r, &oct, &lig, &col, &l0)) return SC2X_INVALIDE;
    if ((uint64_t)col + oct > pitch) return SC2X_INVALIDE;
    *debut = (uint64_t)l0 * pitch + col;
    *taille = (uint64_t)(lig - 1) * pitch + oct;
    return SC2X_OK;
}

/* RGBA8 → un pixel du format f ; 0 si fait. Ordre des octets D3D9 (petit-boutiste). */
static int sc2x_encoder(uint32_t f, const uint8_t *c, uint8_t *o)
{
    uint8_t r = c[0], g = c[1], b = c[2], a = c[3];
    uint32_t v;
    switch (f) {
    case 20: o[0] = b; o[1] = g; o[2] = r; return 0;
    case 21: o[0] = b; o[1] = g; o[2] = r; o[3] = a; return 0;
    case 22: o[0] = b; o[1] = g; o[2] = r; o[3] = 255; return 0;
    case 32: o[0] = r; o[1] = g; o[2] = b; o[3] = a; return 0;
    case 33: o[0] = r; o[1] = g; o[2] = b; o[3] = 255; return 0;
    case 23: v = (uint32_t)(r >> 3) << 11 | (uint32_t)(g >> 2) << 5 | (uint32_t)(b >> 3); break;
    case 24: v = 0x8000u | (uint32_t)(r >> 3) << 10 | (uint32_t)(g >> 3) << 5 | (uint32_t)(b >> 3); break;
    case 25: v = (a >= 128 ? 0x8000u : 0u) | (uint32_t)(r >> 3) << 10 | (uint32_t)(g >> 3) << 5 | (uint32_t)(b >> 3); break;
    case 26: v = (uint32_t)(a >> 4) << 12 | (uint32_t)(r >> 4) << 8 | (uint32_t)(g >> 4) << 4 | (uint32_t)(b >> 4); break;
    case 30: v = 0xF000u | (uint32_t)(r >> 4) << 8 | (uint32_t)(g >> 4) << 4 | (uint32_t)(b >> 4); break;
    case 28: o[0] = a; return 0;
    case 50: o[0] = (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8); return 0;   /* luminance (poids BT.601 / 256) */
    case 51: o[0] = (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8); o[1] = a; return 0;
    default: return -1;
    }
    o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8);
    return 0;
}
static int sc2x_encodable(uint32_t f) { uint8_t c[4] = { 0 }, o[4]; return !sc2x_encoder(f, c, o); }

/* Copie. src : les n octets de la plage source (sc2x_etendue_source), sr : rectangle source (dans l'image source),
 * dst : début du NIVEAU destination (dpitch, dw × dh), dr : rectangle destination ou NULL (tout le niveau).
 * colorkey ≠ 0 : les pixels source égaux (en A8R8G8B8) deviennent noir transparent. note : raison lisible. */
static int sc2x_copier(const uint8_t *src, uint64_t n, uint32_t sfmt, uint32_t spitch, const sc2x_rect *sr,
                       uint8_t *dst, uint32_t dfmt, uint32_t dpitch, uint32_t dw, uint32_t dh, const sc2x_rect *dr,
                       uint32_t colorkey, char *note, size_t cap)
{
    sc2x_rect d = dr ? *dr : (sc2x_rect){ 0, 0, (int32_t)dw, (int32_t)dh };
    uint64_t deb, tai;
    if (note && cap) note[0] = 0;
    if (!src || !dst || !sr) { if (note) snprintf(note, cap, "pointeur nul"); return SC2X_INVALIDE; }
    if (sc2x_etendue_source(sfmt, spitch, sr, &deb, &tai)) { if (note) snprintf(note, cap, "rectangle ou pitch source incohérent"); return SC2X_INVALIDE; }
    if (tai > n) { if (note) snprintf(note, cap, "plage source tronquée (%llu > %llu)", (unsigned long long)tai, (unsigned long long)n); return SC2X_INVALIDE; }
    if (!sc2x_rect_ok(&d) || (uint32_t)d.r > dw || (uint32_t)d.b > dh) { if (note) snprintf(note, cap, "rectangle destination hors surface"); return SC2X_INVALIDE; }
    uint32_t sw = (uint32_t)(sr->r - sr->l), sh = (uint32_t)(sr->b - sr->t);
    uint32_t cw = sw < (uint32_t)(d.r - d.l) ? sw : (uint32_t)(d.r - d.l), ch = sh < (uint32_t)(d.b - d.t) ? sh : (uint32_t)(d.b - d.t);
    int tronque = cw != sw || ch != sh || cw != (uint32_t)(d.r - d.l) || ch != (uint32_t)(d.b - d.t);
    sc2x_rect dc = { d.l, d.t, d.l + (int32_t)cw, d.t + (int32_t)ch };
    uint32_t doct, dlig, dcol, dl0;
    if (sc2x_geo(dfmt, &dc, &doct, &dlig, &dcol, &dl0)) { if (note) snprintf(note, cap, "format destination 0x%X inconnu ou rectangle DXTn non aligné", dfmt); return SC2X_NON_GERE; }
    if ((uint64_t)dcol + doct > dpitch) { if (note) snprintf(note, cap, "pitch destination trop court"); return SC2X_INVALIDE; }

    if (sfmt == dfmt && !(colorkey && !sc2x_bloc(sfmt))) {           /* BRUT */
        sc2x_rect sc = { sr->l, sr->t, sr->l + (int32_t)cw, sr->t + (int32_t)ch };
        uint32_t soct, slig, scol, sl0;
        sc2x_geo(sfmt, &sc, &soct, &slig, &scol, &sl0);
        for (uint32_t y = 0; y < dlig; y++) memcpy(dst + (uint64_t)(dl0 + y) * dpitch + dcol, src + (uint64_t)y * spitch, doct);
        if (note) snprintf(note, cap, "copie brute%s%s", tronque ? ", tailles différentes : partie commune sans mise à l'échelle" : "",
                           colorkey ? ", clé de couleur ignorée (DXTn)" : "");
        return SC2X_OK;
    }
    if (!sc2x_encodable(dfmt)) { if (note) snprintf(note, cap, "conversion vers le format 0x%X non gérée (format destination non encodable)", dfmt); return SC2X_NON_GERE; }
    /* CONVERSION : la plage source tassée (lignes de pixels ou de blocs), décodée en RGBA8 sw × sh */
    uint32_t soct, slig, scol, sl0;
    sc2x_geo(sfmt, sr, &soct, &slig, &scol, &sl0);
    uint8_t *tas = malloc((size_t)soct * slig), *rgba = malloc((size_t)sw * sh * 4);
    if (!tas || !rgba) { free(tas); free(rgba); if (note) snprintf(note, cap, "mémoire"); return SC2X_NON_GERE; }
    for (uint32_t y = 0; y < slig; y++) memcpy(tas + (size_t)y * soct, src + (uint64_t)y * spitch, soct);
    if (d3dv_decoder_rgba(sfmt, tas, sw, sh, rgba)) {
        free(tas); free(rgba); if (note) snprintf(note, cap, "format source 0x%X non décodable", sfmt); return SC2X_NON_GERE;
    }
    uint32_t dbpp = sc2x_bpp(dfmt);
    uint64_t cles = 0;
    for (uint32_t y = 0; y < ch; y++) {
        uint8_t *ligne = dst + (uint64_t)(dl0 + y) * dpitch + dcol;
        for (uint32_t x = 0; x < cw; x++) {
            uint8_t *c = rgba + ((size_t)y * sw + x) * 4;
            if (colorkey && ((uint32_t)c[3] << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2]) == colorkey) { memset(c, 0, 4); cles++; }
            sc2x_encoder(dfmt, c, ligne + (size_t)x * dbpp);
        }
    }
    free(tas); free(rgba);
    if (note) snprintf(note, cap, "conversion 0x%X → 0x%X%s%s", sfmt, dfmt, tronque ? ", tailles différentes : partie commune sans mise à l'échelle" : "",
                       colorkey ? (cles ? ", clé de couleur appliquée" : ", clé de couleur sans effet") : "");
    return SC2X_OK;
}
#endif
