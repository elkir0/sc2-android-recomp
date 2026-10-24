/* sc2_iat.c — les 755 imports de SC2.exe vus par le code lifté.
 *
 * Chaque slot IAT reçoit la VA fictive SC2_IAT_STUB_BASE + 16*i (ou, pour les
 * imports de DONNÉES, l'adresse d'une variable invitée). Un `call [__imp_X]`
 * lifté fait push32(ret) ; dispatch(rd32(slot)) → sc2_iat_call :
 *   - trace « [IAT] DLL!nom(args) = ret » (3 premiers appels, puis compte) ;
 *   - stub implémenté : exécuté, puis dépilage stdcall (ret + 4*argc) ou cdecl (ret) ;
 *   - stub absent : ARRÊT sur point identifié (nom, adresse de retour).
 * Les fibres invitées (008092D0) reposent sur win32_compat (vraies fibres hôtes),
 * une pile invitée et un CPU par fibre. Les objets nommés passent par stubs_sc2.
 */
#define _GNU_SOURCE
#ifdef __APPLE__
#include <stdint.h>
#include <pthread.h>
static unsigned int mach_thread_id(void);
#define gettid() mach_thread_id()
#endif
#include "sc2_host.h"
#include "sc2_lifted.h"
#include "sc2_image.h"
#include "sc2_imports.h"
#include "stubs_sc2.h"
#include "win32_compat.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#define A(i)  (a[(i)])
#define STR(va) ((const char *)GPTR(va))

#include "sc2_iat.h"
#include "sc2_d3d9.h"
#include "sc2_escarmouche.h"            /* V4.10 : carte d'escarmouche du lanceur simulé (SC2_ESCARMOUCHE) */
#include "sc2_attentes.c"              /* SC2_ATTENTES=1 : temps bloqué par fil/cause (même motif que
                                        * sc2_entrees_banc.c dans sc2_input_sim.c — aucune liste de build) */

/* ========================================================================== */
/* Chaînes larges invitées (UTF-16)                                           */
/* ========================================================================== */

static uint32_t wlen(uint32_t w) { uint32_t n = 0; while (rd16(w + 2 * n)) n++; return n; }
static void w2u(uint32_t w, char *o, size_t cap)
{
    if (!w) { snprintf(o, cap, "(null)"); return; }
    w32_wide_to_utf8((LPCWSTR)GPTR(w), o, cap);
}
static uint32_t u2w_new(const char *s)          /* ASCII → UTF-16 dans l'arène */
{
    size_t n = strlen(s);
    uint32_t w = sc2_alloc((uint32_t)(2 * n + 2));
    for (size_t i = 0; i < n; i++) wr16(w + 2 * (uint32_t)i, (uint8_t)s[i]);
    return w;
}

/* ========================================================================== */
/* printf invité : arguments lus sur la pile invitée                          */
/* ========================================================================== */

/* Formate fmt (étroit ou large) avec la liste d'arguments invitée `ap` (VA d'un
 * va_list MSVC = pointeur sur le premier argument). Rend la longueur produite. */
static int gfmt(char *out, size_t cap, uint32_t fmt, int wide, uint32_t ap)
{
    size_t o = 0;
    char spec[32];
#define CH(k) (wide ? rd16(fmt + 2 * (k)) : rd8(fmt + (k)))
#define PUT(s) do { const char *s_ = (s); while (*s_) { if (o + 1 < cap) out[o] = *s_; o++; s_++; } } while (0)
    for (uint32_t k = 0; CH(k); ) {
        uint32_t ch = CH(k);
        if (ch != '%') {
            char b[8];
            if (ch < 0x80) { b[0] = (char)ch; b[1] = 0; } else { b[0] = '?'; b[1] = 0; }
            PUT(b); k++; continue;
        }
        if (CH(k + 1) == '%') { PUT("%"); k += 2; continue; }
        size_t s = 0; spec[s++] = '%'; k++;
        while (strchr("-+ #0", (int)CH(k)) && s < 20) spec[s++] = (char)CH(k++);
        if (CH(k) == '*') { s += (size_t)snprintf(spec + s, sizeof spec - s, "%d", (int)rd32(ap)); ap += 4; k++; }
        else while (isdigit((int)CH(k)) && s < 24) spec[s++] = (char)CH(k++);
        if (CH(k) == '.') {
            spec[s++] = '.'; k++;
            if (CH(k) == '*') { s += (size_t)snprintf(spec + s, sizeof spec - s, "%d", (int)rd32(ap)); ap += 4; k++; }
            else while (isdigit((int)CH(k)) && s < 28) spec[s++] = (char)CH(k++);
        }
        int l64 = 0, lng = 0, sht = 0;
        for (;;) {
            if (CH(k) == 'I' && CH(k + 1) == '6' && CH(k + 2) == '4') { l64 = 1; k += 3; }
            else if (CH(k) == 'l' && CH(k + 1) == 'l') { l64 = 1; k += 2; }
            else if (CH(k) == 'l') { lng = 1; k++; }
            else if (CH(k) == 'h') { sht = 1; k++; }
            else if (CH(k) == 'I' || CH(k) == 'z' || CH(k) == 't' || CH(k) == 'L') k++;
            else break;
        }
        uint32_t conv = CH(k++);
        char buf[512];
        spec[s] = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            char f[40];
            if (l64) {
                uint64_t v = rd32(ap) | ((uint64_t)rd32(ap + 4) << 32); ap += 8;
                snprintf(f, sizeof f, "%sll%c", spec, (char)conv);
                snprintf(buf, sizeof buf, f, v);
            } else {
                uint32_t v = rd32(ap); ap += 4;
                snprintf(f, sizeof f, "%s%c", spec, (char)conv);
                if (conv == 'd' || conv == 'i') snprintf(buf, sizeof buf, f, (int)v);
                else snprintf(buf, sizeof buf, f, v);
            }
            PUT(buf); break;
        }
        case 'c': case 'C': {
            uint32_t v = rd32(ap); ap += 4;
            buf[0] = (char)(v < 0x80 ? v : '?'); buf[1] = 0; PUT(buf); break;
        }
        case 'p': { uint32_t v = rd32(ap); ap += 4; snprintf(buf, sizeof buf, "%08X", v); PUT(buf); break; }
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': case 'a': {
            double d; uint64_t bits = rd32(ap) | ((uint64_t)rd32(ap + 4) << 32); ap += 8;
            memcpy(&d, &bits, 8);
            char f[40]; snprintf(f, sizeof f, "%s%c", spec, (char)conv);
            snprintf(buf, sizeof buf, f, d); PUT(buf); break;
        }
        case 's': case 'S': {
            uint32_t v = rd32(ap); ap += 4;
            /* MSVC : %ls large, %hs étroit ; sinon %s suit le format, %S l'inverse. */
            int w = lng ? 1 : sht ? 0 : (conv == 's' ? wide : !wide);
            char txt[4096];
            if (!v) snprintf(txt, sizeof txt, "(null)");
            else if (w) w2u(v, txt, sizeof txt);
            else snprintf(txt, sizeof txt, "%s", STR(v));
            char f[40]; snprintf(f, sizeof f, "%ss", spec);
            char big[4200]; snprintf(big, sizeof big, f, txt); PUT(big); break;
        }
        case 'n': ap += 4; break;
        default: buf[0] = '%'; buf[1] = (char)conv; buf[2] = 0; PUT(buf); break;
        }
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return (int)o;
#undef CH
#undef PUT
}

/* Écrit le résultat d'un formatage dans un tampon invité (étroit ou large). */
static int gput(uint32_t dst, uint32_t cap, const char *s, int n, int wide)
{
    if (!dst || !cap) return n;
    uint32_t k = 0;
    for (; k < (uint32_t)n && k + 1 < cap; k++) {
        if (wide) wr16(dst + 2 * k, (uint8_t)s[k]); else wr8(dst + k, (uint8_t)s[k]);
    }
    if (wide) wr16(dst + 2 * k, 0); else wr8(dst + k, 0);
    return n;
}

/* ========================================================================== */
/* Fibres invitées                                                            */
/* ========================================================================== */

typedef struct {
    uint32_t gva;              /* bloc invité : [0] = param (GetFiberData inline lit fs:[0x10] → [0]) */
    LPVOID   host;             /* fibre win32_compat */
    CPU     *cpu;              /* état machine de la fibre */
    uint32_t start, param, stack_top, stack_limit;
    sc2_shadow *ss;
} gfiber;
static gfiber g_fib[64];
static int g_nfib;
static __thread gfiber *g_cur_fib;

static gfiber *fib_of(uint32_t gva)
{
    for (int i = 0; i < g_nfib; i++) if (g_fib[i].gva == gva) return &g_fib[i];
    return NULL;
}

static void fiber_host_entry(LPVOID p)
{
    gfiber *f = (gfiber *)p;
    CPU *c = f->cpu;
    sc2_log("[FIBRE] démarrage de la fibre 0x%08X : routine invitée 0x%08X, param 0x%08X, "
            "pile invitée 0x%08X..0x%08X", f->gva, f->start, f->param, f->stack_limit, f->stack_top);
    c->esp = f->stack_top;
    uint32_t arg = f->param;
    sc2_call_guest(c, f->start, 1, &arg, 0);
    sc2_stop(0, "la routine de la fibre 0x%08X (0x%08X) a retourné : ExitThread", f->gva, f->start);
}

static uint32_t h_ConvertThreadToFiber(CPU *c, const uint32_t *a)
{
    if (g_cur_fib) { SetLastError(ERROR_ALREADY_FIBER); return 0; }
    gfiber *f = &g_fib[g_nfib++];
    f->gva = sc2_alloc(64);
    wr32(f->gva, A(0));
    f->host = GetCurrentFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
    f->cpu = c; f->ss = g_sc2_ss; c->fantome = g_sc2_ss;   /* passe 10r : pile fantôme portée par le CPU */
    f->stack_top = rd32(g_sc2_teb + 4); f->stack_limit = rd32(g_sc2_teb + 8);
    g_cur_fib = f;
    wr32(g_sc2_teb + 0x10, f->gva);
    sc2_log("[FIBRE] ConvertThreadToFiber : fibre principale 0x%08X", f->gva);
    return f->gva;
}
static uint32_t h_CreateFiberEx(CPU *c, const uint32_t *a)
{
    (void)c;
    uint32_t reserve = A(1) > A(0) ? A(1) : A(0);
    if (!reserve) reserve = 0x100000;
    gfiber *f = &g_fib[g_nfib++];
    f->gva = sc2_alloc(64);
    wr32(f->gva, A(4));
    f->start = A(3); f->param = A(4);
    f->stack_top = sc2_reserve_stack(reserve, &f->stack_limit);
    f->cpu = (CPU *)calloc(1, sizeof(CPU));
    f->cpu->fs_base = g_sc2_teb;
    f->ss = (sc2_shadow *)calloc(1, sizeof(sc2_shadow));
    f->cpu->fantome = f->ss;                                /* passe 10r : pile fantôme portée par le CPU de la fibre */
    sc2_ss_register(f->ss, "fibre 0x%08X (routine 0x%08X)", f->gva, f->start);
    /* Pile NATIVE de la fibre hôte : 64 Mo (le C lifté à -O0 a de gros cadres). */
    f->host = CreateFiberEx(0, 64u << 20, 0, fiber_host_entry, f);
    sc2_log("[FIBRE] CreateFiberEx(pile invitée 0x%X = %u Mo, routine 0x%08X) → 0x%08X",
            reserve, reserve >> 20, f->start, f->gva);
    return f->host ? f->gva : 0;
}
static uint32_t h_CreateFiber(CPU *c, const uint32_t *a)
{
    uint32_t b[5] = { 0, A(0), 0, A(1), A(2) };
    return h_CreateFiberEx(c, b);
}
static uint32_t h_SwitchToFiber(CPU *c, const uint32_t *a)
{
    (void)c;
    gfiber *to = fib_of(A(0));
    if (!to) sc2_stop(7, "SwitchToFiber(0x%08X) : fibre inconnue", A(0));
    if (to == g_cur_fib) return 0;
    sc2_trace("[FIBRE] SwitchToFiber 0x%08X → 0x%08X", g_cur_fib ? g_cur_fib->gva : 0, to->gva);
    if (g_cur_fib) g_cur_fib->ss = g_sc2_ss;
    g_cur_fib = to;
    g_sc2_ss = to->ss;
    wr32(g_sc2_teb + 0x10, to->gva);
    wr32(g_sc2_teb + 0x04, to->stack_top);
    wr32(g_sc2_teb + 0x08, to->stack_limit);
    SwitchToFiber(to->host);
    return 0;                                     /* on revient ici quand on rebascule vers nous */
}
static uint32_t h_DeleteFiber(CPU *c, const uint32_t *a)
{
    (void)c;
    gfiber *f = fib_of(A(0));
    if (f && f != g_cur_fib && f->start) { DeleteFiber(f->host); f->gva = 0; }
    return 0;
}

/* ========================================================================== */
/* CRT : démarrage                                                            */
/* ========================================================================== */

static uint32_t g_fmode, g_commode, g_errno, g_wcmdln_var, g_huge, g_adjust_fdiv, g_iob;
static const char *g_cmdline = "";
static int g_init_ok, g_init_ko, g_init_total;

/* _initterm / _initterm_e : la table des initialiseurs C/C++. Mode TOLÉRANT : un
 * initialiseur qui atteint du code non lifté est abandonné (état invité restauré)
 * et compté, au lieu d'arrêter le boot avant WinMain (choix POC, documenté). */
/* Initialiseurs STRICTS : ceux dont le chemin principal dépend (ex. l'init Storm
 * 0x013C74C0, qui pose l'allocateur 0x0169B5DC lu par operator new). Pour eux,
 * pas de mode tolérant : un manque arrête le run et la boucle le résout.
 * Liste : $SC2_STRICT_INITS (fichier, une 0x… par ligne, # = commentaire). */
static uint32_t g_strict[256]; static int g_nstrict = -1;
static int is_strict(uint32_t fn)
{
    if (g_nstrict < 0) {
        g_nstrict = 0;
        const char *f = getenv("SC2_STRICT_INITS");
        FILE *fp = f ? fopen(f, "r") : NULL;
        char l[256];
        while (fp && fgets(l, sizeof l, fp) && g_nstrict < 256) {
            char *e; unsigned long v = strtoul(l, &e, 16);
            if (e != l && v) g_strict[g_nstrict++] = (uint32_t)v;
        }
        if (fp) fclose(fp);
        if (g_nstrict) sc2_log("[CRT] %d initialiseurs stricts (%s)", g_nstrict, f);
    }
    for (int i = 0; i < g_nstrict; i++) if (g_strict[i] == fn) return 1;
    return 0;
}

static uint32_t run_initterm(CPU *c, uint32_t lo, uint32_t hi, int with_ret)
{
    int ok = 0, ko = 0, vides = 0;
    sc2_log("[CRT] _initterm%s(0x%08X, 0x%08X) : %u entrées", with_ret ? "_e" : "", lo, hi, (hi - lo) / 4);
    for (uint32_t p = lo; p < hi; p += 4) {
        uint32_t fn = rd32(p);
        if (!fn) { vides++; continue; }
        if (is_strict(fn)) {
            sc2_log("[CRT] initialiseur STRICT 0x%08X", fn);
            uint32_t r = sc2_call_guest(c, fn, 0, NULL, 1);
            ok++;
            if (with_ret && r) return r;
            continue;
        }
        CPU save = *c;
        int ss_n = g_sc2_ss->n;
        uint32_t *saut = c->saut;                       /* 7j : rendu après un siglongjmp de reprise */
        sigjmp_buf jb;
        if (sigsetjmp(jb, 1) == 0) {
            g_sc2_recover = &jb; g_sc2_recover_va = fn;
            uint32_t r = sc2_call_guest(c, fn, 0, NULL, 1);
            g_sc2_recover = NULL;
            ok++;
            if (with_ret && r) {
                sc2_log("[CRT] initialiseur 0x%08X a rendu %u : _initterm_e échoue", fn, r);
                return r;
            }
        } else {
            g_sc2_recover = NULL;
            *c = save; g_sc2_ss->n = ss_n; c->saut = saut;
            ko++;
        }
    }
    g_init_ok += ok; g_init_ko += ko; g_init_total += ok + ko;
    sc2_log("[CRT] _initterm%s : %d initialiseurs exécutés, %d abandonnés (code non lifté), %d vides",
            with_ret ? "_e" : "", ok, ko, vides);
    return 0;
}
static uint32_t h__initterm(CPU *c, const uint32_t *a)   { run_initterm(c, A(0), A(1), 0); return 0; }
static uint32_t h__initterm_e(CPU *c, const uint32_t *a) { return run_initterm(c, A(0), A(1), 1); }

/* Passe 7g : arguments de la ligne de commande du jeu, SC2_ARGS (ex. l'invocation « Tester le document »
 * de l'éditeur : -run <carte> -testmod … -displaymode 0). Découpage Windows simplifié : espaces, et
 * guillemets doubles pour un argument contenant des espaces. Sans SC2_ARGS : « SC2.exe » seul (avant). */
static int args_decouper(const char *s, char out[][512], int max)
{
    int n = 0;
    while (s && *s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        size_t k = 0; int q = 0;
        while (*s && (q || *s != ' ')) {
            if (*s == '"') { q = !q; s++; continue; }
            if (k < 511) out[n][k++] = *s;
            s++;
        }
        out[n++][k] = 0;
    }
    return n;
}
static uint32_t h___wgetmainargs(CPU *c, const uint32_t *a)
{
    (void)c;
    static char args[32][512];
    int n = args_decouper(getenv("SC2_ARGS"), args, 32);
    uint32_t argv = sc2_alloc(4u * (uint32_t)(n + 2)), envp = sc2_alloc(4);
    wr32(argv, u2w_new("SC2.exe"));
    for (int i = 0; i < n; i++) wr32(argv + 4u * (uint32_t)(i + 1), u2w_new(args[i]));
    wr32(argv + 4u * (uint32_t)(n + 1), 0);
    wr32(A(0), (uint32_t)(n + 1)); wr32(A(1), argv); wr32(A(2), envp);
    if (n) sc2_log("[ARGS] __wgetmainargs : %d argument(s) de SC2_ARGS", n);
    return 0;
}
static uint32_t h_ret0(CPU *c, const uint32_t *a)  { (void)c; (void)a; return 0; }
static uint32_t h_ret1(CPU *c, const uint32_t *a)  { (void)c; (void)a; return 1; }
/* Fix Fold 16/10 (M4) : CreateProcessW (crash-monitor externe) -> FAUX SUCCÈS.
 * Le jeu quitte (TerminateProcess) si le lancement échoue ; on remplit
 * lpProcessInformation (arg 10) de valeurs fictives et on retourne VRAI. */
static uint32_t h_CreateProcessW_fake(CPU *c, const uint32_t *a)
{
    (void)c; (void)a;
    uint32_t lpi = A(9);
    if (lpi) { wr32(lpi,     0xF00D0001u); /* hProcess */
               wr32(lpi + 4, 0xF00D0002u); /* hThread  */
               wr32(lpi + 8, 0x1234);      /* dwProcessId */
               wr32(lpi + 12, 0x1235);     /* dwThreadId */ }
    sc2_log("[IAT] CreateProcessW -> faux succès (monitor fictif pid 0x1234)");
    return 1;
}
static uint32_t h_arg0(CPU *c, const uint32_t *a)  { (void)c; return A(0); }
static uint32_t h___p__fmode(CPU *c, const uint32_t *a)   { (void)c; (void)a; return g_fmode; }
static uint32_t h___p__commode(CPU *c, const uint32_t *a) { (void)c; (void)a; return g_commode; }
static uint32_t h__errno(CPU *c, const uint32_t *a)       { (void)c; (void)a; return g_errno; }
static uint32_t h___iob_func(CPU *c, const uint32_t *a)   { (void)c; (void)a; return g_iob; }

static uint32_t h_exit(CPU *c, const uint32_t *a)
{
    (void)c;
    sc2_stop(0, "le jeu appelle exit(%d) : fin propre du processus invité", (int)A(0));
}
static uint32_t h_ExitProcess(CPU *c, const uint32_t *a)
{
    (void)c;
    sc2_stop(0, "le jeu appelle ExitProcess(%d)", (int)A(0));
}
static uint32_t h_TerminateProcess(CPU *c, const uint32_t *a)
{
    sc2_stop(8, "le jeu appelle TerminateProcess(0x%08X, %d) — appelant 0x%08X", A(0), (int)A(1),
             rd32(c->esp));
}
static uint32_t h_amsg_exit(CPU *c, const uint32_t *a)
{
    sc2_stop(9, "_amsg_exit(%d) — erreur fatale du CRT, appelant 0x%08X", (int)A(0), rd32(c->esp));
}
static uint32_t h_fatal(CPU *c, const uint32_t *a)
{
    (void)a;
    sc2_stop(10, "routine fatale du CRT (abort/terminate/purecall/watson) — appelant 0x%08X", rd32(c->esp));
}

/* ========================================================================== */
/* CRT : mémoire, chaînes, maths                                              */
/* ========================================================================== */

static uint32_t h_malloc(CPU *c, const uint32_t *a)  { (void)c; return sc2_alloc_de(A(0), SC2_MO_MALLOC); }
static uint32_t h_free(CPU *c, const uint32_t *a)    { (void)c; sc2_free(A(0)); return 0; }
static uint32_t h_realloc(CPU *c, const uint32_t *a) { (void)c; return sc2_realloc(A(0), A(1)); }
static uint32_t h__aligned_malloc(CPU *c, const uint32_t *a)
{
    (void)c;
    uint32_t al = A(1) < 16 ? 16 : A(1);
    uint32_t p = sc2_alloc_de(A(0) + al, SC2_MO_MALLOC);
    return (p + al - 1) & ~(al - 1);
}
static uint32_t h_memset(CPU *c, const uint32_t *a)  { (void)c; memset(GPTR(A(0)), (int)A(1), A(2)); return A(0); }
static uint32_t h_memcpy(CPU *c, const uint32_t *a)  { (void)c; memmove(GPTR(A(0)), GPTR(A(1)), A(2)); return A(0); }
static uint32_t h_memcpy_s(CPU *c, const uint32_t *a)
{
    (void)c;
    if (A(3) > A(1)) return 34;                                   /* ERANGE */
    memmove(GPTR(A(0)), GPTR(A(2)), A(3));
    return 0;
}
static uint32_t h_memchr(CPU *c, const uint32_t *a)
{
    (void)c;
    void *p = memchr(GPTR(A(0)), (int)A(1), A(2));
    return p ? GVA_OF(p) : 0;
}
static uint32_t h_strncmp(CPU *c, const uint32_t *a)  { (void)c; return (uint32_t)strncmp(STR(A(0)), STR(A(1)), A(2)); }
static uint32_t h__stricmp(CPU *c, const uint32_t *a) { (void)c; return (uint32_t)strcasecmp(STR(A(0)), STR(A(1))); }
static uint32_t h__strnicmp(CPU *c, const uint32_t *a){ (void)c; return (uint32_t)strncasecmp(STR(A(0)), STR(A(1)), A(2)); }
static uint32_t h_strchr(CPU *c, const uint32_t *a)   { (void)c; const char *p = strchr(STR(A(0)), (int)A(1)); return p ? GVA_OF(p) : 0; }
static uint32_t h_strrchr(CPU *c, const uint32_t *a)  { (void)c; const char *p = strrchr(STR(A(0)), (int)A(1)); return p ? GVA_OF(p) : 0; }
static uint32_t h_strstr(CPU *c, const uint32_t *a)   { (void)c; const char *p = strstr(STR(A(0)), STR(A(1))); return p ? GVA_OF(p) : 0; }
static uint32_t h_strspn(CPU *c, const uint32_t *a)   { (void)c; return (uint32_t)strspn(STR(A(0)), STR(A(1))); }
static uint32_t h_strcspn(CPU *c, const uint32_t *a)  { (void)c; return (uint32_t)strcspn(STR(A(0)), STR(A(1))); }
static uint32_t h_strncpy(CPU *c, const uint32_t *a)  { (void)c; strncpy((char *)GPTR(A(0)), STR(A(1)), A(2)); return A(0); }
static uint32_t h_strncat(CPU *c, const uint32_t *a)  { (void)c; strncat((char *)GPTR(A(0)), STR(A(1)), A(2)); return A(0); }
static uint32_t h_strcpy_s(CPU *c, const uint32_t *a)
{
    (void)c;
    size_t n = strlen(STR(A(2)));
    if (n + 1 > A(1)) { if (A(1)) wr8(A(0), 0); return 34; }
    memmove(GPTR(A(0)), STR(A(2)), n + 1);
    return 0;
}
static uint32_t h_strncpy_s(CPU *c, const uint32_t *a)    /* (dst, dstsz, src, count) */
{
    (void)c;
    size_t n = strnlen(STR(A(2)), A(3) == 0xFFFFFFFFu ? (size_t)-1 : A(3));
    if (n + 1 > A(1)) n = A(1) ? A(1) - 1 : 0;
    memmove(GPTR(A(0)), STR(A(2)), n);
    if (A(1)) wr8(A(0) + (uint32_t)n, 0);
    return 0;
}
static uint32_t h_strcat_s(CPU *c, const uint32_t *a)
{
    (void)c;
    size_t d = strnlen(STR(A(0)), A(1)), n = strlen(STR(A(2)));
    if (d + n + 1 > A(1)) return 34;
    memmove((char *)GPTR(A(0)) + d, STR(A(2)), n + 1);
    return 0;
}
static uint32_t h_wcscpy_s(CPU *c, const uint32_t *a)
{
    (void)c;
    uint32_t n = wlen(A(2));
    if (n + 1 > A(1)) { if (A(1)) wr16(A(0), 0); return 34; }
    memmove(GPTR(A(0)), GPTR(A(2)), 2 * (n + 1));
    return 0;
}
static uint32_t h_wcscat_s(CPU *c, const uint32_t *a)
{
    (void)c;
    uint32_t d = wlen(A(0)), n = wlen(A(2));
    if (d + n + 1 > A(1)) return 34;
    memmove((char *)GPTR(A(0)) + 2 * d, GPTR(A(2)), 2 * (n + 1));
    return 0;
}
static uint32_t h_wcsrchr(CPU *c, const uint32_t *a)
{
    (void)c;
    uint32_t r = 0, n = wlen(A(0));
    for (uint32_t i = 0; i <= n; i++) if (rd16(A(0) + 2 * i) == (uint16_t)A(1)) r = A(0) + 2 * i;
    return r;
}
static uint32_t h__wcsnicmp(CPU *c, const uint32_t *a)
{
    (void)c;
    for (uint32_t i = 0; i < A(2); i++) {
        uint16_t x = rd16(A(0) + 2 * i), y = rd16(A(1) + 2 * i);
        if (x < 128) x = (uint16_t)tolower(x);
        if (y < 128) y = (uint16_t)tolower(y);
        if (x != y) return (uint32_t)((int)x - (int)y);
        if (!x) break;
    }
    return 0;
}
static uint32_t h__wcsicoll(CPU *c, const uint32_t *a) { uint32_t b[3] = { A(0), A(1), 0xFFFFFFFFu }; return h__wcsnicmp(c, b); }
static uint32_t h_wcscoll(CPU *c, const uint32_t *a)
{
    (void)c;
    for (uint32_t i = 0;; i++) {
        uint16_t x = rd16(A(0) + 2 * i), y = rd16(A(1) + 2 * i);
        if (x != y) return (uint32_t)((int)x - (int)y);
        if (!x) return 0;
    }
}
static uint32_t h_toupper(CPU *c, const uint32_t *a)  { (void)c; return (uint32_t)toupper((int)A(0)); }
static uint32_t h_tolower(CPU *c, const uint32_t *a)  { (void)c; return (uint32_t)tolower((int)A(0)); }
static uint32_t h_isspace(CPU *c, const uint32_t *a)  { (void)c; return A(0) < 256 && isspace((int)A(0)) ? 8 : 0; }
static uint32_t h_isdigit(CPU *c, const uint32_t *a)  { (void)c; return A(0) < 256 && isdigit((int)A(0)) ? 4 : 0; }
static uint32_t h_isalpha(CPU *c, const uint32_t *a)  { (void)c; return A(0) < 256 && isalpha((int)A(0)) ? 0x103 : 0; }
static uint32_t h_isxdigit(CPU *c, const uint32_t *a) { (void)c; return A(0) < 256 && isxdigit((int)A(0)) ? 0x80 : 0; }
static uint32_t h_isprint(CPU *c, const uint32_t *a)  { (void)c; return A(0) < 256 && isprint((int)A(0)) ? 0x157 : 0; }
/* Passe 10n : long fait 32 bits sous Windows (MSVCR80), 64 sur l'hôte LP64. Hors bornes, strtol/atol/atoi de
 * MSVCR80 saturent à INT32_MIN/INT32_MAX (l'hôte tronquait : « 4294967295 » → -1 au lieu de 0x7FFFFFFF) et strtoul
 * à 0xFFFFFFFF dès que la valeur absolue dépasse 32 bits (l'hôte tronquait : « 4294967296 » → 0). */
static uint32_t msvc_strtol(const char *s, char **e, int base)
{
    long long v = strtoll(s, e, base);
    return (uint32_t)(int32_t)(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v);
}
static uint32_t msvc_strtoul(const char *s, char **e, int base)
{
    errno = 0;
    unsigned long long v = strtoull(s, e, base);
    const char *p = s; while (isspace((unsigned char)*p)) p++;
    unsigned long long abs = *p == '-' ? 0ull - v : v;          /* strtoull a déjà appliqué le signe */
    return errno == ERANGE || abs > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v;
}
static uint32_t h_atoi(CPU *c, const uint32_t *a)     { (void)c; char *e; return msvc_strtol(STR(A(0)), &e, 10); }
static uint32_t h_strtol(CPU *c, const uint32_t *a)
{
    (void)c; char *e; uint32_t v = msvc_strtol(STR(A(0)), &e, (int)A(2));
    if (A(1)) wr32(A(1), GVA_OF(e));
    return v;
}
static uint32_t h_strtoul(CPU *c, const uint32_t *a)
{
    (void)c; char *e; uint32_t v = msvc_strtoul(STR(A(0)), &e, (int)A(2));
    if (A(1)) wr32(A(1), GVA_OF(e));
    return v;
}
static uint32_t h__strtoi64(CPU *c, const uint32_t *a)
{
    char *e; long long v = strtoll(STR(A(0)), &e, (int)A(2));
    if (A(1)) wr32(A(1), GVA_OF(e));
    c->edx = (uint32_t)((uint64_t)v >> 32);
    return (uint32_t)v;
}
static uint32_t h__atoi64(CPU *c, const uint32_t *a)
{
    long long v = atoll(STR(A(0)));
    c->edx = (uint32_t)((uint64_t)v >> 32);
    return (uint32_t)v;
}
/* Fonctions qui rendent un double dans st(0) : l'eax rendu est ignoré. */
static double argd(const uint32_t *a, int i) { double d; uint64_t b = a[i] | ((uint64_t)a[i + 1] << 32); memcpy(&d, &b, 8); return d; }
static uint32_t h_atof(CPU *c, const uint32_t *a)  { fpush(c, atof(STR(A(0)))); return 0; }
static uint32_t h_strtod(CPU *c, const uint32_t *a)
{
    char *e; double v = strtod(STR(A(0)), &e);
    if (A(1)) wr32(A(1), GVA_OF(e));
    fpush(c, v); return 0;
}
static uint32_t h_floor(CPU *c, const uint32_t *a) { fpush(c, floor(argd(a, 0))); return 0; }
static uint32_t h_ceil(CPU *c, const uint32_t *a)  { fpush(c, ceil(argd(a, 0))); return 0; }
static uint32_t h_ldexp(CPU *c, const uint32_t *a) { fpush(c, ldexp(argd(a, 0), (int)a[2])); return 0; }
static uint32_t h_frexp(CPU *c, const uint32_t *a) { int e; fpush(c, frexp(argd(a, 0), &e)); wr32(a[2], (uint32_t)e); return 0; }
static uint32_t h_modf(CPU *c, const uint32_t *a)
{
    double ip, f = modf(argd(a, 0), &ip); uint64_t b; memcpy(&b, &ip, 8);
    wr32(a[2], (uint32_t)b); wr32(a[2] + 4, (uint32_t)(b >> 32));
    fpush(c, f); return 0;
}
/* _CIxxx : argument(s) dans st(0) [st(1)], résultat dans st(0). */
#define CI1(nom, f) static uint32_t h_##nom(CPU *c, const uint32_t *a) { (void)a; double x = fpop(c); fpush(c, f(x)); return 0; }
CI1(_CIsqrt, sqrt) CI1(_CIsin, sin) CI1(_CIcos, cos) CI1(_CItan, tan) CI1(_CIasin, asin)
CI1(_CIacos, acos) CI1(_CIatan, atan) CI1(_CIexp, exp) CI1(_CIlog, log) CI1(_CIlog10, log10)
static uint32_t h__CIpow(CPU *c, const uint32_t *a)   { (void)a; double y = fpop(c), x = fpop(c); fpush(c, pow(x, y)); return 0; }
static uint32_t h__CIatan2(CPU *c, const uint32_t *a) { (void)a; double x = fpop(c), y = fpop(c); fpush(c, atan2(y, x)); return 0; }
static uint32_t h__CIfmod(CPU *c, const uint32_t *a)  { (void)a; double y = fpop(c), x = fpop(c); fpush(c, fmod(x, y)); return 0; }
static uint32_t h__finite(CPU *c, const uint32_t *a)  { (void)c; return isfinite(argd(a, 0)); }

static uint32_t g_rand = 1;
static uint32_t h_rand(CPU *c, const uint32_t *a)  { (void)c; (void)a; g_rand = g_rand * 214013u + 2531011u; return (g_rand >> 16) & 0x7FFF; }
static uint32_t h_srand(CPU *c, const uint32_t *a) { (void)c; g_rand = A(0); return 0; }
static uint32_t h__time64(CPU *c, const uint32_t *a)
{
    uint64_t t = (uint64_t)time(NULL);
    if (A(0)) { wr32(A(0), (uint32_t)t); wr32(A(0) + 4, (uint32_t)(t >> 32)); }
    c->edx = (uint32_t)(t >> 32);
    return (uint32_t)t;
}
static uint32_t h_getenv(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }

/* printf : (dst, [cap,] fmt, ...) — la liste d'args commence après fmt. */
static uint32_t do_sprintf(uint32_t dst, uint32_t cap, uint32_t fmt, uint32_t ap, int wide)
{
    char tmp[16384];
    int n = gfmt(tmp, sizeof tmp, fmt, wide, ap);
    gput(dst, cap, tmp, n, wide);
    return (uint32_t)n;
}
static uint32_t h_sprintf(CPU *c, const uint32_t *a)     { (void)c; return do_sprintf(A(0), 0x7FFFFFFF, A(1), GVA_OF(&a[2]), 0); }
static uint32_t h_sprintf_s(CPU *c, const uint32_t *a)   { (void)c; return do_sprintf(A(0), A(1), A(2), GVA_OF(&a[3]), 0); }
static uint32_t h__snprintf(CPU *c, const uint32_t *a)   { (void)c; return do_sprintf(A(0), A(1), A(2), GVA_OF(&a[3]), 0); }
static uint32_t h_vsprintf(CPU *c, const uint32_t *a)    { (void)c; return do_sprintf(A(0), 0x7FFFFFFF, A(1), A(2), 0); }
static uint32_t h__vsnprintf(CPU *c, const uint32_t *a)  { (void)c; return do_sprintf(A(0), A(1), A(2), A(3), 0); }
static uint32_t h__vsnprintf_s(CPU *c, const uint32_t *a){ (void)c; return do_sprintf(A(0), A(1), A(3), A(4), 0); }
static uint32_t h__vscprintf(CPU *c, const uint32_t *a)  { (void)c; char t[16384]; return (uint32_t)gfmt(t, sizeof t, A(0), 0, A(1)); }
static uint32_t h_swprintf_s(CPU *c, const uint32_t *a)  { (void)c; return do_sprintf(A(0), A(1), A(2), GVA_OF(&a[3]), 1); }
static uint32_t h_vswprintf_s(CPU *c, const uint32_t *a) { (void)c; return do_sprintf(A(0), A(1), A(2), A(3), 1); }
static uint32_t h__vsnwprintf(CPU *c, const uint32_t *a) { (void)c; return do_sprintf(A(0), A(1), A(2), A(3), 1); }
static uint32_t h_fprintf(CPU *c, const uint32_t *a)
{
    (void)c; char t[16384]; int n = gfmt(t, sizeof t, A(1), 0, GVA_OF(&a[2]));
    sc2_log("[CRT fprintf] %s", t); return (uint32_t)n;
}

/* ========================================================================== */
/* KERNEL32                                                                   */
/* ========================================================================== */

static uint32_t h_GetSystemTimeAsFileTime(CPU *c, const uint32_t *a)
{
    (void)c; struct timeval tv; gettimeofday(&tv, NULL);
    uint64_t ft = ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10;
    wr32(A(0), (uint32_t)ft); wr32(A(0) + 4, (uint32_t)(ft >> 32)); return 0;
}
static uint32_t h_GetCurrentProcessId(CPU *c, const uint32_t *a) { (void)c; (void)a; return (uint32_t)getpid(); }
#ifdef __APPLE__
/* GetCurrentThreadId : identifiant système du fil (petit entier, comme gettid sous Linux et Android). L'ancien
 * pthread_self() tronqué était un pointeur à bit de poids fort, jamais vu sous Windows (banc natif macOS, 19/10). */
static unsigned int mach_thread_id(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return (unsigned int)t; }
#endif
static uint32_t h_GetCurrentThreadId(CPU *c, const uint32_t *a)  { (void)c; (void)a; return (uint32_t)gettid(); }
static uint32_t h_GetCurrentProcess(CPU *c, const uint32_t *a)   { (void)c; (void)a; return 0xFFFFFFFFu; }
static uint32_t h_GetCurrentThread(CPU *c, const uint32_t *a)    { (void)c; (void)a; return 0xFFFFFFFEu; }
#include "sc2_horloge.h"                    /* passe 9j : sc2_tick_ms, horloge partagée avec GetMessageTime */
static uint32_t h_GetTickCount(CPU *c, const uint32_t *a) { (void)c; (void)a; return sc2_tick_ms(); }
static uint32_t h_QueryPerformanceCounter(CPU *c, const uint32_t *a)
{
    (void)c; uint64_t v = (uint64_t)sc2_monotone_ns();    /* 21/10 : horloge sans le temps de pause */
    wr32(A(0), (uint32_t)v); wr32(A(0) + 4, (uint32_t)(v >> 32)); return 1;
}
static uint32_t h_QueryPerformanceFrequency(CPU *c, const uint32_t *a)
{
    (void)c; wr32(A(0), 1000000000u); wr32(A(0) + 4, 0); return 1;
}
static uint32_t h_GetStartupInfoW(CPU *c, const uint32_t *a)
{
    (void)c; memset(GPTR(A(0)), 0, 68); wr32(A(0), 68); return 0;
}
static uint32_t h_InterlockedCompareExchange(CPU *c, const uint32_t *a)
{
    (void)c; return __sync_val_compare_and_swap((uint32_t *)GPTR(A(0)), A(2), A(1));
}
static uint32_t h_InterlockedExchange(CPU *c, const uint32_t *a)    { (void)c; return __sync_lock_test_and_set((uint32_t *)GPTR(A(0)), A(1)); }
static uint32_t h_InterlockedIncrement(CPU *c, const uint32_t *a)   { (void)c; return __sync_add_and_fetch((uint32_t *)GPTR(A(0)), 1u); }
static uint32_t h_InterlockedDecrement(CPU *c, const uint32_t *a)   { (void)c; return __sync_sub_and_fetch((uint32_t *)GPTR(A(0)), 1u); }
static uint32_t h_InterlockedExchangeAdd(CPU *c, const uint32_t *a) { (void)c; return __sync_fetch_and_add((uint32_t *)GPTR(A(0)), A(1)); }
static uint32_t h_Sleep(CPU *c, const uint32_t *a) { (void)c; uint64_t t0 = &sc2_att_debut ? sc2_att_debut() : 0;
    if (A(0) && A(0) < 1000) usleep(A(0) * 1000); if (t0) sc2_att_fin(ATT_SLEEP, A(0), t0, rd32(c->esp)); return 0; }
/* Passe 4 : un module SYSTÈME importé par SC2.exe (kernel32, user32…) existe :
 * handle fictif 0x7FE00000 + 0x1000*k. Passe 3 rendait NULL, et le jeu concluait
 * « Win9x » (TryEnterCriticalSection introuvable → abort, 0x00F013ED). */
static const char *const g_mod_sys[] = { "kernel32", "user32", "gdi32", "advapi32", "ntdll", "ole32",
    "shell32", "winmm", "ws2_32", "wsock32", "psapi", "imm32", "comctl32", "msvcr80", "msvcp80", "d3d9" };
static uint32_t module_sys(const char *n)
{
    char b[64]; size_t k = 0;
    for (; n[k] && k < 63; k++) b[k] = (char)tolower((unsigned char)n[k]);
    b[k] = 0;
    char *pt = strstr(b, ".dll"); if (pt) *pt = 0;
    for (unsigned i = 0; i < sizeof g_mod_sys / sizeof g_mod_sys[0]; i++)
        if (!strcmp(b, g_mod_sys[i])) return 0x7FE00000u + 0x1000u * i;
    return 0;
}
static uint32_t h_GetModuleHandleW(CPU *c, const uint32_t *a)
{
    (void)c; if (!A(0)) return SC2_IMAGE_BASE;
    char n[256]; w2u(A(0), n, sizeof n); uint32_t h = module_sys(n);
    sc2_trace("[IAT]   GetModuleHandleW(\"%s\") → 0x%08X", n, h);
    return h;
}
static uint32_t h_GetModuleHandleA(CPU *c, const uint32_t *a)
{
    (void)c; if (!A(0)) return SC2_IMAGE_BASE;
    uint32_t h = module_sys(STR(A(0)));
    sc2_trace("[IAT]   GetModuleHandleA(\"%s\") → 0x%08X", STR(A(0)), h);
    return h;
}
static uint32_t stub_par_nom(const char *nom);   /* plus bas : VA du stub d'un import implémenté */
static uint32_t h_GetModuleFileNameW(CPU *c, const uint32_t *a)
{
    (void)c; const char *p = "C:\\StarCraft II\\Versions\\Base15405\\SC2.exe";
    uint32_t n = (uint32_t)strlen(p);
    if (n + 1 > A(2)) n = A(2) ? A(2) - 1 : 0;
    for (uint32_t i = 0; i < n; i++) wr16(A(1) + 2 * i, (uint8_t)p[i]);
    if (A(2)) wr16(A(1) + 2 * n, 0);
    return n;
}
static uint32_t h_GetProcAddress(CPU *c, const uint32_t *a)
{
    (void)c;
    if (A(0) == SC2_BNET_MODULE) {                 /* Battle.net factice : ordinaux 1..4 */
        uint32_t v = A(1) < 0x10000u ? sc2_bnet_ordinal(A(1)) : 0;
        sc2_log("[BNET] GetProcAddress(Battle.net, %s%u) → %s", A(1) < 0x10000u ? "ordinal " : "", A(1) < 0x10000u ? A(1) : 0,
                v ? "stub factice" : "NULL");
        if (v) return v;
        SetLastError(127); return 0;
    }
    if (A(1) >= 0x10000) {
        uint32_t v = stub_par_nom(STR(A(1)));     /* passe 4 : import implémenté → son stub */
        sc2_trace("[IAT]   GetProcAddress(0x%08X, \"%s\") → %s", A(0), STR(A(1)), v ? "stub d'import" : "NULL (fonction absente)");
        if (v) return v;
    }
    SetLastError(127); return 0;
}
static uint32_t h_LoadLibraryW(CPU *c, const uint32_t *a)
{
    (void)c; char n[260]; w2u(A(0), n, sizeof n);
    if (!strcasecmp(n, "Battle.net.dll")) {
        sc2_log("[BNET] LoadLibraryW(\"Battle.net.dll\") depuis 0x%08X → module Battle.net FACTICE 0x%08X "
                "(le vrai client, Bnet 5760, n'est pas sur le DVD : patch en ligne)", rd32(c->esp), SC2_BNET_MODULE);
        return SC2_BNET_MODULE;
    }
    sc2_trace("[IAT]   LoadLibraryW(\"%s\") depuis 0x%08X → module fictif 0x7FF00000 (aucune fonction résolue)", n, rd32(c->esp));
    return 0x7FF00000u;
}
static uint32_t h_LoadLibraryA(CPU *c, const uint32_t *a)
{
    (void)c;
    sc2_trace("[IAT]   LoadLibraryA(\"%s\") → module fictif 0x7FF00000 (aucune fonction résolue)", STR(A(0)));
    return 0x7FF00000u;
}
static uint32_t h_GetLastError(CPU *c, const uint32_t *a) { (void)c; (void)a; return GetLastError(); }
static uint32_t h_SetLastError(CPU *c, const uint32_t *a) { (void)c; SetLastError(A(0)); return 0; }
static __thread uint32_t g_tls[64]; static uint32_t g_tls_n;   /* valeurs par thread, index globaux */
static uint32_t h_TlsAlloc(CPU *c, const uint32_t *a)    { (void)c; (void)a; uint32_t i = __sync_fetch_and_add(&g_tls_n, 1u); return i < 64 ? i : 0xFFFFFFFFu; }
static uint32_t h_TlsGetValue(CPU *c, const uint32_t *a) { (void)c; SetLastError(0); return A(0) < 64 ? g_tls[A(0)] : 0; }
static uint32_t h_TlsSetValue(CPU *c, const uint32_t *a) { (void)c; if (A(0) < 64) g_tls[A(0)] = A(1); return A(0) < 64; }
static uint32_t h_HeapAlloc(CPU *c, const uint32_t *a)   { (void)c; return sc2_alloc_de(A(2), SC2_MO_TAS); }
static uint32_t h_HeapReAlloc(CPU *c, const uint32_t *a) { (void)c; return sc2_realloc(A(2), A(3)); }
static uint32_t h_HeapFree(CPU *c, const uint32_t *a)    { (void)c; sc2_free(A(2)); return 1; }
static uint32_t h_HeapSize(CPU *c, const uint32_t *a)    { (void)c; return sc2_alloc_size(A(2)); }
static uint32_t h_HeapCreate(CPU *c, const uint32_t *a)  { (void)c; (void)a; return 0x00150000u; }
/* Passe 7h (recyclage actif seulement) : table adresse alignée → bloc d'arène, pour que VirtualFree(MEM_RELEASE)
 * rende le bloc ; sans recyclage, VirtualFree reste sans effet (comportement d'avant). */
#define NVIRT 65536
static struct { uint32_t va, bloc, n; } g_virt[NVIRT];   /* n : taille (passe 7j, pour VirtualQuery) */
static pthread_mutex_t g_virt_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_virt_lib;
/* V5.1 (24/10) — GRANULES de 64 Ko pour VirtualAlloc. Avant, chaque appel demandait taille + 0xFFFF à l'arène et
 * alignait dans le bloc : 64 Ko d'arène perdus par région (marathon acte1, retour au hub après la 4e mission : 6 137
 * régions vivantes, ≈ 390 Mo de marge sur 1 271 Mo « virt » ; le dlmalloc du jeu demande surtout des multiples de 64 Ko,
 * sans perte d'espace sous Windows). Désormais une région de ≤ 4 Mo est prise dans un TRONÇON : bloc d'arène de 16 Mo
 * (+ 64 Ko d'alignement) découpé en 256 granules de 64 Ko alignés, sans en-tête dans la région (table de bits ; premier
 * ajustement contigu, tronçons dans l'ordre). VirtualFree(MEM_RELEASE) rend ses granules (taille dans g_virt) ; au-delà
 * de deux tronçons entièrement libres, le tronçon vidé retourne à l'arène. Région remise à zéro à chaque service (contrat
 * de VirtualAlloc). Plus de 4 Mo, ou SC2_MEM_LIBERER=0 : bloc d'arène d'avant. g_virt[].bloc = 1 : région en granules. */
#define GR_TAILLE 0x10000u
#define GR_PAR_TR 256u
#define GR_MAX_TR 192                             /* 192 × 16 Mo = 3 Go : au-delà de l'arène */
#define GR_MAX_N 64u                              /* 4 Mo */
static struct { uint32_t bloc, base, libres; uint64_t bits[GR_PAR_TR / 64]; } g_gr[GR_MAX_TR];
static int g_ngr;
static pthread_mutex_t g_gr_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_gr_servis, g_gr_rendus, g_gr_tr_rendus, g_gr_tr_pris;
static void gr_marquer(int t, uint32_t i, uint32_t n, int pris)   /* sous g_gr_mu */
{
    for (uint32_t k = i; k < i + n; k++) {
        if (pris) g_gr[t].bits[k >> 6] |= 1ull << (k & 63); else g_gr[t].bits[k >> 6] &= ~(1ull << (k & 63));
    }
    g_gr[t].libres = pris ? g_gr[t].libres - n : g_gr[t].libres + n;
}
static uint32_t gr_chercher(int t, uint32_t n)    /* sous g_gr_mu : premier départ de n granules libres contigus, ou ~0 */
{
    if (!g_gr[t].bloc || g_gr[t].libres < n) return ~0u;
    for (uint32_t i = 0, suite = 0; i < GR_PAR_TR; i++) {
        if (g_gr[t].bits[i >> 6] >> (i & 63) & 1) suite = 0;
        else if (++suite == n) return i + 1 - n;
    }
    return ~0u;
}
static uint32_t gr_prendre(uint32_t taille)       /* va alignée sur 64 Ko, remise à zéro ; 0 : chemin d'avant */
{
    uint32_t n = (taille + GR_TAILLE - 1u) / GR_TAILLE;
    if (!n || n > GR_MAX_N || !sc2_mem_liberer()) return 0;
    pthread_mutex_lock(&g_gr_mu);
    int t = -1; uint32_t i = ~0u;
    for (int k = 0; k < g_ngr && t < 0; k++) if ((i = gr_chercher(k, n)) != ~0u) t = k;
    if (t < 0) {                                  /* tronçon neuf (une case vide, sinon au bout) */
        int k = 0; while (k < g_ngr && g_gr[k].bloc) k++;
        if (k == GR_MAX_TR) { pthread_mutex_unlock(&g_gr_mu); return 0; }
        uint32_t p = sc2_alloc_de(GR_PAR_TR * GR_TAILLE + GR_TAILLE, SC2_MO_VIRT);
        memset(&g_gr[k], 0, sizeof g_gr[k]);
        g_gr[k].bloc = p; g_gr[k].base = (p + GR_TAILLE - 1u) & ~(GR_TAILLE - 1u); g_gr[k].libres = GR_PAR_TR;
        if (k == g_ngr) g_ngr++;
        g_gr_tr_pris++;
        t = k; i = 0;
    }
    gr_marquer(t, i, n, 1);
    g_gr_servis++;
    uint32_t va = g_gr[t].base + i * GR_TAILLE;
    pthread_mutex_unlock(&g_gr_mu);
    memset(GPTR(va), 0, (size_t)n * GR_TAILLE);
    return va;
}
static void gr_rendre(uint32_t va, uint32_t taille)
{
    uint32_t n = (taille + GR_TAILLE - 1u) / GR_TAILLE, rendre = 0;
    pthread_mutex_lock(&g_gr_mu);
    for (int t = 0; t < g_ngr; t++) {
        if (!g_gr[t].bloc || va < g_gr[t].base || va >= g_gr[t].base + GR_PAR_TR * GR_TAILLE) continue;
        uint32_t i = (va - g_gr[t].base) / GR_TAILLE;
        if (i + n <= GR_PAR_TR) { gr_marquer(t, i, n, 0); g_gr_rendus++; }
        if (g_gr[t].libres == GR_PAR_TR) {        /* vide : rendu à l'arène s'il en reste deux autres vides */
            int vides = 0;
            for (int k = 0; k < g_ngr; k++) vides += k != t && g_gr[k].bloc && g_gr[k].libres == GR_PAR_TR;
            if (vides >= 2) { rendre = g_gr[t].bloc; g_gr[t].bloc = 0; g_gr_tr_rendus++; }
        }
        break;
    }
    pthread_mutex_unlock(&g_gr_mu);
    if (rendre) sc2_free(rendre);
}
void sc2_virt_mem(char *o, size_t n)              /* lignes [MEM] (sc2_runtime.c) */
{
    uint32_t tr = 0, libres = 0;
    int v = 0;                                    /* verrou ESSAYÉ : sc2_stop peut venir de gr_prendre (arène épuisée) */
    for (int k = 0; k < 100 && !(v = !pthread_mutex_trylock(&g_gr_mu)); k++) usleep(1000);
    for (int t = 0; t < g_ngr; t++) if (g_gr[t].bloc) { tr++; libres += g_gr[t].libres; }
    snprintf(o, n, "VirtualAlloc en granules de 64 Ko : %u tronçon(s) de 16 Mo, %u granules pris, %u libres ; %llu régions "
             "servies, %llu rendues ; tronçons pris %llu, rendus %llu", tr, tr * GR_PAR_TR - libres, libres,
             (unsigned long long)g_gr_servis, (unsigned long long)g_gr_rendus, (unsigned long long)g_gr_tr_pris,
             (unsigned long long)g_gr_tr_rendus);
    if (v) pthread_mutex_unlock(&g_gr_mu);
}
static uint32_t h_VirtualAlloc(CPU *c, const uint32_t *a)
{
    (void)c;
    if (A(0)) return A(0);                        /* commit dans une réserve déjà servie */
    uint32_t r = gr_prendre(A(1)), p = r ? 1u : 0u;   /* V5.1 : granules (bloc = 1) */
    if (!r) { p = sc2_alloc_de(A(1) + 0xFFFFu, SC2_MO_VIRT); r = (p + 0xFFFFu) & ~0xFFFFu; }
    if (sc2_arene_recycle()) {
        pthread_mutex_lock(&g_virt_mu);
        for (uint32_t k = (r >> 16) % NVIRT, n = 0; n < NVIRT; k = (k + 1) % NVIRT, n++)
            if (!g_virt[k].va || g_virt[k].va == r) { g_virt[k].va = r; g_virt[k].bloc = p; g_virt[k].n = (A(1) + 0xFFFu) & ~0xFFFu; break; }
        pthread_mutex_unlock(&g_virt_mu);
    }
    return r;
}
static uint32_t h_VirtualFree(CPU *c, const uint32_t *a)   /* (adresse, taille, type) ; MEM_RELEASE 0x8000 */
{
    (void)c;
    if (!sc2_arene_recycle() || !(A(2) & 0x8000u) || !A(0)) return 1;
    uint32_t bloc = 0, taille = 0;
    pthread_mutex_lock(&g_virt_mu);
    for (uint32_t k = (A(0) >> 16) % NVIRT, n = 0; n < NVIRT && g_virt[k].va; k = (k + 1) % NVIRT, n++)
        if (g_virt[k].va == A(0)) {
            bloc = g_virt[k].bloc; taille = g_virt[k].n;
            /* retrait avec recompactage de la grappe (adressage ouvert linéaire) */
            uint32_t i = k;
            g_virt[i].va = 0;
            for (uint32_t j = (i + 1) % NVIRT; g_virt[j].va; j = (j + 1) % NVIRT) {
                uint32_t v = g_virt[j].va, b = g_virt[j].bloc, t = g_virt[j].n; g_virt[j].va = 0;
                for (uint32_t m = (v >> 16) % NVIRT;; m = (m + 1) % NVIRT) if (!g_virt[m].va) { g_virt[m].va = v; g_virt[m].bloc = b; g_virt[m].n = t; break; }
            }
            break;
        }
    if (bloc) g_virt_lib++;
    pthread_mutex_unlock(&g_virt_mu);
    if (bloc == 1u) gr_rendre(A(0), taille);     /* V5.1 : région en granules */
    else if (bloc) sc2_free(bloc);
    return 1;
}
static uint32_t h_VirtualQuery(CPU *c, const uint32_t *a)  /* (addr, MEMORY_BASIC_INFORMATION*, len) */
{
    /* Carte de l'espace invité (passe 4) : des régions ENTIÈRES, comme Windows.
     * Passe 3 rendait 4 Ko par appel : le parcours de l'espace d'adresses de
     * 0x0098BA00 (StockageStartup) faisait alors ~500 millions d'appels.
     *   [0, image) libre · image SC2.exe (MEM_IMAGE) · libre · arène engagée
     *   (tas, piles, TEB) · libre jusqu'à 0x7FFF0000 (fin de l'espace utilisateur). */
    (void)c;
    uint32_t m = A(1), va = A(0) & ~0xFFFu;
    uint32_t img_lo = SC2_IMAGE_BASE, img_hi = SC2_IMAGE_BASE + SC2_SIZE_OF_IMAGE;
    uint32_t ar_lo = 0x08000000u, ar_hi = (sc2_arena_top() + 0xFFFFu) & ~0xFFFFu;
    uint32_t lo, hi, state, prot, type, alloc;
    /* Passe 7h : arène de débordement ouverte (exe LAA) → [0x80000000, fin) engagée, libre jusqu'à 0xF0000000 ;
     * fermée : carte d'avant, inchangée. */
    uint32_t a2 = sc2_arena2_top(), a2_hi = (a2 + 0xFFFFu) & ~0xFFFFu, fin = a2 ? 0xF0000000u : 0x7FFF0000u;
    if (va >= fin) return 0;
    if (va < img_lo)      { lo = 0x10000u > va ? 0 : 0x10000u; hi = img_lo; state = 0x10000; prot = 1; type = 0; }
    else if (va < img_hi) { lo = img_lo; hi = img_hi; state = 0x1000; prot = 0x40; type = 0x1000000; }
    else if (va < ar_lo)  { lo = img_hi; hi = ar_lo; state = 0x10000; prot = 1; type = 0; }
    else if (va < ar_hi)  { lo = ar_lo; hi = ar_hi; state = 0x1000; prot = 4; type = 0x20000; }
    else if (!a2)         { lo = ar_hi; hi = 0x7FFF0000u; state = 0x10000; prot = 1; type = 0; }
    else if (va < sc2_arena2_base()) { lo = ar_hi; hi = sc2_arena2_base(); state = 0x10000; prot = 1; type = 0; }
    else if (va < a2_hi)  { lo = sc2_arena2_base(); hi = a2_hi; state = 0x1000; prot = 4; type = 0x20000; }
    else                  { lo = a2_hi; hi = 0xF0000000u; state = 0x10000; prot = 1; type = 0; }
    if (va < 0x10000u) { lo = 0; hi = 0x10000u; }
    alloc = state == 0x10000 ? 0 : lo;
    /* Passe 7j (recyclage actif) : l'adresse d'un bloc rendu par VirtualAlloc décrit CE bloc (base = base
     * d'allocation = va, engagé, taille du bloc), comme Windows. Le dlmalloc du jeu (0x009B9A60, 0x009BA0D0…) ne
     * fait VirtualFree(MEM_RELEASE) qu'à cette condition : sans elle, aucun grand bloc n'était jamais rendu. */
    if (sc2_arene_recycle() && va == A(0)) {
        uint32_t t = 0;
        pthread_mutex_lock(&g_virt_mu);
        for (uint32_t k = (va >> 16) % NVIRT, n = 0; n < NVIRT && g_virt[k].va; k = (k + 1) % NVIRT, n++)
            if (g_virt[k].va == va) { t = g_virt[k].n; break; }
        pthread_mutex_unlock(&g_virt_mu);
        if (t) { alloc = va; hi = va + t; state = 0x1000; prot = 4; type = 0x20000; }
    }
    memset(GPTR(m), 0, 28);
    wr32(m + 0, va); wr32(m + 4, alloc); wr32(m + 8, state == 0x10000 ? 0 : prot);
    wr32(m + 12, hi - va); wr32(m + 16, state); wr32(m + 20, prot); wr32(m + 24, type);
    (void)lo;
    return 28;
}
static uint32_t h_GetVersionExW(CPU *c, const uint32_t *a)   /* Windows 7 SP1 */
{
    (void)c; uint32_t v = A(0);
    wr32(v + 4, 6); wr32(v + 8, 1); wr32(v + 12, 7601); wr32(v + 16, 2);
    return 1;
}
static uint32_t h_GetVersion(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0x1DB10106u; }
static uint32_t g_cmdline_w;
static uint32_t h_GetCommandLineW(CPU *c, const uint32_t *a) { (void)c; (void)a; return g_cmdline_w; }
static uint32_t h_GetSystemInfo(CPU *c, const uint32_t *a)
{
    (void)c; uint32_t s = A(0); memset(GPTR(s), 0, 36);
    wr32(s + 4, 0x1000); wr32(s + 8, 0x10000); wr32(s + 12, 0x7FFEFFFF); wr32(s + 16, 0xF);
    wr32(s + 20, 4); wr32(s + 24, 586); wr32(s + 28, 0x10000); return 0;
}
static uint32_t h_MultiByteToWideChar(CPU *c, const uint32_t *a)  /* (cp, fl, src, n, dst, cap) */
{
    (void)c; uint32_t n = A(3) == 0xFFFFFFFFu ? (uint32_t)strlen(STR(A(2))) + 1 : A(3);
    if (!A(5)) return n;
    uint32_t k = 0;
    for (; k < n && k < A(5); k++) wr16(A(4) + 2 * k, rd8(A(2) + k));
    return k;
}
static uint32_t h_WideCharToMultiByte(CPU *c, const uint32_t *a)  /* (cp, fl, src, n, dst, cap, …) */
{
    (void)c; uint32_t n = A(3) == 0xFFFFFFFFu ? wlen(A(2)) + 1 : A(3);
    if (!A(5)) return n;
    uint32_t k = 0;
    for (; k < n && k < A(5); k++) { uint16_t w = rd16(A(2) + 2 * k); wr8(A(4) + k, w < 128 ? (uint8_t)w : '?'); }
    return k;
}
static uint32_t h_lstrlenW(CPU *c, const uint32_t *a) { (void)c; return A(0) ? wlen(A(0)) : 0; }
static uint32_t h_OutputDebugStringW(CPU *c, const uint32_t *a) { (void)c; char t[1024]; w2u(A(0), t, sizeof t); sc2_log("[DEBUG] %s", t); return 0; }
static uint32_t h_OutputDebugStringA(CPU *c, const uint32_t *a) { (void)c; sc2_log("[DEBUG] %s", STR(A(0))); return 0; }

/* Objets nommés : on passe aux implémentations POSIX de stubs_sc2 (passe 2). Les
 * HANDLE hôtes sont (index+1)*4 : ils tiennent dans un registre invité. */
static uint32_t H(HANDLE h) { return (uint32_t)(uintptr_t)h; }
static HANDLE   HH(uint32_t v) { return (HANDLE)(uintptr_t)v; }
/* Passe 5f : touches lues dans l'état d'entrée (runtime/sc2_input_sim.c), par référence FAIBLE ;
 * sans ce module, ou inactif (pas de SC2_INPUT_SIM) : 0, comme avant (h_ret0). */
extern uint32_t sc2_input_async(uint32_t vk) __attribute__((weak));
extern uint32_t sc2_input_etat(uint32_t vk) __attribute__((weak));
static uint32_t h_GetAsyncKeyState(CPU *c, const uint32_t *a) { (void)c; return sc2_input_async ? sc2_input_async(A(0)) : 0; }
static uint32_t h_GetKeyState(CPU *c, const uint32_t *a)      { (void)c; return sc2_input_etat ? sc2_input_etat(A(0)) : 0; }
static uint32_t h_IsDebuggerPresent(CPU *c, const uint32_t *a) { (void)c; (void)a; return (uint32_t)IsDebuggerPresent(); }
static uint32_t h_CreateEventW(CPU *c, const uint32_t *a) { (void)c; return H(CreateEventW(NULL, (BOOL)A(1), (BOOL)A(2), A(3) ? (LPCWSTR)GPTR(A(3)) : NULL)); }
static uint32_t h_CreateEventA(CPU *c, const uint32_t *a) { (void)c; return H(CreateEventA(NULL, (BOOL)A(1), (BOOL)A(2), A(3) ? STR(A(3)) : NULL)); }
static uint32_t h_OpenEventW(CPU *c, const uint32_t *a)   { (void)c; return H(OpenEventW(A(0), (BOOL)A(1), A(2) ? (LPCWSTR)GPTR(A(2)) : NULL)); }
static uint32_t h_OpenEventA(CPU *c, const uint32_t *a)   { (void)c; return H(OpenEventA(A(0), (BOOL)A(1), STR(A(2)))); }
static uint32_t h_SetEvent(CPU *c, const uint32_t *a)     { (void)c; return (uint32_t)SetEvent(HH(A(0))); }
static uint32_t h_ResetEvent(CPU *c, const uint32_t *a)   { (void)c; return (uint32_t)ResetEvent(HH(A(0))); }
static uint32_t h_WaitForSingleObject(CPU *c, const uint32_t *a)
{ (void)c; uint64_t t0 = &sc2_att_debut ? sc2_att_debut() : 0; uint32_t r = WaitForSingleObject(HH(A(0)), A(1));
  if (t0) sc2_att_fin(ATT_WAIT, A(1), t0, rd32(c->esp)); return r; }
static uint32_t h_CloseHandle(CPU *c, const uint32_t *a);
/* ---- Passe 7h : le LANCEUR simulé (chemin « Tester le document » de l'éditeur), SC2_RUN_SANS_BNET=1 ----
 * SC2.exe ne lit pas l'option « run » : c'est le lanceur (StarCraft II.exe, ou l'éditeur) qui dit au jeu
 * QUOI lancer, par la section partagée « StarCraft II IPC Mem » (64 Ko) :
 *   sc2_init_LauncherHandshakeIPC 0x00809B90 → InterlockedCompareExchange(shm+0x20, 1, 4) : état 4 = « lancement
 *   prêt » → copie de 0xB5C octets de shm+0x30 dans la structure de lancement 0x01699AC8, état 5, puis attente de
 *   l'acquittement du lanceur tant que OpenProcess(shm+0x28) le dit vivant (NULL → pas d'attente) ;
 *   sc2_init_LauncherCheckEtJeu 0x008080F0 → switch sur le mode (struct+0x18, 1 par défaut) : 2/3/4 construisent
 *   un descripteur de lancement (types 1/2/3) depuis le chemin struct+0x2C ; struct+0x130 = nombre de fichiers
 *   supplémentaires (struct+0x134, 0x104 octets chacun).
 * Le lanceur simulé écrit l'état 4 et la structure (valeurs par défaut du handshake + mode SC2_RUN_MODE, défaut 2
 * + carte (« -run » de SC2_ARGS, sinon SC2_RUN_CARTE, sinon la carte d'histoire par défaut TStory01, le bar ;
 * SC2_RUN_CARTE=0 = lanceur inactif) + « -testmod » en fichiers supplémentaires). Aucun processus, aucun réseau. */
static int run_lanceur_actif(void)
{
    const char *e = getenv("SC2_RUN_SANS_BNET");
    return e && *e == '1';
}
static void run_lanceur_preparer(HANDLE h)
{
    static char args[32][512];
    int n = args_decouper(getenv("SC2_ARGS"), args, 32);
    const char *carte = NULL; const char *mods[8]; int nm = 0;
    for (int i = 0; i + 1 < n; i++) {
        if (!strcasecmp(args[i], "-run")) carte = args[++i];
        else if (!strcasecmp(args[i], "-testmod") && nm < 8) mods[nm++] = args[++i];
    }
    /* V4.10 : escarmouche (SC2_ESCARMOUCHE, sc2_escarmouche.c) — la carte préparée au démarrage prend la place de la
     * carte par défaut (TStory01 ou SC2_RUN_CARTE), en mode 2 ; un « -run » explicite l'emporte. Sans elle : NULL. */
    const char *esc = sc2_escarmouche_carte();
    if (esc && carte) sc2_log("[ESCARMOUCHE] « -run %s » de SC2_ARGS l'emporte : carte d'escarmouche ignorée", carte);
    else if (esc) carte = esc;
    if (!carte) {                                          /* pas de « -run » : défaut TStory01 ou SC2_RUN_CARTE */
        const char *ec = getenv("SC2_RUN_CARTE");
        if (ec && !strcmp(ec, "0")) {
            sc2_log("[RUN] SC2_RUN_SANS_BNET=1 mais pas de « -run <carte> » dans SC2_ARGS : lanceur inactif");
            return;                                               /* SC2_RUN_CARTE=0 : ancien comportement exact */
        }
        carte = (ec && *ec) ? ec : "Maps/Campaign/TStory01.SC2Map";
        if (ec && *ec) sc2_log("[RUN] pas de « -run » dans SC2_ARGS : carte « %s » (SC2_RUN_CARTE)", carte);
        else sc2_log("[RUN] pas de « -run » dans SC2_ARGS : carte d'histoire par défaut « %s »", carte);
    }
    const char *em = getenv("SC2_RUN_MODE");
    uint32_t mode = em ? (uint32_t)strtoul(em, NULL, 0) : 2;
    if (esc && carte == esc && mode != 2) { sc2_log("[ESCARMOUCHE] SC2_RUN_MODE=%u ignoré : mode 2 (carte)", mode); mode = 2; }
    /* L3 (24/10) : SC2_SAUVEGARDE=<chemin> — démarrer DIRECTEMENT sur une sauvegarde (bouton « Charger une partie »).
     * Mode 4 de la structure de lancement : sc2_init_LauncherCheckEtJeu (0x008080F0, cas 4 en 0x00808267) en fait un
     * descripteur de type 3 (sauvegarde), chemin converti par L_008027F0 puis passé à L_00804D70 ; le Load de la partie
     * suit le chemin de L_00B18DC0 (ouverture de l'archive par L_0133F7C0). Chemin relatif au dossier
     * « Documents\StarCraft II » du jeu (« Saves\Campaign\The Outlaws.SC2Save », « / » accepté), ou chemin Windows
     * complet (« C:\… ») ; le runtime écrit toujours le chemin Windows complet. L'emporte sur « -run », SC2_RUN_CARTE,
     * SC2_RUN_MODE et l'escarmouche. Absente ou vide : rien ne change. */
    static char sauve[0x104];
    const char *es = getenv("SC2_SAUVEGARDE");
    if (es && *es) {
        size_t k = 0;
        if (!(((es[0] | 32) >= 'a' && (es[0] | 32) <= 'z') && es[1] == ':'))
            k = (size_t)snprintf(sauve, sizeof sauve, "C:\\Users\\sc2\\Documents\\StarCraft II\\");
        for (const char *q = es + (es[0] == '/' || es[0] == '\\'); *q && k + 1 < sizeof sauve; q++)
            sauve[k++] = *q == '/' ? '\\' : *q;
        sauve[k] = 0;
        sc2_log("[RUN] SC2_SAUVEGARDE : démarrage sur la sauvegarde « %s » (mode 4, descripteur de type 3) au lieu de "
                "« %s » (mode %u)", sauve, carte, mode);
        carte = sauve; mode = 4;
    }
    uint8_t *v = (uint8_t *)MapViewOfFile(h, 2 /* FILE_MAP_WRITE */, 0, 0, 0x10000);
    if (!v) { sc2_log("[RUN] vue hôte de « StarCraft II IPC Mem » impossible"); return; }
    uint8_t *st = v + 0x30;                                       /* structure de lancement */
    memset(st, 0, 0xB5C);
    *(uint32_t *)(st + 0x00) = 1; *(uint32_t *)(st + 0x18) = mode;
    *(uint32_t *)(st + 0x1C) = 0xFFFFFFFFu; *(uint32_t *)(st + 0x24) = 0xFFFFFFFFu;
    snprintf((char *)st + 0x2C, 0x104, "%s", carte);
    *(uint32_t *)(st + 0x130) = (uint32_t)nm;
    for (int i = 0; i < nm; i++) snprintf((char *)st + 0x134 + 0x104 * i, 0x104, "%s", mods[i]);
    *(uint32_t *)(v + 0x28) = 0;                                  /* PID du lanceur : aucun */
    *(uint32_t *)(v + 0x20) = 4;                                  /* état : lancement prêt */
    UnmapViewOfFile(v);
    sc2_log("[RUN] lanceur simulé (SC2_RUN_SANS_BNET=1) : « StarCraft II IPC Mem » ← état 4, mode %u, carte « %s », %d fichier(s) "
            "supplémentaire(s)%s%s", mode, carte, nm, nm ? " : " : "", nm ? mods[0] : "");
}
static uint32_t h_CreateFileMappingA(CPU *c, const uint32_t *a)
{
    (void)c;
    HANDLE h = CreateFileMappingA(A(0) == 0xFFFFFFFFu ? INVALID_HANDLE_VALUE : HH(A(0)), NULL, A(2), A(3), A(4),
                                  A(5) ? STR(A(5)) : NULL);
    if (h && A(5) && !strcmp(STR(A(5)), "StarCraft II IPC Mem") && run_lanceur_actif()) run_lanceur_preparer(h);
    return H(h);
}
/* OpenProcess (passe 7h) : le seul appel connu vise le PID du lanceur (handshake IPC) — aucun processus tiers
 * n'existe ici : NULL, comme pour un lanceur déjà terminé (le jeu n'attend alors pas son acquittement). */
static uint32_t h_OpenProcess(CPU *c, const uint32_t *a)
{
    (void)c;
    sc2_log("[RUN] OpenProcess(accès 0x%X, PID %u) → NULL (aucun processus lanceur)", A(0), A(2));
    SetLastError(87);
    return 0;
}
static uint32_t h_CreateFileMappingW(CPU *c, const uint32_t *a)
{
    (void)c;
    char nom[260] = "";
    int ipc = 0;
    if (A(5)) { w2u(A(5), nom, sizeof nom);
                ipc = !strcmp(nom, "StarCraft II IPC Mem");
                if (ipc) sc2_log("[IAT] CreateFileMappingW « %s » (taille demandée %u) : section IPC", nom, A(4)); }
    DWORD hi = A(3), lo = A(4);
    if (ipc && (hi || lo < 0x10000u)) { hi = 0; lo = 0x10000u; }   /* le vrai lanceur crée 64 Ko */
    HANDLE h = CreateFileMappingW(A(0) == 0xFFFFFFFFu ? INVALID_HANDLE_VALUE : HH(A(0)), NULL, A(2), hi, lo,
                                   A(5) ? (LPCWSTR)GPTR(A(5)) : NULL);
    if (h && ipc && run_lanceur_actif()) run_lanceur_preparer(h);
    return H(h);
}
static uint32_t h_OpenFileMappingA(CPU *c, const uint32_t *a) { (void)c; return H(OpenFileMappingA(A(0), (BOOL)A(1), STR(A(2)))); }
/* Vues de sections (passe 5b). Le code invité ne voit que des VA 32 bits : une
 * vue dont l'adresse HÔTE n'en est pas une (macOS : zone invitée décalée de
 * g_hote_delta et pas de MAP_32BIT ; avant la passe 5b → arrêt « vue au-dessus
 * de 4 Go » dès la 1re vue, 0x0099CD72, étape 1) est RELOGÉE dans l'arène
 * invitée : le jeu n'a besoin que d'un pointeur stable et lisible, pas d'une
 * adresse précise. Une copie par SECTION (pas par vue) : deux vues d'une même
 * section — OpenFileMapping du même nom compris — partagent le même bloc,
 * comme sous Windows. Le contenu de la section hôte est copié à la création du
 * bloc et y est réécrit au dernier UnmapViewOfFile (la section hôte reste le
 * stockage entre deux vies de vues). Hors processus, rien d'autre ne lit ces
 * sections (pas de launcher) : la copie ne perd aucun échange.
 * SC2_MAPVIEW_ARENE=1 force la relocation (pour exercer ce chemin sur Linux,
 * où les sections sont déjà sous 4 Go). */
#define MAX_SEC_ARENE 64
static struct { void *hote; uint32_t va, taille; int vues; } g_sec_arene[MAX_SEC_ARENE];
static struct { uint32_t va; void *vue; int sec; } g_vue_arene[256];
static pthread_mutex_t g_arene_vues_mu = PTHREAD_MUTEX_INITIALIZER;

static int g_vue_force = -1;   /* SC2_MAPVIEW_ARENE */
static int vue_representable(const void *p, uint32_t taille)
{
    if (g_vue_force < 0) { const char *e = getenv("SC2_MAPVIEW_ARENE"); g_vue_force = e && *e && *e != '0'; }
    uintptr_t h = (uintptr_t)p;
    return !g_vue_force && h >= g_hote_delta && h - g_hote_delta <= 0xFFFFFFFFull - taille;
}

static uint32_t h_MapViewOfFile(CPU *c, const uint32_t *a)
{
    (void)c;
    void *p = MapViewOfFile(HH(A(0)), A(1), A(2), A(3), A(4));
    if (!p) return 0;
    void *base; SIZE_T taille;
    if (!w32_view_section(p, &base, &taille)) { base = p; taille = A(4); }
    uint32_t off = (uint32_t)((char *)p - (char *)base);
    uint32_t demande = A(4) ? A(4) : (uint32_t)(taille - off);
    if (vue_representable(p, demande)) return (uint32_t)((uintptr_t)p - g_hote_delta);

    pthread_mutex_lock(&g_arene_vues_mu);
    int s = -1, libre = -1, v = -1;
    for (int i = 0; i < MAX_SEC_ARENE; i++) {
        if (g_sec_arene[i].hote == base && g_sec_arene[i].vues > 0) { s = i; break; }
        if (libre < 0 && !g_sec_arene[i].vues) libre = i;
    }
    for (int i = 0; i < 256 && v < 0; i++) if (!g_vue_arene[i].va) v = i;
    if ((s < 0 && libre < 0) || v < 0 || taille > 0x10000000u) {
        pthread_mutex_unlock(&g_arene_vues_mu);
        sc2_stop(11, "MapViewOfFile : vue hôte %p hors de l'espace invité et non relogeable "
                 "(section %zu octets, tables pleines ?)", p, (size_t)taille);
    }
    int neuve = s < 0;
    if (neuve) {
        s = libre;
        g_sec_arene[s].hote = base;
        g_sec_arene[s].taille = (uint32_t)taille;
        g_sec_arene[s].va = sc2_alloc_de((uint32_t)taille, SC2_MO_SECTION);   /* arène : zéro-initialisée */
        memcpy(GPTR(g_sec_arene[s].va), base, taille);       /* + contenu de la section */
    }
    g_sec_arene[s].vues++;
    uint32_t va = g_sec_arene[s].va + off;
    g_vue_arene[v].va = va; g_vue_arene[v].vue = p; g_vue_arene[v].sec = s;
    pthread_mutex_unlock(&g_arene_vues_mu);
    sc2_log("[IAT] MapViewOfFile : vue hôte %p %s → RELOGÉE dans l'arène à 0x%08X "
            "(demandé %u octets à l'offset 0x%X ; alloué %u = section entière%s)",
            p, g_vue_force ? "(relocation forcée, SC2_MAPVIEW_ARENE)" : "hors de l'espace invité", va,
            demande, off, (uint32_t)taille, neuve ? ", nouveau bloc" : ", bloc de la section partagé");
    return va;
}
static uint32_t h_UnmapViewOfFile(CPU *c, const uint32_t *a)
{
    (void)c;
    void *vue = NULL;
    pthread_mutex_lock(&g_arene_vues_mu);
    for (int i = 0; i < 256; i++)
        if (g_vue_arene[i].va && g_vue_arene[i].va == A(0)) {
            int s = g_vue_arene[i].sec;
            vue = g_vue_arene[i].vue;
            g_vue_arene[i].va = 0;
            if (--g_sec_arene[s].vues == 0) {   /* dernière vue : le contenu retourne à la section */
                memcpy(g_sec_arene[s].hote, GPTR(g_sec_arene[s].va), g_sec_arene[s].taille);
                sc2_free(g_sec_arene[s].va);
            }
            break;
        }
    pthread_mutex_unlock(&g_arene_vues_mu);
    return (uint32_t)UnmapViewOfFile(vue ? vue : GPTR(A(0)));
}


/* ========================================================================== */
/* Fichiers : C:\ → $SC2_ROOT (bac à sable hôte)                               */
/*   passe 6a : C:\StarCraft II\ → $SC2_INSTALL (installation extraite du Tome,  */
/*   scripts/extraire-install.sh), traces [FS], énumération FindFirst/NextFile  */
/* ========================================================================== */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include "sc2_mpq.h"
__thread uint32_t sc2_appel_ret;                     /* passe 10j : adresse de retour invitée de l'import en cours (journal d'arène) */

#define FH_BASE 0x00100000u                 /* handles de fichiers : FH_BASE + 4*i */
static int g_fd[256];
static char g_root[512];

static const char *root(void)
{
    if (!g_root[0]) {
        const char *r = getenv("SC2_ROOT");
        snprintf(g_root, sizeof g_root, "%s", r ? r : "sc2-c");
        mkdir(g_root, 0755);
    }
    return g_root;
}
/* Passe 4 : Windows ignore la casse, l'hôte non (« Base.SC2Data » demandé,
 * « base.SC2Data » sur disque). Composant par composant, le nom exact est
 * remplacé par le premier qui lui est égal sans tenir compte de la casse. */
static void resoudre_casse(char *p, size_t debut)
{
    struct stat st;
    if (!stat(p, &st)) return;
    for (size_t i = debut; p[i]; ) {
        size_t j = i; while (p[j] && p[j] != '/') j++;
        char sv = p[j]; p[j] = 0;
        if (stat(p, &st)) {
            char dir[1024]; size_t ld = i > 1 ? i - 1 : 1;
            memcpy(dir, p, ld); dir[ld] = 0;
            DIR *d = opendir(dir); struct dirent *e; int ok = 0;
            while (d && (e = readdir(d)))
                if (strlen(e->d_name) == j - i && !strcasecmp(e->d_name, p + i)) { memcpy(p + i, e->d_name, j - i); ok = 1; break; }
            if (d) closedir(d);
            if (!ok) { p[j] = sv; return; }
        }
        p[j] = sv; i = sv ? j + 1 : j;
    }
}
static char g_cwd[512] = "C:\\StarCraft II";
static void chemin_absolu(const char *rel, char *o, size_t cap);
/* Passe 6a : racine de l'INSTALLATION (« C:\StarCraft II\ ») — SC2_INSTALL, sinon
 * $SC2_ROOT/StarCraft II comme en passes 3-5. Le reste de C:\ (ProgramData, Users, Temp :
 * caches, logs, réglages écrits par le jeu) reste sous $SC2_ROOT. */
static char g_install[512];
static const char *install(void)
{
    if (!g_install[0]) {
        const char *r = getenv("SC2_INSTALL");
        if (r && *r) snprintf(g_install, sizeof g_install, "%s", r);
        else snprintf(g_install, sizeof g_install, "%s/StarCraft II", root());
        struct stat st;
        sc2_log("[FS] racine installation « C:\\StarCraft II\\ » → %s%s", g_install,
                stat(g_install, &st) ? " (ABSENTE)" : "");
    }
    return g_install;
}
static uint64_t g_fs_ok, g_fs_absent, g_fs_enum;   /* bilan [FS] */
/* L3 (24/10) : carte de campagne demandée sur disque par la couche fichiers de Storm (appelant 0x00EE0000-0x00F10000 :
 * GetFileAttributesW ← 0x00EFC456, CreateFileW ← 0x00EFD653…) sous « C:\StarCraft II\Maps\…\X.SC2Map » alors
 * qu'elle n'existe que comme dossier d'une archive Campaigns\*.SC2Campaign\*.SC2Maps : servie depuis une archive
 * synthétique (sc2_mpq_carte_campagne, rapport L3 § 4-5). Le contrôle « fichier présent ? » du jeu lui-même
 * (L_00995470, ← 0x009954E9) n'est PAS détourné : le lancement normal, qui ouvre la carte avec son archive parente,
 * ne voit aucun changement. SC2_CARTES_CAMPAGNE=0 : contournement coupé. */
static void carte_campagne(char *o, size_t cap, size_t r_install)
{
    static int actif = -1;
    if (actif < 0) { const char *e = getenv("SC2_CARTES_CAMPAGNE"); actif = !(e && *e == '0'); }
    if (!actif || !r_install || sc2_appel_ret < 0x00EE0000u || sc2_appel_ret >= 0x00F10000u) return;
    const char *rel = o + r_install;
    size_t l = strlen(rel);
    if (strncasecmp(rel, "/Maps/", 6) || l < 8 || strcasecmp(rel + l - 7, ".SC2Map")) return;
    struct stat st;
    if (!stat(o, &st)) return;                                   /* la carte existe sur disque : rien ne change */
    char cache[1024], rc[512], s[1024];
    snprintf(cache, sizeof cache, "%s/cartes-campagne", root());
    snprintf(rc, sizeof rc, "%s", rel);
    if (sc2_mpq_carte_campagne(install(), rc, cache, s, sizeof s, sc2_appel_ret)) snprintf(o, cap, "%s", s);
}
/* Chemin Windows (UTF-8) → chemin hôte sous la racine (relatif : sous le cwd invité). */
static void hpath(const char *w, char *o, size_t cap)
{
    char abs_[1024];
    const char *p = w;
    if (!strncmp(p, "\\\\?\\", 4)) p += 4;
    if (!(((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':') && p[0] != '\\' && p[0] != '/') {
        chemin_absolu(p, abs_, sizeof abs_); p = abs_;
    }
    if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':') p += 2;
    size_t n, r, r_inst = 0;
    if (!strncasecmp(p + 1, "StarCraft II", 12) && (p[0] == '\\' || p[0] == '/')
        && (!p[13] || p[13] == '\\' || p[13] == '/')) {
        n = r = r_inst = (size_t)snprintf(o, cap, "%s", install());
        p += 13;
    } else
        n = r = (size_t)snprintf(o, cap, "%s/", root());
    for (; *p && n + 1 < cap; p++) o[n++] = (*p == '\\') ? '/' : *p;
    o[n] = 0;
    resoudre_casse(o, r);
    carte_campagne(o, cap, r_inst);
}
static void hpathw(uint32_t w, char *o, size_t cap) { char u[1024]; w2u(w, u, sizeof u); hpath(u, o, cap); }
static void mkdirs(const char *path)
{
    char t[1024]; snprintf(t, sizeof t, "%s", path);
    for (char *q = t + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
}
/* 24/10 (chantier du SIGBUS de TRaynor01) : la case était prise par « if (!g_fd[i]) g_fd[i] = fd + 1 », sans verrou, alors
 * que FindFirstFile et fopen ont le leur : deux CreateFile simultanés (fils différents) pouvaient lire la même case libre
 * et rendre le MÊME handle invité pour deux fichiers (le second fd écrase le premier, qui fuit ; le premier CloseHandle
 * ferme le fichier de l'autre fil). Prise par compare-and-swap ; une course évitée est comptée (ligne [FS], bilan).
 * Défensif : pas la cause du SIGBUS (aucune collision avec la fenêtre de l'ancien code élargie à 4 ms). */
static uint64_t g_fh_courses;
static uint32_t fh_new(int fd)
{
    for (int i = 0; i < 256; i++) {
        int libre = 0;
        if (__atomic_load_n(&g_fd[i], __ATOMIC_ACQUIRE)) continue;
        if (__atomic_compare_exchange_n(&g_fd[i], &libre, fd + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return FH_BASE + 4u * (uint32_t)i;
        uint64_t k = __atomic_add_fetch(&g_fh_courses, 1, __ATOMIC_RELAXED);   /* prise entre la lecture et le CAS */
        if (k <= 8) sc2_log("[FS] CreateFile : case de handle n°%d prise par un autre fil au même instant — course évitée (n°%llu)",
                            i, (unsigned long long)k);
    }
    close(fd); return 0xFFFFFFFFu;
}
static int fd_of(uint32_t h)
{
    if (h < FH_BASE || h >= FH_BASE + 4 * 256) return -1;
    return __atomic_load_n(&g_fd[(h - FH_BASE) / 4], __ATOMIC_ACQUIRE) - 1;
}
static uint32_t file_attr(const char *hp)
{
    struct stat st;
    if (stat(hp, &st)) { SetLastError(2); return 0xFFFFFFFFu; }
    return S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
}
/* Passe 10m : SC2_SANS_VIDEOS=1 → les cinématiques .ogv sont déclarées ABSENTES
 * au FS : le jeu croit la vidéo manquante et la saute (comportement naturel).
 * Les .ogg audio ne sont PAS touchés (musique/voix intactes). Parade en attendant
 * le vrai skip ESC de la 9e (pompe d'input pendant vidéo). */
static int sc2_sans_videos(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("SC2_SANS_VIDEOS"); v = (e && *e == '1'); }
    return v;
}
static int est_ogv(const char *u)
{
    size_t n = strlen(u);
    return n >= 4 && !strcasecmp(u + n - 4, ".ogv");
}
static uint32_t h_GetFileAttributesW(CPU *c, const uint32_t *a)
{
    (void)c; char u[1024], p[1024]; w2u(A(0), u, sizeof u);
    if (sc2_sans_videos() && est_ogv(u)) { sc2_log("[FS] GetFileAttributesW(\"%s\") → absent (SC2_SANS_VIDEOS)", u); g_fs_absent++; return 0xFFFFFFFFu; }
    hpath(u, p, sizeof p);
    uint32_t r = file_attr(p);
    sc2_trace("[FS] GetFileAttributesW(\"%s\") → %s", u, r == 0xFFFFFFFFu ? "absent" : r == 0x10u ? "dossier" : "fichier");
    if (r == 0xFFFFFFFFu) g_fs_absent++; else g_fs_ok++;
    return r;
}
static uint32_t h_GetFileAttributesA(CPU *c, const uint32_t *a)
{
    (void)c; char p[1024]; hpath(STR(A(0)), p, sizeof p);
    uint32_t r = file_attr(p);
    sc2_trace("[FS] GetFileAttributesA(\"%s\") → %s", STR(A(0)), r == 0xFFFFFFFFu ? "absent" : r == 0x10u ? "dossier" : "fichier");
    if (r == 0xFFFFFFFFu) g_fs_absent++; else g_fs_ok++;
    return r;
}
static uint32_t h_GetFileAttributesExW(CPU *c, const uint32_t *a)
{
    (void)c; char u[1024], p[1024]; w2u(A(0), u, sizeof u); hpath(u, p, sizeof p);
    struct stat st;
    int ko = stat(p, &st);
    sc2_trace("[FS] GetFileAttributesExW(\"%s\") → %s", u, ko ? "absent" : S_ISDIR(st.st_mode) ? "dossier" : "fichier");
    if (ko) { g_fs_absent++; SetLastError(2); return 0; }
    g_fs_ok++;
    memset(GPTR(A(2)), 0, 36);
    wr32(A(2), S_ISDIR(st.st_mode) ? 0x10u : 0x80u);
    wr32(A(2) + 28, (uint32_t)((uint64_t)st.st_size >> 32)); wr32(A(2) + 32, (uint32_t)st.st_size);
    return 1;
}
/* L3 (24/10) : le séparateur final est retiré AVANT mkdirs. mkdirs crée chaque composant suivi d'un « / » : avec
 * « …\Liberation Day.SC2Save\ » (chemin du dossier de préparation de la sauvegarde, L_00995F10), il créait le dossier
 * visé lui-même, puis mkdir échouait en EEXIST → ERROR_ALREADY_EXISTS (183) ; L_00995F10 rend ce code, l'ouverture
 * de l'archive (L_0133F7C0) échoue et la sauvegarde aussi (« Save Failed », archive vide de 1 181 o). Windows accepte
 * le séparateur final. Les parents absents restent créés (tolérance du runtime, au lieu d'ERROR_PATH_NOT_FOUND). */
static uint32_t h_CreateDirectoryW(CPU *c, const uint32_t *a)
{
    (void)c; char u[1024], p[1024]; w2u(A(0), u, sizeof u); hpath(u, p, sizeof p);
    for (size_t l = strlen(p); l > 1 && p[l - 1] == '/'; l--) p[l - 1] = 0;
    mkdirs(p);
    int ok = mkdir(p, 0755) == 0, e = errno;
    sc2_trace("[FS] CreateDirectoryW(\"%s\") → %s", u, ok ? "créé" : e == EEXIST ? "existe déjà (183)" : "échec (3)");
    if (ok) return 1;
    SetLastError(e == EEXIST ? 183 : 3); return 0;
}
static uint32_t h_RemoveDirectoryW(CPU *c, const uint32_t *a) { (void)c; char p[1024]; hpathw(A(0), p, sizeof p); return rmdir(p) == 0; }
static uint32_t h_DeleteFileW(CPU *c, const uint32_t *a) { (void)c; char p[1024]; hpathw(A(0), p, sizeof p); if (unlink(p)) { SetLastError(2); return 0; } return 1; }
static uint32_t h_MoveFileW(CPU *c, const uint32_t *a)
{
    (void)c; char p[1024], q[1024]; hpathw(A(0), p, sizeof p); hpathw(A(1), q, sizeof q);
    return rename(p, q) == 0;
}
/* L3 (24/10) : CopyFileW(source, destination, bFailIfExists). Appelé après chaque sauvegarde réussie par
 * L_00995710 ← L_00B23F00 ← L_00B25130 (copie de l'archive de sauvegarde, écrasement permis) : absent, le runtime
 * s'arrêtait (code 6, « NON IMPLÉMENTÉ »). Copie hôte octet à octet ; source absente → ERROR_FILE_NOT_FOUND (2),
 * destination présente avec bFailIfExists → ERROR_FILE_EXISTS (80), même fichier → ERROR_SHARING_VIOLATION (32) comme
 * Windows (la source est ouverte), autre échec → ERROR_ACCESS_DENIED (5). Parents de la destination créés, comme
 * CreateFileW du runtime. */
static uint32_t h_CopyFileW(CPU *c, const uint32_t *a)
{
    (void)c; char us[1024], ud[1024], p[1024], q[1024];
    w2u(A(0), us, sizeof us); w2u(A(1), ud, sizeof ud); hpath(us, p, sizeof p); hpath(ud, q, sizeof q);
    struct stat ss, sd; uint32_t err = 0; int in = -1, out = -1;
    if (stat(p, &ss) || !S_ISREG(ss.st_mode)) err = 2;
    else if (!stat(q, &sd) && sd.st_dev == ss.st_dev && sd.st_ino == ss.st_ino) err = 32;
    else if ((in = open(p, O_RDONLY)) < 0) err = 5;
    else {
        mkdirs(q);
        out = open(q, O_WRONLY | O_CREAT | (A(2) ? O_EXCL : O_TRUNC), 0644);
        if (out < 0) err = errno == EEXIST ? 80 : 5;
        else {
            char b[65536]; ssize_t n;
            while ((n = read(in, b, sizeof b)) > 0)
                if (write(out, b, (size_t)n) != n) { err = 5; break; }
            if (n < 0) err = 5;
        }
    }
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    sc2_trace("[FS] CopyFileW(\"%s\" → \"%s\", échec si présent %u) → %s (%u)", us, ud, A(2), err ? "échec" : "copié", err);
    if (err) { SetLastError(err); return 0; }
    return 1;
}
/* V1.1 (22/10) : ouverture réussie signalée au banc d'entrées (sc2_entrees_banc.c, QUAND:FICHIER) — référence faible */
void sc2_banc_fichier(const char *win, int ecriture) __attribute__((weak));
static uint32_t h_CreateFileW(CPU *c, const uint32_t *a)   /* (nom, accès, partage, sa, disposition, attrs, modèle) */
{
    (void)c; char u[1024], p[1024]; w2u(A(0), u, sizeof u);
    if (sc2_sans_videos() && est_ogv(u)) { sc2_log("[FS] CreateFileW(\"%s\") → absent (SC2_SANS_VIDEOS)", u); g_fs_absent++; SetLastError(2); return 0xFFFFFFFFu; }
    hpath(u, p, sizeof p);
    int fl = (A(1) & 0x40000000u) ? ((A(1) & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (A(4)) { case 1: fl |= O_CREAT | O_EXCL; break; case 2: fl |= O_CREAT | O_TRUNC; break;
                    case 4: fl |= O_CREAT; break; case 5: fl |= O_TRUNC; break; default: break; }
    if (fl & O_CREAT) mkdirs(p);
    int fd = open(p, fl, 0644);
    sc2_trace("[FS] CreateFileW(\"%s\", disp %u) → %s", u, A(4), fd >= 0 ? "ouvert" : "absent");
    if (fd < 0) { g_fs_absent++; SetLastError(errno == ENOENT ? 2 : errno == EEXIST ? 80 : 5); return 0xFFFFFFFFu; }
    g_fs_ok++;
    SetLastError(0);
    uint32_t h = fh_new(fd);
    if (h != 0xFFFFFFFFu && !(fl & (O_WRONLY | O_RDWR))) sc2_mpq_ouvert(h, p, u);   /* [MPQ] si archive */
    if (sc2_banc_fichier) sc2_banc_fichier(u, (fl & (O_WRONLY | O_RDWR)) != 0);       /* V1.1 : QUAND:FICHIER du banc */
    return h;
}
/* OVERLAPPED (passe 4) : le Storm de SC2 lit ses archives MPQ par ReadFile
 * « overlapped », l'offset dans OVERLAPPED.Offset/OffsetHigh (+8/+12). Passe 3
 * lisait à la position courante : tables MPQ lues au mauvais endroit → aucun
 * fichier trouvé (0x2003000A) → e_fileCorrupt sur Shaders/LineVertex.bls.
 * Ici : pread synchrone, Internal = 0, InternalHigh = octets, hEvent signalé. */
static void ov_fini(uint32_t ov, ssize_t n)
{
    wr32(ov + 0, n >= 0 ? 0 : 0xC0000001u); wr32(ov + 4, n > 0 ? (uint32_t)n : 0);
    if (rd32(ov + 16)) SetEvent(HH(rd32(ov + 16)));
}
static uint32_t h_ReadFile(CPU *c, const uint32_t *a)
{
    int fd = fd_of(A(0));
    if (fd < 0) {                                      /* 24/10 : handle de fichier fermé ou inconnu, journalisé */
        static uint64_t n_inv;
        uint64_t k = __atomic_add_fetch(&n_inv, 1, __ATOMIC_RELAXED);
        if (k <= 8) sc2_log("[FS] ReadFile(handle 0x%X, %u o) depuis 0x%08X : handle invalide (fermé ?) → ERROR_INVALID_HANDLE (n°%llu)",
                            A(0), A(2), rd32(c->esp), (unsigned long long)k);
        SetLastError(6); return 0;
    }
    ssize_t n;
    if (A(4)) {
        uint32_t ov = A(4);
        off_t off = (off_t)(rd32(ov + 8) | ((uint64_t)rd32(ov + 12) << 32));
        n = pread(fd, GPTR(A(1)), A(2), off);
        if (n > 0) sc2_mpq_lu(A(0), (uint64_t)off, GPTR(A(1)), (uint32_t)n);   /* 24/10 : alias .wav servis AVANT le signal */
        ov_fini(ov, n);
    } else {
        off_t off = lseek(fd, 0, SEEK_CUR);
        n = read(fd, GPTR(A(1)), A(2));
        if (n > 0 && off >= 0) sc2_mpq_lu(A(0), (uint64_t)off, GPTR(A(1)), (uint32_t)n);
    }
    if (A(3)) wr32(A(3), n > 0 ? (uint32_t)n : 0);
    return n >= 0;
}
static uint32_t h_WriteFile(CPU *c, const uint32_t *a)
{
    (void)c; int fd = fd_of(A(0)); if (fd < 0) { SetLastError(6); return 0; }
    ssize_t n;
    if (A(4)) {
        uint32_t ov = A(4);
        n = pwrite(fd, GPTR(A(1)), A(2), (off_t)(rd32(ov + 8) | ((uint64_t)rd32(ov + 12) << 32)));
        ov_fini(ov, n);
    } else n = write(fd, GPTR(A(1)), A(2));
    if (A(3)) wr32(A(3), n > 0 ? (uint32_t)n : 0);
    return n >= 0;
}
static uint32_t h_GetFileSize(CPU *c, const uint32_t *a)
{
    (void)c; struct stat st; int fd = fd_of(A(0));
    if (fd < 0 || fstat(fd, &st)) return 0xFFFFFFFFu;
    if (A(1)) wr32(A(1), (uint32_t)((uint64_t)st.st_size >> 32));
    return (uint32_t)st.st_size;
}
static uint32_t h_GetFileSizeEx(CPU *c, const uint32_t *a)
{
    (void)c; struct stat st; int fd = fd_of(A(0));
    if (fd < 0 || fstat(fd, &st)) return 0;
    wr32(A(1), (uint32_t)st.st_size); wr32(A(1) + 4, (uint32_t)((uint64_t)st.st_size >> 32));
    return 1;
}
/* L3 (24/10) : GetFileInformationByHandle(h, BY_HANDLE_FILE_INFORMATION *) — 52 o : attributs, 3 FILETIME (création
 * +4, accès +12, écriture +20), n° de volume +28, taille haute +32 / basse +36, liens +40, index haut +44 / bas +48.
 * Appelé par L_00995540 (← L_00B2A3F0) quand le dialogue Load liste les sauvegardes : il y lit taille et date
 * d'écriture. Absent, le runtime s'arrêtait (code 6) au clic sur Load. */
static void ecrire_filetime(uint32_t dst, struct timespec t)
{
    uint64_t ft = ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t.tv_nsec / 100u;
    wr32(dst, (uint32_t)ft); wr32(dst + 4, (uint32_t)(ft >> 32));
}
static uint32_t h_GetFileInformationByHandle(CPU *c, const uint32_t *a)
{
    (void)c; struct stat st; int fd = fd_of(A(0));
    if (fd < 0 || fstat(fd, &st)) { SetLastError(6); return 0; }
    uint32_t d = A(1);
    memset(GPTR(d), 0, 52);
    wr32(d, S_ISDIR(st.st_mode) ? 0x10u : 0x80u);
#ifdef __APPLE__                                     /* macOS : st_*timespec ; POSIX 2008 (Android) : st_*tim */
    ecrire_filetime(d + 4, st.st_birthtimespec);
    ecrire_filetime(d + 12, st.st_atimespec);
    ecrire_filetime(d + 20, st.st_mtimespec);
#else
    ecrire_filetime(d + 4, st.st_ctim);                  /* pas de date de création : changement d'état */
    ecrire_filetime(d + 12, st.st_atim);
    ecrire_filetime(d + 20, st.st_mtim);
#endif
    wr32(d + 28, (uint32_t)st.st_dev);
    wr32(d + 32, (uint32_t)((uint64_t)st.st_size >> 32)); wr32(d + 36, (uint32_t)st.st_size);
    wr32(d + 40, (uint32_t)st.st_nlink);
    wr32(d + 44, (uint32_t)((uint64_t)st.st_ino >> 32)); wr32(d + 48, (uint32_t)st.st_ino);
    return 1;
}
static uint32_t h_SetFilePointer(CPU *c, const uint32_t *a)
{
    (void)c; int fd = fd_of(A(0)); if (fd < 0) return 0xFFFFFFFFu;
    int64_t off = (int32_t)A(1);
    if (A(2)) off = (int64_t)(((uint64_t)rd32(A(2)) << 32) | A(1));
    off_t r = lseek(fd, off, (int)A(3));
    if (A(2)) wr32(A(2), (uint32_t)((uint64_t)r >> 32));
    return (uint32_t)r;
}
static uint32_t h_SetFilePointerEx(CPU *c, const uint32_t *a)   /* (h, lo, hi, *new, méthode) */
{
    (void)c; int fd = fd_of(A(0)); if (fd < 0) return 0;
    off_t r = lseek(fd, (int64_t)(((uint64_t)A(2) << 32) | A(1)), (int)A(4));
    if (A(3)) { wr32(A(3), (uint32_t)r); wr32(A(3) + 4, (uint32_t)((uint64_t)r >> 32)); }
    return r >= 0;
}
static uint32_t h_SetEndOfFile(CPU *c, const uint32_t *a)
{
    (void)c; int fd = fd_of(A(0)); if (fd < 0) return 0;
    return ftruncate(fd, lseek(fd, 0, SEEK_CUR)) == 0;
}
/* Passe 6a : énumération réelle. Le motif Windows (dernier composant : *, ?, insensible à la
 * casse ; « *.* » = tout) est appliqué au dossier hôte ; la liste est figée à FindFirstFile
 * (« . » et « .. » compris, comme Windows hors racine de lecteur) puis servie par FindNextFile.
 * WIN32_FIND_DATAW = 592 o : attrs, 3 FILETIME, taille haute/basse, 2 réservés, cFileName[260],
 * cAlternateFileName[14] ; variante A = 320 o (noms en octets). */
#define FIND_BASE 0x00110000u                /* handles de recherche : FIND_BASE + 4*i */
typedef struct { char **nom; uint8_t *dir; uint64_t *taille; int n, i; } fs_find;
static fs_find *g_find[64];
static pthread_mutex_t g_find_mu = PTHREAD_MUTEX_INITIALIZER;
static int motif_ok(const char *motif, const char *nom)
{
    if (!strcmp(motif, "*") || !strcmp(motif, "*.*")) return 1;
    size_t l = strlen(motif);
    if (l > 2 && !strcmp(motif + l - 2, ".*")) {           /* « x.* » accepte aussi « x » */
        char m[256]; snprintf(m, sizeof m, "%.*s", (int)(l - 2), motif);
        if (!fnmatch(m, nom, FNM_CASEFOLD)) return 1;
    }
    return !fnmatch(motif, nom, FNM_CASEFOLD);
}
static void find_ecrire(fs_find *f, uint32_t dst, int large)
{
    int i = f->i++;
    memset(GPTR(dst), 0, large ? 592 : 320);
    wr32(dst, f->dir[i] ? 0x10u : 0x80u);
    wr32(dst + 28, (uint32_t)(f->taille[i] >> 32)); wr32(dst + 32, (uint32_t)f->taille[i]);
    const char *nm = f->nom[i];
    for (uint32_t k = 0; nm[k] && k < 259; k++)
        if (large) wr16(dst + 44 + 2 * k, (uint8_t)nm[k]); else wr8(dst + 44 + k, (uint8_t)nm[k]);
}
/* Ordre des entrées d'une énumération : celui de readdir par défaut (hachage sous ext4, tri sous APFS, comme NTFS).
 * SC2_FS_TRI=nom (croissant, l'ordre de Windows) ou inverse (diagnostic du banc natif macOS, 19/10). */
static const char *g_fs_tri_ordre;
static int fs_tri_cmp(const void *x, const void *y)
{
    int r = strcmp(*(char *const *)x, *(char *const *)y);
    return g_fs_tri_ordre[0] == 'i' ? -r : r;
}
static void find_trier(fs_find *f)
{
    static int lu; static const char *e;
    if (!lu) { e = getenv("SC2_FS_TRI"); lu = 1; }
    if (!e || !*e || f->n < 2) return;
    g_fs_tri_ordre = e;
    int *ordre = malloc(sizeof(int) * (size_t)f->n); char **noms = malloc(sizeof(char *) * (size_t)f->n);
    for (int i = 0; i < f->n; i++) noms[i] = f->nom[i];
    qsort(noms, (size_t)f->n, sizeof *noms, fs_tri_cmp);
    for (int i = 0; i < f->n; i++) for (int j = 0; j < f->n; j++) if (f->nom[j] == noms[i]) { ordre[i] = j; break; }
    char **n2 = malloc(sizeof(char *) * (size_t)f->n); char *d2 = malloc((size_t)f->n); uint64_t *t2 = malloc(8 * (size_t)f->n);
    for (int i = 0; i < f->n; i++) { n2[i] = f->nom[ordre[i]]; d2[i] = f->dir[ordre[i]]; t2[i] = f->taille[ordre[i]]; }
    for (int i = 0; i < f->n; i++) { f->nom[i] = n2[i]; f->dir[i] = d2[i]; f->taille[i] = t2[i]; }
    free(ordre); free(noms); free(n2); free(d2); free(t2);
}
static uint32_t find_premier(const char *u, uint32_t dst, int large, uint32_t appelant)
{
    char p[1024]; hpath(u, p, sizeof p);
    char *sl = strrchr(p, '/');
    const char *motif = sl ? sl + 1 : p;
    /* le dernier composant est un motif : l'hôte ne l'a pas résolu, on reprend celui du jeu */
    const char *ws = strrchr(u, '\\'); if (!ws) ws = strrchr(u, '/');
    if (ws) motif = ws + 1;
    if (sl) *sl = 0;
    fs_find *f = calloc(1, sizeof *f);
    int cap = 0;
    DIR *d = opendir(sl ? p : ".");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!motif_ok(motif, e->d_name)) continue;
        if (f->n == cap) {
            cap = cap ? 2 * cap : 32;
            f->nom = realloc(f->nom, (size_t)cap * sizeof *f->nom);
            f->dir = realloc(f->dir, (size_t)cap); f->taille = realloc(f->taille, (size_t)cap * 8);
        }
        char q[1300]; struct stat st; snprintf(q, sizeof q, "%s/%s", p, e->d_name);
        int ok = !stat(q, &st);
        f->nom[f->n] = strdup(e->d_name);
        f->dir[f->n] = ok && S_ISDIR(st.st_mode); f->taille[f->n] = ok ? (uint64_t)st.st_size : 0;
        f->n++;
    }
    if (d) closedir(d);
    find_trier(f);
    g_fs_enum++;
    int vrais = 0; for (int i = 0; i < f->n; i++) vrais += strcmp(f->nom[i], ".") && strcmp(f->nom[i], "..");
    sc2_trace("[FS] FindFirstFile%s(\"%s\") depuis 0x%08X → %s%d entrée(s)%s%s", large ? "W" : "A", u, appelant,
            d ? "" : "dossier absent, ", vrais, f->n ? ", 1re : " : "", f->n ? f->nom[0] : "");
    if (!f->n) { free(f->nom); free(f->dir); free(f->taille); free(f); SetLastError(2); return 0xFFFFFFFFu; }
    pthread_mutex_lock(&g_find_mu);
    int slot = -1; for (int i = 0; i < 64; i++) if (!g_find[i]) { g_find[i] = f; slot = i; break; }
    pthread_mutex_unlock(&g_find_mu);
    if (slot < 0) { SetLastError(4); return 0xFFFFFFFFu; }
    find_ecrire(f, dst, large);
    SetLastError(0);
    return FIND_BASE + 4u * (uint32_t)slot;
}
static fs_find *find_of(uint32_t h)
{
    if (h < FIND_BASE || h >= FIND_BASE + 4 * 64 || (h & 3)) return NULL;
    return g_find[(h - FIND_BASE) / 4];
}
static uint32_t h_FindFirstFileW(CPU *c, const uint32_t *a)
{
    char u[1024]; w2u(A(0), u, sizeof u);
    return find_premier(u, A(1), 1, rd32(c->esp));
}
static uint32_t h_FindFirstFileA(CPU *c, const uint32_t *a) { return find_premier(STR(A(0)), A(1), 0, rd32(c->esp)); }
static uint32_t find_suivant(uint32_t h, uint32_t dst, int large)
{
    fs_find *f = find_of(h);
    if (!f) { SetLastError(6); return 0; }
    if (f->i >= f->n) { SetLastError(18); return 0; }      /* ERROR_NO_MORE_FILES */
    find_ecrire(f, dst, large);
    return 1;
}
static uint32_t h_FindNextFileW(CPU *c, const uint32_t *a) { (void)c; return find_suivant(A(0), A(1), 1); }
static uint32_t h_FindNextFileA(CPU *c, const uint32_t *a) { (void)c; return find_suivant(A(0), A(1), 0); }
static uint32_t h_FindClose(CPU *c, const uint32_t *a)
{
    (void)c; fs_find *f = find_of(A(0));
    if (!f) { SetLastError(6); return 0; }
    for (int i = 0; i < f->n; i++) free(f->nom[i]);
    free(f->nom); free(f->dir); free(f->taille); free(f);
    pthread_mutex_lock(&g_find_mu); g_find[(A(0) - FIND_BASE) / 4] = NULL; pthread_mutex_unlock(&g_find_mu);
    return 1;
}
static uint32_t h_GetTempPathW(CPU *c, const uint32_t *a)
{
    (void)c; const char *t = "C:\\Temp\\"; uint32_t n = (uint32_t)strlen(t);
    char p[1024]; hpath(t, p, sizeof p); mkdirs(p);
    if (n + 1 > A(0)) return n + 1;
    for (uint32_t i = 0; i <= n; i++) wr16(A(1) + 2 * i, (uint8_t)t[i]);
    return n;
}
static uint32_t put_w(uint32_t dst, uint32_t cap, const char *s)
{
    uint32_t n = (uint32_t)strlen(s);
    if (n + 1 > cap) return n + 1;
    for (uint32_t i = 0; i <= n; i++) wr16(dst + 2 * i, (uint8_t)s[i]);
    return n;
}
static uint32_t h_GetCurrentDirectoryW(CPU *c, const uint32_t *a) { (void)c; return put_w(A(1), A(0), g_cwd); }
static uint32_t h_SetCurrentDirectoryW(CPU *c, const uint32_t *a)
{
    (void)c; char u[512]; w2u(A(0), u, sizeof u);
    char abs_[1024]; chemin_absolu(u, abs_, sizeof abs_);
    snprintf(g_cwd, sizeof g_cwd, "%s", abs_);          /* passe 6a : relatif → absolu, « .. » résolu */
    size_t l = strlen(g_cwd); if (l > 3 && g_cwd[l - 1] == '\\') g_cwd[l - 1] = 0;
    char p[1024]; hpath(g_cwd, p, sizeof p);
    sc2_trace("[FS] SetCurrentDirectoryW(\"%s\") → cwd invité %s%s", u, g_cwd, file_attr(p) == 0x10u ? "" : " (ABSENT sur l'hôte)");
    return 1;
}

/* ---- passe 4 : _wfullpath et stdio du CRT (fichiers sous $SC2_ROOT) ------------ */
/* Chemin absolu Windows : lecteur + cwd invité, « . » et « .. » résolus. */
static void chemin_absolu(const char *rel, char *o, size_t cap)
{
    char t[1024];
    if (((rel[0] | 32) >= 'a' && (rel[0] | 32) <= 'z') && rel[1] == ':') snprintf(t, sizeof t, "%s", rel);
    else if (rel[0] == '\\' || rel[0] == '/') snprintf(t, sizeof t, "C:%s", rel);
    else snprintf(t, sizeof t, "%s\\%s", g_cwd, rel);
    for (char *q = t; *q; q++) if (*q == '/') *q = '\\';
    char *seg[128]; int n = 0; char *sv = NULL;
    for (char *tok = strtok_r(t + 2, "\\", &sv); tok; tok = strtok_r(NULL, "\\", &sv)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (n) n--; continue; }
        if (n < 128) seg[n++] = tok;
    }
    size_t k = (size_t)snprintf(o, cap, "%c:", t[0]);
    for (int i = 0; i < n && k < cap; i++) k += (size_t)snprintf(o + k, cap - k, "\\%s", seg[i]);
    if (!n && k + 1 < cap) { o[k++] = '\\'; o[k] = 0; }
    size_t l = strlen(rel);
    if (l && (rel[l - 1] == '\\' || rel[l - 1] == '/') && k + 1 < cap && o[k - 1] != '\\') { o[k++] = '\\'; o[k] = 0; }
}
static uint32_t h__wfullpath(CPU *c, const uint32_t *a)   /* (buf, rel, max) */
{
    (void)c;
    if (!A(1)) return 0;
    char rel[1024], abs_[1024]; w2u(A(1), rel, sizeof rel); chemin_absolu(rel, abs_, sizeof abs_);
    uint32_t n = (uint32_t)strlen(abs_), buf = A(0), cap = A(2);
    if (!buf) { buf = sc2_alloc(2 * n + 2); cap = n + 1; }
    if (n + 1 > cap) { wr32(g_errno, 34); return 0; }
    put_w(buf, cap, abs_);
    return buf;
}
static struct { uint32_t g; FILE *f; } g_fich[64];
static pthread_mutex_t g_fich_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *fich_of(uint32_t g)
{
    for (int i = 0; i < 64; i++) if (g_fich[i].g == g) return g_fich[i].f;
    return NULL;
}
static uint32_t fich_ouvrir(const char *win, const char *mode)
{
    char p[1024], m[8]; size_t k = 0;
    hpath(win, p, sizeof p);
    for (const char *q = mode; *q && k < 6; q++) if (strchr("rwab+", *q)) m[k++] = *q;   /* t, ccs=… ignorés */
    m[k] = 0;
    if (strchr(m, 'w') || strchr(m, 'a')) mkdirs(p);
    FILE *f = fopen(p, m);
    sc2_trace("[FS] fopen(\"%s\", \"%s\") → %s", win, mode, f ? "ouvert" : "absent");
    if (!f) { g_fs_absent++; wr32(g_errno, 2); return 0; }
    g_fs_ok++;
    pthread_mutex_lock(&g_fich_mu);
    for (int i = 0; i < 64; i++) if (!g_fich[i].g) { g_fich[i].g = sc2_alloc(32); g_fich[i].f = f;
        pthread_mutex_unlock(&g_fich_mu); return g_fich[i].g; }
    pthread_mutex_unlock(&g_fich_mu);
    fclose(f); return 0;
}
static uint32_t h_fopen(CPU *c, const uint32_t *a) { (void)c; return fich_ouvrir(STR(A(0)), STR(A(1))); }
static uint32_t h__wfopen_s(CPU *c, const uint32_t *a)   /* (FILE**, nom, mode) → errno */
{
    (void)c; char n[1024], m[32]; w2u(A(1), n, sizeof n); w2u(A(2), m, sizeof m);
    uint32_t f = fich_ouvrir(n, m);
    if (A(0)) wr32(A(0), f);
    return f ? 0 : 2;
}
static uint32_t h_fclose(CPU *c, const uint32_t *a)
{
    (void)c;
    for (int i = 0; i < 64; i++) if (g_fich[i].g == A(0)) { fclose(g_fich[i].f); g_fich[i].g = 0; return 0; }
    return 0xFFFFFFFFu;
}
static uint32_t h_fread(CPU *c, const uint32_t *a)  { (void)c; FILE *f = fich_of(A(3)); return f ? (uint32_t)fread(GPTR(A(0)), A(1), A(2), f) : 0; }
static uint32_t h_fwrite(CPU *c, const uint32_t *a)
{
    (void)c; FILE *f = fich_of(A(3));
    if (!f) { sc2_log("[CRT fwrite] %.*s", (int)(A(1) * A(2)), STR(A(0))); return A(2); }
    return (uint32_t)fwrite(GPTR(A(0)), A(1), A(2), f);
}
static uint32_t h_ftell(CPU *c, const uint32_t *a)  { (void)c; FILE *f = fich_of(A(0)); return f ? (uint32_t)ftell(f) : 0xFFFFFFFFu; }
static uint32_t h_fseek(CPU *c, const uint32_t *a)  { (void)c; FILE *f = fich_of(A(0)); return f ? (uint32_t)fseek(f, (int32_t)A(1), (int)A(2)) : 0xFFFFFFFFu; }
static uint32_t h_ferror(CPU *c, const uint32_t *a) { (void)c; FILE *f = fich_of(A(0)); return f ? (uint32_t)ferror(f) : 1; }
static uint32_t h_fflush(CPU *c, const uint32_t *a) { (void)c; FILE *f = fich_of(A(0)); if (f) fflush(f); return 0; }
static uint32_t h_rewind(CPU *c, const uint32_t *a) { (void)c; FILE *f = fich_of(A(0)); if (f) rewind(f); return 0; }
static uint32_t h_fputs(CPU *c, const uint32_t *a)
{
    (void)c; FILE *f = fich_of(A(1));
    if (!f) { sc2_log("[CRT fputs] %s", STR(A(0))); return 0; }
    return fputs(STR(A(0)), f) < 0 ? 0xFFFFFFFFu : 0;
}
static uint32_t h__wstat64(CPU *c, const uint32_t *a)    /* (chemin, struct __stat64*) */
{
    (void)c; char u[1024], p[1024]; w2u(A(0), u, sizeof u); hpath(u, p, sizeof p);
    struct stat st;
    int ko = stat(p, &st);
    sc2_trace("[FS] _wstat64(\"%s\") → %s", u, ko ? "absent" : S_ISDIR(st.st_mode) ? "dossier" : "fichier");
    if (ko) { g_fs_absent++; wr32(g_errno, 2); return 0xFFFFFFFFu; }
    g_fs_ok++;
    uint32_t b = A(1); memset(GPTR(b), 0, 56);
    wr16(b + 6, (uint16_t)(S_ISDIR(st.st_mode) ? 0x41FF : 0x81B6)); wr16(b + 8, 1);
    wr32(b + 24, (uint32_t)st.st_size); wr32(b + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    wr32(b + 32, (uint32_t)st.st_atime); wr32(b + 40, (uint32_t)st.st_mtime); wr32(b + 48, (uint32_t)st.st_ctime);
    return 0;
}
/* Passe 7l : polices système. Le moteur cherche arial.ttf dans CSIDL_FONTS (police de repli de ses
 * styles, FontStyles.SC2Style) : 854 recherches par run quand elle manque, et une mise en page des
 * dialogues différente de Windows (où Arial existe toujours). Arial n'est pas libre : on pose dans
 * C:\Windows\Fonts du bac à sable des liens vers ses équivalents MÉTRIQUES Liberation (SIL OFL),
 * pris dans $SC2_POLICES (défaut /usr/share/fonts/truetype/liberation ; SC2_POLICES=0 : aucune). */
static void polices_systeme(const char *hp)
{
    static const char *const sub[][2] = {
        { "arial.ttf", "LiberationSans-Regular.ttf" }, { "arialbd.ttf", "LiberationSans-Bold.ttf" },
        { "ariali.ttf", "LiberationSans-Italic.ttf" }, { "arialbi.ttf", "LiberationSans-BoldItalic.ttf" },
        { "times.ttf", "LiberationSerif-Regular.ttf" }, { "cour.ttf", "LiberationMono-Regular.ttf" } };
    const char *dir = getenv("SC2_POLICES");
    if (dir && !strcmp(dir, "0")) return;
    if (!dir || !*dir) dir = "/usr/share/fonts/truetype/liberation";
    int n = 0;
    for (size_t i = 0; i < sizeof sub / sizeof sub[0]; i++) {
        char src[1024], dst[1200]; struct stat st;
        snprintf(src, sizeof src, "%s/%s", dir, sub[i][1]); snprintf(dst, sizeof dst, "%s/%s", hp, sub[i][0]);
        if (stat(src, &st) || !lstat(dst, &st)) continue;
        if (!symlink(src, dst)) n++;
    }
    if (n) sc2_log("[POLICE] C:\\Windows\\Fonts : %d polices système posées (Liberation, équivalents métriques d'Arial/Times/Courier, depuis %s)", n, dir);
}
static uint32_t h_SHGetFolderPathW(CPU *c, const uint32_t *a)   /* (hwnd, csidl, token, flags, path) */
{
    (void)c;
    const char *p;
    switch (A(1) & 0xFF) {
    case 5:  p = "C:\\Users\\sc2\\Documents"; break;
    case 0x1A: p = "C:\\Users\\sc2\\AppData\\Roaming"; break;
    case 0x1C: p = "C:\\Users\\sc2\\AppData\\Local"; break;
    case 0x23: p = "C:\\ProgramData"; break;
    case 0x14: p = "C:\\Windows\\Fonts"; break;         /* CSIDL_FONTS (passe 7l ; avant : C:\\Users\\sc2) */
    default: p = "C:\\Users\\sc2"; break;
    }
    char hp[1024]; hpath(p, hp, sizeof hp); mkdirs(hp); mkdir(hp, 0755);
    if ((A(1) & 0xFF) == 0x14) polices_systeme(hp);
    put_w(A(4), 260, p);
    sc2_trace("[IAT]   SHGetFolderPathW(CSIDL 0x%X) → %s", A(1), p);
    return 0;
}
static uint32_t h_GetComputerNameW(CPU *c, const uint32_t *a) { (void)c; uint32_t n = put_w(A(0), rd32(A(1)), "SC2HOST"); wr32(A(1), n); return 1; }
static uint32_t h_GetUserNameW(CPU *c, const uint32_t *a)     { (void)c; uint32_t n = put_w(A(0), rd32(A(1)), "sc2"); wr32(A(1), n + 1); return 1; }
static uint32_t h_GetDiskFreeSpaceExW(CPU *c, const uint32_t *a)
{
    (void)c;
    for (int k = 1; k <= 3; k++) if (A(k)) { wr32(A(k), 0); wr32(A(k) + 4, 0x10); }   /* 64 Go */
    return 1;
}
static uint32_t h_GlobalMemoryStatusEx(CPU *c, const uint32_t *a)
{
    (void)c; uint32_t m = A(0);
    wr32(m + 4, 30);
    for (int k = 0; k < 6; k++) { wr32(m + 8 + 8 * k, 0); wr32(m + 12 + 8 * k, k % 2 ? 1 : 2); }  /* 4–8 Go */
    return 1;
}
static uint32_t h_GetLocalTime(CPU *c, const uint32_t *a)
{
    (void)c; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    uint16_t v[8] = { (uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday,
                      (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, 0 };
    for (int k = 0; k < 8; k++) wr16(A(0) + 2 * (uint32_t)k, v[k]);
    return 0;
}
static uint32_t h_GetTimeZoneInformation(CPU *c, const uint32_t *a) { (void)c; memset(GPTR(A(0)), 0, 172); return 0; }
static uint32_t h_GetACP(CPU *c, const uint32_t *a) { (void)c; (void)a; return 1252; }
static uint32_t h_GetLocaleInfoW(CPU *c, const uint32_t *a) { (void)c; return A(3) ? put_w(A(2), A(3), "1") + 1 : 2; }
static uint32_t h_MulDiv(CPU *c, const uint32_t *a) { (void)c; return A(2) ? (uint32_t)(int32_t)(((int64_t)(int32_t)A(0) * (int32_t)A(1)) / (int32_t)A(2)) : 0xFFFFFFFFu; }
static uint32_t h_timeGetTime(CPU *c, const uint32_t *a) { return h_GetTickCount(c, a); }
static uint32_t h_reg_absent(CPU *c, const uint32_t *a) { (void)c; (void)a; return 2; }   /* ERROR_FILE_NOT_FOUND */
static uint32_t h_GetSystemMetrics(CPU *c, const uint32_t *a)
{
    (void)c;
    switch (A(0)) { case 0: case 16: case 78: return 1920; case 1: case 17: case 79: return 1080;
                    case 80: return 1; default: return 0; }
}
static uint32_t h_MessageBoxW(CPU *c, const uint32_t *a)
{
    char t[1024], cap[256]; w2u(A(1), t, sizeof t); w2u(A(2), cap, sizeof cap);
    sc2_log("[MESSAGEBOX] « %s » : %s   (appelant 0x%08X) → IDOK", cap, t, rd32(c->esp));
    sc2_dump_ss("MessageBoxW");
    return 1;
}
static uint32_t h_MessageBoxA(CPU *c, const uint32_t *a)
{
    sc2_log("[MESSAGEBOX] « %s » : %s   (appelant 0x%08X) → IDOK", A(2) ? STR(A(2)) : "", A(1) ? STR(A(1)) : "", rd32(c->esp));
    return 1;
}
/* Threads invités RÉELS : un pthread hôte par thread invité (pile native 64 Mo :
 * le C lifté à -O0 a de gros cadres), avec son CPU-struct, sa pile invitée, son
 * TEB (fs:), sa pile fantôme. Le code lifté est réentrant (modèle CPU-struct) ;
 * la mémoire invitée est partagée, les verrous du jeu (sections critiques,
 * événements, Interlocked*) sont réels. Le handle de thread est un événement
 * manuel signalé à la fin du thread. CREATE_SUSPENDED : départ à ResumeThread. */
typedef struct {
    uint32_t routine, param, stack_top, stack_limit, handle;
    volatile uint32_t exit_code, suspended;
    pthread_mutex_t mu; pthread_cond_t cv;
} gthread;
static gthread *g_threads[256]; static int g_nthreads;
static pthread_mutex_t g_threads_mu = PTHREAD_MUTEX_INITIALIZER;

static void *thread_host_main(void *p)
{
    gthread *t = (gthread *)p;
    sc2_thread_fault_stack();
#ifndef __APPLE__
    char nom[16]; snprintf(nom, sizeof nom, "j%08X", t->routine);   /* passe 10r : profils par thread (sc2prof-fils) */
    pthread_setname_np(pthread_self(), nom);
#endif
    pthread_mutex_lock(&t->mu);
    while (t->suspended) pthread_cond_wait(&t->cv, &t->mu);
    pthread_mutex_unlock(&t->mu);
    CPU *c = (CPU *)calloc(1, sizeof(CPU));
    g_sc2_teb = sc2_new_teb(t->stack_top, t->stack_limit);
    g_sc2_ss = (sc2_shadow *)calloc(1, sizeof(sc2_shadow));
    sc2_ss_register(g_sc2_ss, "thread %d (routine 0x%08X)", (int)gettid(), t->routine);
    c->fs_base = g_sc2_teb; c->esp = t->stack_top;
    c->fantome = g_sc2_ss;                             /* passe 10r : pile fantôme portée par le CPU du fil */
    sc2_trace("[THREAD %d] démarrage : routine invitée 0x%08X, param 0x%08X", (int)gettid(), t->routine, t->param);
    uint32_t param = t->param;
    t->exit_code = sc2_call_guest(c, t->routine, 1, &param, 0);
    sc2_trace("[THREAD %d] routine 0x%08X terminée, code 0x%08X", (int)gettid(), t->routine, t->exit_code);
    SetEvent(HH(t->handle));
    return NULL;
}
static uint32_t h_CreateThread(CPU *c, const uint32_t *a)  /* (sa, pile, routine, param, flags, *tid) */
{
    gthread *t = (gthread *)calloc(1, sizeof *t);
    t->routine = A(2); t->param = A(3); t->exit_code = 259;  /* STILL_ACTIVE */
    t->suspended = (A(4) & 4u) != 0;                          /* CREATE_SUSPENDED */
    t->stack_top = sc2_reserve_stack(A(1) > 0x100000u ? A(1) : 0x100000u, &t->stack_limit);
    t->handle = H(CreateEventA(NULL, TRUE, FALSE, NULL));
    pthread_mutex_init(&t->mu, NULL); pthread_cond_init(&t->cv, NULL);
    pthread_mutex_lock(&g_threads_mu);
    int id = g_nthreads < 256 ? g_nthreads++ : 255;
    g_threads[id] = t;
    pthread_mutex_unlock(&g_threads_mu);
    if (A(5)) wr32(A(5), 0x1000u + (uint32_t)id);
    sc2_trace("[THREAD] CreateThread(routine 0x%08X, param 0x%08X%s) depuis 0x%08X → handle 0x%X",
            A(2), A(3), t->suspended ? ", SUSPENDU" : "", rd32(c->esp), t->handle);
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t pt;
    if (pthread_create(&pt, &at, thread_host_main, t)) sc2_stop(12, "pthread_create a échoué");
    pthread_attr_destroy(&at);
    return t->handle;
}
static gthread *thread_of(uint32_t h)
{
    gthread *r = NULL;
    pthread_mutex_lock(&g_threads_mu);
    for (int i = 0; i < g_nthreads; i++) if (g_threads[i]->handle == h) r = g_threads[i];
    pthread_mutex_unlock(&g_threads_mu);
    return r;
}
static uint32_t h_ResumeThread(CPU *c, const uint32_t *a)
{
    (void)c; gthread *t = thread_of(A(0));
    if (!t) return 0xFFFFFFFFu;
    pthread_mutex_lock(&t->mu);
    uint32_t was = t->suspended; t->suspended = 0;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mu);
    return was;
}
static uint32_t h_GetExitCodeThread(CPU *c, const uint32_t *a)
{
    (void)c; gthread *t = thread_of(A(0));
    wr32(A(1), t ? t->exit_code : 259); return 1;
}

/* Sections critiques réelles : mutex récursif hôte associé à l'adresse invitée. */
typedef struct { uint32_t va; pthread_mutex_t mu; } gcs;
static gcs *g_cs[8192]; static int g_ncs;
static pthread_mutex_t g_cs_mu = PTHREAD_MUTEX_INITIALIZER;
/* Passe 10r : recherche SANS verrou. Avant, chaque EnterCriticalSection/LeaveCriticalSection prenait g_cs_mu (verrou
 * global commun à tous les threads) et parcourait linéairement jusqu'à 8 192 sections : convoi de verrous, futex dans
 * le fil du jeu (≈ 35 % de son CPU au profil sc2prof-fils) et 5 threads ouvriers à ~100 % libc. Table de hachage à
 * adressage ouvert : les sections ne sont jamais retirées (DeleteCriticalSection = h_ret0), donc une case écrite ne
 * change plus. Création sous g_cs_mu : mutex publié AVANT l'adresse (release), lu après elle (acquire). */
#define CS_HACHE_BITS 14
#define CS_HACHE (1u << CS_HACHE_BITS)                 /* > 2 × 8 192 : sondage court */
static struct { uint32_t va; pthread_mutex_t *mu; } g_cs_hache[CS_HACHE];
static pthread_mutex_t *cs_of(uint32_t va)
{
    uint32_t h0 = (va * 2654435761u) >> (32 - CS_HACHE_BITS), h = h0;
    for (uint32_t k = 0; k < CS_HACHE; k++, h = (h + 1) & (CS_HACHE - 1)) {
        uint32_t v = __atomic_load_n(&g_cs_hache[h].va, __ATOMIC_ACQUIRE);
        if (v == va) return g_cs_hache[h].mu;
        if (!v) break;                                  /* absente : création sous verrou */
    }
    pthread_mutex_lock(&g_cs_mu);
    for (h = h0; g_cs_hache[h].va; h = (h + 1) & (CS_HACHE - 1))
        if (g_cs_hache[h].va == va) { pthread_mutex_unlock(&g_cs_mu); return g_cs_hache[h].mu; }  /* créée entre-temps */
    if (g_ncs >= 8192) { pthread_mutex_unlock(&g_cs_mu); sc2_stop(13, "plus de 8192 sections critiques"); }
    gcs *n = (gcs *)calloc(1, sizeof *n);
    n->va = va;
    pthread_mutexattr_t at; pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&n->mu, &at);
    g_cs[g_ncs++] = n;
    g_cs_hache[h].mu = &n->mu;
    __atomic_store_n(&g_cs_hache[h].va, va, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_cs_mu);
    return &n->mu;
}
static uint32_t h_InitCS(CPU *c, const uint32_t *a)   { (void)c; memset(GPTR(A(0)), 0, 24); cs_of(A(0)); return 1; }
static uint32_t h_EnterCS(CPU *c, const uint32_t *a)  /* SC2_ATTENTES : seules les prises CONTESTÉES sont comptées */
{
    (void)c; pthread_mutex_t *mu = cs_of(A(0));
    if (pthread_mutex_trylock(mu) == 0) return 0;
    { uint64_t t0 = &sc2_att_debut ? sc2_att_debut() : 0;
      pthread_mutex_lock(mu); if (t0) sc2_att_fin(ATT_CS, 0, t0, rd32(c->esp)); }
    return 0;
}
static uint32_t h_LeaveCS(CPU *c, const uint32_t *a)  { (void)c; pthread_mutex_unlock(cs_of(A(0))); return 0; }
static uint32_t h_TryEnterCS(CPU *c, const uint32_t *a) { (void)c; return pthread_mutex_trylock(cs_of(A(0))) == 0; }

/* Mutex/sémaphores : objets nommés via les événements de stubs_sc2 (un seul thread
 * invité : acquisition toujours immédiate). Garde ERROR_ALREADY_EXISTS. */
static uint32_t h_CreateMutexA(CPU *c, const uint32_t *a)
{
    (void)c; return H(CreateEventA(NULL, FALSE, TRUE, A(2) ? STR(A(2)) : NULL));
}
static uint32_t h_ReleaseMutex(CPU *c, const uint32_t *a) { (void)c; return (uint32_t)SetEvent(HH(A(0))); }
static uint32_t h_CreateSemaphoreA(CPU *c, const uint32_t *a)
{
    (void)c; return H(CreateEventA(NULL, TRUE, A(1) > 0, A(3) ? STR(A(3)) : NULL));
}
static uint32_t h_CloseHandle(CPU *c, const uint32_t *a)
{
    (void)c;
    int fd = fd_of(A(0));
    if (fd >= 0) { sc2_mpq_ferme(A(0)); close(fd); __atomic_store_n(&g_fd[(A(0) - FH_BASE) / 4], 0, __ATOMIC_RELEASE); return 1; }
    return A(0) >= 0xFFFFFFFEu ? 1 : (uint32_t)CloseHandle(HH(A(0)));
}
static uint32_t h_WaitForSingleObjectEx(CPU *c, const uint32_t *a)
{ (void)c; uint64_t t0 = &sc2_att_debut ? sc2_att_debut() : 0; uint32_t r = WaitForSingleObject(HH(A(0)), A(1));
  if (t0) sc2_att_fin(ATT_WAIT, A(1), t0, rd32(c->esp)); return r; }


/* ========================================================================== */
/* WinSock (WSOCK32, par ordinaux) : pile présente, AUCUNE connectivité        */
/* ========================================================================== */

/* COM : aucune classe enregistrée (ex. pare-feu « NetFirewall » 010404A0). */
static uint32_t h_CoCreateInstance(CPU *c, const uint32_t *a)   /* (clsid, outer, ctx, iid, *ppv) */
{
    if (A(4)) wr32(A(4), 0);
    if (rd32(A(0)) == 0x529A9E6Bu && rd32(A(0) + 4) == 0x4F236587u) {      /* CLSID_TF_ThreadMgr */
        uint32_t o = sc2_com_tsf_create();
        wr32(A(4), o);
        sc2_log("[COM] CoCreateInstance(CLSID_TF_ThreadMgr) depuis 0x%08X → ITfThreadMgr factice 0x%08X", rd32(c->esp), o);
        return 0;
    }
    sc2_log("[COM] CoCreateInstance(CLSID {%08X-…}) depuis 0x%08X → REGDB_E_CLASSNOTREG", rd32(A(0)), rd32(c->esp));
    return 0x80040154u;
}

/* __RTDynamicCast (passe 7h, MSVCR80, cdecl) : dynamic_cast<> de MSVC sur les structures RTTI 32 bits de
 * SC2.exe. (inptr, VfDelta, SrcType, TargetType, isReference) → pointeur ajusté vers TargetType, ou NULL.
 *   vtable = *(objet + VfDelta) ; COL = vtable[-1] : signature, offset, cdOffset, pTypeDescriptor, pClassDescriptor ;
 *   objet complet = (objet + VfDelta) - COL.offset ; ClassHierarchyDescriptor : signature, attributs, numBaseClasses,
 *   pBaseClassArray ; BaseClassDescriptor : pTypeDescriptor, numContainedBases, PMD {mdisp, pdisp, vdisp}, attributs.
 *   Cible trouvée par adresse du TypeDescriptor, sinon par nom décoré (TypeDescriptor + 8). Pas de contrôle
 *   d'ambiguïté ni d'accessibilité (premier descripteur qui correspond) ; isReference et échec : NULL, journalisé. */
static int rtti_meme_type(uint32_t a, uint32_t b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    return !strcmp((const char *)GPTR(a + 8), (const char *)GPTR(b + 8));
}
static uint32_t h___RTDynamicCast(CPU *c, const uint32_t *a)
{
    (void)c;
    uint32_t obj = A(0), vfdelta = A(1), cible = A(3);
    if (!obj) return 0;
    uint32_t vt = rd32(obj + vfdelta), col = rd32(vt - 4);
    uint32_t complet = obj + vfdelta - rd32(col + 4);
    uint32_t chd = rd32(col + 16), nbases = rd32(chd + 8), bca = rd32(chd + 12);
    for (uint32_t i = 0; i < nbases; i++) {
        uint32_t bcd = rd32(bca + 4 * i);
        if (!rtti_meme_type(rd32(bcd), cible)) continue;
        int32_t mdisp = (int32_t)rd32(bcd + 8), pdisp = (int32_t)rd32(bcd + 12), vdisp = (int32_t)rd32(bcd + 16);
        uint32_t r = complet;
        if (pdisp >= 0) r += (uint32_t)pdisp + rd32(rd32(complet + (uint32_t)pdisp) + (uint32_t)vdisp);
        return r + (uint32_t)mdisp;
    }
    static int n;
    if (n++ < 8) sc2_log("[RTTI] __RTDynamicCast(0x%08X → « %s ») : type absent de la hiérarchie de « %s » → NULL%s", obj,
                         cible ? (const char *)GPTR(cible + 8) : "?", (const char *)GPTR(rd32(col + 12) + 8),
                         A(4) ? " (référence : std::bad_cast non levé)" : "");
    return 0;
}

static uint32_t g_wsa_err;
static uint32_t h_WSAStartup(CPU *c, const uint32_t *a)
{
    (void)c; memset(GPTR(A(1)), 0, 400);
    wr16(A(1), (uint16_t)A(0)); wr16(A(1) + 2, 0x0202);
    sc2_log("[RÉSEAU] WSAStartup(0x%04X) : pile simulée, sans connectivité (socket() échouera)", A(0));
    return 0;
}
static uint32_t h_sock_fail(CPU *c, const uint32_t *a)  { (void)c; (void)a; g_wsa_err = 10050; return 0xFFFFFFFFu; }  /* WSAENETDOWN */
static uint32_t h_null_net(CPU *c, const uint32_t *a)   { (void)c; (void)a; g_wsa_err = 11001; return 0; }           /* WSAHOST_NOT_FOUND */
static uint32_t h_WSAGetLastError(CPU *c, const uint32_t *a) { (void)c; (void)a; return g_wsa_err; }
/* WSAIoctl (passe 7h) : atteint au chargement d'une carte (0x00BE0E4C, sur le socket que socket() a refusé) —
 * hors ligne strict : échec SOCKET_ERROR / WSAENOTSOCK, aucun socket hôte. */
static uint32_t h_WSAIoctl(CPU *c, const uint32_t *a)
{
    (void)c;
    static int n;
    if (n++ < 3) sc2_log("[RÉSEAU] WSAIoctl(socket 0x%X, code 0x%08X) → SOCKET_ERROR, WSAENOTSOCK (hors ligne)", A(0), A(1));
    if (A(6)) wr32(A(6), 0);
    g_wsa_err = 10038;
    return 0xFFFFFFFFu;
}
static uint32_t h_htonl(CPU *c, const uint32_t *a) { (void)c; return __builtin_bswap32(A(0)); }
static uint32_t h_htons(CPU *c, const uint32_t *a) { (void)c; return __builtin_bswap16((uint16_t)A(0)); }
static uint32_t h_gethostname(CPU *c, const uint32_t *a) { (void)c; snprintf((char *)GPTR(A(0)), A(1), "sc2host"); return 0; }
static uint32_t h_inet_addr(CPU *c, const uint32_t *a)
{
    (void)c; unsigned b[4];
    if (sscanf(STR(A(0)), "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4) return 0xFFFFFFFFu;
    return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
}

/* ========================================================================== */
/* Table                                                                      */
/* ========================================================================== */

static const imp_def g_defs[] = {
    /* MSVCR80 — démarrage */
    { "__set_app_type", 1, CDECL, h_ret0 }, { "__p__fmode", 0, CDECL, h___p__fmode },
    { "__p__commode", 0, CDECL, h___p__commode }, { "__setusermatherr", 1, CDECL, h_ret0 },
    { "_configthreadlocale", 1, CDECL, h_ret0 }, { "_controlfp_s", 3, CDECL, h_ret0 },
    { "_initterm", 2, CDECL, h__initterm }, { "_initterm_e", 2, CDECL, h__initterm_e },
    { "__wgetmainargs", 5, CDECL, h___wgetmainargs }, { "_encode_pointer", 1, CDECL, h_arg0 },
    { "_decode_pointer", 1, CDECL, h_arg0 }, { "_onexit", 1, CDECL, h_arg0 },
    { "__dllonexit", 3, CDECL, h_arg0 }, { "_lock", 1, CDECL, h_ret0 }, { "_unlock", 1, CDECL, h_ret0 },
    { "_amsg_exit", 1, CDECL, h_amsg_exit }, { "exit", 1, CDECL, h_exit }, { "_exit", 1, CDECL, h_exit },
    { "_cexit", 0, CDECL, h_ret0 }, { "_XcptFilter", 2, CDECL, h_ret0 },
    { "_crt_debugger_hook", 1, CDECL, h_ret0 }, { "_invoke_watson", 5, CDECL, h_fatal },
    { "_invalid_parameter_noinfo", 0, CDECL, h_fatal }, { "abort", 0, CDECL, h_fatal },
    { "_purecall", 0, CDECL, h_fatal }, { "?terminate@@YAXXZ", 0, CDECL, h_fatal },
    { "_errno", 0, CDECL, h__errno }, { "__iob_func", 0, CDECL, h___iob_func },
    { "_get_heap_handle", 0, CDECL, h_HeapCreate },
    /* MSVCR80 — mémoire, chaînes */
    { "malloc", 1, CDECL, h_malloc }, { "free", 1, CDECL, h_free }, { "realloc", 2, CDECL, h_realloc },
    { "_aligned_malloc", 2, CDECL, h__aligned_malloc }, { "_aligned_free", 1, CDECL, h_ret0 },
    { "memset", 3, CDECL, h_memset }, { "memcpy", 3, CDECL, h_memcpy }, { "memmove", 3, CDECL, h_memcpy },
    { "memcpy_s", 4, CDECL, h_memcpy_s }, { "memmove_s", 4, CDECL, h_memcpy_s }, { "memchr", 3, CDECL, h_memchr },
    { "strncmp", 3, CDECL, h_strncmp }, { "_stricmp", 2, CDECL, h__stricmp }, { "_strnicmp", 3, CDECL, h__strnicmp },
    { "strchr", 2, CDECL, h_strchr }, { "strrchr", 2, CDECL, h_strrchr }, { "strstr", 2, CDECL, h_strstr },
    { "strspn", 2, CDECL, h_strspn }, { "strcspn", 2, CDECL, h_strcspn }, { "strncpy", 3, CDECL, h_strncpy },
    { "strncat", 3, CDECL, h_strncat }, { "strcpy_s", 3, CDECL, h_strcpy_s }, { "strcat_s", 3, CDECL, h_strcat_s },
    { "strncpy_s", 4, CDECL, h_strncpy_s }, { "wcscpy_s", 3, CDECL, h_wcscpy_s }, { "wcscat_s", 3, CDECL, h_wcscat_s },
    { "wcsrchr", 2, CDECL, h_wcsrchr }, { "_wcsnicmp", 3, CDECL, h__wcsnicmp }, { "_wcsicoll", 2, CDECL, h__wcsicoll },
    { "wcscoll", 2, CDECL, h_wcscoll }, { "toupper", 1, CDECL, h_toupper }, { "tolower", 1, CDECL, h_tolower },
    { "isspace", 1, CDECL, h_isspace }, { "isdigit", 1, CDECL, h_isdigit }, { "isalpha", 1, CDECL, h_isalpha },
    { "isxdigit", 1, CDECL, h_isxdigit }, { "isprint", 1, CDECL, h_isprint }, { "atoi", 1, CDECL, h_atoi },
    { "atol", 1, CDECL, h_atoi }, { "strtol", 3, CDECL, h_strtol }, { "strtoul", 3, CDECL, h_strtoul },
    { "_strtoi64", 3, CDECL, h__strtoi64 }, { "_atoi64", 1, CDECL, h__atoi64 }, { "atof", 1, CDECL, h_atof },
    { "strtod", 2, CDECL, h_strtod }, { "floor", 2, CDECL, h_floor }, { "ceil", 2, CDECL, h_ceil },
    { "ldexp", 3, CDECL, h_ldexp }, { "frexp", 3, CDECL, h_frexp }, { "modf", 3, CDECL, h_modf },
    { "_CIsqrt", 0, CDECL, h__CIsqrt }, { "_CIsin", 0, CDECL, h__CIsin }, { "_CIcos", 0, CDECL, h__CIcos },
    { "_CItan", 0, CDECL, h__CItan }, { "_CIasin", 0, CDECL, h__CIasin }, { "_CIacos", 0, CDECL, h__CIacos },
    { "_CIatan", 0, CDECL, h__CIatan }, { "_CIexp", 0, CDECL, h__CIexp }, { "_CIlog", 0, CDECL, h__CIlog },
    { "_CIlog10", 0, CDECL, h__CIlog10 }, { "_CIpow", 0, CDECL, h__CIpow }, { "_CIatan2", 0, CDECL, h__CIatan2 },
    { "_CIfmod", 0, CDECL, h__CIfmod }, { "_finite", 2, CDECL, h__finite },
    { "rand", 0, CDECL, h_rand }, { "srand", 1, CDECL, h_srand }, { "_time64", 1, CDECL, h__time64 },
    { "getenv", 1, CDECL, h_getenv },
    { "sprintf", 2, CDECL, h_sprintf }, { "sprintf_s", 3, CDECL, h_sprintf_s }, { "_snprintf", 3, CDECL, h__snprintf },
    { "vsprintf", 3, CDECL, h_vsprintf }, { "_vsnprintf", 4, CDECL, h__vsnprintf },
    { "_vsnprintf_s", 5, CDECL, h__vsnprintf_s }, { "_vscprintf", 2, CDECL, h__vscprintf },
    { "swprintf_s", 3, CDECL, h_swprintf_s }, { "vswprintf_s", 4, CDECL, h_vswprintf_s },
    { "_vsnwprintf", 4, CDECL, h__vsnwprintf }, { "fprintf", 2, CDECL, h_fprintf },
    /* KERNEL32 (stdcall) */
    { "GetSystemTimeAsFileTime", 1, STD, h_GetSystemTimeAsFileTime },
    { "GetCurrentProcessId", 0, STD, h_GetCurrentProcessId }, { "GetCurrentThreadId", 0, STD, h_GetCurrentThreadId },
    { "GetCurrentProcess", 0, STD, h_GetCurrentProcess }, { "GetCurrentThread", 0, STD, h_GetCurrentThread },
    { "GetTickCount", 0, STD, h_GetTickCount }, { "QueryPerformanceCounter", 1, STD, h_QueryPerformanceCounter },
    { "QueryPerformanceFrequency", 1, STD, h_QueryPerformanceFrequency },
    { "GetStartupInfoW", 1, STD, h_GetStartupInfoW }, { "GetStartupInfoA", 1, STD, h_GetStartupInfoW },
    { "InterlockedCompareExchange", 3, STD, h_InterlockedCompareExchange },
    { "InterlockedExchange", 2, STD, h_InterlockedExchange }, { "InterlockedIncrement", 1, STD, h_InterlockedIncrement },
    { "InterlockedDecrement", 1, STD, h_InterlockedDecrement },
    { "InterlockedExchangeAdd", 2, STD, h_InterlockedExchangeAdd }, { "Sleep", 1, STD, h_Sleep },
    { "GetModuleHandleW", 1, STD, h_GetModuleHandleW }, { "GetModuleHandleA", 1, STD, h_GetModuleHandleA },
    { "GetModuleFileNameW", 3, STD, h_GetModuleFileNameW },
    { "LoadLibraryW", 1, STD, h_LoadLibraryW }, { "LoadLibraryA", 1, STD, h_LoadLibraryA },
    { "LoadLibraryExW", 3, STD, h_LoadLibraryW }, { "LoadLibraryExA", 3, STD, h_LoadLibraryA },
    { "FreeLibrary", 1, STD, h_ret1 }, { "GetProcAddress", 2, STD, h_GetProcAddress },
    { "SetUnhandledExceptionFilter", 1, STD, h_ret0 }, { "UnhandledExceptionFilter", 1, STD, h_ret0 },
    { "TerminateProcess", 2, STD, h_TerminateProcess }, { "ExitProcess", 1, STD, h_ExitProcess },
    { "IsDebuggerPresent", 0, STD, h_IsDebuggerPresent },
    { "GetLastError", 0, STD, h_GetLastError }, { "SetLastError", 1, STD, h_SetLastError },
    { "TlsAlloc", 0, STD, h_TlsAlloc }, { "TlsGetValue", 1, STD, h_TlsGetValue },
    { "TlsSetValue", 2, STD, h_TlsSetValue }, { "TlsFree", 1, STD, h_ret1 },
    { "InitializeCriticalSection", 1, STD, h_InitCS }, { "InitializeCriticalSectionAndSpinCount", 2, STD, h_InitCS },
    { "EnterCriticalSection", 1, STD, h_EnterCS }, { "LeaveCriticalSection", 1, STD, h_LeaveCS },
    { "DeleteCriticalSection", 1, STD, h_ret0 }, { "TryEnterCriticalSection", 1, STD, h_TryEnterCS }, { "CreateProcessW", 10, STD, h_CreateProcessW_fake },  /* faux succès (fix Fold 16/10 M4, à valider) */
    { "HeapCreate", 3, STD, h_HeapCreate }, { "GetProcessHeap", 0, STD, h_HeapCreate },
    { "HeapAlloc", 3, STD, h_HeapAlloc }, { "HeapFree", 3, STD, h_HeapFree },
    { "HeapReAlloc", 4, STD, h_HeapReAlloc }, { "HeapSize", 3, STD, h_HeapSize },
    { "HeapSetInformation", 4, STD, h_ret1 },
    { "VirtualAlloc", 4, STD, h_VirtualAlloc }, { "VirtualFree", 3, STD, h_VirtualFree },
    { "VirtualQuery", 3, STD, h_VirtualQuery }, { "VirtualProtect", 4, STD, h_ret1 },
    { "GetVersionExW", 1, STD, h_GetVersionExW }, { "GetVersionExA", 1, STD, h_GetVersionExW },
    { "GetVersion", 0, STD, h_GetVersion }, { "GetCommandLineW", 0, STD, h_GetCommandLineW },
    { "GetSystemInfo", 1, STD, h_GetSystemInfo },
    { "ConvertThreadToFiber", 1, STD, h_ConvertThreadToFiber }, { "CreateFiberEx", 5, STD, h_CreateFiberEx },
    { "CreateFiber", 3, STD, h_CreateFiber }, { "SwitchToFiber", 1, STD, h_SwitchToFiber },
    { "DeleteFiber", 1, STD, h_DeleteFiber }, { "SetProcessDEPPolicy", 1, STD, h_ret1 },
    { "SetErrorMode", 1, STD, h_ret0 },
    { "MultiByteToWideChar", 6, STD, h_MultiByteToWideChar }, { "WideCharToMultiByte", 8, STD, h_WideCharToMultiByte },
    { "lstrlenW", 1, STD, h_lstrlenW },
    { "OutputDebugStringW", 1, STD, h_OutputDebugStringW }, { "OutputDebugStringA", 1, STD, h_OutputDebugStringA },
    { "CreateEventW", 4, STD, h_CreateEventW }, { "CreateEventA", 4, STD, h_CreateEventA },
    { "OpenEventW", 3, STD, h_OpenEventW }, { "OpenEventA", 3, STD, h_OpenEventA },
    { "SetEvent", 1, STD, h_SetEvent }, { "ResetEvent", 1, STD, h_ResetEvent },
    { "WaitForSingleObject", 2, STD, h_WaitForSingleObject }, { "CloseHandle", 1, STD, h_CloseHandle },
    { "CreateFileMappingA", 6, STD, h_CreateFileMappingA }, { "OpenProcess", 3, STD, h_OpenProcess }, { "CreateFileMappingW", 6, STD, h_CreateFileMappingW },
    { "OpenFileMappingA", 3, STD, h_OpenFileMappingA }, { "MapViewOfFile", 5, STD, h_MapViewOfFile },
    { "UnmapViewOfFile", 1, STD, h_UnmapViewOfFile },
    /* fichiers, dossiers, environnement */
    { "GetFileAttributesW", 1, STD, h_GetFileAttributesW }, { "GetFileAttributesA", 1, STD, h_GetFileAttributesA },
    { "GetFileAttributesExW", 3, STD, h_GetFileAttributesExW }, { "CreateDirectoryW", 2, STD, h_CreateDirectoryW },
    { "RemoveDirectoryW", 1, STD, h_RemoveDirectoryW }, { "DeleteFileW", 1, STD, h_DeleteFileW },
    { "MoveFileW", 2, STD, h_MoveFileW }, { "MoveFileExW", 3, STD, h_MoveFileW }, { "CopyFileW", 3, STD, h_CopyFileW },
    { "CreateFileW", 7, STD, h_CreateFileW }, { "ReadFile", 5, STD, h_ReadFile }, { "WriteFile", 5, STD, h_WriteFile },
    { "GetFileSize", 2, STD, h_GetFileSize }, { "GetFileSizeEx", 2, STD, h_GetFileSizeEx },
    { "GetFileInformationByHandle", 2, STD, h_GetFileInformationByHandle },
    { "SetFilePointer", 4, STD, h_SetFilePointer }, { "SetFilePointerEx", 5, STD, h_SetFilePointerEx },
    { "SetEndOfFile", 1, STD, h_SetEndOfFile }, { "FlushFileBuffers", 1, STD, h_ret1 },
    { "FindFirstFileW", 2, STD, h_FindFirstFileW }, { "FindFirstFileA", 2, STD, h_FindFirstFileA },
    { "FindNextFileW", 2, STD, h_FindNextFileW }, { "FindNextFileA", 2, STD, h_FindNextFileA }, { "FindClose", 1, STD, h_FindClose },
    { "GetTempPathW", 2, STD, h_GetTempPathW }, { "GetCurrentDirectoryW", 2, STD, h_GetCurrentDirectoryW },
    { "SetCurrentDirectoryW", 1, STD, h_SetCurrentDirectoryW },
    { "_wfullpath", 3, CDECL, h__wfullpath }, { "fopen", 2, CDECL, h_fopen }, { "_wfopen_s", 3, CDECL, h__wfopen_s },
    { "fclose", 1, CDECL, h_fclose }, { "fread", 4, CDECL, h_fread }, { "fwrite", 4, CDECL, h_fwrite },
    { "ftell", 1, CDECL, h_ftell }, { "fseek", 3, CDECL, h_fseek }, { "ferror", 1, CDECL, h_ferror },
    { "fflush", 1, CDECL, h_fflush }, { "rewind", 1, CDECL, h_rewind }, { "fputs", 2, CDECL, h_fputs },
    { "_wstat64", 2, CDECL, h__wstat64 }, { "SHGetFolderPathW", 5, STD, h_SHGetFolderPathW },
    { "GetComputerNameW", 2, STD, h_GetComputerNameW }, { "GetUserNameW", 2, STD, h_GetUserNameW },
    { "GetDiskFreeSpaceExW", 4, STD, h_GetDiskFreeSpaceExW }, { "GlobalMemoryStatusEx", 1, STD, h_GlobalMemoryStatusEx },
    { "GetLocalTime", 1, STD, h_GetLocalTime }, { "GetTimeZoneInformation", 1, STD, h_GetTimeZoneInformation },
    { "GetACP", 0, STD, h_GetACP }, { "GetLocaleInfoW", 4, STD, h_GetLocaleInfoW }, { "MulDiv", 3, STD, h_MulDiv },
    { "IsBadReadPtr", 2, STD, h_ret0 }, { "IsBadStringPtrA", 2, STD, h_ret0 },
    { "SetThreadAffinityMask", 2, STD, h_ret1 }, { "SetThreadPriority", 2, STD, h_ret1 },
    { "GetThreadPriority", 1, STD, h_ret0 }, { "SetThreadIdealProcessor", 2, STD, h_ret0 },
    { "CreateThread", 6, STD, h_CreateThread }, { "_beginthreadex", 6, CDECL, h_CreateThread }, { "GetExitCodeThread", 2, STD, h_GetExitCodeThread },
    { "ResumeThread", 1, STD, h_ResumeThread },
    { "CreateMutexA", 3, STD, h_CreateMutexA }, { "ReleaseMutex", 1, STD, h_ReleaseMutex },
    { "CreateSemaphoreA", 4, STD, h_CreateSemaphoreA }, { "ReleaseSemaphore", 3, STD, h_ReleaseMutex },
    { "WaitForSingleObjectEx", 3, STD, h_WaitForSingleObjectEx },
    /* WINMM, ADVAPI32 (registre : toujours absent), USER32, ole32 */
    { "timeGetTime", 0, STD, h_timeGetTime },
    { "RegOpenKeyExW", 5, STD, h_reg_absent }, { "RegOpenKeyExA", 5, STD, h_reg_absent },
    { "RegCreateKeyExW", 9, STD, h_reg_absent }, { "RegQueryValueExW", 6, STD, h_reg_absent },
    { "RegSetValueExW", 6, STD, h_reg_absent }, { "RegCloseKey", 1, STD, h_ret0 }, { "RegFlushKey", 1, STD, h_ret0 },
    { "GetAsyncKeyState", 1, STD, h_GetAsyncKeyState }, { "GetKeyState", 1, STD, h_GetKeyState },
    { "GetSystemMetrics", 1, STD, h_GetSystemMetrics },
    { "MessageBoxW", 4, STD, h_MessageBoxW }, { "MessageBoxA", 4, STD, h_MessageBoxA },
    { "WSOCK32.dll#115", 2, STD, h_WSAStartup }, { "WSOCK32.dll#116", 0, STD, h_ret0 },
    { "WSOCK32.dll#111", 0, STD, h_WSAGetLastError }, { "WSAIoctl", 9, STD, h_WSAIoctl },
    { "__RTDynamicCast", 5, CDECL, h___RTDynamicCast }, { "WSOCK32.dll#23", 3, STD, h_sock_fail },
    { "WSOCK32.dll#3", 1, STD, h_ret0 }, { "WSOCK32.dll#52", 1, STD, h_null_net },
    { "WSOCK32.dll#57", 2, STD, h_gethostname }, { "WSOCK32.dll#8", 1, STD, h_htonl },
    { "WSOCK32.dll#14", 1, STD, h_htonl }, { "WSOCK32.dll#9", 1, STD, h_htons },
    { "WSOCK32.dll#15", 1, STD, h_htons }, { "WSOCK32.dll#11", 1, STD, h_inet_addr },
    { "WSOCK32.dll#4", 3, STD, h_sock_fail }, { "WSOCK32.dll#2", 3, STD, h_sock_fail },
    { "WSOCK32.dll#21", 5, STD, h_sock_fail }, { "WSOCK32.dll#10", 3, STD, h_sock_fail },
    { "WSOCK32.dll#18", 5, STD, h_sock_fail }, { "WSOCK32.dll#19", 4, STD, h_sock_fail },
    { "WSOCK32.dll#16", 4, STD, h_sock_fail }, { "WSOCK32.dll#20", 6, STD, h_sock_fail },
    { "WSOCK32.dll#17", 6, STD, h_sock_fail }, { "WSOCK32.dll#22", 2, STD, h_sock_fail },
    { "WSOCK32.dll#13", 2, STD, h_sock_fail }, { "WSOCK32.dll#1", 3, STD, h_sock_fail },
    { "WSOCK32.dll#151", 2, STD, h_ret0 },
    { "CoCreateInstance", 5, STD, h_CoCreateInstance }, { "CoTaskMemFree", 1, STD, h_ret0 },
    { "CoInitialize", 1, STD, h_ret0 }, { "CoInitializeEx", 2, STD, h_ret0 }, { "CoUninitialize", 0, STD, h_ret0 },
};

/* ========================================================================== */
/* Installation et appel                                                      */
/* ========================================================================== */

typedef struct { uint32_t slot; const char *dll, *nom; const imp_def *def; uint64_t n; } imp_ent;
#define IMP(slot, dll, nom) { slot, dll, nom, NULL, 0 },
static imp_ent g_imp[] = { SC2_IMPORTS(IMP) };
#undef IMP
#define N_IMP ((int)(sizeof g_imp / sizeof g_imp[0]))

void sc2_iat_install(void)
{
    sc2_log("[T] iat: premiers allocs");
    g_fmode = sc2_alloc(4); g_commode = sc2_alloc(4); g_errno = sc2_alloc(4);
    g_iob = sc2_alloc(3 * 32);
    {   /* passe 7g : SC2_ARGS ajouté à la ligne de commande (GetCommandLineW, _wcmdln) */
        const char *sa = getenv("SC2_ARGS");
        char ligne[2048];
        snprintf(ligne, sizeof ligne, "\"SC2.exe\"%s%s", sa && *sa ? " " : "", sa && *sa ? sa : "");
        g_cmdline_w = u2w_new(ligne);
        if (sa && *sa) sc2_log("[ARGS] ligne de commande du jeu : %s", ligne);
    }
    g_wcmdln_var = sc2_alloc(4); wr32(g_wcmdln_var, g_cmdline_w);
    g_huge = sc2_alloc(8); { double h = HUGE_VAL; memcpy(GPTR(g_huge), &h, 8); }
    g_adjust_fdiv = sc2_alloc(4);
    sc2_log("[T] iat: allocs OK, boucle %d imports", N_IMP);
    int impl = 0;
    for (int i = 0; i < N_IMP; i++) {
        if ((i & 127) == 0) sc2_log("[T] iat: i=%d", i);
        imp_ent *e = &g_imp[i];
        for (unsigned k = 0; k < sizeof g_defs / sizeof g_defs[0]; k++)
            if (!strcmp(g_defs[k].nom, e->nom)) { e->def = &g_defs[k]; break; }
        if (!e->def) e->def = sc2_iat_p4_find(e->nom);
        uint32_t v = SC2_IAT_STUB_BASE + 16u * (uint32_t)i;
        if (!strcmp(e->nom, "_wcmdln"))           v = g_wcmdln_var;
        else if (!strcmp(e->nom, "_HUGE"))        v = g_huge;
        else if (!strcmp(e->nom, "_adjust_fdiv")) v = g_adjust_fdiv;
        wr32(e->slot, v);
        if (e->def) impl++;
    }
    (void)g_cmdline;
    sc2_log("[IAT] %d imports installés (%d implémentés, 3 données, le reste arrête le run "
            "s'il est appelé)", N_IMP, impl);
}

static uint32_t stub_par_nom(const char *nom)
{
    for (int i = 0; i < N_IMP; i++)
        if (g_imp[i].def && g_imp[i].def->conv != DATA && !strcmp(g_imp[i].nom, nom))
            return SC2_IAT_STUB_BASE + 16u * (uint32_t)i;
    return 0;
}

/* Fonctions hors IAT servies par GetProcAddress (passe 4) : Battle.net factice.
 * Stub n° N_IMP + k. */
static imp_ent g_extra[] = {
    { 0, "Battle.net.dll", "#1 Init", NULL, 0 }, { 0, "Battle.net.dll", "#2 Shutdown", NULL, 0 },
    { 0, "Battle.net.dll", "#3 Create", NULL, 0 }, { 0, "Battle.net.dll", "#4 Destroy", NULL, 0 },
    /* entrées de vtables ICU factices non importées par SC2.exe (sc2_iat_p4.c) */
    { 0, "icuuc44.dll", "icu:UnicodeString::`scalar deleting destructor'", NULL, 0 },
    { 0, "icuuc44.dll", "icu:Locale::`scalar deleting destructor'", NULL, 0 },
    { 0, "icuuc44.dll", "icu:Locale::getDynamicClassID", NULL, 0 },
    { 0, "icuuc44.dll", "icu:Locale::clone", NULL, 0 },
    /* L5 (24/10) : vtable du NumberFormat factice (sc2_iat_p4.c) ; la dernière n'a pas d'implémentation : son appel
     * arrête le run en nommant l'appelant */
    { 0, "icuin44.dll", "icu:NumberFormat::`scalar deleting destructor'", NULL, 0 },
    { 0, "icuin44.dll", "icu:NumberFormat::getDynamicClassID", NULL, 0 },
    { 0, "icuin44.dll", "icu:NumberFormat::operator==", NULL, 0 }, { 0, "icuin44.dll", "icu:NumberFormat::clone", NULL, 0 },
    { 0, "icuin44.dll", "icu:NumberFormat::setParseIntegerOnly", NULL, 0 },
    { 0, "icuin44.dll", "icu:NumberFormat::setGroupingUsed", NULL, 0 },
    { 0, "icuin44.dll", "icu:NumberFormat::setMaximumIntegerDigits", NULL, 0 },
    { 0, "icuin44.dll", "icu:NumberFormat::setMinimumIntegerDigits", NULL, 0 },
    { 0, "icuin44.dll", "icu:NumberFormat::setMaximumFractionDigits", NULL, 0 },
    { 0, "icuin44.dll", "icu:NumberFormat::setMinimumFractionDigits", NULL, 0 },
    { 0, "icuin44.dll", "icu:NumberFormat::(méthode non fournie)", NULL, 0 },
};
#define N_EXTRA ((int)(sizeof g_extra / sizeof g_extra[0]))
/* VA du stub d'un import implémenté ou d'une entrée hors IAT (passe 4). */
uint32_t sc2_stub_va(const char *nom)
{
    uint32_t v = stub_par_nom(nom);
    if (v) return v;
    for (int k = 0; k < N_EXTRA; k++)
        if (!strcmp(g_extra[k].nom, nom)) {
            if (!g_extra[k].def) g_extra[k].def = sc2_iat_p4_find(nom);
            return SC2_IAT_STUB_BASE + 16u * (uint32_t)(N_IMP + k);
        }
    sc2_stop(15, "sc2_stub_va : « %s » n'a pas de stub", nom);
}
uint32_t sc2_bnet_ordinal(uint32_t ord)
{
    if (ord < 1 || ord > 4) return 0;
    if (!g_extra[ord - 1].def) g_extra[ord - 1].def = sc2_iat_p4_find(g_extra[ord - 1].nom);
    return SC2_IAT_STUB_BASE + 16u * (uint32_t)(N_IMP + (int)ord - 1);
}

/* L3 (24/10, diagnostic de la sauvegarde) : SC2_IAT_PILE=<import>[,<import>…] journalise CHAQUE appel de ces imports
 * (arguments, résultat, GetLastError) puis la pile fantôme, à partir de la frame SC2_IAT_PILE_DES (défaut 0), au plus
 * SC2_IAT_PILE_MAX appels (défaut 64). Sans la variable : un test par appel d'import. */
uint32_t sc2_frames_presentees(void) __attribute__((weak));
/* L3 (24/10) : SC2_GUET=<adr>[>off][,…] (hex, 8 au plus) — guet de mots invités, relu à chaque appel d'import :
 * « 01BF0EA0 » lit [0x01BF0EA0], « 01BF0EA0>240 » lit [[0x01BF0EA0] + 0x240]. Chaque changement est journalisé
 * ([GUET], ancienne → nouvelle valeur, import courant) avec la pile fantôme, au plus SC2_GUET_MAX fois (défaut 40).
 * La granularité est l'appel d'import : la pile est celle du premier import qui suit l'écriture. */
static void guet(const char *nom, uint32_t ret)
{
    static int lu, n_g; static uint32_t base[8], off[8], val[8], vu[8], max = 40, n;
    if (!lu) {
        lu = 1; const char *e = getenv("SC2_GUET"), *m = getenv("SC2_GUET_MAX");
        if (m) max = (uint32_t)strtoul(m, NULL, 10);
        while (e && *e && n_g < 8) {
            char *f; base[n_g] = (uint32_t)strtoul(e, &f, 16); if (f == e) break;
            off[n_g] = 0xFFFFFFFFu; if (*f == '>') { e = f + 1; off[n_g] = (uint32_t)strtoul(e, &f, 16); }
            n_g++; e = *f ? f + 1 : f;
        }
    }
    for (int k = 0; k < n_g && n < max; k++) {
        uint32_t v = rd32(base[k]);
        if (off[k] != 0xFFFFFFFFu) v = (v >= 0x00010000u && v < 0xF0000000u) ? rd32(v + off[k]) : 0xDEADDEADu;
        if (vu[k] && v == val[k]) continue;
        char t[200]; snprintf(t, sizeof t, "[GUET] %08X%s%X : 0x%08X → 0x%08X (au retour de %s ← 0x%08X)", base[k],
                              off[k] != 0xFFFFFFFFu ? ">" : "", off[k] != 0xFFFFFFFFu ? off[k] : 0u, val[k], v, nom, ret);
        val[k] = v; vu[k] = 1; n++;
        sc2_dump_ss(t);
    }
}
static void iat_pile(const char *nom, const uint32_t *a, int argc, uint32_t r, uint32_t ret)
{
    guet(nom, ret);
    static int lu, arme = 1; static const char *liste, *apres; static uint32_t des, max = 64, n;
    if (!lu) { lu = 1; liste = getenv("SC2_IAT_PILE"); apres = getenv("SC2_IAT_PILE_APRES");
               const char *d = getenv("SC2_IAT_PILE_DES"), *m = getenv("SC2_IAT_PILE_MAX");
               if (d) des = (uint32_t)strtoul(d, NULL, 10);
               if (m) max = (uint32_t)strtoul(m, NULL, 10);
               if (apres && *apres) arme = 0; }
    if (!liste || n >= max) return;
    if (!arme) {   /* SC2_IAT_PILE_APRES=<sous-chaîne> : armé au 1er GetFileAttributesW/CreateFileW de ce chemin */
        if (strcmp(nom, "GetFileAttributesW") && strcmp(nom, "CreateFileW")) return;
        char u[1024]; w2u(a[0], u, sizeof u);
        if (!strstr(u, apres)) return;
        arme = 1;
    }
    if (des && (!sc2_frames_presentees || sc2_frames_presentees() < des)) return;
    size_t l = strlen(nom); const char *p = liste;
    for (;;) { p = strstr(p, nom); if (!p) return;
               if ((p == liste || p[-1] == ',') && (p[l] == ',' || !p[l])) break; p += l; }
    n++;
    char args[96] = ""; size_t o = 0;
    for (int k = 0; k < argc && k < 6; k++) o += (size_t)snprintf(args + o, sizeof args - o, "%s0x%X", k ? ", " : "", a[k]);
    char t[160]; snprintf(t, sizeof t, "[IAT-PILE] %s(%s) → 0x%X, GetLastError %u, ← 0x%08X", nom, args, r,
                          (unsigned)GetLastError(), ret);
    sc2_dump_ss(t);
}

int sc2_iat_call(CPU *c, uint32_t va)
{
    uint32_t i = (va - SC2_IAT_STUB_BASE) / 16u;
    if (i >= (uint32_t)(N_IMP + N_EXTRA)) return 0;
    imp_ent *e = i < (uint32_t)N_IMP ? &g_imp[i] : &g_extra[i - (uint32_t)N_IMP];
    e->n++;
    uint32_t ret = rd32(c->esp);
    if (!e->def)
        sc2_stop(6, "[IAT] %s!%s appelé mais NON IMPLÉMENTÉ (appelant 0x%08X, appel n°%llu)",
                 e->dll, e->nom, ret, (unsigned long long)e->n);
    const uint32_t *a = (const uint32_t *)GPTR(c->esp + 4);
    if (e->n <= 3) {
        char args[96] = ""; size_t o = 0;
        for (int k = 0; k < e->def->argc && k < 6; k++)
            o += (size_t)snprintf(args + o, sizeof args - o, "%s0x%X", k ? ", " : "", a[k]);
        sc2_trace("[IAT] %s!%s(%s)  ← 0x%08X%s", e->dll, e->nom, args, ret, e->n == 3 ? "  (suivants comptés)" : "");
    }
    uint32_t av = sc2_appel_ret; sc2_appel_ret = ret;   /* passe 10j : appelant invité de l'import (journal d'arène) */
    static int pile = -1;                                /* L3 : SC2_IAT_PILE, décidé une fois (chemin chaud) */
    if (pile < 0) pile = getenv("SC2_IAT_PILE") != NULL || getenv("SC2_GUET") != NULL;
    uint32_t r;
    if (pile) {
        uint32_t a0[6] = {0};                            /* arguments avant l'appel (l'appelée peut écrire la pile) */
        for (int k = 0; k < e->def->argc && k < 6; k++) a0[k] = a[k];
        r = e->def->fn(c, a);
        iat_pile(e->nom, a0, e->def->argc, r, ret);
    } else r = e->def->fn(c, a);
    sc2_appel_ret = av;
    c->eax = r;
    c->esp += 4u + (e->def->conv == STD ? 4u * (uint32_t)e->def->argc : 0u);
    return 1;
}

void sc2_iat_report(void)
{
    printf("[BILAN] imports appelés :");
    int n = 0;
    for (int i = 0; i < N_IMP; i++)
        if (g_imp[i].n) printf("%s %s×%llu", n++ % 6 ? "," : "\n   ", g_imp[i].nom, (unsigned long long)g_imp[i].n);
    for (int i = 0; i < N_EXTRA; i++)
        if (g_extra[i].n) printf("%s Battle.net%s×%llu", n++ % 6 ? "," : "\n   ", g_extra[i].nom, (unsigned long long)g_extra[i].n);
    printf("\n[BILAN] %d imports distincts appelés sur %d ; initialiseurs C/C++ : %d exécutés, %d abandonnés\n",
           n, N_IMP, g_init_ok, g_init_ko);
    printf("[BILAN] [FS] %llu accès trouvés, %llu absents, %llu énumérations ; installation : %s\n",
           (unsigned long long)g_fs_ok, (unsigned long long)g_fs_absent, (unsigned long long)g_fs_enum,
           g_install[0] ? g_install : "(non consultée)");
    printf("[BILAN] [FS] handles de fichiers : %llu course(s) évitée(s) entre CreateFile simultanés\n",
           (unsigned long long)__atomic_load_n(&g_fh_courses, __ATOMIC_RELAXED));
    sc2_mpq_bilan();
    sc2_com_report();
}
