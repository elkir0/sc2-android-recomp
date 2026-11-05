/* win32_compat.c — handles, dernière erreur et fibres Win32 sur POSIX.
 *
 * Voir win32_compat.h pour la sémantique et les adresses (CARTE-INIT §0, §3.2 :
 * sc2_init_WinMain 0x008092D0 → CreateFiberEx(0, 0x800000, 0, 0x00809030)).
 */
#define _GNU_SOURCE
#ifdef __APPLE__
#define _XOPEN_SOURCE
#endif
#include "win32_compat.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

/* ========================================================================== */
/* Dernière erreur                                                            */
/* ========================================================================== */

static __thread DWORD t_last_error;

DWORD GetLastError(void)      { return t_last_error; }
void  SetLastError(DWORD err) { t_last_error = err; }

/* ========================================================================== */
/* Objets noyau, noms, handles                                                */
/* ========================================================================== */

#define W32_MAX_HANDLES 1024

static w32_object     *g_slots[W32_MAX_HANDLES];
static w32_object     *g_named;                 /* liste des objets nommés vivants */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static HANDLE slot_to_handle(int i) { return (HANDLE)(uintptr_t)((unsigned)(i + 1) * 4u); }

static int handle_to_slot(HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    if (v == 0 || (v & 3u) || v / 4u > W32_MAX_HANDLES) return -1;
    return (int)(v / 4u) - 1;
}

/* « Local\X » et « X » désignent le même objet dans une session. */
static const char *norm_name(const char *name)
{
    return (name && !strncmp(name, "Local\\", 6)) ? name + 6 : name;
}

static w32_object *named_find_locked(const char *name)
{
    for (w32_object *o = g_named; o; o = o->next_named)
        if (!strcmp(o->name, name)) return o;
    return NULL;
}

/* Prend une référence sur o et lui attribue un slot. */
static HANDLE handle_alloc_locked(w32_object *o)
{
    for (int i = 0; i < W32_MAX_HANDLES; i++)
        if (!g_slots[i]) { g_slots[i] = o; o->refs++; return slot_to_handle(i); }
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

/* Rend o si sa dernière référence vient de tomber (à détruire hors verrou). */
static w32_object *unref_locked(w32_object *o)
{
    if (--o->refs) return NULL;
    if (o->name)
        for (w32_object **pp = &g_named; *pp; pp = &(*pp)->next_named)
            if (*pp == o) { *pp = o->next_named; break; }
    return o;
}

static void object_free(w32_object *o)
{
    if (!o) return;
    if (o->destroy) o->destroy(o);
    free(o->name);
    free(o);
}

HANDLE w32_object_create(w32_obj_type type, const char *name, size_t size,
                         BOOL (*init)(w32_object *, void *), void *ctx,
                         void (*destroy)(w32_object *), BOOL *existed)
{
    HANDLE h = NULL;
    if (existed) *existed = FALSE;
    if (size < sizeof(w32_object)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    name = norm_name(name);
    if (name && !*name) name = NULL;

    pthread_mutex_lock(&g_lock);
    w32_object *o = name ? named_find_locked(name) : NULL;
    if (o) {
        if (o->type != type) {
            SetLastError(ERROR_INVALID_HANDLE);
        } else if ((h = handle_alloc_locked(o)) != NULL) {
            if (existed) *existed = TRUE;
            SetLastError(ERROR_ALREADY_EXISTS);
        }
        pthread_mutex_unlock(&g_lock);
        return h;
    }
    o = (w32_object *)calloc(1, size);
    if (!o || (name && !(o->name = strdup(name)))) {
        free(o);
        pthread_mutex_unlock(&g_lock);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    o->type = type;
    SetLastError(ERROR_SUCCESS);
    if (init && !init(o, ctx)) {
        free(o->name); free(o);
        pthread_mutex_unlock(&g_lock);
        if (!GetLastError()) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    o->destroy = destroy;
    if ((h = handle_alloc_locked(o)) != NULL) {
        if (o->name) { o->next_named = g_named; g_named = o; }
        SetLastError(ERROR_SUCCESS);
        pthread_mutex_unlock(&g_lock);
        return h;
    }
    pthread_mutex_unlock(&g_lock);
    object_free(o);
    return NULL;
}

HANDLE w32_object_open(w32_obj_type type, const char *name)
{
    HANDLE h = NULL;
    name = norm_name(name);
    if (!name || !*name) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    pthread_mutex_lock(&g_lock);
    w32_object *o = named_find_locked(name);
    if (!o)                  SetLastError(ERROR_FILE_NOT_FOUND);
    else if (o->type != type) SetLastError(ERROR_INVALID_HANDLE);
    else if ((h = handle_alloc_locked(o)) != NULL) SetLastError(ERROR_SUCCESS);
    pthread_mutex_unlock(&g_lock);
    return h;
}

w32_object *w32_handle_obj(HANDLE h, w32_obj_type type)
{
    int i = handle_to_slot(h);
    if (i < 0) return NULL;
    pthread_mutex_lock(&g_lock);
    w32_object *o = (g_slots[i] && g_slots[i]->type == type) ? g_slots[i] : NULL;
    pthread_mutex_unlock(&g_lock);
    return o;
}

w32_object *w32_handle_ref(HANDLE h, w32_obj_type type)
{
    int i = handle_to_slot(h);
    if (i < 0) return NULL;
    pthread_mutex_lock(&g_lock);
    w32_object *o = (g_slots[i] && g_slots[i]->type == type) ? g_slots[i] : NULL;
    if (o) o->refs++;
    pthread_mutex_unlock(&g_lock);
    return o;
}

void w32_object_ref(w32_object *o)
{
    pthread_mutex_lock(&g_lock);
    o->refs++;
    pthread_mutex_unlock(&g_lock);
}

void w32_object_unref(w32_object *o)
{
    pthread_mutex_lock(&g_lock);
    w32_object *dead = unref_locked(o);
    pthread_mutex_unlock(&g_lock);
    object_free(dead);
}

HANDLE w32_handle_dup(HANDLE h)
{
    int i = handle_to_slot(h);
    HANDLE d = NULL;
    pthread_mutex_lock(&g_lock);
    if (i >= 0 && g_slots[i]) d = handle_alloc_locked(g_slots[i]);
    else SetLastError(ERROR_INVALID_HANDLE);
    pthread_mutex_unlock(&g_lock);
    return d;
}

BOOL CloseHandle(HANDLE h)
{
    int i = handle_to_slot(h);
    w32_object *dead = NULL;
    pthread_mutex_lock(&g_lock);
    if (i < 0 || !g_slots[i]) {
        pthread_mutex_unlock(&g_lock);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    dead = unref_locked(g_slots[i]);
    g_slots[i] = NULL;
    pthread_mutex_unlock(&g_lock);
    object_free(dead);   /* hors verrou */
    return TRUE;
}

/* ========================================================================== */
/* Fibres                                                                     */
/* ========================================================================== */

#define FIBER_MAGIC         0x46494252u   /* "FIBR" */
#define FIBER_DEFAULT_STACK (1u << 20)    /* 1 Mo : SizeOfStackReserve PE par défaut */

typedef struct w32_fiber {
    uint32_t              magic;
    ucontext_t            ctx;
    LPFIBER_START_ROUTINE start;
    LPVOID                param;        /* GetFiberData() */
    void                 *map;          /* zone mmap (page de garde comprise) */
    size_t                map_len;
    void                 *stack;        /* base utilisable */
    size_t                stack_size;
    int                   is_thread;    /* issue de ConvertThreadToFiber */
} w32_fiber;

static __thread w32_fiber *t_cur;       /* fibre courante de ce thread (≈ TEB->FiberData) */
static __thread w32_fiber *t_thread;    /* fibre du thread converti */

static w32_fiber *as_fiber(LPVOID p)
{
    w32_fiber *f = (w32_fiber *)p;
    return (f && f->magic == FIBER_MAGIC) ? f : NULL;
}

LPVOID ConvertThreadToFiberEx(LPVOID param, DWORD flags)
{
    (void)flags;   /* FIBER_FLAG_FLOAT_SWITCH : ucontext sauve toujours le FPU */
    if (t_cur) { SetLastError(ERROR_ALREADY_FIBER); return NULL; }
    w32_fiber *f = (w32_fiber *)calloc(1, sizeof *f);
    if (!f) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    f->magic = FIBER_MAGIC;
    f->param = param;
    f->is_thread = 1;                   /* son contexte sera rempli par swapcontext */
    t_cur = t_thread = f;
    return f;
}

LPVOID ConvertThreadToFiber(LPVOID param) { return ConvertThreadToFiberEx(param, 0); }

BOOL ConvertFiberToThread(void)
{
    if (!t_thread) { SetLastError(ERROR_ALREADY_THREAD); return FALSE; }
    t_thread->magic = 0;
    free(t_thread);
    t_cur = t_thread = NULL;
    return TRUE;
}

/* Point d'entrée commun : la fibre cible est déjà t_cur (posé par SwitchToFiber). */
static void fiber_trampoline(void)
{
    w32_fiber *f = t_cur;
    f->start(f->param);
    /* Windows : une routine de fibre qui retourne termine le thread. */
    pthread_exit(NULL);
}

/* getcontext est `returns_twice` : isolé ici pour que l'appelant n'ait aucune
 * variable locale exposée à -Wclobbered. Le contexte n'est jamais repris à
 * ce point (makecontext le redirige vers le trampoline). */
static __attribute__((noinline)) int fiber_ctx_init(w32_fiber *f)
{
    if (getcontext(&f->ctx) != 0) return 0;
    f->ctx.uc_stack.ss_sp   = f->stack;
    /* Passe 10t : 64 octets nuls (mmap anonyme) laissés au-dessus du sp initial. macOS fait pointer la chaîne des
     * cadres de _ctx_start sur le haut de la pile : backtrace() (appelé par Metal à chaque création de file de
     * commandes, dans vkCreateDevice) y lisait un enregistrement de cadre hors du mapping (SIGSEGV dans
     * __thread_stack_pcs quand la pile hôte est assez courte, avec les appels directs). Il lit désormais un cadre
     * nul et s'arrête. */
    f->ctx.uc_stack.ss_size = f->stack_size - 64;
    f->ctx.uc_link          = NULL;     /* jamais atteint : le trampoline fait pthread_exit */
    makecontext(&f->ctx, fiber_trampoline, 0);
    return 1;
}

void *w32_mmap_low(size_t len, int prot, int flags)
{
#ifdef MAP_32BIT
    return mmap(NULL, len, prot, flags | MAP_32BIT, -1, 0);
#else
    /* Fallback sans MAP_32BIT (AArch64, passe 5a) : mmap avec indice, sans
     * MAP_FIXED (rien d'existant n'est écrasé) ; un résultat hors fenêtre est
     * rendu et l'indice avance. Curseur partagé sous verrou (threads invités). */
    enum { LO = 0x38000000u, HI = 0x40000000u, PAS = 0x100000u };
    static uintptr_t curseur = LO;
    /* Verrou PROPRE : mapping_init (stubs_sc2) est appelé sous g_lock. */
    static pthread_mutex_t verrou = PTHREAD_MUTEX_INITIALIZER;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    len = (len + page - 1) & ~(page - 1);
    void *r = MAP_FAILED;
    pthread_mutex_lock(&verrou);
    for (int tour = 0; tour < 2 && r == MAP_FAILED; tour++) {
        for (uintptr_t h = tour ? LO : curseur; h + len <= HI; h += PAS) {
            void *p = mmap((void *)h, len, prot, flags, -1, 0);
            if (p == MAP_FAILED) break;
            if ((uintptr_t)p >= LO && (uintptr_t)p + len <= HI) {
                r = p;
                curseur = ((uintptr_t)p + len + PAS - 1) & ~(uintptr_t)(PAS - 1);
                break;
            }
            munmap(p, len);
        }
    }
    pthread_mutex_unlock(&verrou);
    return r;
#endif
}

LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags,
                     LPFIBER_START_ROUTINE start, LPVOID param)
{
    (void)flags;
    if (!start) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t size = reserve > commit ? reserve : commit;
    if (size == 0) size = FIBER_DEFAULT_STACK;
    size = (size + page - 1) & ~(page - 1);

    w32_fiber *f = (w32_fiber *)calloc(1, sizeof *f);
    if (!f) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }

    int mflags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    /* VA < 4 Go : la pile peut aussi servir de pile invitée (c->esp 32 bits). */
    void *map = w32_mmap_low(size + page, PROT_READ | PROT_WRITE, mflags);
    if (map == MAP_FAILED)
        map = mmap(NULL, size + page, PROT_READ | PROT_WRITE, mflags, -1, 0);
    if (map == MAP_FAILED) { free(f); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    mprotect(map, page, PROT_NONE);     /* page de garde : débordement → SIGSEGV net */

    f->magic      = FIBER_MAGIC;
    f->start      = start;
    f->param      = param;
    f->map        = map;
    f->map_len    = size + page;
    f->stack      = (char *)map + page;
    f->stack_size = size;

    if (!fiber_ctx_init(f)) {
        munmap(map, size + page); free(f);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return f;
}

LPVOID CreateFiber(SIZE_T stack_size, LPFIBER_START_ROUTINE start, LPVOID param)
{
    return CreateFiberEx(0, stack_size, 0, start, param);
}

void SwitchToFiber(LPVOID target)
{
    w32_fiber *to = as_fiber(target), *from = t_cur;
    if (!to) {
        fprintf(stderr, "win32_compat: SwitchToFiber(%p) : fibre invalide\n", target);
        abort();
    }
    if (!from) {
        /* Windows plante (TEB->FiberData invalide) : on le dit plus clairement. */
        fprintf(stderr, "win32_compat: SwitchToFiber hors fibre (ConvertThreadToFiber manquant)\n");
        abort();
    }
    if (to == from) return;
    t_cur = to;
    swapcontext(&from->ctx, &to->ctx);
    /* On revient ici quand quelqu'un rebascule vers `from`. */
}

void DeleteFiber(LPVOID fiber)
{
    w32_fiber *f = as_fiber(fiber);
    if (!f) return;
    if (f == t_cur) {
        /* Windows : supprimer la fibre courante = ExitThread. La pile sur
         * laquelle on tourne ne peut être libérée ici ; elle est abandonnée. */
        f->magic = 0;
        t_cur = NULL;
        if (t_thread == f) t_thread = NULL;
        pthread_exit(NULL);
    }
    f->magic = 0;
    if (t_thread == f) t_thread = NULL;
    if (f->map) munmap(f->map, f->map_len);
    free(f);
}

LPVOID GetCurrentFiber(void) { return t_cur; }
LPVOID GetFiberData(void)    { return t_cur ? t_cur->param : NULL; }

BOOL w32_fiber_stack(LPVOID fiber, void **base, SIZE_T *size)
{
    w32_fiber *f = as_fiber(fiber);
    if (!f || f->is_thread) return FALSE;
    if (base) *base = f->stack;
    if (size) *size = f->stack_size;
    return TRUE;
}

/* ========================================================================== */
/* Chaînes                                                                    */
/* ========================================================================== */

void w32_wide_to_utf8(LPCWSTR src, char *dst, size_t cap)
{
    size_t o = 0;
    if (!cap) return;
    for (; src && *src; src++) {
        uint32_t c = *src;
        char buf[4]; size_t n;
        if (c >= 0xD800 && c <= 0xDBFF && src[1] >= 0xDC00 && src[1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (src[1] - 0xDC00);
            src++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = '?';
        }
        if (c < 0x80)       { buf[0] = (char)c; n = 1; }
        else if (c < 0x800) { buf[0] = (char)(0xC0 | c >> 6); buf[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
        else if (c < 0x10000) {
            buf[0] = (char)(0xE0 | c >> 12); buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (c & 0x3F)); n = 3;
        } else {
            buf[0] = (char)(0xF0 | c >> 18); buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
            buf[2] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[3] = (char)(0x80 | (c & 0x3F)); n = 4;
        }
        if (o + n >= cap) break;
        memcpy(dst + o, buf, n); o += n;
    }
    dst[o] = '\0';
}
