/* stubs_sc2.h — les appels Win32 du boot de SC2 qui décident « on continue ou
 * on meurt » avant la fenêtre : contrôle launcher, IPC, instance unique.
 *
 * Référence : research/analyse-init/CARTE-INIT.md §1.2 (conditions exactes de
 * sc2_init_LauncherHandshakeIPC 0x00809B90), §1.4 (instance unique 00802610),
 * §1.5 (recommandation de stubs, priorités 1 à 3).
 *
 * ---------------------------------------------------------------------------
 * Disjonction vérifiée (CARTE-INIT §1.2, `decomp/t1-ipc.asm`) :
 *
 *   prev = InterlockedCompareExchange(&shm[+0x20], 1, 4);
 *   if (prev == 4) AAAC = (shm[+0x4C] != -1);          // launcher présent
 *   else           AAAC = (IsDebuggerPresent() == 0);  // 009B3550
 *   rejet  ⇔  AAAC != 0  ET  LaunchParams.mode ∉ {3,4}   (mode = 1 par défaut)
 *   et si MapIpcMem échoue (handle nul) → rend 1 d'emblée.
 *
 * Choix retenu : shm qui RÉUSSIT, zéro-initialisée (+0x20 == 0 ≠ 4, la CAS
 * n'écrit rien) ET IsDebuggerPresent() == TRUE → AAAC = 0 → pas de rejet,
 * sans patch du code lifté. Avec la shm à zéro mais IsDebuggerPresent() ==
 * FALSE, le jeu affiche « Must run from the launcher. » (0x0141EC08) et
 * TerminateProcess(-2001) : c'est IsDebuggerPresent qui porte la décision,
 * la shm ne fait que rester cohérente (le jeu y écrit +0x00/+0x04/+0x08/+0x0C).
 * Les tests (tests/test_win32_compat.c) rejouent ce modèle dans les 4 cas.
 * ---------------------------------------------------------------------------
 */
#ifndef RECOMP_SC2_STUBS_SC2_H
#define RECOMP_SC2_STUBS_SC2_H

#include "win32_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constantes Win32 utilisées ------------------------------------------ */

#define PAGE_READONLY        0x02u
#define PAGE_READWRITE       0x04u
#define SEC_COMMIT           0x08000000u
#define FILE_MAP_COPY        0x0001u
#define FILE_MAP_WRITE       0x0002u
#define FILE_MAP_READ        0x0004u
#define FILE_MAP_ALL_ACCESS  0x000F001Fu
#define SYNCHRONIZE          0x00100000u
#define EVENT_MODIFY_STATE   0x0002u
#define EVENT_ALL_ACCESS     0x001F0003u

/* ---- Constantes SC2 (CARTE-INIT §1.2, §1.4) -------------------------------- */

#define SC2_IPC_MEM_NAME     "StarCraft II IPC Mem"   /* 00809FB0 sc2_init_MapIpcMem */
#define SC2_IPC_MEM_SIZE     0x10000u                  /* CreateFileMappingA(…, 0, 0x10000, …) */
#define SC2_IPC_MEM_PROTECT  (PAGE_READWRITE | SEC_COMMIT)   /* 0x08000004 */

#define SC2_IPC_OFF_INSTANCES   0x00u  /* InterlockedIncrement → [0x0169AAA4] */
#define SC2_IPC_OFF_GAME_PID    0x0Cu  /* InterlockedCompareExchange(+0xC, pid, 0) */
#define SC2_IPC_OFF_STATE       0x20u  /* poignée de main : 4 = launcher prêt, 1/5/0 ensuite */
#define SC2_IPC_OFF_WAIT        0x24u  /* jeu met 1, launcher remet 0 */
#define SC2_IPC_OFF_LAUNCHER_PID 0x28u
#define SC2_IPC_OFF_PARAMS      0x30u  /* 0x2D7 dwords → LaunchParams @0x01699AC8 */
#define SC2_IPC_PARAMS_SIZE     0xB5Cu
#define SC2_IPC_STATE_LAUNCHER_READY 4u

#define SC2_EVENT_GAME_APP   "Global\\StarCraft II Game Application"  /* 0099CA20 */

/* ---- Débogueur ------------------------------------------------------------ */

/* Rend TRUE, toujours : désactive le contrôle launcher (priorité 1 de §1.5).
 * Seuls appelants dans SC2.exe : 009B3550 (contrôle 00809B90) et la CRT
 * 01041B50 (rapport d'erreur CRT, sans effet). */
BOOL IsDebuggerPresent(void);

/* ---- Mappings (mémoire partagée nommée) ------------------------------------
 *
 * Seul le cas « adossé au fichier d'échange » est géré (hFile ==
 * INVALID_HANDLE_VALUE) : c'est celui de 00809FB0. La section est un
 * mmap(MAP_SHARED|MAP_ANONYMOUS) — donc zéro-initialisée comme sous Windows —
 * placé sous 4 Go sur x86-64 (MAP_32BIT) pour qu'une vue soit une VA
 * invitée 32 bits valide. Toutes les vues d'une même section rendent la même
 * adresse de base (+offset) : cohérence triviale dans un seul processus.
 * La section survit à CloseHandle tant qu'une vue est mappée.
 */
HANDLE CreateFileMappingA(HANDLE file, void *sa, DWORD protect,
                          DWORD size_high, DWORD size_low, LPCSTR name);
HANDLE CreateFileMappingW(HANDLE file, void *sa, DWORD protect,
                          DWORD size_high, DWORD size_low, LPCWSTR name);
HANDLE OpenFileMappingA(DWORD access, BOOL inherit, LPCSTR name);
HANDLE OpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name);
LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD off_high, DWORD off_low,
                     SIZE_T bytes);
BOOL   UnmapViewOfFile(const void *view);

/* Extension recomp-sc2 (passe 5b) : section hôte d'une vue ouverte (base et
 * taille), pour reloger la vue dans l'arène invitée quand son adresse hôte
 * n'est pas une VA 32 bits (macOS : g_hote_delta ; cf. h_MapViewOfFile). */
BOOL   w32_view_section(const void *view, void **base, SIZE_T *size);

/* ---- Événements ------------------------------------------------------------
 *
 * pthread mutex + condvar (horloge monotone). Manuel ou auto-reset, état
 * initial, nommés ou non. Un CreateEvent sur un nom existant rend un handle
 * sur l'objet existant avec LastError = ERROR_ALREADY_EXISTS : c'est le test
 * d'instance unique de 00802610 (« Global\StarCraft II Game Application » ;
 * s'il existe déjà → TerminateProcess). Portée : processus (CARTE-INIT §1.5,
 * priorité 3 — instance unique triviale dans un seul processus).
 */
HANDLE CreateEventA(void *sa, BOOL manual_reset, BOOL initial_state, LPCSTR name);
HANDLE CreateEventW(void *sa, BOOL manual_reset, BOOL initial_state, LPCWSTR name);
HANDLE OpenEventA(DWORD access, BOOL inherit, LPCSTR name);
HANDLE OpenEventW(DWORD access, BOOL inherit, LPCWSTR name);
BOOL   SetEvent(HANDLE ev);
BOOL   ResetEvent(HANDLE ev);
DWORD  WaitForSingleObject(HANDLE h, DWORD timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_SC2_STUBS_SC2_H */
