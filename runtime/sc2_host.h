/* sc2_host.h — l'hôte qui fait tourner le code lifté de SC2.exe (passe 3).
 *
 * Modèle mémoire : PLAT, VA invitée == adresse hôte, sous 4 Go.
 *   - l'image SC2.exe est mappée à sa base préférée 0x00800000 (g_image_delta = 0) ;
 *   - piles, TEB/PEB, TLS et tas invités viennent d'une arène basse (sc2_alloc) ;
 *   - l'exécutable hôte est lié à 0x40000000 (-Ttext-segment) pour ne pas
 *     recouvrir l'image (0x00800000..0x035E4000).
 *
 * Exécution : sc2_dispatch(c, va) appelle L_<va>(c) (table LIFTED_FUNCS), un
 * stub d'import (VA 0xF0000000 + 16*i écrite dans le slot IAT), ou s'ARRÊTE sur
 * un point identifié (manque) en imprimant la pile d'appels invitée fantôme.
 */
#ifndef RECOMP_SC2_HOST_H
#define RECOMP_SC2_HOST_H

#include <stdarg.h>
#include <stddef.h>          /* V5.1 : size_t (sc2_d3d9_mem) */
#include <stdint.h>
#include <setjmp.h>
#include "sc2_lifted.h"   /* avant cpu.h : notre RECOMP_TODO */
#include "cpu.h"

#ifndef SC2_GPTR_DEFINED
#define SC2_GPTR_DEFINED
extern uintptr_t g_hote_delta;
#define GPTR(va)      ((void *)((uintptr_t)(uint32_t)(va) + g_hote_delta))
#endif
#define GVA_OF(p)     ((uint32_t)(uintptr_t)(p))

#define SC2_IAT_STUB_BASE  0xF0000000u   /* VA fictive du stub d'import n° i : base + 16*i */
#define SC2_RET_SENTINEL   0xFFFFFFF0u   /* adresse de retour poussée par l'hôte */

/* ---- trace et arrêt ------------------------------------------------------ */
void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Passe 10n : traces par appel / par frame ([FS], [IAT], [MPQ], [D3D9], [TEX]…) coupées par défaut ;
 * SC2_TRACE=1 les rend. Lue une fois. Le [BILAN], les arrêts, MANQUE et les erreurs passent toujours. */
int  sc2_trace_actif(void);
#define sc2_trace(...) do { if (sc2_trace_actif()) sc2_log(__VA_ARGS__); } while (0)
/* Arrêt propre ou sur point identifié : raison + pile fantôme + bilan, puis exit(code). */
void sc2_stop(int code, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
/* M10 (24/10) : crochet FAIBLE de sortie — appelé par sc2_stop sur le fil appelant, après son bilan et fflush, juste avant
 * exit(code). NULL (sa définition dans sc2_runtime.c ; banc, Android, runner) : exit(code) comme avant. L'hôte Mac le pose
 * en mode fenêtre (mac/sc2_mac_sortie.m : exit() renvoyé au fil principal, _exit sur une faute) ; il ne revient pas. */
extern void (*sc2_sortie_crochet)(int code);

/* ---- arène invitée (< 4 Go) ------------------------------------------------ */
uint32_t sc2_alloc(uint32_t size);              /* zéro-initialisé, aligné 16 */
uint32_t sc2_alloc_size(uint32_t va);           /* taille demandée (HeapSize, realloc) */
uint32_t sc2_realloc(uint32_t va, uint32_t size);
void     sc2_free(uint32_t va);                 /* compté ; recyclé si sc2_arene_recycle() (passe 7h) */
int      sc2_arene_recycle(void);               /* SC2_ARENE_RECYCLE=1 ou SC2_RUN_SANS_BNET=1 */
uint32_t sc2_arena_top(void);                   /* fin de la partie servie de l'arène */
uint32_t sc2_arena2_top(void);                  /* passe 7h : fin de l'arène de débordement (0 : pas ouverte) */
uint32_t sc2_arena2_base(void);                 /* passe 7h : début de l'arène de débordement */
uint32_t sc2_reserve_stack(uint32_t size, uint32_t *limit); /* rend le sommet */
/* V5.1 (24/10) : ORIGINE d'un bloc d'arène, gardée dans son en-tête (bits 24..30 du mot [12], recyclage actif) — octets
 * vivants et servis par origine, lignes [MEM] à chaque carte chargée (sc2_mpq.c) et à l'arrêt. sc2_alloc = SC2_MO_AUTRE ;
 * sc2_realloc garde l'origine du bloc d'avant. */
enum { SC2_MO_AUTRE, SC2_MO_MALLOC, SC2_MO_TAS, SC2_MO_VIRT, SC2_MO_SECTION, SC2_MO_D3D_OBJ, SC2_MO_D3D_TEX,
       SC2_MO_D3D_CUBE, SC2_MO_D3D_VOL, SC2_MO_D3D_SURF, SC2_MO_D3D_VB, SC2_MO_D3D_IB, SC2_MO_D3D_SHADER, SC2_MO_N };
uint32_t sc2_alloc_de(uint32_t size, unsigned origine);
/* V5.1 : 1 si la mémoire rendue par le jeu retourne à l'arène (ressources D3D9 au dernier Release, tableaux remplacés des
 * UnicodeString) — SC2_MEM_LIBERER ≠ 0 ET recyclage actif ; lu une fois, journalisé */
int      sc2_mem_liberer(void);
void     sc2_mem_ligne(const char *motif);      /* lignes [MEM] : occupation de l'arène, vivants par origine, objets D3D9 */
void     sc2_d3d9_mem(char *o, size_t n) __attribute__((weak));   /* sc2_d3d9.c : objets D3D9 vivants et libérés */
void     sc2_virt_mem(char *o, size_t n) __attribute__((weak));   /* V5.1, sc2_iat.c : granules de VirtualAlloc */

/* ---- image, TEB ------------------------------------------------------------ */
int      sc2_load_image(const char *exe_path);  /* 0 = OK */
uint32_t sc2_new_teb(uint32_t stack_top, uint32_t stack_limit);
extern __thread uint32_t g_sc2_teb;   /* TEB du thread invité courant */
extern uint32_t g_sc2_peb;

/* ---- appels invités -------------------------------------------------------- */
/* Appelle une fonction invitée cdecl/stdcall depuis l'hôte : pousse les args (de
 * droite à gauche), une adresse de retour sentinelle, puis dispatch. Rend eax. */
uint32_t sc2_call_guest(CPU *c, uint32_t va, int nargs, const uint32_t *args, int cdecl);
/* Passe 10k : écrans natifs du front-end, à chaque tour de la pompe du jeu (journal ; SC2_GLUE_ECRAN, sc2_runtime.c). */
void sc2_glue_pompe(CPU *c);
/* Mode tolérant (initialiseurs C/C++) : un manque ou un TODO dans l'appel fait
 * longjmp ici au lieu d'arrêter le processus. NULL = arrêt. */
extern __thread sigjmp_buf *g_sc2_recover;
extern __thread uint32_t g_sc2_recover_va;

/* Pile d'appels fantôme de la fibre courante (pour dire où l'on s'arrête). Même définition dans sc2_lifted.h (SC2_APPEL,
 * passe 10r) : la première incluse gagne. */
#ifndef SC2_SHADOW_DEFINI
#define SC2_SHADOW_DEFINI
typedef struct { uint32_t va[8192]; int n; uint32_t esp[8192]; } sc2_shadow;   /* esp : à l'entrée (passe 4) */
extern __thread sc2_shadow *g_sc2_ss;
#endif
/* Passe 4 : registre des piles fantômes (fil principal, fibres, threads) pour le
 * chien de garde SC2_WATCHDOG=<s> : à l'échéance, chaque pile est imprimée puis
 * le run s'arrête (diagnostic d'un blocage ou d'une boucle). */
void sc2_ss_register(sc2_shadow *ss, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sc2_watchdog_start(void);
void sc2_dump_ss(const char *pourquoi);
void sc2_longjmp(void *c, uint32_t jb, uint32_t val) __attribute__((noreturn));         /* imprime la pile fantôme courante, arguments compris */

/* ---- imports ---------------------------------------------------------------- */
void sc2_iat_install(void);                     /* remplit les slots IAT */
int  sc2_iat_call(CPU *c, uint32_t va);         /* 1 si va est un stub d'import (exécuté) */
void sc2_iat_report(void);

/* ---- stats -------------------------------------------------------------------- */
extern uint64_t g_sc2_dispatches;
int  sc2_lifted_count(void);
int  sc2_lifted_executed(void);
uint32_t sc2_lifted_precedente(uint32_t va);   /* passe 9h : VA de la fonction liftée la plus proche ≤ va, 0 si aucune */

/* Fautes matérielles du code lifté (SIGSEGV/SIGBUS/SIGFPE/SIGILL) : abandon de
 * l'initialiseur en mode tolérant, sinon arrêt identifié (adresse, pile fantôme). */
void sc2_install_fault_handler(void);
void sc2_thread_fault_stack(void);              /* pile alternative du thread courant */

#endif
