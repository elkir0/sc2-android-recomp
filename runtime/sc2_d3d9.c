/* sc2_d3d9.c — Direct3D 9 FACTICE pour le boot du POC (passe 4).
 *
 * GameMain étape 3 (sc2_init_GfxCreate 0x009F6BE0 → Direct3DCreate9 via
 * 0x00A00FC0, CARTE-INIT §0) demande un IDirect3D9, interroge l'adaptateur puis
 * crée un IDirect3DDevice9. Ici, tout est factice mais COHÉRENT :
 *   - un adaptateur « SC2 Partout D3D9 factice », 1024×768 X8R8G8B8 60 Hz,
 *     capacités SM 3.0 (D3DCAPS9 rempli champ par champ) ;
 *   - un device qui MÉMORISE ses états (render/sampler/texture-stage states,
 *     transformations, viewport, shaders, flux, cibles de rendu) et les rend aux
 *     Get* correspondants, mais ne dessine rien ;
 *   - des ressources (textures, surfaces, VB/IB, requêtes, shaders, déclarations,
 *     state blocks) dont Lock/LockRect rendent une vraie mémoire invitée.
 * Chaque appel est tracé (3 premiers détaillés, puis comptés ; bilan final). Une
 * méthode sans gestionnaire arrête le run en se nommant : rien n'est inventé en
 * silence.
 */
#define _GNU_SOURCE
#include "sc2_d3d9.h"
#include "sc2_d3d9v.h"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>            /* VkFormat des attributs de sommets (passe 5d) */
/* Table des shaders du jeu traduits hors-ligne (scripts/d3d9-vers-spirv.py, hors git :
 * régénération dans src/shaders/README.md). Absente → aucun draw réel (mode « lumière »). */
#if __has_include("sc2_shaders_spv.h")
#include "sc2_shaders_spv.h"
#else
typedef struct { uint8_t reg, usage, index; } sc2_spv_entree;
typedef struct { uint64_t hash; uint8_t ps; uint8_t nentrees; sc2_spv_entree entrees[16]; uint8_t samplers[16];
                 const uint32_t *spv; uint32_t nmots; const uint32_t *spv_ombre; uint32_t nmots_ombre; } sc2_spv_shader;
#define SC2_NB_SPV 0
#define SC2_SPV_OMBRES 1
static const sc2_spv_shader SC2_SHADERS_SPV[1] = { { 0 } };
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef uint32_t (*com_fn)(CPU *c, uint32_t self, const uint32_t *a);
typedef struct { const char *nom; int argc; com_fn fn; uint64_t n; } com_meth;
typedef struct { const char *iface; com_meth *m; int nm; uint32_t vtbl; int thiscall; } com_class;

#define A(i) (a[(i)])
static pthread_mutex_t g_com_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---- objets ---------------------------------------------------------------- */
/* Disposition d'un objet factice dans l'arène. */
enum { O_VTBL = 0, O_REF = 4, O_W = 8, O_H = 12, O_FMT = 16, O_LEVELS = 20, O_SIZE = 24,
       O_BUF = 28, O_USAGE = 32, O_POOL = 36, O_DEPTH = 40, O_KIND = 44, O_AUX = 48,
       O_GEN = 52 /* passe 5d : génération du contenu (Lock) */, O_LEVEL = 56 /* niveau d'une surface de texture */,
       O_SIZEOF = 64 };

static uint32_t obj_new(com_class *k, uint32_t kind)
{
    uint32_t o = sc2_alloc(O_SIZEOF);
    wr32(o + O_VTBL, k->vtbl); wr32(o + O_REF, 1); wr32(o + O_KIND, kind);
    return o;
}

/* ---- IUnknown, IDirect3DResource9 : communs --------------------------------- */
static uint32_t m_ok(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 0; }
static uint32_t m_zero(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 0; }
static uint32_t m_one(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 1; }
static uint32_t m_AddRef(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)a; uint32_t r = rd32(s + O_REF) + 1; wr32(s + O_REF, r); return r; }
static uint32_t m_Release(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)a; uint32_t r = rd32(s + O_REF); if (r) r--; wr32(s + O_REF, r); return r; }
static uint32_t m_QueryInterface(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; if (A(1)) wr32(A(1), s); m_AddRef(c, s, a); return 0; }
static uint32_t g_device;                       /* l'unique IDirect3DDevice9 factice */
static uint32_t g_d3d;                          /* l'unique IDirect3D9 factice */
static uint32_t m_GetDevice(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; if (A(0)) wr32(A(0), g_device); return 0; }
static uint32_t m_GetPriority(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 0; }
static uint32_t m_notfound(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 0x88760866u; }   /* D3DERR_NOTFOUND */
static uint32_t m_invalid(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 0x8876086Cu; }    /* D3DERR_INVALIDCALL */
static uint32_t m_GetType(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; return rd32(s + 44 /* O_KIND */); }

#define IUNK  { "QueryInterface", 2, m_QueryInterface, 0 }, { "AddRef", 0, m_AddRef, 0 }, { "Release", 0, m_Release, 0 }
#define RES   { "GetDevice", 1, m_GetDevice, 0 }, { "SetPrivateData", 4, m_ok, 0 }, \
              { "GetPrivateData", 3, m_notfound, 0 }, { "FreePrivateData", 1, m_ok, 0 }, \
              { "SetPriority", 1, m_zero, 0 }, { "GetPriority", 0, m_GetPriority, 0 }, \
              { "PreLoad", 0, m_ok, 0 }, { "GetType", 0, m_GetType, 0 }

/* ---- formats ------------------------------------------------------------------ */
#define FMT_DXT1 0x31545844u
#define FMT_DXT2 0x32545844u
#define FMT_DXT3 0x33545844u
#define FMT_DXT4 0x34545844u
#define FMT_DXT5 0x35545844u
static int fmt_bloc(uint32_t f) { return f == FMT_DXT1 ? 8 : (f >= FMT_DXT2 && f <= FMT_DXT5) ? 16 : 0; }
static uint32_t fmt_bpp(uint32_t f)
{
    switch (f) {
    case 20: return 3;                                  /* R8G8B8 */
    case 23: case 24: case 25: case 26: case 29: case 30: case 51: case 80: case 81: return 2;
    case 28: case 41: case 50: return 1;                /* A8, P8, L8 */
    case 113: case 115: return 8; case 116: return 16;  /* A16B16G16R16F, G32R32F, A32B32G32R32F */
    case 111: return 2; case 114: return 4;             /* R16F, R32F (passe 7l : 114/115 étaient pris pour R16F/G16R16F) */
    case 101: case 102: return 8;                       /* (rares) */
    default: return 4;
    }
}
static uint32_t pitch_of(uint32_t f, uint32_t w)
{
    int b = fmt_bloc(f);
    if (b) return ((w + 3) / 4 ? (w + 3) / 4 : 1) * (uint32_t)b;
    return w * fmt_bpp(f);
}
static uint32_t rows_of(uint32_t f, uint32_t h) { return fmt_bloc(f) ? ((h + 3) / 4 ? (h + 3) / 4 : 1) : h; }

/* Mémoire de verrouillage : allouée au premier Lock, taille du niveau 0, réutilisée. */
static uint32_t buf_of(uint32_t o, uint32_t size)
{
    uint32_t b = rd32(o + O_BUF);
    if (!b || rd32(o + O_SIZE) < size) {
        b = sc2_alloc(size ? size : 16); wr32(o + O_BUF, b); wr32(o + O_SIZE, size);
    }
    return b;
}
static uint32_t lvl(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1; }
static uint64_t g_n_lock_rect;              /* passe 7l : Lock* avec sous-rectangle / sous-boîte */
/* Passe 7v3 (demande 7w3) : textures de luminance L8/A8L8 (D3DFMT 50/51 : plans Y/CbCr des vidéos .ogv du jeu) —
 * verrouillages, copies UpdateTexture/UpdateSurface, liaisons aux draws et générations distinctes vues au draw. */
static uint64_t g_n_lock_lum, g_n_upd_lum, g_n_lie_lum, g_n_gen_lum;
static int lum(uint32_t o) { uint32_t f = rd32(o + O_FMT); return f == 50 || f == 51; }
static void noter_lock_lum(uint32_t o, uint32_t level, const char *voie)
{
    if (!lum(o)) return;
    if (g_n_lock_lum++ < 40)
        sc2_trace("[VIDEO-TEX] %s texture 0x%08X %u×%u D3DFMT %u usage 0x%X pool %u niveau %u → gen %u", voie, o, rd32(o + O_W),
                rd32(o + O_H), rd32(o + O_FMT), rd32(o + O_USAGE), rd32(o + O_POOL), level, rd32(o + O_GEN));
}
enum { INV_CREE, INV_DDS };                 /* passe 7l : inventaire des textures (au bilan) */
static void inventaire_tex(uint32_t orig, uint32_t t);

/* ---- classes (déclarées d'abord : les fabriques s'y réfèrent) ----------------- */
static com_class K_D3D, K_DEV, K_TEX, K_CUBE, K_VOL, K_SURF, K_VB, K_IB, K_QUERY, K_VS, K_PS,
                 K_DECL, K_SB, K_SWAP, K_TSF, K_BNET;

/* surface (D3DSURFACE_DESC : Format, Type, Usage, Pool, MultiSampleType, Quality, Width, Height) */
static uint32_t surf_new(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool)
{
    uint32_t s = obj_new(&K_SURF, 1);
    wr32(s + O_W, w); wr32(s + O_H, h); wr32(s + O_FMT, fmt); wr32(s + O_USAGE, usage);
    wr32(s + O_POOL, pool); wr32(s + O_LEVELS, 1);
    return s;
}
static void surf_desc(uint32_t s, uint32_t d, uint32_t level, uint32_t type)
{
    wr32(d + 0, rd32(s + O_FMT)); wr32(d + 4, type); wr32(d + 8, rd32(s + O_USAGE));
    wr32(d + 12, rd32(s + O_POOL)); wr32(d + 16, 0); wr32(d + 20, 0);
    wr32(d + 24, lvl(rd32(s + O_W), level)); wr32(d + 28, lvl(rd32(s + O_H), level));
}
/* Passe 5d : mémoire de verrouillage par NIVEAU (avant : un seul tampon pour tous
 * les niveaux — écrire le mip 1 écrasait le niveau 0). Les niveaux se suivent,
 * serrés, dans l'ordre d'un fichier DDS : niveau_offset(o, n) = taille des niveaux < n
 * (volumes : × profondeur du niveau). Chaque Lock incrémente O_GEN : le backend
 * Vulkan re-téléverse une texture dont la génération a changé. */
static uint32_t niveau_taille(uint32_t o, uint32_t l)
{
    uint32_t f = rd32(o + O_FMT), d = rd32(o + O_DEPTH);
    return pitch_of(f, lvl(rd32(o + O_W), l)) * rows_of(f, lvl(rd32(o + O_H), l)) * (d ? lvl(d, l) : 1);
}
static uint32_t niveau_offset(uint32_t o, uint32_t l)
{ uint32_t off = 0; for (uint32_t k = 0; k < l; k++) off += niveau_taille(o, k); return off; }
static uint32_t niveaux(uint32_t o) { uint32_t n = rd32(o + O_LEVELS); return n ? (n > 16 ? 16 : n) : 1; }
/* Passe 7l : pRect (RECT left, top, right, bottom) RESPECTÉ — pBits pointe sur son coin haut-gauche
 * (DXTn : bloc 4×4 qui le contient). Avant : toujours le début du niveau ; l'atlas de glyphes du jeu,
 * rempli glyphe par glyphe en sous-rectangles, recevait tous ses glyphes en (0, 0). */
static uint32_t decalage_rect(uint32_t f, uint32_t pitch, uint32_t x, uint32_t y)
{
    int b = fmt_bloc(f);
    return b ? (y / 4) * pitch + (x / 4) * (uint32_t)b : y * pitch + x * fmt_bpp(f);
}
/* Cubes (passe 7l) : 6 faces, chacune sa chaîne de niveaux complète, dans l'ordre d'un DDS (+X, -X, +Y, -Y, +Z, -Z). */
static uint32_t faces_de(uint32_t o) { return rd32(o + O_KIND) == 5 ? 6u : 1u; }
static uint32_t lock_rect(uint32_t o, uint32_t face, uint32_t level, uint32_t locked, uint32_t rect)
{
    uint32_t f = rd32(o + O_FMT), w = lvl(rd32(o + O_W), level), h = lvl(rd32(o + O_H), level);
    if (level >= niveaux(o)) level = 0;
    if (face >= faces_de(o)) face = 0;
    uint32_t chaine = niveau_offset(o, niveaux(o));
    uint32_t b = buf_of(o, chaine * faces_de(o)) + face * chaine, pitch = pitch_of(f, w), dec = 0;
    if (rect) {
        uint32_t x = rd32(rect), y = rd32(rect + 4);
        if (x < w && y < h) dec = decalage_rect(f, pitch, x, y);
        g_n_lock_rect++;
    }
    wr32(locked + 0, pitch); wr32(locked + 4, b + niveau_offset(o, level) + dec);
    wr32(o + O_GEN, rd32(o + O_GEN) + 1);
    noter_lock_lum(o, level, "LockRect");
    return 0;
}

/* Surface */
static uint32_t s_GetDesc(CPU *c, uint32_t s, const uint32_t *a) { (void)c; surf_desc(s, A(0), 0, 1); return 0; }
/* Surface d'une texture (GetSurfaceLevel) : verrouille le NIVEAU de la texture conteneur. */
static uint32_t s_LockRect(CPU *c, uint32_t s, const uint32_t *a)
{   /* O_LEVEL d'une surface de texture : niveau | face << 16 (cubes, passe 7l) */
    (void)c; uint32_t p = rd32(s + O_AUX), l = rd32(s + O_LEVEL);
    return p ? lock_rect(p, l >> 16, l & 0xFFFF, A(0), A(1)) : lock_rect(s, 0, 0, A(0), A(1));
}
/* Passe 7w7 (patch préventif proposé par la 7w5, TRACE-PASSE7W5 §4) : la génération avance AUSSI au déverrouillage.
 * Au Lock seul, un draw (autre fil, ou entre Lock et Unlock) qui téléverse la texture fige un contenu à moitié écrit :
 * la génération est déjà vue, l'écriture qui suit n'est jamais re-téléversée. En D3D9, le contenu n'est défini qu'à
 * l'Unlock ; coût : au plus un téléversement de plus par Lock déjà consommé. */
static uint32_t gen_unlock(uint32_t o) { wr32(o + O_GEN, rd32(o + O_GEN) + 1); return 0; }
static uint32_t s_UnlockRect(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)a; uint32_t p = rd32(s + O_AUX); return gen_unlock(p ? p : s); }
static uint32_t t_UnlockRect(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; return gen_unlock(s); }
static uint32_t s_GetDC(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; if (A(0)) wr32(A(0), 0x0000DC01u); return 0; }
static uint32_t s_GetType(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; return rd32(s + O_KIND); }
static uint32_t s_GetContainer(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; uint32_t p = rd32(s + O_AUX);
    if (!p) { if (A(1)) wr32(A(1), 0); return 0x80004002u; }   /* E_NOINTERFACE : surface autonome */
    wr32(A(1), p); wr32(p + O_REF, rd32(p + O_REF) + 1); return 0;
}
static com_meth M_SURF[] = { IUNK, RES, { "GetContainer", 2, s_GetContainer, 0 }, { "GetDesc", 1, s_GetDesc, 0 },
    { "LockRect", 3, s_LockRect, 0 }, { "UnlockRect", 0, s_UnlockRect, 0 }, { "GetDC", 1, s_GetDC, 0 },
    { "ReleaseDC", 1, m_ok, 0 } };

/* Textures : D3DRTYPE_TEXTURE 3, CUBE 5, VOLUME 4 */
static uint32_t t_GetLevelCount(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; return rd32(s + O_LEVELS); }
static uint32_t t_GetLevelDesc(CPU *c, uint32_t s, const uint32_t *a) { (void)c; surf_desc(s, A(1), A(0), 1); return 0; }
/* Passe 10j : UNE surface par (texture, niveau[, face]), comme D3D9 (GetSurfaceLevel rend le même objet, AddRef).
 * Avant : une surface neuve à chaque appel, jamais libérée (m_Release ne libère rien) — ~7 par frame dans la boucle de
 * rendu, 320 000 objets (35 Mo d'arène) en 20 min de banc, une part de l'épuisement d'arène du Fold. Les objets
 * n'étant jamais libérés, une adresse de texture n'est jamais réutilisée : le cache ne peut pas rendre une surface
 * d'une autre texture. Table pleine : surface neuve, comme avant. */
#define NSURFC 16384
static struct { uint32_t tex, code, sf; } g_surfc[NSURFC];
static pthread_mutex_t g_surfc_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_n_surfc_neuves, g_n_surfc_reprises;
static uint32_t surf_niveau(uint32_t s, uint32_t niveau, uint32_t code)
{
    pthread_mutex_lock(&g_surfc_mu);
    uint32_t k = ((s >> 4) * 2654435761u ^ code * 40503u) % NSURFC, n = 0, sf = 0;
    for (; n < NSURFC && g_surfc[k].tex; k = (k + 1) % NSURFC, n++)
        if (g_surfc[k].tex == s && g_surfc[k].code == code) { sf = g_surfc[k].sf; break; }
    if (sf) { wr32(sf + O_REF, rd32(sf + O_REF) + 1); g_n_surfc_reprises++; pthread_mutex_unlock(&g_surfc_mu); return sf; }
    sf = surf_new(lvl(rd32(s + O_W), niveau), lvl(rd32(s + O_H), niveau), rd32(s + O_FMT), rd32(s + O_USAGE), rd32(s + O_POOL));
    wr32(sf + O_AUX, s); wr32(sf + O_LEVEL, code);
    if (n < NSURFC) { g_surfc[k].tex = s; g_surfc[k].code = code; g_surfc[k].sf = sf; }
    g_n_surfc_neuves++;
    pthread_mutex_unlock(&g_surfc_mu);
    return sf;
}
static uint32_t t_GetSurfaceLevel(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c;
    wr32(A(1), surf_niveau(s, A(0), A(0))); return 0;
}
static uint32_t t_LockRect(CPU *c, uint32_t s, const uint32_t *a) { (void)c; return lock_rect(s, 0, A(0), A(1), A(2)); }
static uint32_t t_GetAutoGen(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 2; }
#define BASETEX { "SetLOD", 1, m_zero, 0 }, { "GetLOD", 0, m_zero, 0 }, { "GetLevelCount", 0, t_GetLevelCount, 0 }, \
                { "SetAutoGenFilterType", 1, m_ok, 0 }, { "GetAutoGenFilterType", 0, t_GetAutoGen, 0 }, \
                { "GenerateMipSubLevels", 0, m_ok, 0 }
static com_meth M_TEX[] = { IUNK, RES, BASETEX, { "GetLevelDesc", 2, t_GetLevelDesc, 0 },
    { "GetSurfaceLevel", 2, t_GetSurfaceLevel, 0 }, { "LockRect", 4, t_LockRect, 0 },
    { "UnlockRect", 1, t_UnlockRect, 0 }, { "AddDirtyRect", 1, m_ok, 0 } };
static uint32_t cu_GetCubeMapSurface(CPU *c, uint32_t s, const uint32_t *a)   /* (face, niveau, **surface) */
{
    (void)c;                                           /* passe 10j : surface mise en cache par (cube, niveau, face) */
    wr32(A(2), surf_niveau(s, A(1), (A(1) & 0xFFFF) | (A(0) < 6 ? A(0) : 0) << 16));
    return 0;
}
static uint32_t cu_LockRect(CPU *c, uint32_t s, const uint32_t *a) { (void)c; return lock_rect(s, A(0), A(1), A(2), A(3)); }
static com_meth M_CUBE[] = { IUNK, RES, BASETEX, { "GetLevelDesc", 2, t_GetLevelDesc, 0 },
    { "GetCubeMapSurface", 3, cu_GetCubeMapSurface, 0 }, { "LockRect", 5, cu_LockRect, 0 },
    { "UnlockRect", 2, t_UnlockRect, 0 }, { "AddDirtyRect", 2, m_ok, 0 } };
static uint32_t vo_LockBox(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c;
    uint32_t f = rd32(s + O_FMT), l = A(0) < niveaux(s) ? A(0) : 0;
    uint32_t w = lvl(rd32(s + O_W), l), h = lvl(rd32(s + O_H), l);
    uint32_t dec = 0;
    if (A(2)) {                                   /* pBox (Left, Top, Right, Bottom, Front, Back) : passe 7l */
        uint32_t x = rd32(A(2)), y = rd32(A(2) + 4), z = rd32(A(2) + 16);
        if (x < w && y < h && z < lvl(rd32(s + O_DEPTH), l)) dec = z * pitch_of(f, w) * rows_of(f, h) + decalage_rect(f, pitch_of(f, w), x, y);
        g_n_lock_rect++;
    }
    wr32(A(1) + 0, pitch_of(f, w)); wr32(A(1) + 4, pitch_of(f, w) * rows_of(f, h));
    wr32(A(1) + 8, buf_of(s, niveau_offset(s, niveaux(s))) + niveau_offset(s, l) + dec);
    wr32(s + O_GEN, rd32(s + O_GEN) + 1);
    return 0;
}
static uint32_t vo_GetLevelDesc(CPU *c, uint32_t s, const uint32_t *a)   /* D3DVOLUME_DESC */
{
    (void)c; uint32_t d = A(1);
    wr32(d, rd32(s + O_FMT)); wr32(d + 4, 2); wr32(d + 8, rd32(s + O_USAGE)); wr32(d + 12, rd32(s + O_POOL));
    wr32(d + 16, lvl(rd32(s + O_W), A(0))); wr32(d + 20, lvl(rd32(s + O_H), A(0))); wr32(d + 24, lvl(rd32(s + O_DEPTH), A(0)));
    return 0;
}
static com_meth M_VOL[] = { IUNK, RES, BASETEX, { "GetLevelDesc", 2, vo_GetLevelDesc, 0 },
    { "GetVolumeLevel", 2, m_invalid, 0 }, { "LockBox", 4, vo_LockBox, 0 },
    { "UnlockBox", 1, t_UnlockRect, 0 }, { "AddDirtyBox", 1, m_ok, 0 } };

/* Vertex / index buffers (D3DVERTEXBUFFER_DESC : Format, Type, Usage, Pool, Size, FVF) */
/* passe 10r : chaque Lock avance O_GEN du tampon (offset 52, déjà la génération des textures —
 * pour un VB/IB il n'avait AUCUN usage : cf. b_GetDesc, m_Release, buf_of) ; le backend Vulkan
 * recopie un tampon statique mis en cache dont la génération a changé. */
static uint32_t b_Lock(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; uint32_t b = buf_of(s, rd32(s + O_W)); wr32(A(2), b + A(0));
  wr32(s + O_GEN, rd32(s + O_GEN) + 1); return 0; }
static uint32_t b_GetDesc(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; uint32_t d = A(0);
    wr32(d + 0, rd32(s + O_FMT)); wr32(d + 4, rd32(s + O_KIND)); wr32(d + 8, rd32(s + O_USAGE));
    wr32(d + 12, rd32(s + O_POOL)); wr32(d + 16, rd32(s + O_W)); wr32(d + 20, rd32(s + O_H));
    return 0;
}
static com_meth M_VB[] = { IUNK, RES, { "Lock", 4, b_Lock, 0 }, { "Unlock", 0, m_ok, 0 }, { "GetDesc", 1, b_GetDesc, 0 } };
static com_meth M_IB[] = { IUNK, RES, { "Lock", 4, b_Lock, 0 }, { "Unlock", 0, m_ok, 0 }, { "GetDesc", 1, b_GetDesc, 0 } };

/* Requêtes : GetData rend S_OK et des zéros (occlusion : 0 pixel ; event : fini). */
static uint32_t q_GetType(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; return rd32(s + O_FMT); }
static uint32_t q_GetDataSize(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 8; }
static uint32_t q_GetData(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    if (A(0) && A(1)) { memset(GPTR(A(0)), 0, A(1)); if (rd32(s + O_FMT) == 8 && A(1) >= 4) wr32(A(0), 1); }
    return 0;
}
static com_meth M_QUERY[] = { IUNK, { "GetDevice", 1, m_GetDevice, 0 }, { "GetType", 0, q_GetType, 0 },
    { "GetDataSize", 0, q_GetDataSize, 0 }, { "Issue", 1, m_ok, 0 }, { "GetData", 3, q_GetData, 0 } };
static com_meth M_VS[] = { IUNK, { "GetDevice", 1, m_GetDevice, 0 }, { "GetFunction", 2, m_invalid, 0 } };
static com_meth M_PS[] = { IUNK, { "GetDevice", 1, m_GetDevice, 0 }, { "GetFunction", 2, m_invalid, 0 } };
static com_meth M_DECL[] = { IUNK, { "GetDevice", 1, m_GetDevice, 0 }, { "GetDeclaration", 2, m_invalid, 0 } };
static com_meth M_SB[] = { IUNK, { "GetDevice", 1, m_GetDevice, 0 }, { "Capture", 0, m_ok, 0 }, { "Apply", 0, m_ok, 0 } };
static uint32_t sw_GetBackBuffer(CPU *c, uint32_t s, const uint32_t *a);
/* Present (device ou swap chain) : chaque frame présentée est comptée et les 5
 * premières journalisées — c'est la preuve qu'une frame complète a été rendue. */
static uint32_t g_frames;
uint32_t sc2_frames_presentees(void) { return __atomic_load_n(&g_frames, __ATOMIC_RELAXED); }   /* passe 7v */
static int cible_de(uint32_t surf);
static int shader_id(uint32_t o) { int i = o ? (int)rd32(o + O_AUX) - 1 : -1; return i >= 0 && i < g_nshaders ? i : -1; }
static uint32_t g_backbuf_va;                 /* = D.backbuf (déclaré plus bas) */
/* Passe 5c : PPM du back buffer aux frames 1, 10, 100 puis toutes les 100 (+ SC2_FRAMES_PAS, passe 7m). */
static void present_vk(uint32_t n)
{
    if (!d3dv_actif()) return;
    /* passe 7m : SC2_FRAMES_PAS=<k> → aussi une frame toutes les k (écran éphémère avant un arrêt) ;
     * passe 8f : SC2_FRAMES_TOUTES=<n> → TOUTES les frames 1..n (paliers du chargement de la démo) */
    static uint32_t pas = ~0u, toutes = ~0u;
    if (pas == ~0u) { const char *e = getenv("SC2_FRAMES_PAS"); pas = e ? (uint32_t)strtoul(e, NULL, 10) : 0; }
    if (toutes == ~0u) { const char *e = getenv("SC2_FRAMES_TOUTES"); toutes = e ? (uint32_t)strtoul(e, NULL, 10) : 0; }
    int sauver = n == 1 || n == 10 || n % 100 == 0 || (pas && n % pas == 0) || n <= toutes;
    char chemin[512] = "";
    if (sauver) {
        const char *dir = getenv("SC2_FRAMES_DIR"); if (!dir || !*dir) dir = "frames";
        mkdir(dir, 0755);
        snprintf(chemin, sizeof chemin, "%s/frame_%05u.ppm", dir, n);
    }
    double moy[3] = { 0, 0, 0 };
    /* passe 7m : SC2_CIBLES_DUMP=<frame> → toutes les cibles de rendu en PPM (dans SC2_FRAMES_DIR) */
    static uint32_t fc = ~0u;
    if (fc == ~0u) { const char *e = getenv("SC2_CIBLES_DUMP"); fc = e ? (uint32_t)strtoul(e, NULL, 10) : 0; }
    if (fc && n == fc) { const char *dir = getenv("SC2_FRAMES_DIR"); d3dv_vider_cibles(dir && *dir ? dir : "frames", n); }
    int r = d3dv_present(cible_de(g_backbuf_va), sauver ? chemin : NULL, moy);
    if (sauver)
        sc2_trace("[VK] Present frame n°%u → %s : %s, moyenne RGB %.2f %.2f %.2f%s", n, chemin, r ? "ÉCHEC" : "écrit",
                moy[0], moy[1], moy[2], !r && (moy[0] + moy[1] + moy[2]) > 0 ? " (NON NOIRE)" : "");
}
static void ui2d_fenetre(uint32_t n);   /* passe 10k, plus bas */
static uint32_t m_Present(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)s; (void)a;
    uint32_t n = __atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
    if (n <= 5 || (n & (n - 1)) == 0)
        sc2_trace("[D3D9] Present : FRAME n°%u présentée (%s, appelant 0x%08X)", n,
                d3dv_actif() ? "back buffer Vulkan" : "device factice", rd32(c->esp));
    present_vk(n);
    ui2d_fenetre(n);
    /* SC2_FRAMES_MAX=N : arrêt volontaire et propre après N frames (run de référence). */
    static uint32_t max = 1;
    if (max == 1) { const char *e = getenv("SC2_FRAMES_MAX"); max = e ? (uint32_t)strtoul(e, NULL, 10) : 0; }
    if (max && n == max)
        sc2_stop(0, "SC2_FRAMES_MAX=%u : %u frames présentées par la boucle de rendu — arrêt volontaire du run", max, n);
    return 0;
}
static uint32_t sw_GetRasterStatus(CPU *c, uint32_t s, const uint32_t *a);
static uint32_t sw_GetDisplayMode(CPU *c, uint32_t s, const uint32_t *a);
static uint32_t sw_GetPresentParameters(CPU *c, uint32_t s, const uint32_t *a);
static com_meth M_SWAP[] = { IUNK, { "Present", 5, m_Present, 0 }, { "GetFrontBufferData", 1, m_ok, 0 },
    { "GetBackBuffer", 3, sw_GetBackBuffer, 0 }, { "GetRasterStatus", 1, sw_GetRasterStatus, 0 },
    { "GetDisplayMode", 1, sw_GetDisplayMode, 0 },
    { "GetDevice", 1, m_GetDevice, 0 }, { "GetPresentParameters", 1, sw_GetPresentParameters, 0 } };

/* ---- adaptateur : IDirect3D9 ---------------------------------------------------- */
/* Passe 5c : mode unique annoncé, 1280×720 par défaut (back buffer Vulkan
 * offscreen) ; SC2_ECRAN=LxH le change (passe 4 : 1024×768). */
static uint32_t g_ecran_w, g_ecran_h;
static void ecran_init(void)
{
    if (g_ecran_w) return;
    g_ecran_w = 1280; g_ecran_h = 720;
    const char *e = getenv("SC2_ECRAN");
    unsigned w, h;
    if (e && sscanf(e, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 8192 && h <= 8192) { g_ecran_w = w; g_ecran_h = h; }
}
#define ECRAN_W (ecran_init(), g_ecran_w)
#define ECRAN_H (ecran_init(), g_ecran_h)
#define FMT_X8R8G8B8 22u

static void mode_ecrire(uint32_t m)
{ wr32(m + 0, ECRAN_W); wr32(m + 4, ECRAN_H); wr32(m + 8, 60); wr32(m + 12, FMT_X8R8G8B8); }
static uint32_t d_GetAdapterCount(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 1; }
static uint32_t d_GetAdapterIdentifier(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s; uint32_t p = A(2);
    memset(GPTR(p), 0, 1100);
    strcpy((char *)GPTR(p), "sc2partout_d3d9.dll");
    strcpy((char *)GPTR(p + 512), "SC2 Partout D3D9 factice (passe 4)");
    strcpy((char *)GPTR(p + 1024), "\\\\.\\DISPLAY1");
    wr32(p + 1056, 0x00000000u); wr32(p + 1060, 0x00080010u);   /* DriverVersion 8.16.x */
    wr32(p + 1064, 0x10DE); wr32(p + 1068, 0x0402);             /* « GeForce 8600 GT » */
    wr32(p + 1072, 0); wr32(p + 1076, 0xA1);
    wr32(p + 1080, 0x53433250u);                                /* GUID : « SC2P » … */
    wr32(p + 1096, 1);
    return 0;
}
static uint32_t d_GetAdapterModeCount(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; return A(1) == FMT_X8R8G8B8 ? 1 : 0; }
static uint32_t d_EnumAdapterModes(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; if (A(2) != 0) return 0x8876086Cu; mode_ecrire(A(3)); return 0; }   /* INVALIDCALL */
static uint32_t d_GetAdapterDisplayMode(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; mode_ecrire(A(1)); return 0; }
static uint32_t d_CheckDeviceMultiSampleType(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    if (A(5)) wr32(A(5), 1);
    return A(4) <= 1 ? 0 : 0x8876086Au;                         /* NOTAVAILABLE au-delà de 1× */
}
static void caps_ecrire(uint32_t p)
{
    float f;
#define WF(off, v) do { f = (v); memcpy(GPTR(p + (off)), &f, 4); } while (0)
    memset(GPTR(p), 0, 304);
    wr32(p + 0, 1);                       /* DeviceType = HAL */
    for (uint32_t o = 8; o <= 84; o += 4) wr32(p + o, 0xFFFFFFFFu);   /* Caps … LineCaps */
    wr32(p + 20, 0x8000000Fu);            /* PresentationIntervals : IMMEDIATE | 1..4 */
    wr32(p + 88, 8192); wr32(p + 92, 8192); wr32(p + 96, 2048);
    wr32(p + 100, 8192); wr32(p + 104, 8192); wr32(p + 108, 16);
    WF(112, 1e10f); WF(116, -1e8f); WF(120, -1e8f); WF(124, 1e8f); WF(128, 1e8f); WF(132, 0.f);
    wr32(p + 136, 0x1FF); wr32(p + 140, 0x00080008u); wr32(p + 144, 0x03FFFFFFu);
    wr32(p + 148, 8); wr32(p + 152, 8); wr32(p + 156, 0x17B); wr32(p + 160, 8); wr32(p + 164, 6);
    wr32(p + 168, 4); wr32(p + 172, 0); WF(176, 8192.f); wr32(p + 180, 0xFFFFF); wr32(p + 184, 0xFFFFFF);
    wr32(p + 188, 16); wr32(p + 192, 255);
    wr32(p + 196, 0xFFFE0300u); wr32(p + 200, 256); wr32(p + 204, 0xFFFF0300u); WF(208, 65504.f);
    wr32(p + 212, 0x7F); WF(216, 0.f); wr32(p + 224, 0); wr32(p + 228, 0); wr32(p + 232, 1);
    wr32(p + 236, 0x3FF); wr32(p + 240, 4); wr32(p + 244, 0x03030303u);
    wr32(p + 248, 1); wr32(p + 252, 24); wr32(p + 256, 32); wr32(p + 260, 4);             /* VS20Caps */
    wr32(p + 264, 0x1F); wr32(p + 268, 24); wr32(p + 272, 32); wr32(p + 276, 4); wr32(p + 280, 512);
    wr32(p + 284, 0x03030300u); wr32(p + 288, 65535); wr32(p + 292, 65535);
    wr32(p + 296, 32768); wr32(p + 300, 32768);
#undef WF
}
static uint32_t d_GetDeviceCaps(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; caps_ecrire(A(2)); return 0; }
static uint32_t d_GetAdapterMonitor(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 0x0000F001u; }

/* état du device factice */
static struct {
    uint32_t pp[14];                     /* D3DPRESENT_PARAMETERS mémorisés */
    uint32_t rs[256], tss[16][33], ss[20][14];
    uint32_t xf[512][16];                /* transformations (VIEW 2, PROJ 3, TEX 16.., WORLD 256..) */
    uint32_t vp[6], scissor[4], tex[20], rt[4], ds, backbuf, decl, fvf, vs, ps, indices;
    uint32_t stream[16][3], freq[16];
    uint32_t vsf[256 * 4], psf[224 * 4], vsi[16 * 4], psi[16 * 4], vsb[16], psb[16];
    /* passe 10r : générations des blocs de constantes — avancées dès qu'un Set*ShaderConstant*
     * change un mot (contenu identique réécrit : bloc inchangé, génération tenue). Le draw ne
     * réassemble un bloc que si sa génération a bougé ; le backend y réutilise l'emplacement. */
    uint32_t gen_vs, gen_ps;
    uint32_t clip[6][4], material[17], light_on[8];
} D;

static uint32_t addref_out(uint32_t out, uint32_t o);
static uint32_t sw_GetBackBuffer(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; return addref_out(A(2), D.backbuf); }
static uint32_t sw_GetRasterStatus(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; wr32(A(0), 0); wr32(A(0) + 4, 0); return 0; }
static uint32_t sw_GetPresentParameters(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; for (int k = 0; k < 14; k++) wr32(A(0) + 4u * (uint32_t)k, D.pp[k]); return 0; }
static void mode_ecrire(uint32_t m);
static uint32_t sw_GetDisplayMode(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; mode_ecrire(A(0)); return 0; }

/* Passe 7s : CheckDeviceFormat journalisé (une ligne par triplet usage/ressource/format distinct) et refus OPTIONNEL.
 * Par défaut tout est accepté (comportement antérieur). SC2_FORMATS_REFUS="<fmt>[/<type>],…" : <fmt> = 0x… ou FOURCC
 * de 4 lettres (NULL, INTZ, DF24, DF16, RAWZ…), <type> = D3DRESOURCETYPE (1 surface, 3 texture) ; réponse
 * D3DERR_NOTAVAILABLE. Sert à voir quel chemin le jeu prend (ombres : D16 en texture + cible « NULL »). */
#define D3DERR_NOTAVAILABLE 0x8876086Au
static uint32_t d_CheckDeviceFormat(CPU *c, uint32_t s, const uint32_t *a)   /* (adaptateur, type, fmt adaptateur, usage, ressource, fmt) */
{
    (void)c; (void)s;
    static struct { uint32_t fmt, type; } refus[32]; static int nrefus = -1;
    if (nrefus < 0) {
        nrefus = 0;
        const char *e = getenv("SC2_FORMATS_REFUS");
        char buf[512]; snprintf(buf, sizeof buf, "%s", e ? e : "");
        for (char *t = strtok(buf, ","); t && nrefus < 32; t = strtok(NULL, ",")) {
            char *barre = strchr(t, '/'); uint32_t type = 0;
            if (barre) { *barre = 0; type = (uint32_t)strtoul(barre + 1, NULL, 0); }
            uint32_t f = (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) ? (uint32_t)strtoul(t, NULL, 16)
                       : strlen(t) == 4 ? (uint32_t)(uint8_t)t[0] | (uint32_t)(uint8_t)t[1] << 8 | (uint32_t)(uint8_t)t[2] << 16 | (uint32_t)(uint8_t)t[3] << 24
                       : (uint32_t)strtoul(t, NULL, 0);
            refus[nrefus].fmt = f; refus[nrefus].type = type; nrefus++;
        }
    }
    uint32_t r = 0;
    for (int k = 0; k < nrefus; k++)
        if (refus[k].fmt == A(5) && (!refus[k].type || refus[k].type == A(4))) r = D3DERR_NOTAVAILABLE;
    static uint32_t vus[256][3]; static int nvus;
    int deja = 0;
    for (int k = 0; k < nvus; k++) if (vus[k][0] == A(3) && vus[k][1] == A(4) && vus[k][2] == A(5)) { deja = 1; break; }
    if (!deja && nvus < 256) {
        vus[nvus][0] = A(3); vus[nvus][1] = A(4); vus[nvus][2] = A(5); nvus++;
        char cc[5] = { 0 };
        for (int k = 0; k < 4; k++) { char ch = (char)(A(5) >> (8 * k)); cc[k] = ch >= 32 && ch < 127 ? ch : '.'; }
        sc2_trace("[D3D9] CheckDeviceFormat : usage 0x%X, ressource %u, format %u (0x%X « %s ») → %s", A(3), A(4), A(5), A(5), cc,
                r ? "REFUSÉ (SC2_FORMATS_REFUS)" : "accepté");
    }
    return r;
}

static uint32_t d_CreateDevice(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    uint32_t pp = A(4);
    for (int k = 0; k < 14; k++) D.pp[k] = rd32(pp + 4u * (uint32_t)k);
    if (!D.pp[0]) { D.pp[0] = ECRAN_W; wr32(pp + 0, ECRAN_W); }
    if (!D.pp[1]) { D.pp[1] = ECRAN_H; wr32(pp + 4, ECRAN_H); }
    if (!D.pp[2]) { D.pp[2] = FMT_X8R8G8B8; wr32(pp + 8, FMT_X8R8G8B8); }
    sc2_log("[D3D9] CreateDevice(adaptateur %u, type %u, hFocus 0x%X, comportement 0x%X) : back buffer "
            "%u×%u fmt %u ×%u, fenêtré %u, profondeur auto %u fmt %u, intervalle 0x%X → device factice",
            A(0), A(1), A(2), A(3), D.pp[0], D.pp[1], D.pp[2], D.pp[3], D.pp[8], D.pp[9], D.pp[10], D.pp[13]);
    g_device = obj_new(&K_DEV, 0);
    /* Passe 5d : états par défaut de D3D9 (le tableau était à zéro : masque d'écriture
     * couleur nul, facteurs de blend invalides…) — ce que rendent aussi les Get*. */
    D.rs[7] = 1; D.rs[14] = 1; D.rs[19] = 2; D.rs[20] = 1; D.rs[22] = 3; D.rs[23] = 4; D.rs[25] = 8;
    D.rs[168] = 0xF; D.rs[171] = 1; D.rs[207] = 2; D.rs[208] = 1; D.rs[209] = 1;
    for (int k = 0; k < 20; k++) { D.ss[k][1] = D.ss[k][2] = D.ss[k][3] = 1; D.ss[k][5] = D.ss[k][6] = 1; }
    D.backbuf = surf_new(D.pp[0], D.pp[1], D.pp[2], 1, 0);
    D.rt[0] = D.backbuf;
    if (D.pp[9]) D.ds = surf_new(D.pp[0], D.pp[1], D.pp[10], 2, 0);
    D.vp[0] = 0; D.vp[1] = 0; D.vp[2] = D.pp[0]; D.vp[3] = D.pp[1];
    { float z1 = 1.0f; memcpy(&D.vp[5], &z1, 4); }
    for (int m = 0; m < 512; m++) { float one = 1.f; uint32_t u; memcpy(&u, &one, 4);
        D.xf[m][0] = D.xf[m][5] = D.xf[m][10] = D.xf[m][15] = u; }
    wr32(A(5), g_device);
    g_backbuf_va = D.backbuf;
    d3dv_journal = sc2_log;
    if (d3dv_init()) {
        d3dv_rt(cible_de(D.backbuf));
        d3dv_ds(cible_de(D.ds));
        d3dv_viewport(0, 0, D.pp[0], D.pp[1], 0.f, 1.f);
        sc2_log("[VK] IDirect3DDevice9 adossé à Vulkan : back buffer %u×%u → VkImage (cible %d)",
                D.pp[0], D.pp[1], cible_de(D.backbuf));
    }
    return 0;
}
static com_meth M_D3D[] = { IUNK, { "RegisterSoftwareDevice", 1, m_ok, 0 },
    { "GetAdapterCount", 0, d_GetAdapterCount, 0 }, { "GetAdapterIdentifier", 3, d_GetAdapterIdentifier, 0 },
    { "GetAdapterModeCount", 2, d_GetAdapterModeCount, 0 }, { "EnumAdapterModes", 4, d_EnumAdapterModes, 0 },
    { "GetAdapterDisplayMode", 2, d_GetAdapterDisplayMode, 0 }, { "CheckDeviceType", 5, m_ok, 0 },
    { "CheckDeviceFormat", 6, d_CheckDeviceFormat, 0 }, { "CheckDeviceMultiSampleType", 6, d_CheckDeviceMultiSampleType, 0 },
    { "CheckDepthStencilMatch", 5, m_ok, 0 }, { "CheckDeviceFormatConversion", 4, m_ok, 0 },
    { "GetDeviceCaps", 3, d_GetDeviceCaps, 0 }, { "GetAdapterMonitor", 1, d_GetAdapterMonitor, 0 },
    { "CreateDevice", 6, d_CreateDevice, 0 } };

/* ---- IDirect3DDevice9 ------------------------------------------------------------ */
static uint32_t addref_out(uint32_t out, uint32_t o)
{ if (out) wr32(out, o); if (o) wr32(o + O_REF, rd32(o + O_REF) + 1); return 0; }

static uint32_t v_GetAvailableTextureMem(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 512u << 20; }
static uint32_t v_GetDirect3D(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; return addref_out(A(0), g_d3d); }
static uint32_t v_GetDeviceCaps(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; caps_ecrire(A(0)); return 0; }
static uint32_t v_GetDisplayMode(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; mode_ecrire(A(1)); return 0; }
static uint32_t v_GetCreationParameters(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; wr32(A(0), 0); wr32(A(0) + 4, 1); wr32(A(0) + 8, D.pp[7]); wr32(A(0) + 12, 0x40); return 0; }
static uint32_t v_GetNumberOfSwapChains(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 1; }
static uint32_t v_GetSwapChain(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; static uint32_t sw; if (!sw) sw = obj_new(&K_SWAP, 0); return addref_out(A(1), sw); }
static uint32_t v_Reset(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    for (int k = 0; k < 14; k++) D.pp[k] = rd32(A(0) + 4u * (uint32_t)k);
    /* Passe 5c : le back buffer prend la taille des nouveaux paramètres (sa cible Vulkan suit). */
    if (D.pp[0] && D.pp[1]) { wr32(D.backbuf + O_W, D.pp[0]); wr32(D.backbuf + O_H, D.pp[1]); }
    if (d3dv_actif()) {
        D.rt[0] = D.backbuf;
        d3dv_rt(cible_de(D.backbuf));
        d3dv_viewport(0, 0, rd32(D.backbuf + O_W), rd32(D.backbuf + O_H), 0.f, 1.f);
        sc2_log("[VK] Reset : back buffer %u×%u", rd32(D.backbuf + O_W), rd32(D.backbuf + O_H));
    }
    return 0;
}
static uint32_t v_GetBackBuffer(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; return addref_out(A(3), D.backbuf); }
static uint32_t v_GetRasterStatus(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; wr32(A(1), 0); wr32(A(1) + 4, 0); return 0; }
static uint32_t v_GetGammaRamp(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; for (uint32_t i = 0; i < 256; i++) for (uint32_t k = 0; k < 3; k++) wr16(A(1) + 512 * k + 2 * i, (uint16_t)(i * 257)); return 0; }

static uint32_t v_CreateTexture(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    uint32_t t = obj_new(&K_TEX, 3), lv = A(2);
    if (!lv) { uint32_t m = A(0) > A(1) ? A(0) : A(1); lv = 1; while (m > 1) { m >>= 1; lv++; } }
    wr32(t + O_W, A(0)); wr32(t + O_H, A(1)); wr32(t + O_LEVELS, lv); wr32(t + O_USAGE, A(3));
    wr32(t + O_FMT, A(4)); wr32(t + O_POOL, A(5));
    inventaire_tex(INV_CREE, t);
    wr32(A(6), t); return 0;
}
static uint32_t v_CreateVolumeTexture(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    uint32_t t = obj_new(&K_VOL, 4);
    wr32(t + O_W, A(0)); wr32(t + O_H, A(1)); wr32(t + O_DEPTH, A(2)); wr32(t + O_LEVELS, A(3) ? A(3) : 1);
    wr32(t + O_USAGE, A(4)); wr32(t + O_FMT, A(5)); wr32(t + O_POOL, A(6));
    inventaire_tex(INV_CREE, t);
    wr32(A(7), t); return 0;
}
static uint32_t v_CreateCubeTexture(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    uint32_t t = obj_new(&K_CUBE, 5), lv = A(1);
    if (!lv) { uint32_t m = A(0); lv = 1; while (m > 1) { m >>= 1; lv++; } }
    wr32(t + O_W, A(0)); wr32(t + O_H, A(0)); wr32(t + O_LEVELS, lv); wr32(t + O_USAGE, A(2));
    wr32(t + O_FMT, A(3)); wr32(t + O_POOL, A(4));
    inventaire_tex(INV_CREE, t);
    wr32(A(5), t); return 0;
}
static uint32_t v_CreateVertexBuffer(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    uint32_t b = obj_new(&K_VB, 6);           /* D3DRTYPE_VERTEXBUFFER */
    wr32(b + O_W, A(0)); wr32(b + O_USAGE, A(1)); wr32(b + O_H, A(2)); wr32(b + O_POOL, A(3));
    wr32(b + O_FMT, 100);                     /* D3DFMT_VERTEXDATA */
    wr32(A(4), b); return 0;
}
static uint32_t v_CreateIndexBuffer(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    uint32_t b = obj_new(&K_IB, 7);
    wr32(b + O_W, A(0)); wr32(b + O_USAGE, A(1)); wr32(b + O_FMT, A(2)); wr32(b + O_POOL, A(3));
    wr32(A(4), b); return 0;
}
static uint32_t v_CreateRenderTarget(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; wr32(A(6), surf_new(A(0), A(1), A(2), 1, 0)); return 0; }
static uint32_t v_CreateDepthStencilSurface(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; wr32(A(6), surf_new(A(0), A(1), A(2), 2, 0)); return 0; }
static uint32_t v_CreateOffscreenPlainSurface(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; wr32(A(4), surf_new(A(0), A(1), A(2), 0, A(3))); return 0; }

/* ---- passe 7m : copies de surfaces/textures en mémoire (UpdateSurface, UpdateTexture) ----------
 * Le menu de campagne remplit les textures DEFAULT de ses modèles (DXTn + mips) par
 * UpdateSurface depuis des surfaces SYSTEMMEM verrouillées (292 appels) : c'était un no-op,
 * donc 10 911 échantillonnages sur texture « sans données » (factice blanc). */
static uint64_t g_n_upd_surf, g_n_upd_tex, g_o_upd;
static uint32_t surf_donnees(uint32_t sf, uint32_t *pitch, uint32_t *w, uint32_t *h, uint32_t *fmt)
{   /* adresse invitée du niveau (et de la face) que désigne une surface ; 0 si refus */
    uint32_t p = rd32(sf + O_AUX), l = p ? rd32(sf + O_LEVEL) : 0, o = p ? p : sf;
    uint32_t niv = l & 0xFFFF, face = l >> 16;
    if (niv >= niveaux(o)) return 0;
    *fmt = rd32(o + O_FMT); *w = lvl(rd32(o + O_W), niv); *h = lvl(rd32(o + O_H), niv); *pitch = pitch_of(*fmt, *w);
    uint32_t chaine = niveau_offset(o, niveaux(o));
    return buf_of(o, chaine * faces_de(o)) + (face < faces_de(o) ? face : 0) * chaine + niveau_offset(o, niv);
}
static void gen_plus(uint32_t sf) { uint32_t p = rd32(sf + O_AUX), o = p ? p : sf; wr32(o + O_GEN, rd32(o + O_GEN) + 1); }
static uint32_t v_UpdateSurface(CPU *c, uint32_t s, const uint32_t *a)   /* (src, pSrcRect, dst, pDestPoint) */
{
    (void)c; (void)s;
    uint32_t sp, sw, sh, sfm, dp, dw, dh, dfm;
    uint32_t src = surf_donnees(A(0), &sp, &sw, &sh, &sfm), dst = surf_donnees(A(2), &dp, &dw, &dh, &dfm);
    if (!src || !dst || sfm != dfm) return 0x8876086Cu;                 /* D3DERR_INVALIDCALL */
    uint32_t x0 = 0, y0 = 0, x1 = sw, y1 = sh, dx = 0, dy = 0;
    if (A(1)) { x0 = rd32(A(1)); y0 = rd32(A(1) + 4); x1 = rd32(A(1) + 8); y1 = rd32(A(1) + 12); }
    if (A(3)) { dx = rd32(A(3)); dy = rd32(A(3) + 4); }
    if (x1 > sw) x1 = sw; if (y1 > sh) y1 = sh;
    if (x0 >= x1 || y0 >= y1) return 0;
    uint32_t lw = x1 - x0, lh = y1 - y0;
    if (dx + lw > dw) lw = dw > dx ? dw - dx : 0;
    if (dy + lh > dh) lh = dh > dy ? dh - dy : 0;
    int b = fmt_bloc(sfm);
    uint32_t octets = b ? (lw + 3) / 4 * (uint32_t)b : lw * fmt_bpp(sfm), lignes = b ? (lh + 3) / 4 : lh;
    for (uint32_t r = 0; r < lignes; r++) {
        uint32_t ys = b ? y0 / 4 + r : y0 + r, yd = b ? dy / 4 + r : dy + r;
        memmove(GPTR(dst + yd * dp + (b ? dx / 4 * (uint32_t)b : dx * fmt_bpp(sfm))),
                GPTR(src + ys * sp + (b ? x0 / 4 * (uint32_t)b : x0 * fmt_bpp(sfm))), octets);
    }
    gen_plus(A(2));
    { uint32_t p = rd32(A(2) + O_AUX); if (lum(p ? p : A(2))) { g_n_upd_lum++; noter_lock_lum(p ? p : A(2), 0, "UpdateSurface"); } }
    g_n_upd_surf++; g_o_upd += (uint64_t)octets * lignes;
    return 0;
}
static uint32_t v_UpdateTexture(CPU *c, uint32_t s, const uint32_t *a)   /* (src, dst) : niveaux communs */
{
    (void)c; (void)s;
    uint32_t t0 = A(0), t1 = A(1);
    if (!t0 || !t1 || rd32(t0 + O_FMT) != rd32(t1 + O_FMT) || rd32(t0 + O_KIND) != rd32(t1 + O_KIND)) return 0x8876086Cu;
    uint32_t d0 = niveaux(t0), d1 = niveaux(t1), sk = 0;
    while (sk < d0 && lvl(rd32(t0 + O_W), sk) > rd32(t1 + O_W)) sk++;                  /* source plus grande : niveaux sautés */
    uint32_t c0 = niveau_offset(t0, d0), c1 = niveau_offset(t1, d1), f = faces_de(t1);
    if (!rd32(t0 + O_BUF)) return 0;
    uint32_t b0 = buf_of(t0, c0 * f), b1 = buf_of(t1, c1 * f);
    for (uint32_t k = 0; k < f; k++)
        for (uint32_t l = 0; l < d1 && l + sk < d0; l++) {
            uint32_t n = niveau_taille(t1, l);
            if (niveau_taille(t0, l + sk) != n) break;
            memmove(GPTR(b1 + k * c1 + niveau_offset(t1, l)), GPTR(b0 + k * c0 + niveau_offset(t0, l + sk)), n);
            g_o_upd += n;
        }
    wr32(t1 + O_GEN, rd32(t1 + O_GEN) + 1);
    if (lum(t1)) { g_n_upd_lum++; noter_lock_lum(t1, 0, "UpdateTexture"); }
    g_n_upd_tex++;
    return 0;
}
/* Passe 5c : surface invitée → cible Vulkan. Les surfaces rendues par
 * GetSurfaceLevel sont NEUVES à chaque appel (5 406 en 600 frames) : clé =
 * (texture conteneur, taille du niveau) ; surface autonome : clé = sa VA. */
static int cible_de(uint32_t surf)
{
    if (!surf || !d3dv_actif()) return -1;
    uint32_t cont = rd32(surf + O_AUX), w = rd32(surf + O_W), h = rd32(surf + O_H);
    uint64_t cle = cont ? ((uint64_t)cont << 32) | ((uint64_t)(w & 0xFFFF) << 16) | (h & 0xFFFF) : surf;
    return d3dv_cible(cle, w, h, rd32(surf + O_FMT), (rd32(surf + O_USAGE) & 2u) != 0);
}
/* ---- passe 7w : cibles de rendu secondaires (RTT) ---------------------------------------------------------
 * SC2_RTT_LOG=1 : trace (300 premières) SetRenderTarget vers une cible ≠ backbuffer, StretchRect, ColorFill,
 * GetRenderTargetData ; bilan par genre à l'arrêt. StretchRect et ColorFill étaient des no-op : la copie d'une
 * surface de rendu vers une texture (ou une autre surface) n'arrivait jamais → texture jamais remplie. */
static uint64_t g_n_stretch, g_n_stretch_ok, g_n_colorfill, g_n_rtdata, g_n_setrt_sec;
static int rtt_log(void) { static int v = -1; if (v < 0) { const char *e = getenv("SC2_RTT_LOG"); v = e && *e == '1'; } return v; }
static void rtt_desc(char *o, size_t n, uint32_t sf)
{
    if (!sf) { snprintf(o, n, "NULL"); return; }
    uint32_t cont = rd32(sf + O_AUX);
    /* la cible Vulkan n'est consultée que pour une surface de rendu (cible_de en CRÉE une sinon) */
    int rtt = (rd32(sf + O_USAGE) & 3u) || sf == D.backbuf;
    snprintf(o, n, "surf 0x%08X %u×%u fmt %u usage 0x%X pool %u%s%s (cible %d)", sf, rd32(sf + O_W), rd32(sf + O_H),
             rd32(sf + O_FMT), rd32(sf + O_USAGE), rd32(sf + O_POOL), cont ? " niveau de texture" : "",
             sf == D.backbuf ? " BACKBUFFER" : "", rtt ? cible_de(sf) : -1);
    if (cont) { size_t l = strlen(o); snprintf(o + l, n - l, " [tex 0x%08X niv %u]", cont, rd32(sf + O_LEVEL)); }
}
static uint32_t v_StretchRect(CPU *c, uint32_t s, const uint32_t *a)   /* (src, pSrcRect, dst, pDstRect, filtre) */
{
    (void)c; (void)s;
    g_n_stretch++;
    int32_t rs[4], rd[4];
    if (A(1)) memcpy(rs, GPTR(A(1)), 16);
    if (A(3)) memcpy(rd, GPTR(A(3)), 16);
    int src = cible_de(A(0)), dst = cible_de(A(2));
    int ok = d3dv_copier(src, A(1) ? rs : NULL, dst, A(3) ? rd : NULL, A(4) == 2) == 0;
    if (ok) g_n_stretch_ok++;
    if (rtt_log() && g_n_stretch <= 300) {
        char ds[200], dd[200]; rtt_desc(ds, sizeof ds, A(0)); rtt_desc(dd, sizeof dd, A(2));
        sc2_log("[RTT] StretchRect n°%llu %s %s → %s %s filtre %u : %s", (unsigned long long)g_n_stretch, ds,
                A(1) ? "(rect)" : "", dd, A(3) ? "(rect)" : "", A(4), ok ? "COPIÉ" : "non copié");
    }
    return 0;
}
static uint32_t v_ColorFill(CPU *c, uint32_t s, const uint32_t *a)     /* (surface, pRect, couleur) */
{
    (void)c; (void)s;
    g_n_colorfill++;
    int id = cible_de(A(0)), r = -1;
    if (id >= 0) {
        int ancienne = d3dv_rt_courante(); int32_t rc[4];
        if (A(1)) memcpy(rc, GPTR(A(1)), 16);
        d3dv_rt(id); d3dv_clear(A(1) ? 1 : 0, A(1) ? rc : NULL, 1u, A(2), 1.f, 0); d3dv_rt(ancienne); r = 0;
    }
    if (rtt_log() && g_n_colorfill <= 300) { char d[200]; rtt_desc(d, sizeof d, A(0));
        sc2_log("[RTT] ColorFill %s couleur 0x%08X %s", d, A(2), r ? "ignoré" : "fait"); }
    return 0;
}
/* GetRenderTargetData : le jeu relit sur le CPU une petite cible (64×64 X8R8G8B8 en mission). C'était un no-op :
 * la surface SYSTEMMEM gardait ses octets d'avant (zéros). Relecture Vulkan (RGBA) → mémoire invitée (BGRA de
 * D3DFMT 21/22, pas de la surface). */
static uint64_t g_n_rtdata_ok;
static uint32_t v_GetRenderTargetData(CPU *c, uint32_t s, const uint32_t *a)   /* (cible, surface SYSTEMMEM) */
{
    (void)c; (void)s;
    g_n_rtdata++;
    uint32_t src = A(0), dst = A(1), w = src ? rd32(src + O_W) : 0, h = src ? rd32(src + O_H) : 0, f = dst ? rd32(dst + O_FMT) : 0;
    int ok = 0, id = src ? cible_de(src) : -1;
    if (id >= 0 && dst && rd32(dst + O_W) == w && rd32(dst + O_H) == h && (f == 21 || f == 22) && w * h <= 4096u * 4096u) {
        uint8_t *rgba = malloc((size_t)w * h * 4);
        if (rgba && d3dv_lire(id, rgba) == 0) {
            uint32_t pitch = pitch_of(f, w), b = buf_of(dst, niveau_offset(dst, 1));
            for (uint32_t y = 0; y < h; y++) {
                uint8_t *l = (uint8_t *)GPTR(b + y * pitch), *r = rgba + (size_t)y * w * 4;
                for (uint32_t x = 0; x < w; x++) { l[4 * x] = r[4 * x + 2]; l[4 * x + 1] = r[4 * x + 1]; l[4 * x + 2] = r[4 * x]; l[4 * x + 3] = r[4 * x + 3]; }
            }
            wr32(dst + O_GEN, rd32(dst + O_GEN) + 1);
            ok = 1; g_n_rtdata_ok++;
        }
        free(rgba);
    }
    if (rtt_log() && g_n_rtdata <= 300) { char d0[200], d1[200]; rtt_desc(d0, sizeof d0, src); rtt_desc(d1, sizeof d1, dst);
        sc2_log("[RTT] GetRenderTargetData %s → %s : %s", d0, d1, ok ? "RELU" : "non relu"); }
    return 0;
}
static uint32_t v_SetRenderTarget(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    if (A(1) && A(1) != D.backbuf) {
        g_n_setrt_sec++;
        if (rtt_log() && g_n_setrt_sec <= 300) { char d[200]; rtt_desc(d, sizeof d, A(1));
            sc2_log("[RTT] SetRenderTarget(%u) n°%llu → %s", A(0), (unsigned long long)g_n_setrt_sec, d); }
    }
    if (A(0) < 4) D.rt[A(0)] = A(1);
    if (A(0) == 0 && A(1)) {
        /* D3D9 : changer la RT0 remet le viewport à sa taille entière. */
        D.vp[0] = 0; D.vp[1] = 0; D.vp[2] = rd32(A(1) + O_W); D.vp[3] = rd32(A(1) + O_H);
        float z0 = 0.f, z1 = 1.f; memcpy(&D.vp[4], &z0, 4); memcpy(&D.vp[5], &z1, 4);
        if (d3dv_actif()) { d3dv_rt(cible_de(A(1))); d3dv_viewport(0, 0, D.vp[2], D.vp[3], 0.f, 1.f); }
    }
    return 0;
}
static uint32_t v_GetRenderTarget(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; if (A(0) >= 4 || !D.rt[A(0)]) { wr32(A(1), 0); return 0x8876086Cu; } return addref_out(A(1), D.rt[A(0)]); }
static uint32_t v_SetDepthStencilSurface(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; D.ds = A(0); if (d3dv_actif()) d3dv_ds(cible_de(A(0))); return 0; }
static uint32_t v_GetDepthStencilSurface(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; if (!D.ds) { wr32(A(0), 0); return 0x8876086Cu; } return addref_out(A(0), D.ds); }
static uint32_t v_SetTransform(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; if (A(0) < 512) memcpy(D.xf[A(0)], GPTR(A(1)), 64); return 0; }
static uint32_t v_GetTransform(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; if (A(0) < 512) memcpy(GPTR(A(1)), D.xf[A(0)], 64); return 0; }
static uint32_t v_SetViewport(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s; memcpy(D.vp, GPTR(A(0)), 24);
    if (d3dv_actif()) { float z0, z1; memcpy(&z0, &D.vp[4], 4); memcpy(&z1, &D.vp[5], 4); d3dv_viewport(D.vp[0], D.vp[1], D.vp[2], D.vp[3], z0, z1); }
    return 0;
}
/* Passe 5c : Clear / scène / draws → backend Vulkan. */
static uint32_t v_Clear(CPU *c, uint32_t s, const uint32_t *a)   /* (Count, pRects, Flags, Color, Z, Stencil) */
{
    (void)c; (void)s;
    if (!d3dv_actif()) return 0;
    int32_t r[4 * 64]; uint32_t n = A(1) ? (A(0) < 64 ? A(0) : 64) : 0;
    if (n) memcpy(r, GPTR(A(1)), 16u * n);
    float z; memcpy(&z, &a[4], 4);
    /* DIAGNOSTIC de chaîne (pas l'image du jeu) : SC2_VK_TEINTE=0xAARRGGBB remplace
     * la couleur des Clear visant le back buffer. Le jeu y efface en 0x00000000 :
     * une frame teintée prouve que SES Clear atteignent l'image Vulkan relue. */
    static int teinte_lue; static uint32_t teinte; static int teinte_on;
    if (!teinte_lue) { const char *e = getenv("SC2_VK_TEINTE"); teinte_lue = 1;
        if (e && *e) { teinte = (uint32_t)strtoul(e, NULL, 16); teinte_on = 1;
            sc2_log("[VK] DIAGNOSTIC SC2_VK_TEINTE=0x%08X : Clear du back buffer teintés (ce n'est PAS l'image du jeu)", teinte); } }
    uint32_t col = teinte_on && D.rt[0] == D.backbuf && (A(2) & 1u) ? teinte : A(3);
    d3dv_clear(n, n ? r : NULL, A(2), col, z, A(5));
    return 0;
}
static uint32_t v_BeginScene(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; d3dv_scene(1); return 0; }
static uint32_t v_EndScene(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; d3dv_scene(0); return 0; }
/* ---- passe 5d : draws réels ------------------------------------------------- */
static uint64_t g_n_reels, g_n_lumiere, g_n_sans_spv, g_n_sans_decl, g_n_sans_donnees;
/* Passe 5e : replis de texture (sampler lu par le shader → texture factice blanche), par cause. */
enum { R_AUCUNE, R_CUBE, R_SANS_DONNEES, R_FORMAT, R_NB };
static uint64_t g_replis[R_NB], g_samplers_servis;
static struct { uint32_t fmt, w, h, kind, usage, pool, cause; uint64_t n; } g_repli_tex[24];
static int g_nrepli_tex;
static void noter_repli(int cause, uint32_t t)
{
    g_replis[cause]++;
    if (!t) return;
    uint32_t f = rd32(t + O_FMT), w = rd32(t + O_W), h = rd32(t + O_H), kd = rd32(t + O_KIND);
    for (int i = 0; i < g_nrepli_tex; i++)
        if (g_repli_tex[i].fmt == f && g_repli_tex[i].w == w && g_repli_tex[i].h == h && g_repli_tex[i].kind == kd) { g_repli_tex[i].n++; return; }
    if (g_nrepli_tex < 24) g_repli_tex[g_nrepli_tex++] = (typeof(g_repli_tex[0])){ f, w, h, kd, rd32(t + O_USAGE), rd32(t + O_POOL), (uint32_t)cause, 1 };
}
/* Passe 7l : INVENTAIRE ([BILAN] [TEX]) — textures créées (par origine, genre, format), textures
 * échantillonnées par les draws réels, combinaisons d'états de sampler et d'états de rendu. */
static const char *nom_fmt(uint32_t f)
{
    static char b[8][16]; static int k; char *s = b[k++ & 7];
    switch (f) {
    case 20: return "R8G8B8"; case 21: return "A8R8G8B8"; case 22: return "X8R8G8B8"; case 23: return "R5G6B5";
    case 24: return "X1R5G5B5"; case 25: return "A1R5G5B5"; case 26: return "A4R4G4B4"; case 28: return "A8";
    case 32: return "A8B8G8R8"; case 50: return "L8"; case 51: return "A8L8"; case 75: return "D24S8"; case 77: return "D24X8";
    case 80: return "D16"; case 111: return "R16F"; case 112: return "G16R16F"; case 113: return "A16B16G16R16F";
    case 114: return "R32F"; case 115: return "G32R32F"; case 116: return "A32B32G32R32F";
    }
    if (f > 0x20202020u) { memcpy(s, &f, 4); s[4] = 0; return s; }
    snprintf(s, 16, "%u", f); return s;
}
static struct { uint32_t orig, genre, fmt, usage, pool, wmax, hmax, nivmax; uint64_t n, octets; } g_inv[64];
static int g_ninv;
static uint32_t niveau_offset(uint32_t o, uint32_t l);
static uint32_t niveaux(uint32_t o);
static void inventaire_tex(uint32_t orig, uint32_t t)
{
    uint32_t g = rd32(t + O_KIND), f = rd32(t + O_FMT), u = rd32(t + O_USAGE), p = rd32(t + O_POOL);
    uint32_t w = rd32(t + O_W), h = rd32(t + O_H), nv = rd32(t + O_LEVELS);
    int i = 0;
    while (i < g_ninv && !(g_inv[i].orig == orig && g_inv[i].genre == g && g_inv[i].fmt == f && g_inv[i].usage == u && g_inv[i].pool == p)) i++;
    if (i == g_ninv) { if (g_ninv == 64) return; g_ninv++; memset(&g_inv[i], 0, sizeof g_inv[i]);
                       g_inv[i].orig = orig; g_inv[i].genre = g; g_inv[i].fmt = f; g_inv[i].usage = u; g_inv[i].pool = p; }
    g_inv[i].n++; g_inv[i].octets += (uint64_t)niveau_offset(t, niveaux(t)) * (g == 5 ? 6 : 1);
    if (w > g_inv[i].wmax) g_inv[i].wmax = w;
    if (h > g_inv[i].hmax) g_inv[i].hmax = h;
    if (nv > g_inv[i].nivmax) g_inv[i].nivmax = nv;
}
static struct { uint32_t t, genre, fmt, w, h, niv; uint64_t n; } g_inv_lie[48];   /* textures échantillonnées */
static int g_ninv_lie;
static void inventaire_lie(uint32_t t)
{
    for (int i = 0; i < g_ninv_lie; i++) if (g_inv_lie[i].t == t) { g_inv_lie[i].n++; return; }
    if (g_ninv_lie < 48) g_inv_lie[g_ninv_lie++] = (typeof(g_inv_lie[0])){ t, rd32(t + O_KIND), rd32(t + O_FMT), rd32(t + O_W),
                                                                         rd32(t + O_H), rd32(t + O_LEVELS), 1 };
}
/* sampler : U V W, bordure, mag, min, mip, biais LOD, MAXMIPLEVEL, anisotropie, sRGB ;
 * rendu : test alpha (actif, fonction, référence), écriture sRGB, Z (actif, écriture), cull, scissor */
static struct { uint32_t k[11]; uint64_t n; } g_inv_ss[48];
static struct { uint32_t k[11]; uint64_t n; } g_inv_rs[48];
static int g_ninv_ss, g_ninv_rs;
static void inventaire_etats(int k)
{
    static const int idx[11] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    uint32_t v[11]; for (int i = 0; i < 11; i++) v[i] = D.ss[k][idx[i]];
    int i = 0; while (i < g_ninv_ss && memcmp(g_inv_ss[i].k, v, sizeof v)) i++;
    if (i < g_ninv_ss) g_inv_ss[i].n++;
    else if (g_ninv_ss < 48) { memcpy(g_inv_ss[i].k, v, sizeof v); g_inv_ss[i].n = 1; g_ninv_ss++; }
}
static void inventaire_rendu(void)
{
    uint32_t v[11] = { D.rs[15], D.rs[25], D.rs[24], D.rs[194], D.rs[7], D.rs[14], D.rs[22], D.rs[174],
                       D.rs[168], D.rs[27], D.rt[0] == D.backbuf };   /* 7m : masque couleur, blend, cible = back buffer */
    int i = 0; while (i < g_ninv_rs && memcmp(g_inv_rs[i].k, v, sizeof v)) i++;
    if (i < g_ninv_rs) g_inv_rs[i].n++;
    else if (g_ninv_rs < 48) { memcpy(g_inv_rs[i].k, v, sizeof v); g_inv_rs[i].n = 1; g_ninv_rs++; }
}
static void inventaire_bilan(void)
{
    static const char *orig[2] = { "Create*Texture", "D3DX DDS" }, *genre[6] = { "?", "?", "?", "2D", "volume", "cube" };
    if (!sc2_trace_actif()) {                                   /* passe 10n : l'inventaire est un vidage */
        printf("[BILAN] [TEX] %d sortes de textures créées, %d échantillonnées par les draws (inventaire : SC2_TRACE=1)\n",
               g_ninv, g_ninv_lie);
        return;
    }
    printf("[BILAN] [TEX] inventaire des textures créées (origine, genre, format, usage, pool : nombre, max L×H, max niveaux, Mo) :\n");
    for (int i = 0; i < g_ninv; i++)
        printf("[BILAN] [TEX]   %-14s %-6s %-14s usage 0x%-4X pool %u : ×%-4llu max %u×%u, %u niv., %.1f Mo\n", orig[g_inv[i].orig],
               g_inv[i].genre < 6 ? genre[g_inv[i].genre] : "?", nom_fmt(g_inv[i].fmt), g_inv[i].usage, g_inv[i].pool,
               (unsigned long long)g_inv[i].n, g_inv[i].wmax, g_inv[i].hmax, g_inv[i].nivmax, g_inv[i].octets / 1048576.0);
    printf("[BILAN] [TEX] textures échantillonnées par les draws réels (%d) :\n", g_ninv_lie);
    for (int i = 0; i < g_ninv_lie; i++)
        printf("[BILAN] [TEX]   0x%08X %-6s %-14s %u×%u, %u niv. : %llu échantillonnages\n", g_inv_lie[i].t,
               g_inv_lie[i].genre < 6 ? genre[g_inv_lie[i].genre] : "?", nom_fmt(g_inv_lie[i].fmt), g_inv_lie[i].w, g_inv_lie[i].h,
               g_inv_lie[i].niv, (unsigned long long)g_inv_lie[i].n);
    printf("[BILAN] [TEX] états de sampler (U V W bordure mag min mip biaisLOD maxmip aniso sRGB) :\n");
    for (int i = 0; i < g_ninv_ss; i++) {
        const uint32_t *k = g_inv_ss[i].k;
        printf("[BILAN] [TEX]   %u %u %u 0x%08X %u %u %u %g %u %u %u : ×%llu\n", k[0], k[1], k[2], k[3], k[4], k[5], k[6],
               (double)(union { uint32_t u; float f; }){ .u = k[7] }.f, k[8], k[9], k[10], (unsigned long long)g_inv_ss[i].n);
    }
    printf("[BILAN] [TEX] états de rendu des draws (alphatest fonction réf sRGBwrite zenable zwrite cull scissor masque blend backbuf) :\n");
    for (int i = 0; i < g_ninv_rs; i++) {
        const uint32_t *k = g_inv_rs[i].k;
        printf("[BILAN] [TEX]   %u %u %u %u %u %u %u %u 0x%X %u %u : ×%llu\n", k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
               k[8], k[9], k[10], (unsigned long long)g_inv_rs[i].n);
    }
}
static const sc2_spv_shader *spv_de(uint64_t h, int ps)
{
    for (int i = 0; i < SC2_NB_SPV; i++) if (SC2_SHADERS_SPV[i].hash == h && SC2_SHADERS_SPV[i].ps == ps) return &SC2_SHADERS_SPV[i];
    return NULL;
}
/* Passe 10t : entrée de SC2_SHADERS_SPV de chaque shader, résolue avec son module. dessin_reel la cherchait à chaque
 * dessin (deux parcours linéaires des 2 501 entrées : 4,1 → 0,2 ms/frame mesurés sur le Fold le 20/10) ; les emplacements de g_shaders ne sont
 * jamais réutilisés (dédoublonnés par hash à la création). */
static const sc2_spv_shader *g_spv[SC2_MAX_SHADERS];
static const sc2_spv_shader *spv_shader(int sid, int ps)
{
    return g_shaders[sid].ps == ps && g_shaders[sid].module != -1 ? g_spv[sid] : spv_de(g_shaders[sid].hash, ps);
}
static int module_de(int sid)                       /* -1 : pas de traduction */
{
    if (sid < 0) return -1;
    sc2_shader_info *si = &g_shaders[sid];
    if (si->module == -1) {
        const sc2_spv_shader *t = spv_de(si->hash, si->ps);
        g_spv[sid] = t;
        const uint32_t *spv = t ? t->spv : NULL; uint32_t nmots = t ? t->nmots : 0;
#ifdef SC2_SPV_OMBRES
        /* passe 7v3 : SC2_OMBRES=1 → variante « ombres » du PS (lecture Dref au binding 18 + k sous profondeur_k),
         * traduite par scripts/d3d9-vers-spirv.py --ombres ; sans la variable, la traduction d'origine */
        if (t && t->spv_ombre && d3dv_ombres()) {
            spv = t->spv_ombre; nmots = t->nmots_ombre;
            static int n_var = 0;
            if (n_var++ < 200) sc2_log("[OMBRES] PS %016llx : variante Dref (sampler2DShadow 18+k, profondeur_k) chargée",
                                        (unsigned long long)si->hash);
        }
#endif
        si->module = t ? d3dv_module(spv, nmots, si->ps) : -2;
        if (si->module < 0) si->module = -2;
    }
    return si->module >= 0 ? si->module : -1;
}
static uint32_t vk_decl(uint32_t t)                  /* D3DDECLTYPE → VkFormat */
{
    static const uint32_t m[17] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
        VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R16G16_SSCALED,
        VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT };
    return t < 17 ? m[t] : VK_FORMAT_UNDEFINED;
}
static uint32_t nb_elements(uint32_t type, uint32_t nprim)
{
    switch (type) { case 1: return nprim; case 2: return 2 * nprim; case 3: return nprim + 1; case 4: return 3 * nprim;
                    default: return nprim + 2; }
}
/* Passe 10s : blocs de constantes ASSEMBLÉS tenus à jour au fil des Set*ShaderConstant* (écriture directe du mot changé
 * à sa place : F en 4·i, I en 4096 + 4·i, B en 4352 + 16·i) au lieu d'être réassemblés à chaque dessin dont une constante
 * a changé — en jeu actif, ≈ 87 % des 1 400 dessins par frame, memset + memcpy de ≈ 9 Kio chacun (6,8 % du fil du jeu
 * sur le Fold). Assemblés en entier une seule fois, au premier dessin. */
static uint8_t g_cvs[D3DV_CONST_TAILLE], g_cps[D3DV_CONST_TAILLE];
static int g_blocs_prets;
static void bloc_consts(uint8_t *b, const uint32_t *f, uint32_t nf, const uint32_t *iv, const uint32_t *bv)
{
    memset(b, 0, D3DV_CONST_TAILLE);
    memcpy(b, f, 16u * nf);
    memcpy(b + 256 * 16, iv, 16 * 16);
    for (int k = 0; k < 16; k++) memcpy(b + 256 * 16 + 16 * 16 + 16 * k, &bv[k], 4);
}
/* Passe 7n (diagnostic) : SC2_DRAW_LOG=<préfixe hex du hash VS>[:N] → pour les N premiers draws (défaut 3)
 * de ce VS : appel, déclaration de sommets, flux, 2 premiers sommets bruts (en flottants), viewport,
 * cible, constantes VS non nulles parmi c0..c11 et c188..c203. */
static void journal_draw(int sv, uint32_t type, uint32_t nprim, int indexe, int32_t base, uint32_t minidx, uint32_t nverts,
                         uint32_t start, const d3dv_dessin *d)
{
    static int lu; static char pref[24]; static int nmax = 3, n;
    if (!lu) { lu = 1; const char *e = getenv("SC2_DRAW_LOG");
        if (e && *e) { snprintf(pref, sizeof pref, "%s", e); char *dp = strchr(pref, ':'); if (dp) { *dp = 0; nmax = atoi(dp + 1); } } }
    if (!pref[0] || n >= nmax) return;
    char h[24]; snprintf(h, sizeof h, "%016llx", (unsigned long long)g_shaders[sv].hash);
    if (strncmp(h, pref, strlen(pref))) return;
    n++;
    sc2_log("[DRAW] n°%d VS %s : type %u, %u prim, indexé %d, base %d, minidx %u, nverts %u, start %u ; viewport %u,%u %u×%u z %g..%g ; RT0 0x%08X",
            n, h, type, nprim, indexe, base, minidx, nverts, start, D.vp[0], D.vp[1], D.vp[2], D.vp[3],
            (double)(union { uint32_t u; float f; }){ .u = D.vp[4] }.f, (double)(union { uint32_t u; float f; }){ .u = D.vp[5] }.f, D.rt[0]);
    uint32_t el = rd32(D.decl + O_BUF), nel = rd32(D.decl + O_W);
    for (uint32_t k = 0; k < nel; k++)
        sc2_log("[DRAW]   élément %u : flux %u off %u type %u méthode %u usage %u index %u", k, rd16(el + 8 * k), rd16(el + 8 * k + 2),
                rd8(el + 8 * k + 4), rd8(el + 8 * k + 5), rd8(el + 8 * k + 6), rd8(el + 8 * k + 7));
    for (uint32_t i = 0; i < d->nattr; i++)
        sc2_log("[DRAW]   attribut loc %u ← flux %u off %u VkFormat %u", d->attr[i].loc, d->attr[i].flux, d->attr[i].offset, d->attr[i].vkfmt);
    for (uint32_t f = 0; f < d->nflux; f++) {
        char t[400]; size_t o = 0;
        const uint8_t *p = d->flux[f].donnees;
        for (uint32_t v = 0; v < 2 && p && d->flux[f].taille >= (v + 1) * d->flux[f].stride && d->flux[f].stride; v++)
            for (uint32_t w = 0; w < d->flux[f].stride / 4 && w < 12 && o < sizeof t - 24; w++) {
                float x; uint32_t u; memcpy(&u, p + v * d->flux[f].stride + 4 * w, 4); memcpy(&x, &u, 4);
                o += (size_t)snprintf(t + o, sizeof t - o, "%s%08X(%g)", w ? " " : v ? " | " : "", u, (double)x);
            }
        sc2_log("[DRAW]   flux %u : stride %u, %u o — %s", f, d->flux[f].stride, d->flux[f].taille, t);
    }
    if (d->index) {
        char t[200]; size_t o = 0;
        for (int i = 0; i < 9; i++) o += (size_t)snprintf(t + o, sizeof t - o, " %u", d->index32 ? ((const uint32_t *)d->index)[i] : ((const uint16_t *)d->index)[i]);
        sc2_log("[DRAW]   index%s (décalage sommet %d) :%s", d->index32 ? "32" : "16", d->decalage_sommet, t);
    }
    static const int cs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201 };
    for (size_t k = 0; k < sizeof cs / sizeof cs[0]; k++) {
        const float *c = (const float *)&D.vsf[4 * cs[k]];
        if (c[0] || c[1] || c[2] || c[3]) sc2_log("[DRAW]   c%d = %g %g %g %g", cs[k], (double)c[0], (double)c[1], (double)c[2], (double)c[3]);
    }
}
/* up : NULL, ou données UP (flux 0) ; up_i : index UP. Rend 0 si le draw réel a été émis. */
static int dessin_reel(uint32_t type, uint32_t nprim, int indexe, int32_t base, uint32_t minidx, uint32_t nverts,
                       uint32_t start, const uint8_t *up, uint32_t up_stride, const uint8_t *up_i, int up_i32)
{
    static int actifs = -1;                           /* SC2_DRAWS_REELS=0 : mode « lumière » de la passe 5c */
    if (actifs < 0) { const char *e = getenv("SC2_DRAWS_REELS"); actifs = !(e && *e == '0'); }
    if (!actifs) return -1;
    int sv = shader_id(D.vs), sp = shader_id(D.ps);
    int mv = module_de(sv), mp = module_de(sp);
    if (mv < 0 || mp < 0) { g_n_sans_spv++; return -1; }
    const sc2_spv_shader *tv = spv_shader(sv, 0), *tp = spv_shader(sp, 1);
    const int trace = sc2_trace_actif();              /* passe 10t : inventaires d'états, imprimés seulement sous SC2_TRACE */
    if (!D.decl || !rd32(D.decl + O_BUF)) { g_n_sans_decl++; return -1; }
    static d3dv_dessin d; memset(&d, 0, sizeof d);
    d.vs = mv; d.ps = mp; d.topo = type;
    uint32_t el = rd32(D.decl + O_BUF), nel = rd32(D.decl + O_W);
    uint32_t premier = indexe ? (uint32_t)(base + (int32_t)minidx) : start;
    uint32_t nsommets = indexe ? nverts : nb_elements(type, nprim);
    int flux_de[16]; for (int k = 0; k < 16; k++) flux_de[k] = -1;
    static const float zero_w1[4] = { 0, 0, 0, 1 };
    int flux_const = -1;
    for (int e = 0; e < tv->nentrees; e++) {
        const sc2_spv_entree *en = &tv->entrees[e];
        uint32_t trouve = nel;
        for (uint32_t k = 0; k < nel; k++)
            if (rd8(el + 8 * k + 6) == en->usage && rd8(el + 8 * k + 7) == en->index) { trouve = k; break; }
        uint32_t fl, fmt, off;
        if (trouve < nel && vk_decl(rd8(el + 8 * trouve + 4)) != VK_FORMAT_UNDEFINED) {
            uint32_t s = rd16(el + 8 * trouve);
            if (s >= 16) return -1;
            if (flux_de[s] < 0) {
                flux_de[s] = (int)d.nflux++;
                uint32_t stride = up ? up_stride : D.stream[s][2];
                const uint8_t *src;
                uint32_t dispo;
                uint32_t vb = 0, buf = 0, tvb = 0;   /* passe 10r : tampon invité du flux (0 : draw UP) */
                if (up) { if (s) return -1; src = up; dispo = ~0u; }
                else {
                    vb = D.stream[s][0]; buf = vb ? rd32(vb + O_BUF) : 0;
                    if (!buf) { g_n_sans_donnees++; return -1; }
                    src = (const uint8_t *)GPTR(buf + D.stream[s][1]);
                    tvb = rd32(vb + O_W);
                    dispo = tvb > D.stream[s][1] ? tvb - D.stream[s][1] : 0;
                }
                uint64_t debut = (uint64_t)premier * stride, n = (uint64_t)nsommets * stride;
                if (debut + n > dispo) { n = dispo > debut ? dispo - debut : 0; }
                d.flux[flux_de[s]].stride = stride;
                d.flux[flux_de[s]].donnees = src + debut;
                d.flux[flux_de[s]].taille = (uint32_t)n;
                if (vb) {   /* passe 10r : identité du tampon invité, pour le cache statique du backend */
                    d.flux[flux_de[s]].obj = vb;
                    d.flux[flux_de[s]].gen = rd32(vb + O_GEN);
                    d.flux[flux_de[s]].usage = rd32(vb + O_USAGE);
                    d.flux[flux_de[s]].taille_tot = tvb;
                    d.flux[flux_de[s]].donnees_tot = GPTR(buf);
                    d.flux[flux_de[s]].decalage = D.stream[s][1] + (uint32_t)debut;
                }
            }
            fl = (uint32_t)flux_de[s]; fmt = vk_decl(rd8(el + 8 * trouve + 4)); off = rd16(el + 8 * trouve + 2);
        } else {                                       /* élément absent : (0, 0, 0, 1) */
            if (flux_const < 0) { flux_const = (int)d.nflux++; d.flux[flux_const].stride = 0;
                                  d.flux[flux_const].donnees = zero_w1; d.flux[flux_const].taille = 16; }
            fl = (uint32_t)flux_const; fmt = VK_FORMAT_R32G32B32A32_SFLOAT; off = 0;
        }
        d.attr[d.nattr].loc = en->reg; d.attr[d.nattr].flux = fl; d.attr[d.nattr].vkfmt = fmt; d.attr[d.nattr].offset = off;
        d.nattr++;
    }
    if (indexe) {
        uint32_t ni = nb_elements(type, nprim);
        if (up_i) { d.index = up_i; d.index32 = up_i32; }
        else {
            uint32_t ib = D.indices, buf = ib ? rd32(ib + O_BUF) : 0;
            if (!buf) { g_n_sans_donnees++; return -1; }
            d.index32 = rd32(ib + O_FMT) == 102;
            d.index = GPTR(buf + start * (d.index32 ? 4u : 2u));
            /* passe 10r : identité du tampon d'indices invité, pour le cache statique du backend */
            d.index_obj = ib;
            d.index_gen = rd32(ib + O_GEN);
            d.index_usage = rd32(ib + O_USAGE);
            d.index_taille_tot = rd32(ib + O_W);
            d.index_donnees_tot = GPTR(buf);
            d.index_decalage = start * (d.index32 ? 4u : 2u);
        }
        d.index_taille = ni * (d.index32 ? 4u : 2u);
        d.nombre = ni;
        d.decalage_sommet = -(int32_t)minidx;
    } else d.nombre = nsommets;
    /* passe 10s : g_cvs/g_cps sont tenus à jour par les Set*ShaderConstant* (cf. bloc_consts) ; leur contenu est
     * toujours celui des générations courantes. SC2_VK_SANS_CACHE_CONSTS=1 : assemblage complet à chaque dessin,
     * comportement d'avant la passe 10r. */
    static int sans_cache_consts = -1;
    if (sans_cache_consts < 0) { const char *e = getenv("SC2_VK_SANS_CACHE_CONSTS"); sans_cache_consts = e && *e == '1'; }
    if (sans_cache_consts || !g_blocs_prets) {
        bloc_consts(g_cvs, D.vsf, 256, D.vsi, D.vsb); bloc_consts(g_cps, D.psf, 224, D.psi, D.psb); g_blocs_prets = 1;
    }
    d.cvs = g_cvs; d.cps = g_cps;
    d.gen_vs = D.gen_vs; d.gen_ps = D.gen_ps;
    for (int k = 0; k < 16; k++) {
        d.tex[k].genre_shader = tp->samplers[k];
        if (!tp->samplers[k]) continue;
        uint32_t t = D.tex[k];
        d.tex[k].adresse[0] = D.ss[k][1]; d.tex[k].adresse[1] = D.ss[k][2]; d.tex[k].adresse[2] = D.ss[k][3];
        d.tex[k].mag = D.ss[k][5]; d.tex[k].min = D.ss[k][6]; d.tex[k].mip = D.ss[k][7];
        d.tex[k].bordure = D.ss[k][4]; d.tex[k].biais = D.ss[k][8]; d.tex[k].maxmip = D.ss[k][9]; d.tex[k].aniso = D.ss[k][10];
        if (trace) inventaire_etats(k);
        if (!t) { noter_repli(R_AUCUNE, 0); continue; }
        inventaire_lie(t);
        uint32_t kind = rd32(t + O_KIND), w = rd32(t + O_W), h = rd32(t + O_H);
        if (kind == 3 && (rd32(t + O_USAGE) & 1u)) {         /* texture RENDERTARGET : sa cible Vulkan */
            d.tex[k].source = D3DV_TEX_CIBLE;
            d.tex[k].id = d3dv_cible(((uint64_t)t << 32) | ((uint64_t)(w & 0xFFFF) << 16) | (h & 0xFFFF), w, h, rd32(t + O_FMT), 0);
        } else if (kind == 3 && (rd32(t + O_USAGE) & 2u) && d3dv_ombres()) {
            /* passe 7s : texture DEPTHSTENCIL (carte d'ombres D16 2048²) → sa cible de profondeur Vulkan (même clé que
             * cible_de du niveau 0 rendu par GetSurfaceLevel) ; sans SC2_OMBRES=1 : repli « sans données » (factice) */
            d.tex[k].source = D3DV_TEX_CIBLE;
            d.tex[k].id = d3dv_cible(((uint64_t)t << 32) | ((uint64_t)(w & 0xFFFF) << 16) | (h & 0xFFFF), w, h, rd32(t + O_FMT), 1);
            /* passe 7v2 : (hash du PS, slot) liés à une profondeur, journalisés une fois — pour la recapture côté traducteur */
            { static struct { uint64_t h; int k; } vus[128]; static int nvus; int deja = 0;
              for (int q = 0; q < nvus; q++) if (vus[q].h == g_shaders[sp].hash && vus[q].k == k) { deja = 1; break; }
              if (!deja && nvus < 128) { vus[nvus].h = g_shaders[sp].hash; vus[nvus++].k = k;
                  sc2_log("[OMBRES] PS %016llx sampler s%d ← texture de profondeur %u×%u D3DFMT %u (drapeau profondeur_%d)",
                          (unsigned long long)g_shaders[sp].hash, k, w, h, rd32(t + O_FMT), k); } }
        } else if (kind == 3 || kind == 4) {
            uint32_t buf = rd32(t + O_BUF);
            d.tex[k].source = D3DV_TEX_TEXTURE;
            /* passe 7l : tous les niveaux (sauf AUTOGENMIPMAP 0x400 : seul le niveau 0 est rempli par le jeu) */
            uint32_t nv = rd32(t + O_USAGE) & 0x400u ? 1 : niveaux(t), dispo = rd32(t + O_SIZE), tot = niveau_offset(t, nv);
            d.tex[k].id = buf ? d3dv_texture_niveaux(t, kind == 4 ? 4 : 2, w, h, kind == 4 ? rd32(t + O_DEPTH) : 1, rd32(t + O_FMT),
                                                     nv, GPTR(buf), dispo < tot ? dispo : tot, rd32(t + O_GEN)) : -1;
            if (!buf) noter_repli(R_SANS_DONNEES, t);
            else if (d.tex[k].id < 0) noter_repli(R_FORMAT, t);
            else g_samplers_servis++;
            if (lum(t)) {                                     /* passe 7v3 : génération vue au draw (re-téléversement) */
                static uint32_t der_t, der_gen = ~0u;
                g_n_lie_lum++;
                if (t != der_t || rd32(t + O_GEN) != der_gen) { der_t = t; der_gen = rd32(t + O_GEN); g_n_gen_lum++; }
            }
        } else if (kind == 5) {                               /* passe 7l : cube réel (6 faces × chaîne) */
            uint32_t buf = rd32(t + O_BUF);
            d.tex[k].source = D3DV_TEX_TEXTURE;
            uint32_t nv = rd32(t + O_USAGE) & 0x400u ? 1 : niveaux(t), dispo = rd32(t + O_SIZE), tot = 6 * niveau_offset(t, niveaux(t));
            d.tex[k].id = buf ? d3dv_texture_niveaux(t, 3, w, w, 1, rd32(t + O_FMT), nv, GPTR(buf), dispo < tot ? dispo : tot,
                                                     rd32(t + O_GEN)) : -1;
            if (!buf) noter_repli(R_SANS_DONNEES, t);
            else if (d.tex[k].id < 0) noter_repli(R_CUBE, t);
            else g_samplers_servis++;
        } else noter_repli(R_FORMAT, t);
        if (d.tex[k].source == D3DV_TEX_CIBLE) g_samplers_servis++;
    }
    if (trace) inventaire_rendu();
    d.blend = D.rs[27]; d.src = D.rs[19]; d.dst = D.rs[20]; d.op = D.rs[171];
    if (D.rs[206]) { d.src_a = D.rs[207]; d.dst_a = D.rs[208]; d.op_a = D.rs[209]; }
    else { d.src_a = d.src; d.dst_a = d.dst; d.op_a = d.op; }
    d.masque_couleur = D.rs[168];
    d.zt = D.rs[7]; d.zw = D.rs[14]; d.zf = D.rs[23]; d.cull = D.rs[22];     /* passe 7n : profondeur et culling */
    d.zbiais = D.rs[195]; d.zpente = D.rs[175];                               /* passe 7v3 : biais de profondeur */
    float vw = D.vp[2] ? (float)D.vp[2] : 1.f, vh = D.vp[3] ? (float)D.vp[3] : 1.f;
    d.hp[0] = 1.f / vw; d.hp[1] = 1.f / vh;
    d.at[0] = D.rs[15] ? 1.f : 0.f; d.at[1] = (float)D.rs[25]; d.at[2] = (float)(D.rs[24] & 0xFF) / 255.f;
    journal_draw(sv, type, nprim, indexe, base, minidx, nverts, start, &d);
    if (d3dv_dessiner(&d)) return -1;
    g_n_reels++;
    return 0;
}

/* Passe 10k (diagnostic) : SC2_UI2D_LOG=<k> → toutes les k frames, les draws de la fenêtre écoulée, réels ou
 * « lumière », groupés par (VS, cible = back buffer, blend), les 8 plus nombreux. Tranche « l'UI 2D est-elle
 * émise au menu ? » : une superposition 2D = cible back buffer + blend actif. Sans la variable : un test d'entier. */
static int g_ui2d_k = -1;
static struct { uint64_t vs; uint32_t bb, blend, n; } g_ui2d[64];
static int g_ui2d_n;
static uint32_t g_ui2d_tot, g_ui2d_hors;
static void ui2d_compter(int v)
{
    if (g_ui2d_k < 0) { const char *e = getenv("SC2_UI2D_LOG"); g_ui2d_k = e ? atoi(e) : 0; }
    if (!g_ui2d_k) return;
    uint64_t h = v >= 0 ? g_shaders[v].hash : 0; uint32_t bb = D.rt[0] == D.backbuf, bl = D.rs[27] != 0;
    g_ui2d_tot++;
    int i = 0; while (i < g_ui2d_n && !(g_ui2d[i].vs == h && g_ui2d[i].bb == bb && g_ui2d[i].blend == bl)) i++;
    if (i == g_ui2d_n) { if (g_ui2d_n == 64) { g_ui2d_hors++; return; } g_ui2d[g_ui2d_n++] = (typeof(g_ui2d[0])){ h, bb, bl, 0 }; }
    g_ui2d[i].n++;
}
static void ui2d_fenetre(uint32_t n)
{
    if (g_ui2d_k <= 0 || n % (uint32_t)g_ui2d_k) return;
    uint32_t sup = 0, bbt = 0;
    for (int i = 0; i < g_ui2d_n; i++) { if (g_ui2d[i].bb) bbt += g_ui2d[i].n; if (g_ui2d[i].bb && g_ui2d[i].blend) sup += g_ui2d[i].n; }
    char t[700]; size_t o = 0;
    for (int r = 0; r < 8; r++) {
        int m = -1; for (int i = 0; i < g_ui2d_n; i++) if (g_ui2d[i].n && (m < 0 || g_ui2d[i].n > g_ui2d[m].n)) m = i;
        if (m < 0) break;
        o += (size_t)snprintf(t + o, sizeof t - o, " %016llx%s%s×%u", (unsigned long long)g_ui2d[m].vs,
                              g_ui2d[m].bb ? "/BB" : "", g_ui2d[m].blend ? "/blend" : "", g_ui2d[m].n);
        g_ui2d[m].n = 0;
    }
    sc2_log("[UI2D] frames %u-%u : %u draws, %u vers le back buffer, dont %u en superposition 2D (blend) ;%s%s", n - (uint32_t)g_ui2d_k + 1, n,
            g_ui2d_tot, bbt, sup, o ? t : " aucun", g_ui2d_hors ? " (+ groupes hors table)" : "");
    g_ui2d_n = 0; g_ui2d_tot = g_ui2d_hors = 0;
}
static void compter_draw(void)
{ int v = shader_id(D.vs), p = shader_id(D.ps); if (v >= 0) g_shaders[v].n_draw++; if (p >= 0) g_shaders[p].n_draw++; ui2d_compter(v); }
static void lumiere(uint32_t t, uint32_t n) { g_n_lumiere++; d3dv_draw(t, n); }
static uint32_t v_DrawPrimitive(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; compter_draw(); if (!d3dv_actif() || dessin_reel(A(0), A(2), 0, 0, 0, 0, A(1), NULL, 0, NULL, 0)) lumiere(A(0), A(2)); return 0; }
/* (Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimCount) */
static uint32_t v_DrawIndexedPrimitive(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; compter_draw(); if (!d3dv_actif() || dessin_reel(A(0), A(5), 1, (int32_t)A(1), A(2), A(3), A(4), NULL, 0, NULL, 0)) lumiere(A(0), A(5)); return 0; }
/* (Type, PrimCount, pVertexStreamZeroData, Stride) */
static uint32_t v_DrawPrimitiveUP(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; compter_draw(); if (!d3dv_actif() || dessin_reel(A(0), A(1), 0, 0, 0, 0, 0, GPTR(A(2)), A(3), NULL, 0)) lumiere(A(0), A(1)); return 0; }
/* (Type, MinVertexIndex, NumVertices, PrimCount, pIndexData, IndexFormat, pVertexData, Stride) */
static uint32_t v_DrawIndexedPrimitiveUP(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; compter_draw();
  if (!d3dv_actif() || dessin_reel(A(0), A(3), 1, 0, A(1), A(2), 0, GPTR(A(6)), A(7), GPTR(A(4)), A(5) == 102)) lumiere(A(0), A(3)); return 0; }
static uint32_t v_GetViewport(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; memcpy(GPTR(A(0)), D.vp, 24); return 0; }
static uint32_t v_SetMaterial(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; memcpy(D.material, GPTR(A(0)), 68); return 0; }
static uint32_t v_GetMaterial(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; memcpy(GPTR(A(0)), D.material, 68); return 0; }
static uint32_t v_LightEnable(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; if (A(0) < 8) D.light_on[A(0)] = A(1); return 0; }
static uint32_t v_SetClipPlane(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; if (A(0) < 6) memcpy(D.clip[A(0)], GPTR(A(1)), 16); return 0; }
static uint32_t v_GetClipPlane(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; if (A(0) < 6) memcpy(GPTR(A(1)), D.clip[A(0)], 16); return 0; }
static uint32_t v_SetRenderState(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; if (A(0) < 256) D.rs[A(0)] = A(1); return 0; }
static uint32_t v_GetRenderState(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; wr32(A(1), A(0) < 256 ? D.rs[A(0)] : 0); return 0; }
static uint32_t v_CreateStateBlock(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; wr32(A(1), obj_new(&K_SB, 0)); return 0; }
static uint32_t v_EndStateBlock(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; wr32(A(0), obj_new(&K_SB, 0)); return 0; }
static uint32_t stage(uint32_t st) { return st >= 0x100000u ? 16 + (st - 0x100000u) : st; }   /* D3DVERTEXTEXTURESAMPLER0 */
static uint32_t v_GetTexture(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; uint32_t k = stage(A(0)); return addref_out(A(1), k < 20 ? D.tex[k] : 0); }
static uint32_t v_SetTexture(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; uint32_t k = stage(A(0)); if (k < 20) D.tex[k] = A(1); return 0; }
static uint32_t v_GetTextureStageState(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; wr32(A(2), A(0) < 16 && A(1) < 33 ? D.tss[A(0)][A(1)] : 0); return 0; }
static uint32_t v_SetTextureStageState(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; if (A(0) < 16 && A(1) < 33) D.tss[A(0)][A(1)] = A(2); return 0; }
static uint32_t v_GetSamplerState(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; uint32_t k = stage(A(0)); wr32(A(2), k < 20 && A(1) < 14 ? D.ss[k][A(1)] : 0); return 0; }
static uint32_t v_SetSamplerState(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; uint32_t k = stage(A(0)); if (k < 20 && A(1) < 14) D.ss[k][A(1)] = A(2); return 0; }
static uint32_t v_ValidateDevice(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; if (A(0)) wr32(A(0), 1); return 0; }
static uint32_t v_SetScissorRect(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; memcpy(D.scissor, GPTR(A(0)), 16); return 0; }
static uint32_t v_GetScissorRect(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; memcpy(GPTR(A(0)), D.scissor, 16); return 0; }
/* Passe 5d : la déclaration est COPIÉE (D3DVERTEXELEMENT9, 8 octets, fin Stream=0xFF). */
static uint32_t v_CreateVertexDeclaration(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    uint32_t n = 0;
    while (n < 64 && rd16(A(0) + 8 * n) != 0xFF) n++;
    uint32_t d = obj_new(&K_DECL, 0), cp = sc2_alloc(8 * (n + 1));
    memcpy(GPTR(cp), GPTR(A(0)), 8 * (n + 1));
    wr32(d + O_BUF, cp); wr32(d + O_W, n);
    wr32(A(1), d); return 0;
}
static uint32_t v_SetVertexDeclaration(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; D.decl = A(0); return 0; }
static uint32_t v_GetVertexDeclaration(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; return addref_out(A(0), D.decl); }
static uint32_t v_SetFVF(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; D.fvf = A(0); return 0; }
static uint32_t v_GetFVF(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; wr32(A(0), D.fvf); return 0; }
/* Passe 5d : shaders RÉELS. Le bytecode D3D9 (SM2/3) est mesuré jeton par jeton,
 * haché (FNV-1a 64), copié dans l'arène et enregistré dans g_shaders : c'est la
 * clé de la table des traductions SPIR-V embarquées (sc2_shaders_spv.h).
 * SC2_SHADERS_DUMP=<dossier> : écrit chaque bytecode une fois (vs_/ps_<hash>.bin). */
static uint32_t shader_longueur(uint32_t p)          /* octets, fin comprise ; 0 si invalide */
{
    uint32_t v = rd32(p), i = 1;
    if ((v & 0xFFFE0000u) != 0xFFFE0000u) return 0;
    while (i < 65536) {
        uint32_t t = rd32(p + 4 * i);
        if (t == 0x0000FFFFu) return 4 * (i + 1);
        if ((t & 0xFFFFu) == 0xFFFEu) i += 1 + ((t >> 16) & 0x7FFFu);
        else i += 1 + ((t >> 24) & 0xFu);
    }
    return 0;
}
static uint64_t fnv1a64(const uint8_t *p, uint32_t n)
{ uint64_t h = 0xCBF29CE484222325ull; for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; } return h; }
sc2_shader_info g_shaders[SC2_MAX_SHADERS];
int g_nshaders;
static uint32_t shader_creer(com_class *k, uint32_t code, uint32_t out, int ps)
{
    uint32_t o = obj_new(k, 0), n = shader_longueur(code);
    uint64_t h = n ? fnv1a64((const uint8_t *)GPTR(code), n) : 0;
    int id = -1;
    for (int i = 0; i < g_nshaders; i++) if (g_shaders[i].hash == h && g_shaders[i].ps == ps) { id = i; break; }
    if (id < 0 && g_nshaders < SC2_MAX_SHADERS) {
        id = g_nshaders++;
        sc2_shader_info *si = &g_shaders[id];
        si->hash = h; si->ps = ps; si->ver = n ? rd32(code) : 0; si->taille = n;
        si->code = n ? sc2_alloc(n) : 0;
        if (n) memcpy(GPTR(si->code), GPTR(code), n);
        si->module = -1;
        const char *dir = getenv("SC2_SHADERS_DUMP");
        if (dir && *dir && n) {
            char ch[600]; mkdir(dir, 0755);
            snprintf(ch, sizeof ch, "%s/%s_%016llx.bin", dir, ps ? "ps" : "vs", (unsigned long long)h);
            FILE *f = fopen(ch, "wb"); if (f) { fwrite(GPTR(code), 1, n, f); fclose(f); }
        }
    }
    wr32(o + O_AUX, (uint32_t)(id + 1));
    wr32(out, o);
    return 0;
}
static uint32_t v_CreateVertexShader(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; return shader_creer(&K_VS, A(0), A(1), 0); }
static uint32_t v_SetVertexShader(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; D.vs = A(0); int i = shader_id(A(0)); if (i >= 0) g_shaders[i].n_set++; return 0; }
static uint32_t v_GetVertexShader(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; return addref_out(A(0), D.vs); }
static uint32_t v_CreatePixelShader(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; return shader_creer(&K_PS, A(0), A(1), 1); }
static uint32_t v_SetPixelShader(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; D.ps = A(0); int i = shader_id(A(0)); if (i >= 0) g_shaders[i].n_set++; return 0; }
static uint32_t v_GetPixelShader(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; return addref_out(A(0), D.ps); }
/* constantes de shaders : (StartRegister, pData, Count) ; F/I : 4 mots par registre, B : 1 */
/* Passe 7n (diagnostic) : SC2_CONST_LOG=<registre>[:N] → les N premiers SetVertexShaderConstantF qui
 * écrivent ce registre : appelant, plage, valeurs du registre. */
static void journal_vsc(CPU *c, const uint32_t *a)
{
    static int lu, reg = -1, nmax = 6, n;
    if (!lu) { lu = 1; const char *e = getenv("SC2_CONST_LOG"); if (e && *e) { reg = atoi(e); const char *dp = strchr(e, ':'); if (dp) nmax = atoi(dp + 1); } }
    if (reg < 0 || n >= nmax || (uint32_t)reg < A(0) || (uint32_t)reg >= A(0) + A(2)) return;
    n++;
    uint32_t p = A(1) + 16u * ((uint32_t)reg - A(0));
    float v[4]; for (int k = 0; k < 4; k++) { uint32_t u = rd32(p + 4u * (uint32_t)k); memcpy(&v[k], &u, 4); }
    sc2_log("[CONST] n°%d SetVertexShaderConstantF(c%u, %u registres) ← 0x%08X (source 0x%08X) : c%d = %g %g %g %g", n, A(0), A(2),
            rd32(c->esp), A(1), reg, (double)v[0], (double)v[1], (double)v[2], (double)v[3]);
}
/* passe 10r : le Set n'avance la génération du bloc (VS ou PS) que s'il change AU MOINS un mot —
 * réécrire les mêmes valeurs laisse le contenu (donc l'empreinte du bloc assemblé) inchangé. */
/* passe 10s : le mot changé est aussi écrit à sa place dans le bloc assemblé (bloc, base, pas : cf. bloc_consts) */
#define CONSTS(nom, tab, mots, cap, gen, bloc, base, pas) \
    static uint32_t v_Set##nom(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; \
        if ((void *)D.tab == (void *)D.vsf) journal_vsc(c, a); \
        int change = 0; \
        for (uint32_t i = 0; i < A(2) * (mots); i++) if (A(0) * (mots) + i < (cap)) { \
            uint32_t v = rd32(A(1) + 4 * i); \
            if (D.tab[A(0) * (mots) + i] != v) { D.tab[A(0) * (mots) + i] = v; change = 1; \
                memcpy(bloc + (base) + (pas) * (A(0) * (mots) + i), &v, 4); } } \
        if (change) D.gen++; return 0; } \
    static uint32_t v_Get##nom(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; \
        for (uint32_t i = 0; i < A(2) * (mots); i++) wr32(A(1) + 4 * i, A(0) * (mots) + i < (cap) ? D.tab[A(0) * (mots) + i] : 0); return 0; }
CONSTS(VertexShaderConstantF, vsf, 4, 1024, gen_vs, g_cvs, 0u, 4u) CONSTS(VertexShaderConstantI, vsi, 4, 64, gen_vs, g_cvs, 4096u, 4u)
CONSTS(VertexShaderConstantB, vsb, 1, 16, gen_vs, g_cvs, 4352u, 16u)
CONSTS(PixelShaderConstantF, psf, 4, 896, gen_ps, g_cps, 0u, 4u) CONSTS(PixelShaderConstantI, psi, 4, 64, gen_ps, g_cps, 4096u, 4u)
CONSTS(PixelShaderConstantB, psb, 1, 16, gen_ps, g_cps, 4352u, 16u)
static uint32_t v_SetStreamSource(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; if (A(0) < 16) { D.stream[A(0)][0] = A(1); D.stream[A(0)][1] = A(2); D.stream[A(0)][2] = A(3); } return 0; }
static uint32_t v_GetStreamSource(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s; uint32_t k = A(0) < 16 ? A(0) : 0;
    addref_out(A(1), D.stream[k][0]); if (A(2)) wr32(A(2), D.stream[k][1]); if (A(3)) wr32(A(3), D.stream[k][2]);
    return 0;
}
static uint32_t v_SetStreamSourceFreq(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; if (A(0) < 16) D.freq[A(0)] = A(1); return 0; }
static uint32_t v_GetStreamSourceFreq(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; wr32(A(1), A(0) < 16 ? D.freq[A(0)] : 1); return 0; }
static uint32_t v_SetIndices(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; D.indices = A(0); return 0; }
static uint32_t v_GetIndices(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; return addref_out(A(0), D.indices); }
static uint32_t v_CreateQuery(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    if (!A(1)) return 0;                      /* ppQuery NULL : test de support → D3D_OK */
    uint32_t q = obj_new(&K_QUERY, 0); wr32(q + O_FMT, A(0)); wr32(A(1), q); return 0;
}

static uint32_t v_CreateAdditionalSwapChain(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; wr32(A(1), obj_new(&K_SWAP, 0)); return 0; }
static uint32_t v_MultiplyTransform(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s; if (A(0) >= 512) return 0;
    float m[16], r[16], n[16]; memcpy(m, D.xf[A(0)], 64); memcpy(n, GPTR(A(1)), 64);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float t = 0; for (int k = 0; k < 4; k++) t += m[i * 4 + k] * n[k * 4 + j]; r[i * 4 + j] = t; }
    memcpy(D.xf[A(0)], r, 64); return 0;
}
static uint32_t v_GetLight(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; memset(GPTR(A(1)), 0, 104); return 0; }
static uint32_t v_GetLightEnable(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; wr32(A(1), A(0) < 8 ? D.light_on[A(0)] : 0); return 0; }
static uint32_t v_GetClipStatus(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; memset(GPTR(A(0)), 0, 24); return 0; }
static uint32_t v_GetPaletteEntries(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; memset(GPTR(A(1)), 0, 1024); return 0; }
static uint32_t v_GetCurrentTexturePalette(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; wr32(A(0), 0); return 0; }
static com_meth M_DEV[] = { IUNK,
    { "TestCooperativeLevel", 0, m_ok, 0 }, { "GetAvailableTextureMem", 0, v_GetAvailableTextureMem, 0 },
    { "EvictManagedResources", 0, m_ok, 0 }, { "GetDirect3D", 1, v_GetDirect3D, 0 },
    { "GetDeviceCaps", 1, v_GetDeviceCaps, 0 }, { "GetDisplayMode", 2, v_GetDisplayMode, 0 },
    { "GetCreationParameters", 1, v_GetCreationParameters, 0 }, { "SetCursorProperties", 3, m_ok, 0 },
    { "SetCursorPosition", 3, m_ok, 0 }, { "ShowCursor", 1, m_zero, 0 },
    { "CreateAdditionalSwapChain", 2, v_CreateAdditionalSwapChain, 0 }, { "GetSwapChain", 2, v_GetSwapChain, 0 },
    { "GetNumberOfSwapChains", 0, v_GetNumberOfSwapChains, 0 }, { "Reset", 1, v_Reset, 0 },
    { "Present", 4, m_Present, 0 }, { "GetBackBuffer", 4, v_GetBackBuffer, 0 },
    { "GetRasterStatus", 2, v_GetRasterStatus, 0 }, { "SetDialogBoxMode", 1, m_ok, 0 },
    { "SetGammaRamp", 3, m_ok, 0 }, { "GetGammaRamp", 2, v_GetGammaRamp, 0 },
    { "CreateTexture", 8, v_CreateTexture, 0 }, { "CreateVolumeTexture", 9, v_CreateVolumeTexture, 0 },
    { "CreateCubeTexture", 7, v_CreateCubeTexture, 0 }, { "CreateVertexBuffer", 6, v_CreateVertexBuffer, 0 },
    { "CreateIndexBuffer", 6, v_CreateIndexBuffer, 0 }, { "CreateRenderTarget", 8, v_CreateRenderTarget, 0 },
    { "CreateDepthStencilSurface", 8, v_CreateDepthStencilSurface, 0 }, { "UpdateSurface", 4, v_UpdateSurface, 0 },
    { "UpdateTexture", 2, v_UpdateTexture, 0 }, { "GetRenderTargetData", 2, v_GetRenderTargetData, 0 },
    { "GetFrontBufferData", 2, m_ok, 0 }, { "StretchRect", 5, v_StretchRect, 0 }, { "ColorFill", 3, v_ColorFill, 0 },
    { "CreateOffscreenPlainSurface", 6, v_CreateOffscreenPlainSurface, 0 },
    { "SetRenderTarget", 2, v_SetRenderTarget, 0 }, { "GetRenderTarget", 2, v_GetRenderTarget, 0 },
    { "SetDepthStencilSurface", 1, v_SetDepthStencilSurface, 0 },
    { "GetDepthStencilSurface", 1, v_GetDepthStencilSurface, 0 },
    { "BeginScene", 0, v_BeginScene, 0 }, { "EndScene", 0, v_EndScene, 0 }, { "Clear", 6, v_Clear, 0 },
    { "SetTransform", 2, v_SetTransform, 0 }, { "GetTransform", 2, v_GetTransform, 0 },
    { "MultiplyTransform", 2, v_MultiplyTransform, 0 }, { "SetViewport", 1, v_SetViewport, 0 },
    { "GetViewport", 1, v_GetViewport, 0 }, { "SetMaterial", 1, v_SetMaterial, 0 },
    { "GetMaterial", 1, v_GetMaterial, 0 }, { "SetLight", 2, m_ok, 0 }, { "GetLight", 2, v_GetLight, 0 },
    { "LightEnable", 2, v_LightEnable, 0 }, { "GetLightEnable", 2, v_GetLightEnable, 0 },
    { "SetClipPlane", 2, v_SetClipPlane, 0 }, { "GetClipPlane", 2, v_GetClipPlane, 0 },
    { "SetRenderState", 2, v_SetRenderState, 0 }, { "GetRenderState", 2, v_GetRenderState, 0 },
    { "CreateStateBlock", 2, v_CreateStateBlock, 0 }, { "BeginStateBlock", 0, m_ok, 0 },
    { "EndStateBlock", 1, v_EndStateBlock, 0 }, { "SetClipStatus", 1, m_ok, 0 },
    { "GetClipStatus", 1, v_GetClipStatus, 0 }, { "GetTexture", 2, v_GetTexture, 0 },
    { "SetTexture", 2, v_SetTexture, 0 }, { "GetTextureStageState", 3, v_GetTextureStageState, 0 },
    { "SetTextureStageState", 3, v_SetTextureStageState, 0 }, { "GetSamplerState", 3, v_GetSamplerState, 0 },
    { "SetSamplerState", 3, v_SetSamplerState, 0 }, { "ValidateDevice", 1, v_ValidateDevice, 0 },
    { "SetPaletteEntries", 2, m_ok, 0 }, { "GetPaletteEntries", 2, v_GetPaletteEntries, 0 },
    { "SetCurrentTexturePalette", 1, m_ok, 0 }, { "GetCurrentTexturePalette", 1, v_GetCurrentTexturePalette, 0 },
    { "SetScissorRect", 1, v_SetScissorRect, 0 }, { "GetScissorRect", 1, v_GetScissorRect, 0 },
    { "SetSoftwareVertexProcessing", 1, m_ok, 0 }, { "GetSoftwareVertexProcessing", 0, m_zero, 0 },
    { "SetNPatchMode", 1, m_ok, 0 }, { "GetNPatchMode", 0, m_zero, 0 },
    { "DrawPrimitive", 3, v_DrawPrimitive, 0 }, { "DrawIndexedPrimitive", 6, v_DrawIndexedPrimitive, 0 },
    { "DrawPrimitiveUP", 4, v_DrawPrimitiveUP, 0 }, { "DrawIndexedPrimitiveUP", 8, v_DrawIndexedPrimitiveUP, 0 },
    { "ProcessVertices", 6, m_ok, 0 }, { "CreateVertexDeclaration", 2, v_CreateVertexDeclaration, 0 },
    { "SetVertexDeclaration", 1, v_SetVertexDeclaration, 0 }, { "GetVertexDeclaration", 1, v_GetVertexDeclaration, 0 },
    { "SetFVF", 1, v_SetFVF, 0 }, { "GetFVF", 1, v_GetFVF, 0 },
    { "CreateVertexShader", 2, v_CreateVertexShader, 0 }, { "SetVertexShader", 1, v_SetVertexShader, 0 },
    { "GetVertexShader", 1, v_GetVertexShader, 0 },
    { "SetVertexShaderConstantF", 3, v_SetVertexShaderConstantF, 0 }, { "GetVertexShaderConstantF", 3, v_GetVertexShaderConstantF, 0 },
    { "SetVertexShaderConstantI", 3, v_SetVertexShaderConstantI, 0 }, { "GetVertexShaderConstantI", 3, v_GetVertexShaderConstantI, 0 },
    { "SetVertexShaderConstantB", 3, v_SetVertexShaderConstantB, 0 }, { "GetVertexShaderConstantB", 3, v_GetVertexShaderConstantB, 0 },
    { "SetStreamSource", 4, v_SetStreamSource, 0 }, { "GetStreamSource", 4, v_GetStreamSource, 0 },
    { "SetStreamSourceFreq", 2, v_SetStreamSourceFreq, 0 }, { "GetStreamSourceFreq", 2, v_GetStreamSourceFreq, 0 },
    { "SetIndices", 1, v_SetIndices, 0 }, { "GetIndices", 1, v_GetIndices, 0 },
    { "CreatePixelShader", 2, v_CreatePixelShader, 0 }, { "SetPixelShader", 1, v_SetPixelShader, 0 },
    { "GetPixelShader", 1, v_GetPixelShader, 0 },
    { "SetPixelShaderConstantF", 3, v_SetPixelShaderConstantF, 0 }, { "GetPixelShaderConstantF", 3, v_GetPixelShaderConstantF, 0 },
    { "SetPixelShaderConstantI", 3, v_SetPixelShaderConstantI, 0 }, { "GetPixelShaderConstantI", 3, v_GetPixelShaderConstantI, 0 },
    { "SetPixelShaderConstantB", 3, v_SetPixelShaderConstantB, 0 }, { "GetPixelShaderConstantB", 3, v_GetPixelShaderConstantB, 0 },
    { "DrawRectPatch", 3, m_ok, 0 }, { "DrawTriPatch", 3, m_ok, 0 }, { "DeletePatch", 1, m_ok, 0 },
    { "CreateQuery", 2, v_CreateQuery, 0 } };

/* ---- D3DX (d3dx9_41) : images DDS -------------------------------------------------- */
/* D3DXIMAGE_INFO : Width, Height, Depth, MipLevels, Format, ResourceType, ImageFileFormat.
 * Seul le DDS est compris (en-tête lu, pas de décodage) ; la texture créée est une
 * texture factice de la bonne taille et du bon format. */
static int dds_info(uint32_t src, uint32_t n, uint32_t info[7])
{
    if (n < 128 || rd32(src) != 0x20534444u) return 0;            /* « DDS » */
    uint32_t h = rd32(src + 12), w = rd32(src + 16), d = rd32(src + 24), mips = rd32(src + 28);
    uint32_t pff = rd32(src + 80), fourcc = rd32(src + 84), bits = rd32(src + 88);
    uint32_t rm = rd32(src + 92), am = rd32(src + 104), caps2 = rd32(src + 112);
    uint32_t fmt;
    if (pff & 4) fmt = fourcc;                                    /* DDPF_FOURCC : DXTn tel quel */
    else if (bits == 32) fmt = am ? 21u : 22u;                    /* A8R8G8B8 / X8R8G8B8 */
    else if (bits == 24) fmt = 20u;
    else if (bits == 16) fmt = am == 0x8000 ? 25u : am ? 26u : (rm == 0xF800 ? 23u : 24u);
    else if (bits == 8) fmt = (pff & 2) && !(pff & 0x40) ? 28u : 50u;      /* A8 / L8 */
    else fmt = 21u;
    info[0] = w; info[1] = h; info[2] = (caps2 & 0x200000) ? (d ? d : 1) : 1;
    info[3] = mips ? mips : 1; info[4] = fmt;
    info[5] = (caps2 & 0x200) ? 5 : (caps2 & 0x200000) ? 4 : 3;   /* cube / volume / texture */
    info[6] = 4;                                                  /* D3DXIFF_DDS */
    return 1;
}
/* Passe 7w — images TGA (la MINICARTE des cartes : Maps/…/Minimap.tga, 49 196 o pour TRaynor01) : D3DX ne
 * comprenait que le DDS → « pas un DDS (00020000) », pas de texture, minicarte noire. Le TGA (types 2/10 couleur
 * 24/32 bits, 3/11 gris 8 bits, brut ou RLE, origine en bas sauf bit 5) est converti en un DDS A8R8G8B8 à un niveau
 * dans l'arène, puis suit le chemin DDS. Cache : la dernière conversion (GetImageInfo puis Create sur le même tampon). */
static uint32_t g_tga_src, g_tga_n, g_tga_dds, g_tga_ndds;
static uint64_t g_n_tga;
static int tga_en_dds(uint32_t src, uint32_t n, uint32_t *dds, uint32_t *ndds)
{
    if (src == g_tga_src && n == g_tga_n && g_tga_dds) { *dds = g_tga_dds; *ndds = g_tga_ndds; return 1; }
    if (n < 18) return 0;
    uint32_t idl = rd8(src), cmt = rd8(src + 1), ty = rd8(src + 2), w = rd16(src + 12), h = rd16(src + 14);
    uint32_t bpp = rd8(src + 16), desc = rd8(src + 17);
    uint32_t cml = rd16(src + 5), cmb = rd8(src + 7);
    int gris = ty == 3 || ty == 11, rle = ty == 10 || ty == 11;
    if (!(ty == 2 || ty == 3 || ty == 10 || ty == 11) || !w || !h || w > 8192 || h > 8192) return 0;
    if (gris ? bpp != 8 : (bpp != 24 && bpp != 32)) return 0;
    uint32_t px = bpp / 8, p = src + 18 + idl + (cmt ? cml * ((cmb + 7) / 8) : 0), fin = src + n;
    uint32_t taille = 128 + w * h * 4, d = sc2_alloc(taille);
    uint8_t *o = (uint8_t *)GPTR(d), *im = o + 128;
    memset(o, 0, 128);
    uint32_t hd[32] = { 0 };
    hd[0] = 0x20534444u; hd[1] = 124; hd[2] = 0x100Fu; hd[3] = h; hd[4] = w; hd[5] = w * 4; hd[7] = 1;
    hd[19] = 32; hd[20] = 0x41; hd[22] = 32; hd[23] = 0x00FF0000u; hd[24] = 0x0000FF00u; hd[25] = 0x000000FFu;
    hd[26] = 0xFF000000u; hd[27] = 0x1000u;
    memcpy(o, hd, sizeof hd);
    uint32_t total = w * h, k = 0;
    while (k < total) {
        uint32_t cnt = 1, rep = 0;
        if (rle) { if (p >= fin) break; uint32_t c0 = rd8(p++); cnt = (c0 & 0x7F) + 1; rep = (c0 & 0x80) != 0; }
        for (uint32_t r = 0; r < cnt && k < total; r++, k++) {
            if (!rle || !rep || r == 0) { if (p + px > fin) { k = total; break; } }
            uint32_t q = (rle && rep) ? p : p;
            uint8_t b0 = rd8(q), g = gris ? b0 : rd8(q + 1), rr = gris ? b0 : rd8(q + 2), al = px == 4 ? rd8(q + 3) : 255;
            uint32_t x = k % w, y = k / w, yy = (desc & 0x20) ? y : h - 1 - y;
            uint8_t *t = im + 4 * ((size_t)yy * w + x);
            t[0] = gris ? b0 : b0; t[1] = g; t[2] = rr; t[3] = al;
            if (!(rle && rep)) p += px;
        }
        if (rle && rep) p += px;
    }
    g_tga_src = src; g_tga_n = n; g_tga_dds = d; g_tga_ndds = taille; g_n_tga++;
    sc2_trace("[D3DX] image TGA %u×%u %u bits (type %u%s) convertie en DDS A8R8G8B8 (%u o)", w, h, bpp, ty, rle ? ", RLE" : "", taille);
    *dds = d; *ndds = taille;
    return 1;
}
uint32_t sc2_d3dx_image_info(CPU *c, const uint32_t *a)          /* (src, taille, *info) */
{
    (void)c; uint32_t i[7];
    uint32_t b2[3] = { A(0), A(1), A(2) };
    if (rd32(A(0)) != 0x20534444u && tga_en_dds(A(0), A(1), &b2[0], &b2[1])) {
        if (!dds_info(b2[0], b2[1], i)) return 0x8876086Cu;
        i[6] = 3;                                                /* D3DXIFF_TGA */
        if (A(2)) for (int k = 0; k < 7; k++) wr32(A(2) + 4u * (uint32_t)k, i[k]);
        return 0;
    }
    if (!dds_info(A(0), A(1), i)) { sc2_log("[D3DX] GetImageInfoFromFileInMemory : pas un DDS (%08X)", rd32(A(0))); return 0x8876086Cu; }
    if (A(2)) for (int k = 0; k < 7; k++) wr32(A(2) + 4u * (uint32_t)k, i[k]);
    return 0;
}
/* D3DXCreate{,Cube,Volume}TextureFromFileInMemoryEx : (dev, src, taille, L, H, [P,] mips, usage,
 * fmt, pool, filtre, filtre mip, colorkey, *info, *palette, **tex). D3DX_DEFAULT = 0xFFFFFFFF,
 * D3DX_DEFAULT_NONPOW2 = 0xFFFFFFFE, D3DX_FROM_FILE = 0xFFFFFFFD, D3DFMT_FROM_FILE = 0xFFFFFFFD. */
static uint32_t val(uint32_t v, uint32_t f) { return v >= 0xFFFFFFFDu || v == 0 ? f : v; }
static uint32_t n_dds_ok, n_dds_ko;         /* passe 5e : au bilan */
uint32_t sc2_d3dx_create_texture(CPU *c, const uint32_t *a, int genre)   /* 3 tex, 5 cube, 4 volume */
{
    (void)c; uint32_t i[7];
    int vol = genre == 4, cube = genre == 5;
    uint32_t a2[17]; memcpy(a2, a, sizeof a2);                  /* passe 7w : TGA → DDS (src, taille remplacés) */
    if (rd32(A(1)) != 0x20534444u && tga_en_dds(A(1), A(2), &a2[1], &a2[2])) a = a2;
    const uint32_t *b = a + (vol ? 1 : 0);                        /* volume : un argument Depth de plus */
    if (!dds_info(A(1), A(2), i)) return 0x8876086Cu;
    uint32_t w = val(A(3), i[0]), h = cube ? w : val(A(4), i[1]);
    if (cube) b = a - 1;                                           /* cube : Size seul (pas de Height) */
    uint32_t mips = b[5] == 0xFFFFFFFDu || b[5] == 0xFFFFFFFFu || !b[5] ? i[3] : b[5];
    uint32_t fmt = b[7] >= 0xFFFFFFFDu || !b[7] ? i[4] : b[7];
    uint32_t t = obj_new(vol ? &K_VOL : cube ? &K_CUBE : &K_TEX, (uint32_t)genre);
    wr32(t + O_W, w); wr32(t + O_H, h); wr32(t + O_LEVELS, mips); wr32(t + O_USAGE, b[6]);
    wr32(t + O_FMT, fmt); wr32(t + O_POOL, b[8]);
    if (vol) wr32(t + O_DEPTH, val(A(5), i[2]));
    if (b[12]) for (int k = 0; k < 7; k++) wr32(b[12] + 4u * (uint32_t)k, i[k]);
    /* Passe 5d : les PIXELS du DDS sont copiés (avant : taille et format seuls). Sans
     * conversion : seulement si taille et format demandés = ceux du fichier (sinon la
     * texture reste vide et c'est compté). Les niveaux DDS sont dans l'ordre de nos
     * niveaux (niveau_offset) ; cube (passe 7l) : 6 faces × chaîne, même ordre que le DDS. */
    uint32_t debut = rd32(A(1) + 84) == 0x30315844u ? 148u : 128u;   /* fourcc « DX10 » : en-tête étendu */
    uint32_t niv = mips < i[3] ? mips : i[3];
    int cube_ok = !cube || (rd32(A(1) + 112) & 0xFC00u) == 0xFC00u;    /* DDSCAPS2 : les 6 faces présentes */
    int fmt_ok = fmt == i[4] || ((fmt == 21 || fmt == 22) && (i[4] == 21 || i[4] == 22));   /* 7w : X8/A8R8G8B8 = même mémoire */
    if (cube_ok && w == i[0] && h == i[1] && fmt_ok && (!vol || rd32(t + O_DEPTH) == i[2])) {
        wr32(t + O_LEVELS, niv);
        uint32_t total = niveau_offset(t, niveaux(t)) * faces_de(t);
        if (debut + total <= A(2)) {
            uint32_t dst = buf_of(t, total);
            memcpy(GPTR(dst), GPTR(A(1) + debut), total);
            wr32(t + O_GEN, 1);
            n_dds_ok++;
        } else n_dds_ko++;
    } else n_dds_ko++;
    if (!rd32(t + O_GEN))
        sc2_log("[D3DX] texture DDS NON remplie : demandé %u×%u fmt 0x%X genre %d (fichier %u×%u×%u fmt 0x%X, %u niveaux, %u o)",
                w, h, fmt, genre, i[0], i[1], i[2], i[4], i[3], A(2));
    else if (n_dds_ok <= 3 || (n_dds_ok & 63) == 0)
        sc2_trace("[D3DX] texture DDS %u×%u fmt 0x%X ×%u niveaux : %s (%u remplies, %u vides)", w, h, fmt, niv,
                rd32(t + O_GEN) ? "PIXELS COPIÉS" : "taille/format convertis : NON remplie", n_dds_ok, n_dds_ko);
    inventaire_tex(INV_DDS, t);
    wr32(b[14], t);
    return 0;
}
uint32_t sc2_d3dx_check_tex(CPU *c, const uint32_t *a)        /* (dev, *L, *H, *mips, usage, *fmt, pool) */
{
    (void)c;
    if (A(5) && (rd32(A(5)) == 0 || rd32(A(5)) >= 0xFFFFFFFDu)) wr32(A(5), 21);
    return 0;
}

/* ---- passe 9h : D3DXLoadSurfaceFromMemory ------------------------------------------------------------------------
 * (pDestSurface, pDestPalette, pDestRect, pSrcMemory, SrcFormat, SrcPitch, pSrcPalette, pSrcRect, Filter, ColorKey),
 * stdcall, 10 arguments (table d'import, sc2_iat_p4.c). Un seul site d'appel dans SC2.exe : L_00A05760 (0x00A05806,
 * via le thunk 0x010A16E0 = jmp [0x01419934]) — destination = GetSurfaceLevel d'une texture, rectangle destination
 * optionnel, format source pris dans la table moteur → D3DFMT 0x01572230, palettes NULL, Filter 3 (LINEAR),
 * ColorKey 0 ; un retour ≠ 0 y part dans le chemin d'erreur 0x00A07920. Avant : S_OK sans copier (p_ret0).
 * Maintenant : chaque appel journalisé (20 premiers), copie réelle par sc2_d3dx_surface.h, entrées invalides →
 * D3DERR_INVALIDCALL journalisé, conversions non gérées → S_OK sans copie, journalisé (comportement d'avant). */
#include "sc2_d3dx_surface.h"
#ifdef __linux__
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#define D3DERR_INVALIDCALL 0x8876086Cu
static uint64_t g_lsm_appels, g_lsm_copies, g_lsm_refus, g_lsm_non_geres;

/* Lecture de n octets invités qui ne peut pas fauter : process_vm_readv sur soi-même rend EFAULT sur une page absente
 * ou protégée au lieu d'un SIGSEGV. Appel indisponible (ENOSYS : qemu-user ; EPERM : filtre seccomp) ou autre
 * système : memcpy après les bornes. */
static int lire_invite_sure(uint32_t va, void *dst, size_t n)
{
    if (!n) return 0;
    if ((uint64_t)va + n > 0x100000000ull || va < 0x10000u) return -1;   /* débordement 32 bits, page nulle */
#ifdef __linux__
    struct iovec l = { dst, n }, r = { GPTR(va), n };
    ssize_t k = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (k == (ssize_t)n) return 0;
    if (k >= 0 || errno == EFAULT) return -1;
#endif
    memcpy(dst, GPTR(va), n);
    return 0;
}
static void texte_rect(const sc2x_rect *r, int present, char *o, size_t cap)
{
    if (!present) snprintf(o, cap, "NULL");
    else snprintf(o, cap, "(%d,%d)-(%d,%d)", r->l, r->t, r->r, r->b);
}
uint32_t sc2_d3dx_load_surface_memory(CPU *c, const uint32_t *a)
{
    uint32_t ret = rd32(c->esp), s = A(0), sfmt = A(4), spitch = A(5), filtre = A(8), ck = A(9);
    uint64_t n = ++g_lsm_appels;
    int journal = n <= 20;
    uint32_t fn = sc2_lifted_precedente(ret);
    sc2x_rect sr = { 0 }, dr = { 0 };
    uint32_t brut[8];
    int sr_ok = A(7) && !lire_invite_sure(A(7), brut, 16), dr_ok = A(2) && !lire_invite_sure(A(2), brut + 4, 16);
    if (sr_ok) sr = (sc2x_rect){ (int32_t)brut[0], (int32_t)brut[1], (int32_t)brut[2], (int32_t)brut[3] };
    if (dr_ok) dr = (sc2x_rect){ (int32_t)brut[4], (int32_t)brut[5], (int32_t)brut[6], (int32_t)brut[7] };
    char rs[48], rd[48], note[160] = "";
    texte_rect(&sr, sr_ok, rs, sizeof rs); texte_rect(&dr, dr_ok, rd, sizeof rd);
    if (journal)
        sc2_log("[D3DX] D3DXLoadSurfaceFromMemory n°%llu depuis 0x%08X (%s%08X+0x%X) : dst 0x%08X rect %s pal 0x%08X ; src 0x%08X "
                "D3DFMT 0x%X pitch %u rect %s pal 0x%08X ; filtre 0x%X%s ; colorkey 0x%08X", (unsigned long long)n, ret,
                fn ? "L_" : "?", fn, fn ? ret - fn : 0, s, rd, A(1), A(3), sfmt, spitch, rs, A(6), filtre,
                filtre != 1 && filtre != 2 ? " (pas de mise à l'échelle : copie simple)" : "", ck);
    /* surface destination : un objet IDirect3DSurface9 de ce device, lisible */
    uint32_t hd[O_SIZEOF / 4];
    const char *refus = NULL;
    if (!s || !A(3)) refus = !s ? "surface destination NULL" : "mémoire source NULL";
    else if (A(2) && !dr_ok) refus = "rectangle destination illisible";
    else if (A(7) && !sr_ok) refus = "rectangle source illisible";
    else if (lire_invite_sure(s, hd, sizeof hd) || hd[O_VTBL / 4] != K_SURF.vtbl) refus = "destination qui n'est pas une surface";
    uint32_t o = 0, niv = 0, face = 0, f = 0, w = 0, h = 0, base = 0, pitch = 0;
    if (!refus) {
        uint32_t p = rd32(s + O_AUX), l = rd32(s + O_LEVEL);
        o = p ? p : s; niv = p ? (l & 0xFFFF) : 0; face = p ? (l >> 16) : 0;
        if (niv >= niveaux(o)) niv = 0;
        if (face >= faces_de(o)) face = 0;
        f = rd32(o + O_FMT); w = lvl(rd32(o + O_W), niv); h = lvl(rd32(o + O_H), niv); pitch = pitch_of(f, w);
        uint32_t chaine = niveau_offset(o, niveaux(o));
        base = buf_of(o, chaine * faces_de(o)) + face * chaine + niveau_offset(o, niv);
        if (!A(7)) sr = (sc2x_rect){ 0, 0, (int32_t)w, (int32_t)h };          /* tolérance : D3DX l'exige non NULL */
    }
    uint64_t deb = 0, tai = 0;
    if (!refus && sc2x_etendue_source(sfmt, spitch, &sr, &deb, &tai)) refus = "rectangle ou pitch source incohérent (ou format inconnu)";
    uint8_t *tampon = NULL;
    if (!refus && tai > (64u << 20)) refus = "plage source > 64 Mo";
    if (!refus && !(tampon = malloc(tai ? tai : 1))) refus = "mémoire";
    if (!refus && ((uint64_t)A(3) + deb > 0xFFFFFFFFull || lire_invite_sure((uint32_t)(A(3) + deb), tampon, tai))) refus = "mémoire source illisible";
    int r = SC2X_INVALIDE;
    if (!refus) {
        r = sc2x_copier(tampon, tai, sfmt, spitch, &sr, (uint8_t *)GPTR(base), f, pitch, w, h, dr_ok ? &dr : NULL, ck, note, sizeof note);
        if (r == SC2X_INVALIDE) refus = note;
    }
    free(tampon);
    if (refus) {
        g_lsm_refus++;
        if (journal || g_lsm_refus <= 5)
            sc2_log("[D3DX] D3DXLoadSurfaceFromMemory n°%llu : D3DERR_INVALIDCALL — %s", (unsigned long long)n, refus);
        return D3DERR_INVALIDCALL;
    }
    if (r == SC2X_OK) {
        g_lsm_copies++;
        wr32(o + O_GEN, rd32(o + O_GEN) + 1);                               /* le backend re-téléverse la texture */
    } else g_lsm_non_geres++;
    if (journal || (r != SC2X_OK && g_lsm_non_geres <= 5))
        sc2_log("[D3DX] D3DXLoadSurfaceFromMemory n°%llu → %s : %ux%u D3DFMT 0x%X (niveau %u, face %u) ; %s", (unsigned long long)n,
                r == SC2X_OK ? "S_OK, copié" : "S_OK SANS copie", w, h, f, niv, face, note);
    return 0;
}

/* ID3DXBuffer factice : [O_BUF] données, [O_SIZE] taille. */
static uint32_t xb_GetBufferPointer(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; return rd32(s + O_BUF); }
static uint32_t xb_GetBufferSize(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; return rd32(s + O_SIZE); }
static com_meth M_XBUF[] = { IUNK, { "GetBufferPointer", 0, xb_GetBufferPointer, 0 }, { "GetBufferSize", 0, xb_GetBufferSize, 0 } };
static com_class K_XBUF;
static void com_install(void);
/* D3DXCompileShader(src, len, defines, include, fonction, profil, flags, **shader, **erreurs, **constantes)
 * Compilation FACTICE : bytecode minimal du profil demandé (version, « mov oPos, v0 », fin) ;
 * le device factice ne l'exécute pas. Le source est journalisé. */
uint32_t sc2_d3dx_compile(CPU *c, const uint32_t *a)
{
    char src[256]; uint32_t n = A(1) < 255 ? A(1) : 255;
    for (uint32_t i = 0; i < n; i++) { char ch = (char)rd8(A(0) + i); src[i] = (ch == '\n' || ch == '\r') ? ' ' : ch; }
    src[n] = 0;
    const char *prof = A(5) ? (const char *)GPTR(A(5)) : "?";
    pthread_mutex_lock(&g_com_mu); com_install();
    uint32_t b = obj_new(&K_XBUF, 0);
    pthread_mutex_unlock(&g_com_mu);
    int ps = prof[0] == 'p';
    uint32_t code[] = { ps ? 0xFFFF0300u : 0xFFFE0300u,
                        0x02000001u, ps ? 0x800F0800u : 0xE00F0000u, 0x90E40000u,   /* mov oC0/oPos, v0 */
                        0x0000FFFFu };
    uint32_t d = sc2_alloc(sizeof code);
    memcpy(GPTR(d), code, sizeof code);
    wr32(b + O_BUF, d); wr32(b + O_SIZE, sizeof code);
    if (A(7)) wr32(A(7), b);
    if (A(8)) wr32(A(8), 0);
    if (A(9)) wr32(A(9), 0);
    sc2_trace("[D3DX] D3DXCompileShader(« %s », fonction \"%s\", profil %s) depuis 0x%08X → bytecode factice (%u octets)",
            src, A(4) ? (const char *)GPTR(A(4)) : "", prof, rd32(c->esp), (unsigned)sizeof code);
    return 0;
}

/* ID3DXConstantTable factice : 0 constante (le bytecode factice n'en déclare pas). */
static uint32_t ct_GetDesc(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    static uint32_t createur;
    if (!createur) { createur = sc2_alloc(32); strcpy((char *)GPTR(createur), "SC2 Partout (factice)"); }
    wr32(A(0), createur); wr32(A(0) + 4, rd32(s + O_FMT)); wr32(A(0) + 8, 0);
    return 0;
}
static uint32_t ct_GetConstantDesc(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)s; if (A(2)) wr32(A(2), 0); return 0x8876086Cu; }
static uint32_t ct_handle_nul(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 0; }
static uint32_t ct_GetSamplerIndex(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 0xFFFFFFFFu; }
static com_meth M_CTAB[] = { IUNK, { "GetBufferPointer", 0, xb_GetBufferPointer, 0 }, { "GetBufferSize", 0, xb_GetBufferSize, 0 },
    { "GetDesc", 1, ct_GetDesc, 0 }, { "GetConstantDesc", 3, ct_GetConstantDesc, 0 },
    { "GetSamplerIndex", 1, ct_GetSamplerIndex, 0 }, { "GetConstant", 2, ct_handle_nul, 0 },
    { "GetConstantByName", 2, ct_handle_nul, 0 }, { "GetConstantElement", 2, ct_handle_nul, 0 },
    { "SetDefaults", 1, m_ok, 0 }, { "SetValue", 4, m_ok, 0 }, { "SetBool", 3, m_ok, 0 },
    { "SetBoolArray", 4, m_ok, 0 }, { "SetInt", 3, m_ok, 0 }, { "SetIntArray", 4, m_ok, 0 },
    { "SetFloat", 3, m_ok, 0 }, { "SetFloatArray", 4, m_ok, 0 }, { "SetVector", 3, m_ok, 0 },
    { "SetVectorArray", 4, m_ok, 0 }, { "SetMatrix", 3, m_ok, 0 }, { "SetMatrixArray", 4, m_ok, 0 },
    { "SetMatrixPointerArray", 4, m_ok, 0 }, { "SetMatrixTranspose", 3, m_ok, 0 },
    { "SetMatrixTransposeArray", 4, m_ok, 0 }, { "SetMatrixTransposePointerArray", 4, m_ok, 0 } };
static com_class K_CTAB;
uint32_t sc2_d3dx_constant_table(CPU *c, const uint32_t *a)   /* (fonction, flags, **table) */
{
    (void)c;
    pthread_mutex_lock(&g_com_mu); com_install();
    uint32_t t = obj_new(&K_CTAB, 0);
    pthread_mutex_unlock(&g_com_mu);
    wr32(t + O_FMT, A(0) ? rd32(A(0)) : 0);              /* version du bytecode */
    wr32(t + O_BUF, sc2_alloc(16)); wr32(t + O_SIZE, 16);
    wr32(A(2), t);
    return 0;
}

/* ---- ICU BreakIterator FACTICE (thiscall) ------------------------------------------ */
/* Vtable ICU 4.4 sous MSVC (surcharges groupées en ordre inverse) — indices vérifiés sur
 * les appels de SC2.exe (0x013468F0 : +0x1C setText(UnicodeString&) ; 0x01346970 : +0x34
 * next() ; +0x3C following ; +0x40 preceding). Frontières simplifiées mais cohérentes :
 * 0 caractère (chaque position), 1 mot (changement de classe), 2 ligne (après une suite
 * d'espaces), 3 phrase (après . ! ? suivis d'espaces). Objet : +8 texte UTF-16 (copie),
 * +12 longueur, +16 position, +20 genre. */
enum { BI_TXT = 8, BI_LEN = 12, BI_POS = 16, BI_GENRE = 20 };
static int bi_classe(uint16_t ch)
{
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == 0x3000) return 0;
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch >= 0xC0) return 1;
    return 2;
}
static int bi_frontiere(uint32_t o, int32_t i)
{
    int32_t n = (int32_t)rd32(o + BI_LEN);
    if (i <= 0 || i >= n) return i == 0 || i == n;
    uint32_t t = rd32(o + BI_TXT);
    uint16_t a = rd16(t + 2u * (uint32_t)(i - 1)), b = rd16(t + 2u * (uint32_t)i);
    switch (rd32(o + BI_GENRE)) {
    case 0: return 1;
    case 1: return bi_classe(a) != bi_classe(b) || bi_classe(a) == 2;
    case 2: return bi_classe(a) == 0 && bi_classe(b) != 0;
    default: {
        if (bi_classe(a) != 0 || bi_classe(b) == 0) return 0;
        for (int32_t k = i - 1; k >= 0; k--) { uint16_t ch = rd16(t + 2u * (uint32_t)k);
            if (bi_classe(ch) == 0) continue; return ch == '.' || ch == '!' || ch == '?'; }
        return 0; }
    }
}
static uint32_t bi_suivant(uint32_t o, int32_t depuis)   /* plus petite frontière > depuis, ou DONE (-1) */
{
    int32_t n = (int32_t)rd32(o + BI_LEN);
    for (int32_t i = depuis + 1; i <= n; i++) if (bi_frontiere(o, i)) { wr32(o + BI_POS, (uint32_t)i); return (uint32_t)i; }
    wr32(o + BI_POS, (uint32_t)n); return 0xFFFFFFFFu;
}
static uint32_t bi_precedent(uint32_t o, int32_t depuis)
{
    for (int32_t i = depuis - 1; i >= 0; i--) if (bi_frontiere(o, i)) { wr32(o + BI_POS, (uint32_t)i); return (uint32_t)i; }
    wr32(o + BI_POS, 0); return 0xFFFFFFFFu;
}
static uint32_t bi_setText(CPU *c, uint32_t s, const uint32_t *a)    /* (const UnicodeString&) */
{
    uint32_t us = A(0), n = rd32(us + 4), src = rd32(us + 12), b = sc2_alloc(2 * n + 2);
    if (n) memcpy(GPTR(b), GPTR(src), 2 * n);
    wr32(s + BI_TXT, b); wr32(s + BI_LEN, n); wr32(s + BI_POS, 0);
    /* passe 7l : les textes que le jeu met en page (32 premiers, UTF-8) — diagnostic du rendu de police */
    static int ntxt, ntxt_max = -1;
    if (ntxt_max < 0) { const char *e = getenv("SC2_TEXTE_MAX"); ntxt_max = e ? atoi(e) : 32; }   /* passe 7p */
    if (ntxt < ntxt_max) {
        char u[400]; size_t k = 0;
        for (uint32_t i = 0; i < n && k + 4 < sizeof u; i++) {
            uint16_t ch = rd16(src + 2 * i);
            if (ch < 0x80) u[k++] = ch < 0x20 ? '.' : (char)ch;
            else if (ch < 0x800) { u[k++] = (char)(0xC0 | ch >> 6); u[k++] = (char)(0x80 | (ch & 63)); }
            else { u[k++] = (char)(0xE0 | ch >> 12); u[k++] = (char)(0x80 | (ch >> 6 & 63)); u[k++] = (char)(0x80 | (ch & 63)); }
        }
        u[k] = 0;
        sc2_trace("[TEXTE] n°%d BreakIterator::setText (%u car.) « %s »  ← 0x%08X", ++ntxt, n, u, rd32(c->esp));
        /* passe 7m : SC2_TEXTE_PILE=<sous-chaîne> → pile d'appels invitée quand ce texte est mis en page */
        const char *pile = getenv("SC2_TEXTE_PILE");
        if (pile && *pile && strstr(u, pile)) sc2_dump_ss("[TEXTE] texte surveillé (SC2_TEXTE_PILE)");
    }
    return 0;
}
static uint32_t bi_first(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; wr32(s + BI_POS, 0); return 0; }
static uint32_t bi_last(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; wr32(s + BI_POS, rd32(s + BI_LEN)); return rd32(s + BI_LEN); }
static uint32_t bi_previous(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; return bi_precedent(s, (int32_t)rd32(s + BI_POS)); }
static uint32_t bi_next(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; return bi_suivant(s, (int32_t)rd32(s + BI_POS)); }
static uint32_t bi_next_n(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; int32_t k = (int32_t)A(0); uint32_t r = rd32(s + BI_POS);
    for (; k > 0 && r != 0xFFFFFFFFu; k--) r = bi_suivant(s, (int32_t)rd32(s + BI_POS));
    for (; k < 0 && r != 0xFFFFFFFFu; k++) r = bi_precedent(s, (int32_t)rd32(s + BI_POS));
    return r;
}
static uint32_t bi_current(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)a; return rd32(s + BI_POS); }
static uint32_t bi_following(CPU *c, uint32_t s, const uint32_t *a) { (void)c; return bi_suivant(s, (int32_t)A(0)); }
static uint32_t bi_preceding(CPU *c, uint32_t s, const uint32_t *a) { (void)c; return bi_precedent(s, (int32_t)A(0)); }
static uint32_t bi_isBoundary(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; int r = bi_frontiere(s, (int32_t)A(0)); if (r) wr32(s + BI_POS, A(0)); return (uint32_t)r; }
static com_class K_BRKI;
static uint32_t bi_clone(CPU *c, uint32_t s, const uint32_t *a)
{ (void)c; (void)a; uint32_t o = sc2_alloc(O_SIZEOF); memcpy(GPTR(o), GPTR(s), O_SIZEOF); wr32(o + O_REF, 1); return o; }
static uint32_t bi_ddtor(CPU *c, uint32_t s, const uint32_t *a) { (void)c; if (A(0) & 1) sc2_free(s); return s; }
static uint32_t bi_classid(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 0x7FE90000u; }
static uint32_t bi_null(CPU *c, uint32_t s, const uint32_t *a) { (void)c; (void)s; (void)a; return 0; }
static com_meth M_BRKI[] = {
    { "`scalar deleting destructor'", 1, bi_ddtor, 0 }, { "getDynamicClassID", 0, bi_classid, 0 },
    { "operator==", 1, bi_null, 0 }, { "clone", 0, bi_clone, 0 }, { "getText", 0, NULL, 0 },
    { "getUText", 2, bi_null, 0 }, { "setText(UText*, UErrorCode&)", 2, NULL, 0 },
    { "setText(const UnicodeString&)", 1, bi_setText, 0 }, { "adoptText", 1, NULL, 0 },
    { "first", 0, bi_first, 0 }, { "last", 0, bi_last, 0 }, { "previous", 0, bi_previous, 0 },
    { "next(int32_t)", 1, bi_next_n, 0 }, { "next()", 0, bi_next, 0 }, { "current", 0, bi_current, 0 },
    { "following", 1, bi_following, 0 }, { "preceding", 1, bi_preceding, 0 }, { "isBoundary", 1, bi_isBoundary, 0 },
    { "createBufferClone", 3, NULL, 0 }, { "getRuleStatus", 0, bi_null, 0 }, { "getRuleStatusVec", 3, bi_null, 0 } };
uint32_t sc2_icu_breakiterator(uint32_t genre)
{
    pthread_mutex_lock(&g_com_mu); com_install();
    uint32_t o = obj_new(&K_BRKI, 0);
    pthread_mutex_unlock(&g_com_mu);
    wr32(o + BI_GENRE, genre); wr32(o + BI_TXT, sc2_alloc(2)); wr32(o + BI_LEN, 0); wr32(o + BI_POS, 0);
    return o;
}

/* ---- Text Services Framework : ITfThreadMgr FACTICE (passe 4) -------------------- */
/* 0x012DE740 (IME) suppose que CoCreateInstance(CLSID_TF_ThreadMgr) réussit (sinon
 * Release sur NULL). On rend un objet dont QueryInterface ne connaît qu'IUnknown et
 * ITfThreadMgr : la demande d'ITfThreadMgrEx échoue et le jeu renonce proprement à
 * l'IME (Release puis retour 0, branche 0x012DE78B). */
static uint32_t tsf_QueryInterface(CPU *c, uint32_t s, const uint32_t *a)
{
    static const uint32_t unk[4] = { 0x00000000u, 0x00000000u, 0x000000C0u, 0x46000000u };
    static const uint32_t tm[4] = { 0xAA80E801u, 0x11D22021u, 0x6000E093u, 0x6EB867B0u };
    const uint32_t *g = (const uint32_t *)GPTR(A(0));
    if (!memcmp(g, unk, 16) || !memcmp(g, tm, 16)) return m_QueryInterface(c, s, a);
    if (A(1)) wr32(A(1), 0);
    sc2_log("[TSF] ITfThreadMgr::QueryInterface({%08X-…}) → E_NOINTERFACE", g[0]);
    return 0x80004002u;
}
static com_meth M_TSF[] = { { "QueryInterface", 2, tsf_QueryInterface, 0 }, { "AddRef", 0, m_AddRef, 0 },
    { "Release", 0, m_Release, 0 }, { "Activate", 1, NULL, 0 }, { "Deactivate", 0, m_ok, 0 },
    { "CreateDocumentMgr", 1, NULL, 0 }, { "EnumDocumentMgrs", 1, NULL, 0 }, { "GetFocus", 1, NULL, 0 },
    { "SetFocus", 1, NULL, 0 }, { "AssociateFocus", 3, NULL, 0 }, { "IsThreadFocus", 1, NULL, 0 },
    { "GetFunctionProvider", 2, NULL, 0 }, { "EnumFunctionProviders", 1, NULL, 0 },
    { "GetGlobalCompartment", 1, NULL, 0 } };

/* ---- client Battle.net FACTICE (objet rendu par Battle.net.dll!#3, thiscall) ----------- */
/* Méthodes déduites des sites d'appel de SC2.exe ; arité vérifiée site par site. Une
 * méthode encore inconnue arrête le run en donnant son indice et son appelant. */
/* Passe 7h : méthode8 = envoi d'un message au client (0x00BD3FC0 : message de vtable 0x0148CB20, descripteur
 * 0x01455C64…) — atteint au chargement d'une carte (lanceur simulé). Hors ligne : accepté et jeté. */
static uint32_t bn_envoyer(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    static int n;
    if (n++ < 8) sc2_log("[BNET] client::méthode8(message 0x%08X [vtable 0x%08X, %08X %08X], 0x%X) → accepté, jeté (hors ligne)",
                         A(0), rd32(A(0)), rd32(A(0) + 8), rd32(A(0) + 12), A(1));
    return 0;
}
/* Passe 10l (Tumor minimal, étape a) : client::méthode2 remet au client le rappel 0x00BC6CF0 (cdecl
 * rappel(ctx, nom, fn, fnctx) → ctx->vtbl[0](nom, fn, fnctx)) et son contexte, un
 * Battlenet::TumorManagerImplementation (vtable 0x0148BDDC) : [0] 0x00BC7990 Register(nom, fn, fnctx) range
 * nom → (fn, fnctx) dans la table de hachage en +0xA48 (entrées de 0x14 : hachage FNV-1a|0x80000000, chaîne
 * [+4, +8 = caractères], fn +0xC, fnctx +0x10 ; borne en +0xA48+8) ; [3] 0x00BC7560 Invoke(ret, nom, a, b)
 * cherche nom et appelle fn(ret, fnctx, a, b) en cdecl (ret = valeur à étiquette de type, 0 = indéfinie).
 * Le jeu y range lui-même « SendEvent » → 0x00BC7100. Sous SC2_BNET_TUMOR=1, au premier Poll, le client
 * factice s'enregistre par CE rappel (gestionnaire hôte sonde, tracé), puis vérifie l'aller-retour par
 * l'Invoke du jeu. Hors garde : mémorisé, jamais rappelé (comportement 7h, défaut inchangé). */
#define TUMOR_NOM "Tumor.HorsLigne.Sonde"
static uint32_t g_bn_rappel, g_bn_ctx;
static com_class K_TUMOR;
static uint32_t tumor_sonde(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;   /* cdecl fn(ret, fnctx, a, b) : thiscall sans argument dépilé = ne dépile que le retour */
    static int n;
    if (n++ < 16) sc2_log("[TUMOR] gestionnaire sonde « " TUMOR_NOM " » APPELÉ par le jeu (ret 0x%08X, fnctx 0x%08X, a 0x%X, b 0x%X)",
                          a[0], a[1], a[2], a[3]);
    if (a[0]) wr32(a[0], 0);   /* valeur de retour indéfinie */
    return a[0];
}
static void tumor_table(const char *quand)
{
    uint32_t t = g_bn_ctx + 0xA48u, e = rd32(t), n = rd32(t + 8), k = 0;
    sc2_log("[TUMOR] table des gestionnaires (%s) : entrées 0x%08X, borne %u", quand, e, n);
    for (uint32_t i = 0; e && i < n && i < 4096; i++) {
        uint32_t x = e + 0x14u * i, h = rd32(x), nom = rd32(x + 8);
        if (!h) continue;
        char b[64]; int m = 0;
        for (; nom && m < 63 && rd8(nom + (uint32_t)m); m++) b[m] = (char)rd8(nom + (uint32_t)m);
        b[m] = 0; k++;
        sc2_log("[TUMOR]   #%u hachage 0x%08X « %s » → fn 0x%08X, fnctx 0x%08X", i, h, b, rd32(x + 0xC), rd32(x + 0x10));
    }
    sc2_log("[TUMOR] %u gestionnaire(s) rangé(s)", k);
}
static void tumor_enregistrer(CPU *c)
{
    uint32_t sv[4] = { c->eax, c->ecx, c->edx, c->esp };
    uint32_t fn = rd32(K_TUMOR.vtbl), nom = sc2_alloc(sizeof TUMOR_NOM);
    memcpy(GPTR(nom), TUMOR_NOM, sizeof TUMOR_NOM);
    sc2_log("[TUMOR] étape a : rappel 0x%08X(ctx 0x%08X, « " TUMOR_NOM " », fn 0x%08X, fnctx 0x%08X) — Register attendu 0x%08X",
            g_bn_rappel, g_bn_ctx, fn, g_bn_ctx, rd32(rd32(g_bn_ctx)));
    tumor_table("avant");
    uint32_t args[4] = { g_bn_ctx, nom, fn, g_bn_ctx };
    sc2_call_guest(c, g_bn_rappel, 4, args, 1);
    sc2_log("[TUMOR] rappel revenu (esp %s)", c->esp == sv[3] ? "équilibré" : "DÉSÉQUILIBRÉ");
    tumor_table("après");
    /* aller-retour : Invoke du jeu (vtable[3], thiscall, ret 0x10) sur notre nom → doit appeler tumor_sonde */
    uint32_t ret = sc2_alloc(16), inv = rd32(rd32(g_bn_ctx) + 12);
    c->ecx = g_bn_ctx;
    uint32_t ia[4] = { ret, nom, 0, 0 };
    uint32_t r = sc2_call_guest(c, inv, 4, ia, 0);
    sc2_log("[TUMOR] Invoke 0x%08X(« " TUMOR_NOM " ») du jeu → 0x%08X, type de retour %u (esp %s)",
            inv, r, rd32(ret), c->esp == sv[3] ? "équilibré" : "DÉSÉQUILIBRÉ");
    c->eax = sv[0]; c->ecx = sv[1]; c->edx = sv[2]; c->esp = sv[3];
}
/* Passe 10n (diagnostic) : SC2_TUMOR_CHERCHE=<frame>:<va>,<va>… — à la frame donnée, cherche dans .data
 * (0x0156F000…0x034C73EC) et dans l'arène principale (0x08000000…0x38000000) les mots égaux à chaque va, puis,
 * pour chaque mot trouvé dans l'arène, les globaux de .data qui pointent jusqu'à 0x800 octets avant lui. */
static void tumor_chercher(void)
{
    static int etat = -1; static uint32_t fr, v[8]; static int nv;
    if (etat < 0) {
        const char *e = getenv("SC2_TUMOR_CHERCHE"); char *f; etat = 0;
        if (!e || (fr = (uint32_t)strtoul(e, &f, 10)) == 0 || *f != ':') return;
        for (e = f + 1; *e && nv < 8; e = *f ? f + 1 : f) { v[nv] = (uint32_t)strtoul(e, &f, 16); if (f == e) break; nv++; }
        etat = 1;
    }
    if (etat != 1 || sc2_frames_presentees() < fr) return;
    etat = 2;
    for (int k = 0; k < nv; k++) {
        int n = 0;
        for (uint32_t x = 0x0156F000u; x < 0x034C73E8u && n < 16; x += 4)
            if (rd32(x) == v[k]) { sc2_log("[TUMOR-CHERCHE] 0x%08X trouvé dans .data en 0x%08X", v[k], x); n++; }
        for (uint32_t x = 0x08000000u; x < 0x37FFFFFCu && n < 48; x += 4) {
            if (rd32(x) != v[k]) continue;
            n++; int m = 0;
            for (uint32_t g = 0x0156F000u; g < 0x034C73E8u && m < 4; g += 4) {
                uint32_t p = rd32(g);
                if (p <= x && x - p < 0x800u) { m++;
                    sc2_log("[TUMOR-CHERCHE] 0x%08X dans l'arène en 0x%08X ← global 0x%08X = 0x%08X (+0x%X)", v[k], x, g, p, x - p); }
            }
            if (!m) sc2_log("[TUMOR-CHERCHE] 0x%08X dans l'arène en 0x%08X (aucun global à moins de 0x800)", v[k], x);
        }
        sc2_log("[TUMOR-CHERCHE] 0x%08X : %d occurrence(s) relevée(s) à la frame %u", v[k], n, sc2_frames_presentees());
    }
}
/* Passe 10n (Tumor minimal, étape b) : faire charger MainUI.gfx par le jeu, voie A (interposition). Le front-end
 * héberge l'UI Battle.net dans le cadre « Flash » de son écran glue (singleton [0x01932338], vtable 0x0143C510) :
 * 0x00B140F0 range ce cadre en écran+0x620 (vtable 0x01451F90), résout l'asset @BattleNetInterface
 * (Assets.txt : Battle.net/Battle.net.gfx, film coquille que fournissait le vrai client : absent de Battle.net.mpq)
 * et le passe au chargeur 0x00B7A140 → 0x00B773B0 (thiscall, ret 4 ; arg = vue {ptr, len, -1, -1, -1}), qui
 * teste l'existence par le GFxLoader (0x011D3820) puis crée le film (0x011D3870). Le client factice rejoue CE
 * chargeur, sur CE cadre, avec la page d'entrée réelle de l'archive : Battle.net\SWF\MainUI.gfx
 * (SC2_TUMOR_FILM=<chemin> pour une autre page). Une seule fois, au premier Poll où le cadre existe. */
#define TUMOR_FILM "Battle.net\\SWF\\MainUI.gfx"
static void tumor_film(CPU *c)
{
    static int fait;
    if (fait) return;
    uint32_t ecran = rd32(0x01932338u), cadre = ecran && rd32(ecran) == 0x0143C510u ? rd32(ecran + 0x620u) : 0;
    if (!cadre || rd32(cadre) != 0x01451F90u) return;
    fait = 1;
    const char *e = getenv("SC2_TUMOR_FILM"), *chemin = e && *e ? e : TUMOR_FILM;
    uint32_t n = (uint32_t)strlen(chemin), p = sc2_alloc(n + 1), vue = sc2_alloc(20);
    memcpy(GPTR(p), chemin, n + 1);
    wr32(vue, p); wr32(vue + 4, n); wr32(vue + 8, 0xFFFFFFFFu); wr32(vue + 12, 0xFFFFFFFFu); wr32(vue + 16, 0xFFFFFFFFu);
    uint32_t sv[4] = { c->eax, c->ecx, c->edx, c->esp };
    sc2_log("[TUMOR] étape b : frame %u, écran glue 0x%08X, cadre Flash 0x%08X → chargeur de film 0x00B7A140(« %s »)",
            sc2_frames_presentees(), ecran, cadre, chemin);
    c->ecx = cadre;
    uint32_t r = sc2_call_guest(c, 0x00B7A140u, 1, &vue, 0);
    sc2_log("[TUMOR] chargeur revenu → 0x%08X (%s), film du cadre [+0x1D8]+0x28 = 0x%08X (esp %s)", r,
            r ? "film créé" : "échec", rd32(cadre + 0x1D8u) ? rd32(rd32(cadre + 0x1D8u) + 0x28u) : 0,
            c->esp == sv[3] ? "équilibré" : "DÉSÉQUILIBRÉ");
    c->eax = sv[0]; c->ecx = sv[1]; c->edx = sv[2]; c->esp = sv[3];
}
static uint32_t bn_poll(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)s; (void)a;
    static int fait;
    if (!fait && g_bn_rappel && g_bn_ctx) { fait = 1; tumor_enregistrer(c); }
    if (g_bn_rappel) { tumor_chercher(); tumor_film(c); }
    return 0;
}
static uint32_t bn_enregistrer(CPU *c, uint32_t s, const uint32_t *a)
{
    (void)c; (void)s;
    uint32_t vt = A(1) ? rd32(A(1)) : 0;   /* passe 7p : l'objet qui recevrait les événements du client */
    const char *g = getenv("SC2_BNET_TUMOR");
    int tumor = g && *g == '1';
    if (tumor) { g_bn_rappel = A(0); g_bn_ctx = A(1); }
    sc2_log("[BNET] client::méthode2(rappel 0x%08X, contexte 0x%08X [vtable 0x%08X, méthode0 0x%08X]) → %s",
            A(0), A(1), vt, vt ? rd32(vt) : 0,
            tumor ? "mémorisé, rappelé au premier Poll (SC2_BNET_TUMOR=1, passe 10l)" : "mémorisé, jamais rappelé (hors ligne)");
    return 0;
}
static com_meth M_BNET[] = {
    { "méthode0", 0, NULL, 0 }, { "méthode1", 0, NULL, 0 }, { "méthode2 (enregistre un rappel)", 2, bn_enregistrer, 0 },
    { "méthode3", 0, NULL, 0 }, { "méthode4", 0, NULL, 0 }, { "méthode5", 0, NULL, 0 }, { "méthode6", 0, NULL, 0 },
    { "méthode7", 0, NULL, 0 }, { "méthode8 (envoie un message au client)", 2, bn_envoyer, 0 }, { "méthode9 (Poll, chaque frame)", 0, bn_poll, 0 }, { "méthode10", 0, NULL, 0 },
    { "méthode11", 0, NULL, 0 }, { "méthode12", 0, NULL, 0 }, { "méthode13", 0, NULL, 0 }, { "méthode14", 0, NULL, 0 },
    { "méthode15", 0, NULL, 0 }, { "méthode16", 0, NULL, 0 }, { "méthode17", 0, NULL, 0 }, { "méthode18", 0, NULL, 0 },
    { "méthode19", 0, NULL, 0 }, { "méthode20", 0, NULL, 0 }, { "méthode21", 0, NULL, 0 }, { "méthode22", 0, NULL, 0 },
    { "méthode23", 0, NULL, 0 } };
static com_meth M_TUMOR[] = { { "gestionnaire sonde (cdecl fn(ret, fnctx, a, b))", 0, tumor_sonde, 0 } };

/* ---- table des classes ------------------------------------------------------------ */
#define NM(t) ((int)(sizeof t / sizeof t[0]))
static com_class K_D3D = { "IDirect3D9", M_D3D, NM(M_D3D), 0 };
static com_class K_DEV = { "IDirect3DDevice9", M_DEV, NM(M_DEV), 0 };
static com_class K_TEX = { "IDirect3DTexture9", M_TEX, NM(M_TEX), 0 };
static com_class K_CUBE = { "IDirect3DCubeTexture9", M_CUBE, NM(M_CUBE), 0 };
static com_class K_VOL = { "IDirect3DVolumeTexture9", M_VOL, NM(M_VOL), 0 };
static com_class K_SURF = { "IDirect3DSurface9", M_SURF, NM(M_SURF), 0 };
static com_class K_VB = { "IDirect3DVertexBuffer9", M_VB, NM(M_VB), 0 };
static com_class K_IB = { "IDirect3DIndexBuffer9", M_IB, NM(M_IB), 0 };
static com_class K_QUERY = { "IDirect3DQuery9", M_QUERY, NM(M_QUERY), 0 };
static com_class K_VS = { "IDirect3DVertexShader9", M_VS, NM(M_VS), 0 };
static com_class K_PS = { "IDirect3DPixelShader9", M_PS, NM(M_PS), 0 };
static com_class K_DECL = { "IDirect3DVertexDeclaration9", M_DECL, NM(M_DECL), 0 };
static com_class K_SB = { "IDirect3DStateBlock9", M_SB, NM(M_SB), 0 };
static com_class K_SWAP = { "IDirect3DSwapChain9", M_SWAP, NM(M_SWAP), 0 };
static com_class K_TSF = { "ITfThreadMgr", M_TSF, NM(M_TSF), 0 };
static com_class K_BNET = { "BnetClient", M_BNET, NM(M_BNET), 0, 1 };
static com_class K_XBUF = { "ID3DXBuffer", M_XBUF, NM(M_XBUF), 0, 0 };
static com_class K_CTAB = { "ID3DXConstantTable", M_CTAB, NM(M_CTAB), 0, 0 };
static com_class K_BRKI = { "icu::BreakIterator", M_BRKI, NM(M_BRKI), 0, 1 };
static com_class K_TUMOR = { "TumorSonde", M_TUMOR, NM(M_TUMOR), 0, 1 };
static com_class *const g_classes[] = { &K_D3D, &K_DEV, &K_TEX, &K_CUBE, &K_VOL, &K_SURF, &K_VB, &K_IB,
                                        &K_QUERY, &K_VS, &K_PS, &K_DECL, &K_SB, &K_SWAP, &K_TSF, &K_BNET, &K_XBUF, &K_CTAB, &K_BRKI, &K_TUMOR };
#define N_CLASSES ((int)(sizeof g_classes / sizeof g_classes[0]))

static void com_install(void)
{
    for (int n = 0; n < N_CLASSES; n++) {
        com_class *k = g_classes[n];
        if (k->vtbl) continue;
        uint32_t v = sc2_alloc(4u * (uint32_t)k->nm);
        for (int i = 0; i < k->nm; i++) wr32(v + 4u * (uint32_t)i, SC2_COM_BASE + 0x1000u * (uint32_t)n + 16u * (uint32_t)i);
        k->vtbl = v;
    }
}

uint32_t sc2_d3d9_create(uint32_t sdk)
{
    pthread_mutex_lock(&g_com_mu);
    com_install();
    if (!g_d3d) g_d3d = obj_new(&K_D3D, 0);
    else wr32(g_d3d + O_REF, rd32(g_d3d + O_REF) + 1);
    pthread_mutex_unlock(&g_com_mu);
    sc2_log("[D3D9] Direct3DCreate9(SDK %u) → IDirect3D9 factice 0x%08X (vtable 0x%08X, %d méthodes)",
            sdk, g_d3d, K_D3D.vtbl, K_D3D.nm);
    return g_d3d;
}

uint32_t sc2_com_tsf_create(void)
{
    pthread_mutex_lock(&g_com_mu);
    com_install();
    uint32_t o = obj_new(&K_TSF, 0);
    pthread_mutex_unlock(&g_com_mu);
    return o;
}

uint32_t sc2_bnet_client_create(void)
{
    pthread_mutex_lock(&g_com_mu);
    com_install();
    uint32_t o = obj_new(&K_BNET, 0);
    pthread_mutex_unlock(&g_com_mu);
    return o;
}

int sc2_com_call(CPU *c, uint32_t va)
{
    if (va < SC2_COM_BASE || va >= SC2_COM_END) return 0;
    uint32_t n = (va - SC2_COM_BASE) / 0x1000u, i = ((va - SC2_COM_BASE) % 0x1000u) / 16u;
    if (n >= (uint32_t)N_CLASSES || i >= (uint32_t)g_classes[n]->nm) return 0;
    com_class *k = g_classes[n];
    com_meth *m = &k->m[i];
    uint32_t ret = rd32(c->esp), self = k->thiscall ? c->ecx : rd32(c->esp + 4);
    const uint32_t *a = (const uint32_t *)GPTR(c->esp + (k->thiscall ? 4u : 8u));
    uint64_t cnt = __atomic_add_fetch(&m->n, 1, __ATOMIC_RELAXED);
    if (!m->fn)
        sc2_stop(11, "[D3D9] %s::%s (vtable n°%u, %d args) appelée mais NON IMPLÉMENTÉE (objet 0x%08X, appelant 0x%08X)",
                 k->iface, m->nom, i, m->argc, self, ret);
    if (cnt <= 3) {
        char args[128] = ""; size_t o = 0;
        for (int j = 0; j < m->argc && j < 8; j++)
            o += (size_t)snprintf(args + o, sizeof args - o, "%s0x%X", j ? ", " : "", a[j]);
        sc2_trace("[D3D9] %s::%s(%s)  ← 0x%08X%s", k->iface, m->nom, args, ret, cnt == 3 ? "  (suivants comptés)" : "");
    }
    uint32_t r = m->fn(c, self, a);
    c->eax = r;
    c->esp += (k->thiscall ? 4u : 8u) + 4u * (uint32_t)m->argc;
    return 1;
}

void sc2_com_report(void)
{
    printf("[BILAN] frames présentées (Present) : %u\n", g_frames);
    printf("[BILAN] [D3D9] surfaces de texture (passe 10j) : %llu créées, %llu reprises du cache\n",
           (unsigned long long)g_n_surfc_neuves, (unsigned long long)g_n_surfc_reprises);
    d3dv_bilan();
    /* Passe 5d : shaders créés / liés / utilisés par des draws (les plus dessinés d'abord). */
    int nps = 0, nvs = 0, ord[SC2_MAX_SHADERS];
    for (int i = 0; i < g_nshaders; i++) { ord[i] = i; if (g_shaders[i].ps) nps++; else nvs++; }
    for (int i = 1; i < g_nshaders; i++) for (int j = i; j > 0 && g_shaders[ord[j]].n_draw > g_shaders[ord[j - 1]].n_draw; j--)
        { int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
    printf("[BILAN] draws : %llu RÉELS (shaders traduits), %llu « lumière » (sans SPIR-V %llu, sans déclaration %llu, sans données %llu) ; "
           "table SPIR-V embarquée : %d shaders\n", (unsigned long long)g_n_reels, (unsigned long long)g_n_lumiere,
           (unsigned long long)g_n_sans_spv, (unsigned long long)g_n_sans_decl, (unsigned long long)g_n_sans_donnees, SC2_NB_SPV);
    printf("[BILAN] textures D3DX (DDS) : %u remplies (pixels copiés), %u NON remplies\n", n_dds_ok, n_dds_ko);
    printf("[BILAN] [D3DX] D3DXLoadSurfaceFromMemory : %llu appels, %llu copiés, %llu refusés (D3DERR_INVALIDCALL), %llu conversions "
           "non gérées (S_OK sans copie)\n", (unsigned long long)g_lsm_appels, (unsigned long long)g_lsm_copies,
           (unsigned long long)g_lsm_refus, (unsigned long long)g_lsm_non_geres);   /* passe 9h */
    printf("[BILAN] [TEX] LockRect/LockBox avec sous-rectangle (pRect/pBox respectés) : %llu\n", (unsigned long long)g_n_lock_rect);
    printf("[BILAN] [VIDEO-TEX] textures L8/A8L8 (D3DFMT 50/51) : %llu LockRect, %llu UpdateTexture/UpdateSurface, %llu liaisons "
           "aux draws, %llu changements (texture, génération) vus au draw\n", (unsigned long long)g_n_lock_lum,
           (unsigned long long)g_n_upd_lum, (unsigned long long)g_n_lie_lum, (unsigned long long)g_n_gen_lum);
    printf("[BILAN] [RTT] SetRenderTarget hors backbuffer %llu ; StretchRect %llu (copiés %llu) ; ColorFill %llu ; "
           "GetRenderTargetData %llu (relus %llu)\n", (unsigned long long)g_n_setrt_sec, (unsigned long long)g_n_stretch,
           (unsigned long long)g_n_stretch_ok, (unsigned long long)g_n_colorfill, (unsigned long long)g_n_rtdata,
           (unsigned long long)g_n_rtdata_ok);
    printf("[BILAN] [TEX] copies mémoire : %llu UpdateSurface, %llu UpdateTexture (%.1f Mo)\n", (unsigned long long)g_n_upd_surf,
           (unsigned long long)g_n_upd_tex, g_o_upd / 1048576.0);
    printf("[BILAN] samplers des draws réels : %llu servis (texture ou cible), replis sur texture factice : %llu sans texture liée, "
           "%llu cube, %llu texture sans données, %llu format non géré\n", (unsigned long long)g_samplers_servis,
           (unsigned long long)g_replis[R_AUCUNE], (unsigned long long)g_replis[R_CUBE], (unsigned long long)g_replis[R_SANS_DONNEES],
           (unsigned long long)g_replis[R_FORMAT]);
    for (int i = 0; i < g_nrepli_tex; i++)
        printf("[BILAN]   repli : texture %u×%u D3DFMT 0x%X (genre %u, usage 0x%X, pool %u) — %s ×%llu\n", g_repli_tex[i].w, g_repli_tex[i].h,
               g_repli_tex[i].fmt, g_repli_tex[i].kind, g_repli_tex[i].usage, g_repli_tex[i].pool,
               g_repli_tex[i].cause == R_SANS_DONNEES ? "sans données" : g_repli_tex[i].cause == R_CUBE ? "cube" : "format non géré",
               (unsigned long long)g_repli_tex[i].n);
    inventaire_bilan();
    printf("[BILAN] shaders distincts créés : %d PS, %d VS ; utilisés par des draws :\n", nps, nvs);
    for (int i = 0; i < g_nshaders && g_shaders[ord[i]].n_draw; i++) {
        sc2_shader_info *si = &g_shaders[ord[i]];
        printf("[BILAN]   %s %016llx v%X.%X %5u o : %llu draws, %llu Set, module %d\n", si->ps ? "PS" : "VS",
               (unsigned long long)si->hash, (si->ver >> 8) & 0xFF, si->ver & 0xFF, si->taille,
               (unsigned long long)si->n_draw, (unsigned long long)si->n_set, si->module);
    }
    int tot = 0;
    for (int n = 0; n < N_CLASSES; n++) for (int i = 0; i < g_classes[n]->nm; i++) if (g_classes[n]->m[i].n) tot++;
    if (!tot) return;
    printf("[BILAN] méthodes COM factices appelées (%d) :", tot);
    int k = 0;
    for (int n = 0; n < N_CLASSES; n++)
        for (int i = 0; i < g_classes[n]->nm; i++)
            if (g_classes[n]->m[i].n)
                printf("%s %s::%s×%llu", k++ % 4 ? "," : "\n   ", g_classes[n]->iface + (strncmp(g_classes[n]->iface, "IDirect", 7) ? 0 : 7),
                       g_classes[n]->m[i].nom, (unsigned long long)g_classes[n]->m[i].n);
    printf("\n");
}
