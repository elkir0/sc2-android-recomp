/* sc2_d3d9.h — objets COM FACTICES vus par le code lifté (passe 4).
 *
 * Un objet COM invité est un bloc de l'arène : [0] lpVtbl, [4] compteur de
 * références, [8..] données de la classe. La vtable (dans l'arène aussi) contient
 * pour la méthode k de la classe n la VA fictive SC2_COM_BASE + n*0x1000 + 16*k.
 * Un `call [eax+4*k]` lifté arrive donc dans sc2_dispatch, qui la route vers
 * sc2_com_call : trace « [D3D9] IDirect3DDevice9::SetRenderState(…) », exécution
 * du gestionnaire, dépilage stdcall (this + argc). Une méthode sans gestionnaire
 * ARRÊTE le run en se nommant (interface, n° de vtable, appelant).
 */
#ifndef RECOMP_SC2_D3D9_H
#define RECOMP_SC2_D3D9_H
#include "sc2_host.h"

#define SC2_COM_BASE 0xF1000000u
#define SC2_COM_END  0xF1100000u

int      sc2_com_call(CPU *c, uint32_t va);     /* 1 si va est une méthode COM factice */
uint32_t sc2_d3d9_create(uint32_t sdk);        /* Direct3DCreate9 → IDirect3D9* factice */
void     sc2_com_report(void);
/* 22/10 (V1.1) : interface du banc d'entrées et du device. sc2_d3d9_taille : taille du client (back buffer courant) ;
 * sc2_banc_capture_prendre (sc2_entrees_banc.c) : nom de la capture CAPTURE:<nom> demandée, rendu UNE fois au Present
 * suivant puis oublié, NULL sinon — appelé par present_vk sur le fil du jeu. */
void        sc2_d3d9_taille(uint32_t *w, uint32_t *h);
const char *sc2_banc_capture_prendre(void);
uint32_t sc2_com_tsf_create(void);
uint32_t sc2_bnet_client_create(void);
uint32_t sc2_icu_breakiterator(uint32_t genre);  /* 0 caractère, 1 mot, 2 ligne, 3 phrase */         /* objet client Battle.net factice (thiscall) */
/* D3DX : images DDS → textures factices */
uint32_t sc2_d3dx_image_info(CPU *c, const uint32_t *a);
uint32_t sc2_d3dx_create_texture(CPU *c, const uint32_t *a, int genre);
uint32_t sc2_d3dx_check_tex(CPU *c, const uint32_t *a);
uint32_t sc2_d3dx_compile(CPU *c, const uint32_t *a);
uint32_t sc2_d3dx_constant_table(CPU *c, const uint32_t *a);             /* ITfThreadMgr factice (IME : refus propre) */
uint32_t sc2_d3dx_load_surface_memory(CPU *c, const uint32_t *a);       /* passe 9h : copie réelle + journal */


/* Passe 5d : shaders D3D9 réels créés par le jeu (bytecode SM2/3 copié dans l'arène). */
#define SC2_MAX_SHADERS 32768  /* passe 7q : la mission TRaynor01 en crée > 1 024 (1 179 en 1 500 f) ; au-delà, non suivis → draws « lumière ».
                               * Passe 10t : 4 096 atteints en UNE mission (TTychus01 : 57 524 à 549 647 dessins « lumière ») ; la
                               * progression persistante enchaîne les missions dans un même lancement → 32 768 (≈ 62 Mo d'arène au pire) */
typedef struct {
    uint64_t hash;          /* FNV-1a 64 du bytecode (clé de sc2_shaders_spv.h) */
    int      ps;            /* 1 pixel, 0 vertex */
    uint32_t ver, taille;   /* jeton de version, octets */
    uint32_t code;          /* VA de la copie */
    int      module;        /* index du module Vulkan (-1 : non traduit / pas encore créé) */
    uint64_t n_set, n_draw;
} sc2_shader_info;
extern sc2_shader_info g_shaders[SC2_MAX_SHADERS];
extern int g_nshaders;

#endif
