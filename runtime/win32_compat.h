/* win32_compat.h — socle Win32 portable de recomp-sc2 : types, handles, fibres.
 *
 * Ce que SC2.exe (build 15405) attend du système avant même d'ouvrir une
 * fenêtre, implémenté sur libc/POSIX (référence : research/analyse-init/
 * CARTE-INIT.md, citée « CARTE-INIT §x ») :
 *
 *   - les FIBRES Win32 (CARTE-INIT §0, §3.2) : sc2_init_WinMain (0x008092D0)
 *     fait  ConvertThreadToFiber ; CreateFiberEx(0, 0x800000, 0, 0x00809030, &local)
 *     ; SwitchToFiber  — tout le jeu (sc2_init_FiberMain 0x00809030 →
 *     sc2_init_GameMain 0x00808F80) tourne dans une fibre de 8 Mo de pile,
 *     qui finit par SwitchToFiber(fibre principale) pour revenir à WinMain ;
 *   - une table de HANDLE commune (événements, mappings — cf. stubs_sc2.h) ;
 *   - Get/SetLastError par thread (ERROR_ALREADY_EXISTS pilote le test
 *     d'instance unique de 00802610).
 *
 * Ce ne sont PAS des shims à ABI stdcall (le style A32/RET de
 * ~/tools/pcrecomp/runtime/win32hle) : ce sont les sémantiques, en C natif.
 * Le shim qui lira les arguments dans le modèle CPU-struct (cpu.h) les
 * appellera ; les valeurs qui doivent tenir dans un registre 32 bits invité
 * (HANDLE, piles de fibres) sont donc choisies < 4 Go (voir plus bas).
 */
#ifndef RECOMP_SC2_WIN32_COMPAT_H
#define RECOMP_SC2_WIN32_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Types Win32 (sous-ensemble) ---------------------------------------- */

typedef int            BOOL;
typedef uint32_t       DWORD;
typedef void          *LPVOID;
typedef void          *HANDLE;
typedef uint16_t       WCHAR;          /* UTF-16 comme sous Windows (≠ wchar_t Linux) */
typedef const char    *LPCSTR;
typedef const WCHAR   *LPCWSTR;
typedef size_t         SIZE_T;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE             0xFFFFFFFFu

#define WAIT_OBJECT_0        0x00000000u
#define WAIT_TIMEOUT         0x00000102u
#define WAIT_FAILED          0xFFFFFFFFu

#define ERROR_SUCCESS          0u
#define ERROR_FILE_NOT_FOUND   2u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_HANDLE   6u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_ALREADY_EXISTS   183u
#define ERROR_ALREADY_FIBER    1280u
#define ERROR_ALREADY_THREAD   1281u

/* ---- Dernière erreur (par thread, comme le TEB) -------------------------- */

DWORD GetLastError(void);
void  SetLastError(DWORD err);

/* ---- Objets noyau et table de handles --------------------------------------
 *
 * Un HANDLE est (index+1)*4 : petit, aligné sur 4 comme les vrais handles NT,
 * donc transportable tel quel dans un registre 32 bits invité. Il désigne un
 * objet noyau (en-tête w32_object en tête de struct) compté en références :
 * chaque handle en tient une, une vue de mapping aussi. À zéro, l'objet quitte
 * l'espace de noms (un nouveau CreateEvent du même nom le recrée — c'est ce
 * qui rend l'instance unique de 00802610 correcte) puis est détruit.
 *
 * Espace de noms : un seul, partagé par tous les types (comme
 * \Sessions\N\BaseNamedObjects). « Local\X » ≡ « X » ; « Global\X » est
 * gardé tel quel. Portée : le PROCESSUS (un seul SC2 vit ici — CARTE-INIT
 * §1.5 « instance unique triviale dans un seul processus »).
 * Thread-safe (un verrou global pour table + noms).
 */
typedef enum {
    W32_OBJ_FREE = 0,
    W32_OBJ_EVENT,
    W32_OBJ_MAPPING,
} w32_obj_type;

typedef struct w32_object {
    w32_obj_type        type;
    unsigned            refs;
    char               *name;                         /* NULL si anonyme */
    void              (*destroy)(struct w32_object *); /* libère l'intérieur, pas l'en-tête */
    struct w32_object  *next_named;
} w32_object;

/* Crée (ou ouvre, si `name` existe déjà) un objet de `size` octets.
 *   - existe, même type : nouveau handle dessus, *existed = TRUE,
 *     LastError = ERROR_ALREADY_EXISTS (sémantique CreateEvent/CreateFileMapping) ;
 *   - existe, autre type : NULL, ERROR_INVALID_HANDLE ;
 *   - sinon : calloc(size), init(o, ctx) sous le verrou (ne doit pas bloquer),
 *     LastError = 0.  init peut échouer (rend FALSE) → NULL. */
HANDLE w32_object_create(w32_obj_type type, const char *name, size_t size,
                         BOOL (*init)(w32_object *o, void *ctx), void *ctx,
                         void (*destroy)(w32_object *o), BOOL *existed);
/* Ouvre un objet nommé existant (OpenEvent/OpenFileMapping) ;
 * NULL + ERROR_FILE_NOT_FOUND s'il n'existe pas. */
HANDLE w32_object_open(w32_obj_type type, const char *name);
w32_object *w32_handle_obj(HANDLE h, w32_obj_type type);  /* NULL si invalide/autre type */
void   w32_object_ref(w32_object *o);
/* w32_handle_obj + référence prise atomiquement (à rendre par w32_object_unref) :
 * sûr même si un autre thread ferme le handle en même temps. */
w32_object *w32_handle_ref(HANDLE h, w32_obj_type type);
void   w32_object_unref(w32_object *o);
HANDLE w32_handle_dup(HANDLE h);                          /* DuplicateHandle simplifié */
BOOL   CloseHandle(HANDLE h);

/* ---- Fibres ---------------------------------------------------------------
 *
 * Sémantique Win32 reproduite (MSDN, et ce que 0x008092D0 en exige) :
 *
 *   ConvertThreadToFiber(param)
 *       Fait du thread courant une fibre (son contexte est capturé au
 *       prochain SwitchToFiber). Rend l'adresse de la fibre. Deuxième appel
 *       sur le même thread : NULL + ERROR_ALREADY_FIBER.
 *   CreateFiberEx(commit, reserve, flags, start, param)
 *       Alloue une fibre NON démarrée. La pile vaut max(reserve, commit),
 *       arrondie à la page ; 0 → 1 Mo (défaut PE). SC2 passe reserve =
 *       0x800000 (8 Mo). Pile mmap'ée avec une page de garde basse et, sur
 *       x86-64, MAP_32BIT : elle reste adressable comme VA invitée 32 bits.
 *       `start` est appelée au premier SwitchToFiber vers elle.
 *   SwitchToFiber(f)
 *       Sauve le contexte de la fibre courante, reprend f. Doit être appelée
 *       depuis une fibre (thread converti). Basculer vers soi-même : no-op.
 *   DeleteFiber(f)
 *       Libère pile et contexte. Supprimer la fibre COURANTE termine le
 *       thread (ExitThread) — comme sous Windows.
 *   GetCurrentFiber() / GetFiberData()
 *       Fibre courante / son paramètre (lus dans le TEB sous Windows, ici TLS).
 *   ConvertFiberToThread()
 *       Défait ConvertThreadToFiber (à n'appeler que depuis la fibre du thread).
 *
 *   Si la routine d'une fibre RETOURNE, Windows appelle ExitThread : on fait
 *   pthread_exit(NULL). SC2 ne le fait pas : 00809030 rend la main à WinMain
 *   par SwitchToFiber(fibre principale) (CARTE-INIT §0).
 *
 * Implémentation : ucontext (getcontext/makecontext/swapcontext). Une fibre
 * n'est pas liée à un thread : elle peut être reprise depuis n'importe quel
 * thread converti, mais jamais par deux threads à la fois (règle Win32).
 *
 * Modèle CPU-struct : ces piles sont les piles NATIVES du code lifté (appels
 * C profonds). La pile INVITÉE (c->esp) de la fibre est l'affaire du shim,
 * qui peut utiliser w32_fiber_stack() pour la placer dans la même zone.
 */
typedef void (*LPFIBER_START_ROUTINE)(LPVOID param);

#define FIBER_FLAG_FLOAT_SWITCH 0x1u   /* accepté, sans effet : ucontext sauve déjà le FPU */

LPVOID ConvertThreadToFiber(LPVOID param);
LPVOID ConvertThreadToFiberEx(LPVOID param, DWORD flags);
BOOL   ConvertFiberToThread(void);
LPVOID CreateFiber(SIZE_T stack_size, LPFIBER_START_ROUTINE start, LPVOID param);
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags,
                     LPFIBER_START_ROUTINE start, LPVOID param);
void   SwitchToFiber(LPVOID fiber);
void   DeleteFiber(LPVOID fiber);
LPVOID GetCurrentFiber(void);
LPVOID GetFiberData(void);

/* Extension recomp-sc2 : bornes de la pile d'une fibre créée par CreateFiberEx
 * (base = adresse basse utilisable, hors page de garde). FALSE pour une fibre
 * issue de ConvertThreadToFiber (pile du thread, inconnue ici). */
BOOL   w32_fiber_stack(LPVOID fiber, void **base, SIZE_T *size);

/* Extension recomp-sc2 (passe 5a) : mmap anonyme sous 4 Go, pour ce qu'une VA
 * 32 bits invitée doit pouvoir désigner (vues de mappings, piles de fibres).
 * x86-64 : MAP_32BIT. AArch64 (pas de MAP_32BIT) : recherche par indice dans
 * la fenêtre libre 0x38000000..0x40000000, entre l'arène invitée et
 * l'exécutable hôte (lié à 0x40000000). Rend MAP_FAILED si rien de bas n'est
 * disponible : l'appelant décide du repli. */
void  *w32_mmap_low(size_t len, int prot, int flags);

/* ---- Utilitaire chaînes ---------------------------------------------------- */

/* UTF-16 (WCHAR, terminé par 0) → UTF-8, tronqué à cap-1 octets ; les paires
 * de substitution non appariées deviennent '?'. Sert aux noms d'objets W. */
void w32_wide_to_utf8(LPCWSTR src, char *dst, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_SC2_WIN32_COMPAT_H */
