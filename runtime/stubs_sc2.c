/* stubs_sc2.c — IsDebuggerPresent, mappings nommés, événements, sur POSIX.
 *
 * Sémantique et preuves : stubs_sc2.h (CARTE-INIT §1.2, §1.4, §1.5).
 */
#define _GNU_SOURCE
#include "stubs_sc2.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define NAME_MAX_UTF8 512

/* ========================================================================== */
/* Débogueur                                                                  */
/* ========================================================================== */

BOOL IsDebuggerPresent(void)
{
    /* 009B3550 : AAAC = (IsDebuggerPresent() == 0) quand shm[+0x20] != 4 ;
     * TRUE ⇒ AAAC = 0 ⇒ 00809B90 rend 1 ⇒ pas de « Must run from the launcher. » */
    return TRUE;
}

/* ========================================================================== */
/* Mappings                                                                   */
/* ========================================================================== */

typedef struct {
    w32_object hdr;
    void      *base;
    size_t     size;
    DWORD      protect;
} w32_mapping;

typedef struct { size_t size; DWORD protect; } mapping_ctx;

static BOOL mapping_init(w32_object *o, void *vctx)
{
    w32_mapping *m = (w32_mapping *)o;
    const mapping_ctx *c = (const mapping_ctx *)vctx;
    int flags = MAP_SHARED | MAP_ANONYMOUS;
    void *p = w32_mmap_low(c->size, PROT_READ | PROT_WRITE, flags);   /* VA < 4 Go */
    if (p == MAP_FAILED) p = mmap(NULL, c->size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    m->base = p;
    m->size = c->size;
    m->protect = c->protect;
    return TRUE;
}

static void mapping_destroy(w32_object *o)
{
    w32_mapping *m = (w32_mapping *)o;
    if (m->base) munmap(m->base, m->size);
}

/* Vues ouvertes : adresse rendue → section (qui garde une référence par vue). */
#define MAX_VIEWS 256
static struct { const void *addr; w32_mapping *m; } g_views[MAX_VIEWS];
static pthread_mutex_t g_views_lock = PTHREAD_MUTEX_INITIALIZER;

HANDLE CreateFileMappingA(HANDLE file, void *sa, DWORD protect,
                          DWORD size_high, DWORD size_low, LPCSTR name)
{
    (void)sa;
    if (file != INVALID_HANDLE_VALUE) {
        /* Mapping de fichier : hors du boot SC2, non implémenté. */
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    uint64_t size = ((uint64_t)size_high << 32) | size_low;
    if (size == 0 || size > (uint64_t)SIZE_MAX) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    mapping_ctx ctx = { (size_t)size, protect };
    /* Nom existant : Windows ignore la taille demandée et rend la section
     * existante avec ERROR_ALREADY_EXISTS — w32_object_create fait de même. */
    return w32_object_create(W32_OBJ_MAPPING, name, sizeof(w32_mapping),
                             mapping_init, &ctx, mapping_destroy, NULL);
}

HANDLE CreateFileMappingW(HANDLE file, void *sa, DWORD protect,
                          DWORD size_high, DWORD size_low, LPCWSTR name)
{
    char n[NAME_MAX_UTF8];
    if (name) w32_wide_to_utf8(name, n, sizeof n);
    return CreateFileMappingA(file, sa, protect, size_high, size_low, name ? n : NULL);
}

HANDLE OpenFileMappingA(DWORD access, BOOL inherit, LPCSTR name)
{
    (void)access; (void)inherit;
    return w32_object_open(W32_OBJ_MAPPING, name);
}

HANDLE OpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name)
{
    char n[NAME_MAX_UTF8];
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    w32_wide_to_utf8(name, n, sizeof n);
    return OpenFileMappingA(access, inherit, n);
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD off_high, DWORD off_low,
                     SIZE_T bytes)
{
    (void)access;   /* toujours RW en dessous ; le jeu demande FILE_MAP_WRITE */
    /* Référence prise ici ; elle devient celle de la vue (rendue par Unmap). */
    w32_mapping *m = (w32_mapping *)w32_handle_ref(mapping, W32_OBJ_MAPPING);
    if (!m) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    uint64_t off = ((uint64_t)off_high << 32) | off_low;
    if (off > m->size || (bytes && bytes > m->size - off)) {
        w32_object_unref(&m->hdr);
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    void *view = (char *)m->base + off;

    pthread_mutex_lock(&g_views_lock);
    int slot = -1;
    for (int i = 0; i < MAX_VIEWS; i++) if (!g_views[i].addr) { slot = i; break; }
    if (slot >= 0) {
        g_views[slot].addr = view;
        g_views[slot].m = m;
    }
    pthread_mutex_unlock(&g_views_lock);
    if (slot < 0) { w32_object_unref(&m->hdr); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    SetLastError(ERROR_SUCCESS);
    return view;
}

BOOL w32_view_section(const void *view, void **base, SIZE_T *size)
{
    BOOL ok = FALSE;
    pthread_mutex_lock(&g_views_lock);
    for (int i = 0; i < MAX_VIEWS; i++)
        if (g_views[i].addr == view) {
            *base = g_views[i].m->base; *size = g_views[i].m->size; ok = TRUE;
            break;
        }
    pthread_mutex_unlock(&g_views_lock);
    return ok;
}

BOOL UnmapViewOfFile(const void *view)
{
    w32_mapping *m = NULL;
    pthread_mutex_lock(&g_views_lock);
    for (int i = 0; i < MAX_VIEWS; i++)
        if (g_views[i].addr == view) { m = g_views[i].m; g_views[i].addr = NULL; g_views[i].m = NULL; break; }
    pthread_mutex_unlock(&g_views_lock);
    if (!m) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    w32_object_unref(&m->hdr);
    return TRUE;
}

/* ========================================================================== */
/* Événements                                                                 */
/* ========================================================================== */

typedef struct {
    w32_object      hdr;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             manual;
    int             signaled;
} w32_event;

typedef struct { BOOL manual, initial; } event_ctx;

static BOOL event_init(w32_object *o, void *vctx)
{
    w32_event *e = (w32_event *)o;
    const event_ctx *c = (const event_ctx *)vctx;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
#ifndef __APPLE__
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv, &ca);
    pthread_condattr_destroy(&ca);
    e->manual = c->manual != 0;
    e->signaled = c->initial != 0;
    return TRUE;
}

static void event_destroy(w32_object *o)
{
    w32_event *e = (w32_event *)o;
    pthread_cond_destroy(&e->cv);
    pthread_mutex_destroy(&e->mu);
}

HANDLE CreateEventA(void *sa, BOOL manual_reset, BOOL initial_state, LPCSTR name)
{
    (void)sa;   /* 0099CA20 passe une DACL nulle : sans objet ici */
    event_ctx ctx = { manual_reset, initial_state };
    return w32_object_create(W32_OBJ_EVENT, name, sizeof(w32_event),
                             event_init, &ctx, event_destroy, NULL);
}

HANDLE CreateEventW(void *sa, BOOL manual_reset, BOOL initial_state, LPCWSTR name)
{
    char n[NAME_MAX_UTF8];
    if (name) w32_wide_to_utf8(name, n, sizeof n);
    return CreateEventA(sa, manual_reset, initial_state, name ? n : NULL);
}

HANDLE OpenEventA(DWORD access, BOOL inherit, LPCSTR name)
{
    (void)access; (void)inherit;
    return w32_object_open(W32_OBJ_EVENT, name);
}

HANDLE OpenEventW(DWORD access, BOOL inherit, LPCWSTR name)
{
    char n[NAME_MAX_UTF8];
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    w32_wide_to_utf8(name, n, sizeof n);
    return OpenEventA(access, inherit, n);
}

static BOOL event_set_state(HANDLE h, int state)
{
    w32_event *e = (w32_event *)w32_handle_ref(h, W32_OBJ_EVENT);
    if (!e) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&e->mu);
    e->signaled = state;
    if (state) {
        if (e->manual) pthread_cond_broadcast(&e->cv);
        else           pthread_cond_signal(&e->cv);
    }
    pthread_mutex_unlock(&e->mu);
    w32_object_unref(&e->hdr);
    return TRUE;
}

BOOL SetEvent(HANDLE ev)   { return event_set_state(ev, 1); }
BOOL ResetEvent(HANDLE ev) { return event_set_state(ev, 0); }

DWORD WaitForSingleObject(HANDLE h, DWORD timeout_ms)
{
    /* Référence tenue le temps de l'attente : un autre thread peut fermer le handle. */
    w32_event *e = (w32_event *)w32_handle_ref(h, W32_OBJ_EVENT);
    if (!e) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    struct timespec dl;
    if (timeout_ms != INFINITE) {
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec  += timeout_ms / 1000u;
        dl.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    }
    DWORD r = WAIT_OBJECT_0;
    pthread_mutex_lock(&e->mu);
    while (!e->signaled) {
        if (timeout_ms == 0) { r = WAIT_TIMEOUT; break; }
        int rc = timeout_ms == INFINITE ? pthread_cond_wait(&e->cv, &e->mu)
                                        : pthread_cond_timedwait(&e->cv, &e->mu, &dl);
        if (rc == ETIMEDOUT && !e->signaled) { r = WAIT_TIMEOUT; break; }
    }
    if (r == WAIT_OBJECT_0 && !e->manual) e->signaled = 0;   /* auto-reset consommé */
    pthread_mutex_unlock(&e->mu);
    w32_object_unref(&e->hdr);
    return r;
}
