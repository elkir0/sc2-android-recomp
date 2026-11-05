/* stubs_sc2.h — appels Win32 du démarrage, avant la fenêtre, sur POSIX : débogueur,
 * mémoire partagée nommée, événements, instance unique.
 *
 * Chaque appel rend la réponse de Windows pour un processus seul, sans
 * débogueur ; les tests (tests/test_win32_compat.c) vérifient ces réponses.
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

#define SC2_EVENT_GAME_APP   "Global\\StarCraft II Game Application"  /* 0099CA20 */

/* ---- Débogueur ------------------------------------------------------------ */

/* Rend FALSE : aucun débogueur n'est attaché. */
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
