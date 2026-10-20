/* sc2_iat_p4.c — imports ajoutés en passe 4, pour pousser le boot au-delà de
 * GameMain étape 1 (CARTE-INIT §0) : compléments CRT, USER32/GDI32 (fenêtre
 * factice), et Direct3DCreate9 → IDirect3D9 / IDirect3DDevice9 FACTICES
 * (sc2_d3d9.c). Même contrat que sc2_iat.c : chaque stub est tracé par
 * sc2_iat_call (3 premiers appels), et ce qui n'est pas ici arrête le run en se
 * nommant.
 */
#define _GNU_SOURCE
#include "sc2_iat.h"
/* Passe 5e : FMOD Ex implémenté (runtime/sc2_audio_*.c). Référence FAIBLE : un build qui ne lie pas
 * les sources audio (android/CMakeLists.txt liste ses sources) garde le comportement d'avant. */
extern const imp_def *sc2_audio_find(const char *nom) __attribute__((weak));
/* Passe 5f : entrées injectables (runtime/sc2_input_*.c), même principe : référence FAIBLE. */
extern const imp_def *sc2_input_find(const char *nom) __attribute__((weak));
#include "sc2_d3d9.h"
#include "sc2_image.h"
#include "win32_compat.h"

#include <pthread.h>
#include "sc2_horloge.h"   /* passe 9j : GetMessageTime */
#include <time.h>
#include <unistd.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A(i)  (a[(i)])
#define STR(va) ((const char *)GPTR(va))

static uint32_t p_ret0(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
static uint32_t p_ret1(CPU *c, const uint32_t *a) { (void)c; (void)a; return 1; }
static uint32_t p_invalid(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0xFFFFFFFFu; }
/* GetMessageTime sans la file de sc2_input_sim.c (SC2_INPUT_SIM absent) : l'heure courante, plus jamais 0 (passe 9j).
 * Avec la file, c'est sc2_input_sim.c qui répond : l'heure du dernier message retiré. */
static uint32_t p_GetMessageTime(CPU *c, const uint32_t *a) { (void)c; (void)a; return sc2_tick_ms(); }

/* ---- MSVCR80 ------------------------------------------------------------- */

/* _create_locale(cat, nom) : objet _locale_t opaque (le jeu ne fait que le
 * repasser aux fonctions _l : _strtod_l). */
static uint32_t p__create_locale(CPU *c, const uint32_t *a)
{
    (void)c;
    sc2_log("[P4] _create_locale(%d, \"%s\") → locale C factice", (int)A(0), A(1) ? STR(A(1)) : "(null)");
    return sc2_alloc(0x40);
}
static uint32_t p__strtod_l(CPU *c, const uint32_t *a)
{
    (void)c;
    char *fin;
    double d = strtod(STR(A(0)), &fin);
    if (A(1)) wr32(A(1), GVA_OF(fin));
    fpush(c, d);                                /* double rendu en ST0 (MSVC) */
    return 0;
}

/* ---- MSVCR80 : qsort (comparateur invité, cdecl) -------------------------- */
static uint32_t p_qsort(CPU *c, const uint32_t *a)        /* (base, n, taille, cmp) */
{
    uint32_t base = A(0), n = A(1), sz = A(2), cmp = A(3);
    if (n < 2 || !sz) return 0;
    uint8_t *tmp = malloc(sz);
    for (uint32_t i = 1; i < n; i++) {               /* insertion : stable, peu d'appels invités */
        memcpy(tmp, GPTR(base + i * sz), sz);
        uint32_t t = sc2_alloc(sz); memcpy(GPTR(t), tmp, sz);
        uint32_t j = i;
        while (j > 0) {
            uint32_t args[2] = { base + (j - 1) * sz, t };
            if ((int32_t)sc2_call_guest(c, cmp, 2, args, 1) <= 0) break;
            memcpy(GPTR(base + j * sz), GPTR(base + (j - 1) * sz), sz);
            j--;
        }
        memcpy(GPTR(base + j * sz), tmp, sz);
        sc2_free(t);
    }
    free(tmp);
    return 0;
}

/* ---- MSVCR80 : sscanf / sscanf_s (arguments invités : pointeurs) --------------- */
/* Conversions : d i u x X o f e g c s [..] n %, largeur, h/l/ll/I64 ; « * » =
 * ignorée. _s : %c %s %[ prennent un argument de taille après le pointeur. */
static uint32_t gscanf(const char *in, const char *fmt, uint32_t ap, int sec)
{
    const char *p = in; int n = 0;
    for (const char *f = fmt; *f; f++) {
        if (isspace((unsigned char)*f)) { while (isspace((unsigned char)*p)) p++; continue; }
        if (*f != '%') { if (*p != *f) break; p++; continue; }
        f++;
        if (*f == '%') { if (*p != '%') break; p++; continue; }
        int ignore = 0, larg = 0, l64 = 0, court = 0;
        if (*f == '*') { ignore = 1; f++; }
        while (isdigit((unsigned char)*f)) larg = larg * 10 + (*f++ - '0');
        for (;;) {
            if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { l64 = 1; f += 3; }
            else if (f[0] == 'l' && f[1] == 'l') { l64 = 1; f += 2; }
            else if (*f == 'l' || *f == 'L') f++;
            else if (*f == 'h') { court = 1; f++; }
            else break;
        }
        char cv = *f;
        if (cv != 'c' && cv != '[' && cv != 'n') while (isspace((unsigned char)*p)) p++;
        if (cv == 'n') { if (!ignore) { wr32(rd32(ap), (uint32_t)(p - in)); ap += 4; } continue; }
        if (!*p) break;
        char tok[512]; int k = 0, lim = larg ? larg : 511;
        if (cv == 'c') {
            int m = larg ? larg : 1;
            if ((int)strlen(p) < m) break;
            if (!ignore) { uint32_t d = rd32(ap); ap += 4; if (sec) ap += 4; memcpy(GPTR(d), p, (size_t)m); n++; }
            p += m; continue;
        }
        if (cv == 's' || cv == '[') {
            char set[256] = { 0 }; int neg = 0;
            if (cv == '[') {
                f++; if (*f == '^') { neg = 1; f++; }
                if (*f == ']') { set[(unsigned char)']'] = 1; f++; }
                for (; *f && *f != ']'; f++) {
                    if (f[1] == '-' && f[2] && f[2] != ']') { for (int ch = (unsigned char)f[0]; ch <= (unsigned char)f[2]; ch++) set[ch] = 1; f += 2; }
                    else set[(unsigned char)*f] = 1;
                }
            }
            while (*p && k < lim && (cv == 's' ? !isspace((unsigned char)*p) : (neg ? !set[(unsigned char)*p] : set[(unsigned char)*p])))
                tok[k++] = *p++;
            if (!k) break;
            tok[k] = 0;
            if (!ignore) { uint32_t d = rd32(ap); ap += 4; if (sec) ap += 4; memcpy(GPTR(d), tok, (size_t)k + 1); n++; }
            continue;
        }
        const char *deb = p;
        if (strchr("dDiuoxX", cv)) {
            int base = (cv == 'x' || cv == 'X') ? 16 : cv == 'o' ? 8 : (cv == 'i' ? 0 : 10);
            while (*p && k < lim && (isxdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == 'x' || *p == 'X')) {
                if (base == 10 && !(isdigit((unsigned char)*p) || ((*p == '-' || *p == '+') && k == 0))) break;
                tok[k++] = *p++;
            }
            tok[k] = 0;
            char *e; long long v = strtoll(tok, &e, base);
            if (e == tok) { p = deb; break; }
            p = deb + (e - tok);
            if (!ignore) { uint32_t d = rd32(ap); ap += 4;
                if (l64) { wr32(d, (uint32_t)v); wr32(d + 4, (uint32_t)((uint64_t)v >> 32)); }
                else if (court) wr16(d, (uint16_t)v); else wr32(d, (uint32_t)v);
                n++; }
            continue;
        }
        if (strchr("feEgGa", cv)) {
            while (*p && k < lim && strchr("0123456789+-.eE", *p)) tok[k++] = *p++;
            tok[k] = 0;
            char *e; double v = strtod(tok, &e);
            if (e == tok) { p = deb; break; }
            p = deb + (e - tok);
            if (!ignore) { uint32_t d = rd32(ap); ap += 4;
                /* %f → float*, %lf → double* : l lu plus haut ; on regarde le format */
                if (f[-1] == 'l' || f[-1] == 'L') { uint64_t b; memcpy(&b, &v, 8); wr32(d, (uint32_t)b); wr32(d + 4, (uint32_t)(b >> 32)); }
                else { float x = (float)v; uint32_t b; memcpy(&b, &x, 4); wr32(d, b); }
                n++; }
            continue;
        }
        break;
    }
    return n ? (uint32_t)n : (*in ? 0u : 0xFFFFFFFFu);
}
static uint32_t p_sscanf(CPU *c, const uint32_t *a)   { return gscanf(STR(A(0)), STR(A(1)), c->esp + 12, 0); }
static uint32_t p_sscanf_s(CPU *c, const uint32_t *a) { return gscanf(STR(A(0)), STR(A(1)), c->esp + 12, 1); }

/* ---- ADVAPI32 : sécurité (descripteurs inertes, tout réussit) --------------- */
static uint32_t p_InitializeSecurityDescriptor(CPU *c, const uint32_t *a)
{ (void)c; memset(GPTR(A(0)), 0, 20); wr8(A(0), (uint8_t)A(1)); return 1; }
static uint32_t p_ConvertStringSD(CPU *c, const uint32_t *a)  /* (chaîne, rév, *pSD, *taille) */
{
    (void)c;
    uint32_t sd = sc2_alloc(20); wr8(sd, 1);
    if (A(2)) wr32(A(2), sd);
    if (A(3)) wr32(A(3), 20);
    return 1;
}
static uint32_t p_SetEntriesInAclW(CPU *c, const uint32_t *a) { (void)c; if (A(3)) wr32(A(3), sc2_alloc(8)); return 0; }
static uint32_t p_GetNamedSecurityInfoW(CPU *c, const uint32_t *a)
{
    (void)c;
    for (int k = 3; k < 7; k++) if (A(k)) wr32(A(k), 0);
    if (A(7)) wr32(A(7), sc2_alloc(20));
    return 0;
}
static uint32_t p_reg_absent(CPU *c, const uint32_t *a) { (void)c; (void)a; return 2; }      /* FILE_NOT_FOUND */
static uint32_t p_reg_fin(CPU *c, const uint32_t *a)    { (void)c; (void)a; return 259; }    /* NO_MORE_ITEMS */

/* ---- KERNEL32 ------------------------------------------------------------------ */
static uint32_t p_alloc(CPU *c, const uint32_t *a)   { (void)c; return sc2_alloc(A(1) ? A(1) : 1); }  /* (flags, taille) */
static uint32_t p_arg0(CPU *c, const uint32_t *a)    { (void)c; return A(0); }
static uint32_t p_size(CPU *c, const uint32_t *a)    { (void)c; return sc2_alloc_size(A(0)); }
static uint32_t p_SleepEx(CPU *c, const uint32_t *a) { (void)c; usleep(A(0) > 1000 ? 1000000 : A(0) * 1000u); return 0; }
static uint32_t p_GetProcessId(CPU *c, const uint32_t *a) { (void)c; (void)a; return (uint32_t)getpid(); }
static uint32_t p_DuplicateHandle(CPU *c, const uint32_t *a)   /* (proc, h, proc2, *h2, accès, hérit, options) */
{ (void)c; if (A(3)) wr32(A(3), A(1)); return 1; }
/* FILETIME (100 ns depuis 1601) <-> SYSTEMTIME (8 WORD) */
static uint32_t p_SystemTimeToFileTime(CPU *c, const uint32_t *a)
{
    (void)c;
    struct tm t = { 0 };
    t.tm_year = rd16(A(0)) - 1900; t.tm_mon = rd16(A(0) + 2) - 1; t.tm_mday = rd16(A(0) + 6);
    t.tm_hour = rd16(A(0) + 8); t.tm_min = rd16(A(0) + 10); t.tm_sec = rd16(A(0) + 12);
    uint64_t ft = ((uint64_t)timegm(&t) + 11644473600ull) * 10000000ull + rd16(A(0) + 14) * 10000ull;
    wr32(A(1), (uint32_t)ft); wr32(A(1) + 4, (uint32_t)(ft >> 32));
    return 1;
}
static uint32_t p_FileTimeToSystemTime(CPU *c, const uint32_t *a)
{
    (void)c;
    uint64_t ft = rd32(A(0)) | ((uint64_t)rd32(A(0) + 4) << 32);
    time_t s = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm t; gmtime_r(&s, &t);
    wr16(A(1), (uint16_t)(t.tm_year + 1900)); wr16(A(1) + 2, (uint16_t)(t.tm_mon + 1));
    wr16(A(1) + 4, (uint16_t)t.tm_wday); wr16(A(1) + 6, (uint16_t)t.tm_mday);
    wr16(A(1) + 8, (uint16_t)t.tm_hour); wr16(A(1) + 10, (uint16_t)t.tm_min);
    wr16(A(1) + 12, (uint16_t)t.tm_sec); wr16(A(1) + 14, (uint16_t)((ft / 10000ull) % 1000));
    return 1;
}
static uint32_t p_FileTimeToLocalFileTime(CPU *c, const uint32_t *a)
{ (void)c; wr32(A(1), rd32(A(0))); wr32(A(1) + 4, rd32(A(0) + 4)); return 1; }
static uint32_t p_GetFileTime(CPU *c, const uint32_t *a)
{
    (void)c;
    uint64_t ft = ((uint64_t)time(NULL) + 11644473600ull) * 10000000ull;
    for (int k = 1; k < 4; k++) if (A(k)) { wr32(A(k), (uint32_t)ft); wr32(A(k) + 4, (uint32_t)(ft >> 32)); }
    return 1;
}
static uint32_t p_GetDiskFreeSpaceW(CPU *c, const uint32_t *a)   /* secteurs/cluster, octets/secteur, libres, total */
{
    (void)c;
    if (A(1)) wr32(A(1), 8);
    if (A(2)) wr32(A(2), 512);
    if (A(3)) wr32(A(3), 0x01000000u);
    if (A(4)) wr32(A(4), 0x04000000u);
    return 1;
}
static uint32_t p_GetLogicalDriveStringsW(CPU *c, const uint32_t *a)
{
    (void)c;
    static const char d[] = "C:\\";
    if (A(0) < 5) return 5;
    for (int i = 0; i < 3; i++) wr16(A(1) + 2u * (uint32_t)i, (uint8_t)d[i]);
    wr16(A(1) + 6, 0); wr16(A(1) + 8, 0);
    return 4;
}
static uint32_t p_GetShortPathNameW(CPU *c, const uint32_t *a)   /* (long, court, cap) : identique */
{
    (void)c; uint32_t n = 0; while (rd16(A(0) + 2 * n)) n++;
    if (A(2) <= n) return n + 1;
    for (uint32_t i = 0; i <= n; i++) wr16(A(1) + 2 * i, rd16(A(0) + 2 * i));
    return n;
}
static uint32_t p_GetProcessWorkingSetSize(CPU *c, const uint32_t *a)
{ (void)c; wr32(A(1), 200u << 20); wr32(A(2), 1400u << 20); return 1; }
static uint32_t p_GetLocaleInfoA(CPU *c, const uint32_t *a) { (void)c; if (A(3) && A(2)) wr8(A(2), 0); return 0; }

/* ---- USER32 : fenêtre FACTICE ------------------------------------------------- */
/* Les classes enregistrées gardent leur wndproc ; CreateWindowExW crée un HWND
 * factice et, comme Windows, envoie SYNCHRONEMENT WM_NCCREATE puis WM_CREATE à la
 * wndproc invitée (CREATESTRUCTW construit dans l'arène). Rien n'est affiché. */
#define WM_CREATE   0x0001u
#define WM_NCCREATE 0x0081u
typedef struct { char nom[128]; uint32_t atom, wndproc, style; } wclasse;
typedef struct { uint32_t hwnd, wndproc, x, y, w, h, style, exstyle, longs[8], userdata; char titre[128]; } wfen;
static wclasse g_cls[32]; static int g_ncls;
static wfen g_fen[16]; static int g_nfen;
static pthread_mutex_t g_user_mu = PTHREAD_MUTEX_INITIALIZER;

static void w2u(uint32_t w, char *o, size_t cap)
{
    if (!w) { snprintf(o, cap, "(null)"); return; }
    if (w < 0x10000u) { snprintf(o, cap, "#%u", w); return; }      /* atome */
    w32_wide_to_utf8((LPCWSTR)GPTR(w), o, cap);
}
static wfen *fen_of(uint32_t h)
{
    for (int i = 0; i < g_nfen; i++) if (g_fen[i].hwnd == h) return &g_fen[i];
    return g_nfen ? &g_fen[0] : NULL;              /* HWND inconnu : la fenêtre principale */
}
static uint32_t enregistrer_classe(uint32_t wc, int large)   /* WNDCLASSEX : cbSize, style, lpfnWndProc… lpszClassName(40) */
{
    pthread_mutex_lock(&g_user_mu);
    wclasse *k = &g_cls[g_ncls < 32 ? g_ncls++ : 31];
    k->style = rd32(wc + 4); k->wndproc = rd32(wc + 8); k->atom = 0xC000u + (uint32_t)(k - g_cls);
    if (large) w2u(rd32(wc + 40), k->nom, sizeof k->nom);
    else snprintf(k->nom, sizeof k->nom, "%s", STR(rd32(wc + 40)));
    pthread_mutex_unlock(&g_user_mu);
    sc2_log("[USER32] RegisterClassEx%s(\"%s\", wndproc 0x%08X) → atome 0x%X", large ? "W" : "A", k->nom, k->wndproc, k->atom);
    return k->atom;
}
static uint32_t p_RegisterClassExW(CPU *c, const uint32_t *a) { (void)c; return enregistrer_classe(A(0), 1); }
static uint32_t p_RegisterClassExA(CPU *c, const uint32_t *a) { (void)c; return enregistrer_classe(A(0), 0); }
static uint32_t envoyer(CPU *c, wfen *f, uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (!f->wndproc) return 0;
    uint32_t args[4] = { f->hwnd, msg, wp, lp };
    return sc2_call_guest(c, f->wndproc, 4, args, 0);     /* WNDPROC : stdcall */
}
static uint32_t creer_fenetre(CPU *c, const uint32_t *a, int large)
{
    char cls[128], titre[128];
    if (large) { w2u(A(1), cls, sizeof cls); w2u(A(2), titre, sizeof titre); }
    else { snprintf(cls, sizeof cls, "%s", A(1) < 0x10000u ? "#atome" : STR(A(1)));
           snprintf(titre, sizeof titre, "%s", A(2) ? STR(A(2)) : ""); }
    pthread_mutex_lock(&g_user_mu);
    wclasse *k = NULL;
    for (int i = 0; i < g_ncls; i++)
        if ((A(1) < 0x10000u && g_cls[i].atom == A(1)) || !strcmp(g_cls[i].nom, cls)) k = &g_cls[i];
    wfen *f = &g_fen[g_nfen < 16 ? g_nfen++ : 15];
    memset(f, 0, sizeof *f);
    f->hwnd = 0x00010000u + 0x10u * (uint32_t)(f - g_fen) + 0x2u;
    f->wndproc = k ? k->wndproc : 0;
    f->x = A(4) == 0x80000000u ? 0 : A(4); f->y = A(5) == 0x80000000u ? 0 : A(5);
    f->w = A(6) == 0x80000000u || !A(6) ? 1024 : A(6); f->h = A(7) == 0x80000000u || !A(7) ? 768 : A(7);
    f->style = A(3); f->exstyle = A(0);
    snprintf(f->titre, sizeof f->titre, "%s", titre);
    pthread_mutex_unlock(&g_user_mu);
    sc2_log("[USER32] CreateWindowEx%s(classe \"%s\", titre \"%s\", style 0x%X, %d,%d %u×%u) → HWND factice 0x%X "
            "(wndproc 0x%08X)", large ? "W" : "A", cls, titre, A(3), (int)f->x, (int)f->y, f->w, f->h, f->hwnd, f->wndproc);
    if (!f->wndproc) return f->hwnd;
    uint32_t cs = sc2_alloc(48);                     /* CREATESTRUCT */
    wr32(cs + 0, A(11)); wr32(cs + 4, A(10)); wr32(cs + 8, A(9)); wr32(cs + 12, A(8));
    wr32(cs + 16, f->h); wr32(cs + 20, f->w); wr32(cs + 24, f->y); wr32(cs + 28, f->x);
    wr32(cs + 32, A(3)); wr32(cs + 36, A(2)); wr32(cs + 40, A(1)); wr32(cs + 44, A(0));
    uint32_t r = envoyer(c, f, WM_NCCREATE, 0, cs);
    sc2_log("[USER32]   WM_NCCREATE → wndproc 0x%08X a rendu %u", f->wndproc, r);
    if (!r) return 0;
    r = envoyer(c, f, WM_CREATE, 0, cs);
    sc2_log("[USER32]   WM_CREATE → wndproc 0x%08X a rendu %d", f->wndproc, (int)r);
    return (int32_t)r == -1 ? 0 : f->hwnd;
}
static uint32_t p_CreateWindowExW(CPU *c, const uint32_t *a) { return creer_fenetre(c, a, 1); }
static uint32_t p_CreateWindowExA(CPU *c, const uint32_t *a) { return creer_fenetre(c, a, 0); }
static uint32_t p_DefWindowProc(CPU *c, const uint32_t *a)   /* (hwnd, msg, wp, lp) */
{ (void)c; return A(1) == WM_NCCREATE ? 1 : 0; }
static uint32_t p_GetClientRect(CPU *c, const uint32_t *a)
{ (void)c; wfen *f = fen_of(A(0)); wr32(A(1), 0); wr32(A(1) + 4, 0);
  wr32(A(1) + 8, f ? f->w : 1024); wr32(A(1) + 12, f ? f->h : 768); return 1; }
static uint32_t p_GetWindowRect(CPU *c, const uint32_t *a)
{
    (void)c; wfen *f = fen_of(A(0));
    uint32_t x = f ? f->x : 0, y = f ? f->y : 0, w = f ? f->w : 1024, h = f ? f->h : 768;
    if (A(0) == 0x00010010u || !f) { x = 0; y = 0; w = 1024; h = 768; }    /* bureau */
    wr32(A(1), x); wr32(A(1) + 4, y); wr32(A(1) + 8, x + w); wr32(A(1) + 12, y + h); return 1;
}
static uint32_t p_SetWindowPos(CPU *c, const uint32_t *a)    /* (hwnd, après, x, y, cx, cy, flags) */
{
    (void)c; wfen *f = fen_of(A(0));
    if (f && !(A(6) & 0x2u)) { f->x = A(2); f->y = A(3); }    /* SWP_NOMOVE */
    if (f && !(A(6) & 0x1u)) { f->w = A(4); f->h = A(5); }    /* SWP_NOSIZE */
    return 1;
}
static uint32_t p_MoveWindow(CPU *c, const uint32_t *a)
{ (void)c; wfen *f = fen_of(A(0)); if (f) { f->x = A(1); f->y = A(2); f->w = A(3); f->h = A(4); } return 1; }
static uint32_t p_AdjustWindowRectEx(CPU *c, const uint32_t *a) { (void)c; (void)a; return 1; }   /* sans bordure */
static uint32_t p_handle_fictif(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0x0000C001u; }
static uint32_t p_GetDesktopWindow(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0x00010010u; }
static uint32_t p_GetDC(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0x0000DC01u; }
static uint32_t p_fen_principale(CPU *c, const uint32_t *a) { (void)c; (void)a; return g_nfen ? g_fen[0].hwnd : 0; }
static uint32_t p_SetWindowLong(CPU *c, const uint32_t *a)   /* (hwnd, index, valeur) → ancienne */
{
    (void)c; wfen *f = fen_of(A(0)); if (!f) return 0;
    int32_t i = (int32_t)A(1); uint32_t old;
    if (i == -4) { old = f->wndproc; f->wndproc = A(2); }                 /* GWL_WNDPROC */
    else if (i == -16) { old = f->style; f->style = A(2); }
    else if (i == -20) { old = f->exstyle; f->exstyle = A(2); }
    else if (i == -21) { old = f->userdata; f->userdata = A(2); }          /* GWL_USERDATA */
    else if (i >= 0 && i < 32) { old = f->longs[i / 4]; f->longs[i / 4] = A(2); }
    else old = 0;
    return old;
}
static uint32_t p_GetWindowLong(CPU *c, const uint32_t *a)
{
    (void)c; wfen *f = fen_of(A(0)); if (!f) return 0;
    int32_t i = (int32_t)A(1);
    return i == -4 ? f->wndproc : i == -16 ? f->style : i == -20 ? f->exstyle : i == -21 ? f->userdata
         : (i >= 0 && i < 32) ? f->longs[i / 4] : 0;
}
static uint32_t p_GetWindowPlacement(CPU *c, const uint32_t *a)
{
    (void)c; wfen *f = fen_of(A(0)); uint32_t p = A(1);
    memset(GPTR(p + 4), 0, 40); wr32(p, 44); wr32(p + 8, 1);           /* SW_SHOWNORMAL */
    if (f) { wr32(p + 28, f->x); wr32(p + 32, f->y); wr32(p + 36, f->x + f->w); wr32(p + 40, f->y + f->h); }
    return 1;
}
static uint32_t p_GetMonitorInfoW(CPU *c, const uint32_t *a)    /* rcMonitor, rcWork, flags */
{
    (void)c; uint32_t p = A(1);
    for (int k = 0; k < 2; k++) { wr32(p + 4 + 16u * (uint32_t)k, 0); wr32(p + 8 + 16u * (uint32_t)k, 0);
        wr32(p + 12 + 16u * (uint32_t)k, 1024); wr32(p + 16 + 16u * (uint32_t)k, 768); }
    wr32(p + 36, 1);                                                  /* MONITORINFOF_PRIMARY */
    if (rd32(p) >= 104) { static const char n[] = "\\\\.\\DISPLAY1"; for (uint32_t i = 0; i < sizeof n; i++) wr16(p + 40 + 2 * i, (uint8_t)n[i]); }
    return 1;
}
static uint32_t p_EnumDisplayMonitors(CPU *c, const uint32_t *a)   /* (hdc, clip, proc, data) : un écran */
{
    uint32_t r = sc2_alloc(16); wr32(r + 8, 1024); wr32(r + 12, 768);
    uint32_t args[4] = { 0x0000F001u, 0x0000DC01u, r, A(3) };
    sc2_call_guest(c, A(2), 4, args, 0);
    return 1;
}
static uint32_t p_SystemParametersInfoW(CPU *c, const uint32_t *a)   /* (action, param, pv, winini) */
{
    (void)c;
    if (A(0) == 0x30 && A(2)) { wr32(A(2), 0); wr32(A(2) + 4, 0); wr32(A(2) + 8, 1024); wr32(A(2) + 12, 728); }  /* SPI_GETWORKAREA */
    else if (A(2) && (A(0) == 0x3A || A(0) == 0x34 || A(0) == 0x52)) { if (rd32(A(2)) && rd32(A(2)) < 0x400) memset(GPTR(A(2) + 4), 0, rd32(A(2)) - 4); }  /* STICKYKEYS / TOGGLE / FILTER */
    return 1;
}
static uint32_t p_GetCursorPos(CPU *c, const uint32_t *a) { (void)c; wr32(A(0), 512); wr32(A(0) + 4, 384); return 1; }
static uint32_t p_GetKeyboardLayout(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0x04090409u; }
static uint32_t p_GetKeyboardLayoutList(CPU *c, const uint32_t *a) { (void)c; if (A(0) && A(1)) wr32(A(1), 0x04090409u); return 1; }
static uint32_t p_GetKeyboardLayoutNameA(CPU *c, const uint32_t *a) { (void)c; strcpy((char *)GPTR(A(0)), "00000409"); return 1; }
static uint32_t p_GetKeyboardState(CPU *c, const uint32_t *a) { (void)c; memset(GPTR(A(0)), 0, 256); return 1; }
static uint32_t p_GetWindowInfo(CPU *c, const uint32_t *a)
{
    (void)c; wfen *f = fen_of(A(0)); uint32_t p = A(1);
    memset(GPTR(p + 4), 0, 56);
    uint32_t w = f ? f->w : 1024, h = f ? f->h : 768;
    wr32(p + 12, w); wr32(p + 16, h); wr32(p + 28, w); wr32(p + 32, h);
    wr32(p + 36, f ? f->style : 0); wr32(p + 40, f ? f->exstyle : 0); wr32(p + 44, 1);
    return 1;
}
static uint32_t p_PeekMessage(CPU *c, const uint32_t *a)                                   /* file vide */
{ (void)a; if (rd32(c->esp) == 0x0099A3CCu) sc2_glue_pompe(c); return 0; }   /* passe 10k : pompe du jeu */
static uint32_t p_MsgWait(CPU *c, const uint32_t *a)                                       /* (n, handles, tous, ms, masque) */
{ (void)c; if (A(3) && A(3) != 0xFFFFFFFFu) usleep(A(3) > 100 ? 100000 : A(3) * 1000u); return 0x102u; }   /* WAIT_TIMEOUT */

/* ---- GDI32 ------------------------------------------------------------------------ */
static uint32_t p_GetDeviceCaps(CPU *c, const uint32_t *a)
{
    (void)c;
    switch (A(1)) {
    case 8: return 1024; case 10: return 768; case 12: return 32; case 14: return 1;
    case 88: case 90: return 96; case 116: return 60; case 117: return 1024; case 118: return 768;
    default: return 0;
    }
}
static uint32_t p_CreateDIBSection(CPU *c, const uint32_t *a)   /* (hdc, BITMAPINFO, usage, **bits, sect, off) */
{
    (void)c; uint32_t bi = A(1);
    int32_t w = (int32_t)rd32(bi + 4), h = (int32_t)rd32(bi + 8); uint32_t bpp = rd16(bi + 14);
    uint32_t taille = (uint32_t)((w * (int32_t)bpp + 31) / 32 * 4) * (uint32_t)(h < 0 ? -h : h);
    if (A(3)) wr32(A(3), sc2_alloc(taille ? taille : 4));
    return 0x0000B001u;
}
static uint32_t p_GetTextMetricsA(CPU *c, const uint32_t *a)
{ (void)c; memset(GPTR(A(1)), 0, 53); wr32(A(1), 16); wr32(A(1) + 4, 13); wr32(A(1) + 8, 3); wr32(A(1) + 20, 8); wr32(A(1) + 24, 16); return 1; }
static uint32_t p_GetTextFaceA(CPU *c, const uint32_t *a) { (void)c; if (A(1) > 6) strcpy((char *)GPTR(A(2)), "Arial"); return 6; }

/* ---- PSAPI, WINMM (mixer absent), DSOUND (pas de pilote), COMCTL32 ---------------- */
static uint32_t p_GetProcessMemoryInfo(CPU *c, const uint32_t *a)
{
    (void)c; uint32_t p = A(1), n = A(2) < 64 ? A(2) : 64;
    memset(GPTR(p), 0, n); wr32(p, n);
    if (n >= 16) { wr32(p + 8, 300u << 20); wr32(p + 12, 200u << 20); }
    if (n >= 40) { wr32(p + 32, 250u << 20); wr32(p + 36, 300u << 20); }
    return 1;
}
static uint32_t p_GetModuleInformation(CPU *c, const uint32_t *a)
{ (void)c; wr32(A(2), SC2_IMAGE_BASE); wr32(A(2) + 4, SC2_SIZE_OF_IMAGE); wr32(A(2) + 8, SC2_ENTRY); return 1; }
static uint32_t p_GetModuleBaseNameW(CPU *c, const uint32_t *a)
{
    (void)c; static const char n[] = "SC2.exe";
    if (A(3) < sizeof n) return 0;
    for (uint32_t i = 0; i < sizeof n; i++) wr16(A(2) + 2 * i, (uint8_t)n[i]);
    return sizeof n - 1;
}
static uint32_t p_EnumProcessModules(CPU *c, const uint32_t *a)
{ (void)c; if (A(2) >= 4) wr32(A(1), SC2_IMAGE_BASE); wr32(A(3), 4); return 1; }
static uint32_t p_mm_nodriver(CPU *c, const uint32_t *a) { (void)c; (void)a; return 6; }          /* MMSYSERR_NODRIVER */
static uint32_t p_ds_nodriver(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0x88780078u; } /* DSERR_NODRIVER */
static uint32_t p_e_fail(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0x80004005u; }

/* ---- SHELL32 : aucun processus lancé (journalisé, échec) ------------------------ */
static uint32_t p_ShellExecuteExW(CPU *c, const uint32_t *a)   /* SHELLEXECUTEINFOW* */
{
    char verbe[64], fich[512], params[1024];
    uint32_t i = A(0);
    w2u(rd32(i + 12), verbe, sizeof verbe); w2u(rd32(i + 16), fich, sizeof fich); w2u(rd32(i + 20), params, sizeof params);
    sc2_log("[SHELL] ShellExecuteExW(verbe \"%s\", \"%s\", params \"%s\") depuis 0x%08X → refusé (pas de processus)",
            verbe, fich, params, rd32(c->esp));
    wr32(i + 32, 2);                                   /* hInstApp = SE_ERR_FNF */
    SetLastError(2);
    return 0;
}
static uint32_t p_ShellExecuteW(CPU *c, const uint32_t *a)
{
    char verbe[64], fich[512], params[1024];
    w2u(A(1), verbe, sizeof verbe); w2u(A(2), fich, sizeof fich); w2u(A(3), params, sizeof params);
    sc2_log("[SHELL] ShellExecuteW(\"%s\", \"%s\", \"%s\") depuis 0x%08X → refusé", verbe, fich, params, rd32(c->esp));
    return 2;
}

/* ---- icuuc44 : ICU absente — initialisation réussie, rien d'autre ------------- */
static uint32_t p_u_setMemoryFunctions(CPU *c, const uint32_t *a) { (void)c; if (A(4)) wr32(A(4), 0); return 0; }
static uint32_t p_u_init(CPU *c, const uint32_t *a) { (void)c; if (A(0)) wr32(A(0), 0); return 0; }

/* ---- fmodex : pas d'audio. Mémoire OK ; System_Create échoue (FMOD_ERR_OUTPUT_INIT). */
static uint32_t p_FMOD_Memory_GetStats(CPU *c, const uint32_t *a) { (void)c; if (A(0)) wr32(A(0), 0); if (A(1)) wr32(A(1), 0); return 0; }
static uint32_t p_FMOD_System_Create(CPU *c, const uint32_t *a)
{
    if (A(0)) wr32(A(0), 0);
    sc2_log("[FMOD] FMOD_System_Create depuis 0x%08X → FMOD_ERR_OUTPUT_INIT (pas d'audio)", rd32(c->esp));
    return 52;
}

/* ---- Battle.net FACTICE hors ligne ---------------------------------------------------- */
/* Interface déduite de SC2.exe (0x00BD3F20, 0x00BD4020, 0x00BD3550, 0x00BD3590) :
 *   #1 Init() → bool   #2 Shutdown()   #3 Create(rappels*, données, taille) → poignée (cdecl)
 *   #4 Destroy(poignée) (cdecl). Le faux client « s'initialise », rend une poignée opaque
 *   et ne rappelle JAMAIS le jeu (pas de réseau, pas de connexion) : hors ligne. */
static uint32_t p_bnet_init(CPU *c, const uint32_t *a)
{ (void)a; sc2_log("[BNET] #1 Init() depuis 0x%08X → TRUE (client factice hors ligne)", rd32(c->esp)); return 1; }
static uint32_t p_bnet_create(CPU *c, const uint32_t *a)
{
    uint32_t h = sc2_bnet_client_create();               /* objet C++ : le jeu appelle sa vtable */
    sc2_log("[BNET] #3 Create(rappels 0x%08X, données 0x%08X, %u octets) depuis 0x%08X → poignée factice 0x%08X",
            A(0), A(1), A(2), rd32(c->esp), h);
    /* Passe 7g (diagnostic) : la table de rappels que le client Battle.net appellerait dans le jeu, et
     * les données de création (SC2_BNET_DUMP=1). */
    const char *e = getenv("SC2_BNET_DUMP");
    if (e && *e == '1') {
        char l[512]; size_t o = 0;
        for (uint32_t k = 0; k < 16 && A(0); k++) o += (size_t)snprintf(l + o, sizeof l - o, " %08X", rd32(A(0) + 4 * k));
        sc2_log("[BNET] rappels[0..15] :%s", l);
        uint32_t vt = A(0) ? rd32(A(0)) : 0;
        if (vt >= 0x00800000u && vt < 0x035E4000u) {
            o = 0; for (uint32_t k = 0; k < 24; k++) o += (size_t)snprintf(l + o, sizeof l - o, " %08X", rd32(vt + 4 * k));
            sc2_log("[BNET] rappels->vtable 0x%08X[0..23] :%s", vt, l);
        }
        o = 0; for (uint32_t k = 0; k < A(2) && k < 64 && A(1); k++) o += (size_t)snprintf(l + o, sizeof l - o, " %02X", rd8(A(1) + k));
        sc2_log("[BNET] données :%s", l);
    }
    return h;
}

/* ---- ICU 4.4 FACTICE (icuuc44 / icuin44) -------------------------------------------- */
/* Dispositions mémoire d'ICU 4.4 (le code inline de SC2.exe lit les champs) :
 *   UnicodeString (32 o) : +0 vptr, +4 fLength, +8 fCapacity, +12 fArray (UChar*),
 *                          +16 fFlags (0 = tampon modifiable), +18 fStackBuffer[7]
 *   Locale : +0 vptr, +4 language[12], +16 script[6], +22 country[4], +28 variantBegin,
 *            +32 fullName (char*), +36 fullNameBuffer[157], +196 baseName, +200 baseNameBuffer
 * Vtable UnicodeString (Replaceable, MSVC) : [0] dtor deleting, [1] getDynamicClassID,
 *   [2] extractBetween, [3] handleReplaceBetween, [4] copy, [5] hasMetaData, [6] clone,
 *   [7] getLength, [8] getCharAt, [9] getChar32At — entrées = stubs des imports.
 * Les méthodes C++ importées sont des stubs STD dont `this` est dans ecx (thiscall).
 * BreakIterator / NumberFormat / DateFormat : U_UNSUPPORTED_ERROR et NULL. */
#define US_LEN 4
#define US_CAP 8
#define US_ARR 12
#define US_FLG 16
static uint32_t g_us_vtbl, g_loc_vtbl, g_loc_defaut, g_us_classid, g_loc_classid;
static void icu_vtables(void)
{
    if (g_us_vtbl) return;
    static const char *const us[10] = {
        "icu:UnicodeString::`scalar deleting destructor'", "?getDynamicClassID@UnicodeString@icu_44@@UBEPAXXZ",
        "?extractBetween@UnicodeString@icu_44@@UBEXHHAAV12@@Z", "?handleReplaceBetween@UnicodeString@icu_44@@UAEXHHABV12@@Z",
        "?copy@UnicodeString@icu_44@@UAEXHHH@Z", "?hasMetaData@UnicodeString@icu_44@@UBECXZ",
        "?clone@UnicodeString@icu_44@@UBEPAVReplaceable@2@XZ", "?getLength@UnicodeString@icu_44@@MBEHXZ",
        "?getCharAt@UnicodeString@icu_44@@MBE_WH@Z", "?getChar32At@UnicodeString@icu_44@@MBEHH@Z" };
    uint32_t v = sc2_alloc(4 * 10);
    for (uint32_t i = 0; i < 10; i++) wr32(v + 4 * i, sc2_stub_va(us[i]));
    uint32_t l = sc2_alloc(4 * 3);
    wr32(l, sc2_stub_va("icu:Locale::`scalar deleting destructor'"));
    wr32(l + 4, sc2_stub_va("icu:Locale::getDynamicClassID"));
    wr32(l + 8, sc2_stub_va("icu:Locale::clone"));
    g_us_classid = sc2_alloc(4); g_loc_classid = sc2_alloc(4);
    g_loc_vtbl = l; g_us_vtbl = v;
}
static void us_set(uint32_t o, uint32_t src, uint32_t n)   /* src : UTF-16 invité (ou 0) */
{
    uint32_t b = sc2_alloc(2 * n + 2);
    if (src && n) memcpy(GPTR(b), GPTR(src), 2 * n);
    wr16(b + 2 * n, 0);
    wr32(o + 0, g_us_vtbl); wr32(o + US_LEN, n); wr32(o + US_CAP, n + 1); wr32(o + US_ARR, b); wr16(o + US_FLG, 0);
}
static uint32_t wlen16(uint32_t w) { uint32_t n = 0; if (w) while (rd16(w + 2 * n)) n++; return n; }
static uint32_t icu_us_ctor(CPU *c, const uint32_t *a) { (void)a; icu_vtables(); us_set(c->ecx, 0, 0); return c->ecx; }
static uint32_t icu_us_ctor_w(CPU *c, const uint32_t *a) { icu_vtables(); us_set(c->ecx, A(0), wlen16(A(0))); return c->ecx; }
static uint32_t icu_us_dtor(CPU *c, const uint32_t *a) { (void)a; sc2_free(rd32(c->ecx + US_ARR)); return 0; }
static uint32_t icu_us_ddtor(CPU *c, const uint32_t *a)  /* (flags) */
{ sc2_free(rd32(c->ecx + US_ARR)); if (A(0) & 1) sc2_free(c->ecx); return c->ecx; }
static uint32_t icu_us_assign(CPU *c, const uint32_t *a)   /* operator= / setTo (const UnicodeString&) */
{
    uint32_t s = A(0);
    if (s != c->ecx) us_set(c->ecx, rd32(s + US_ARR), rd32(s + US_LEN));
    return c->ecx;
}
static uint32_t icu_us_terminated(CPU *c, const uint32_t *a)
{ (void)a; uint32_t o = c->ecx; wr16(rd32(o + US_ARR) + 2 * rd32(o + US_LEN), 0); return rd32(o + US_ARR); }
static uint32_t icu_us_casse(CPU *c, int haut)
{
    uint32_t o = c->ecx, b = rd32(o + US_ARR), n = rd32(o + US_LEN);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t ch = rd16(b + 2 * i);
        if (haut ? ((ch >= 'a' && ch <= 'z') || (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7)) :
                   ((ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7)))
            wr16(b + 2 * i, (uint16_t)(haut ? ch - 32 : ch + 32));
    }
    return o;
}
static uint32_t icu_us_lower(CPU *c, const uint32_t *a) { (void)a; return icu_us_casse(c, 0); }
static uint32_t icu_us_upper(CPU *c, const uint32_t *a) { (void)a; return icu_us_casse(c, 1); }
static uint32_t icu_us_extract(CPU *c, const uint32_t *a)   /* (début, fin, UnicodeString& cible) */
{
    uint32_t o = c->ecx, n = rd32(o + US_LEN), d = A(0) < n ? A(0) : n, f = A(1) < n ? A(1) : n;
    if (f < d) f = d;
    us_set(A(2), rd32(o + US_ARR) + 2 * d, f - d);
    return 0;
}
static uint32_t icu_us_replace(CPU *c, const uint32_t *a)   /* (début, fin, const UnicodeString& texte) */
{
    uint32_t o = c->ecx, n = rd32(o + US_LEN), d = A(0) < n ? A(0) : n, f = A(1) < n ? A(1) : n;
    if (f < d) f = d;
    uint32_t tn = rd32(A(2) + US_LEN), ta = rd32(A(2) + US_ARR), ar = rd32(o + US_ARR);
    uint32_t m = n - (f - d) + tn, b = sc2_alloc(2 * m + 2);
    memcpy(GPTR(b), GPTR(ar), 2 * d); memcpy(GPTR(b + 2 * d), GPTR(ta), 2 * tn);
    memcpy(GPTR(b + 2 * (d + tn)), GPTR(ar + 2 * f), 2 * (n - f)); wr16(b + 2 * m, 0);
    wr32(o + US_ARR, b); wr32(o + US_LEN, m); wr32(o + US_CAP, m + 1);
    return 0;
}
static uint32_t icu_us_copy(CPU *c, const uint32_t *a)      /* (début, fin, dest) : insère [début,fin) en dest */
{
    uint32_t o = c->ecx, n = rd32(o + US_LEN), d = A(0) < n ? A(0) : n, f = A(1) < n ? A(1) : n, x = A(2) < n ? A(2) : n;
    if (f < d) f = d;
    uint32_t k = f - d, ar = rd32(o + US_ARR), m = n + k, b = sc2_alloc(2 * m + 2);
    memcpy(GPTR(b), GPTR(ar), 2 * x); memcpy(GPTR(b + 2 * x), GPTR(ar + 2 * d), 2 * k);
    memcpy(GPTR(b + 2 * (x + k)), GPTR(ar + 2 * x), 2 * (n - x)); wr16(b + 2 * m, 0);
    wr32(o + US_ARR, b); wr32(o + US_LEN, m); wr32(o + US_CAP, m + 1);
    return 0;
}
static uint32_t icu_us_clone(CPU *c, const uint32_t *a)
{ (void)a; uint32_t n = sc2_alloc(32); us_set(n, rd32(c->ecx + US_ARR), rd32(c->ecx + US_LEN)); return n; }
static uint32_t icu_us_length(CPU *c, const uint32_t *a) { (void)a; return rd32(c->ecx + US_LEN); }
static uint32_t icu_us_charat(CPU *c, const uint32_t *a)
{ return A(0) < rd32(c->ecx + US_LEN) ? rd16(rd32(c->ecx + US_ARR) + 2 * A(0)) : 0xFFFFu; }
static uint32_t icu_us_classid(CPU *c, const uint32_t *a) { (void)c; (void)a; return g_us_classid; }
static uint32_t icu_new(CPU *c, const uint32_t *a) { (void)c; return sc2_alloc(A(0) ? A(0) : 1); }
static uint32_t icu_delete(CPU *c, const uint32_t *a) { (void)c; sc2_free(A(0)); return 0; }
/* Locale */
static void loc_set(uint32_t o, const char *lang, const char *pays)
{
    icu_vtables();
    wr32(o, g_loc_vtbl);
    memset(GPTR(o + 4), 0, 12); memset(GPTR(o + 16), 0, 6); memset(GPTR(o + 22), 0, 4);
    snprintf((char *)GPTR(o + 4), 12, "%s", lang); snprintf((char *)GPTR(o + 22), 4, "%s", pays);
    char nom[32]; snprintf(nom, sizeof nom, "%s%s%s", lang, *pays ? "_" : "", pays);
    wr32(o + 28, (uint32_t)strlen(nom));
    strcpy((char *)GPTR(o + 36), nom); wr32(o + 32, o + 36);
    strcpy((char *)GPTR(o + 200), nom); wr32(o + 196, o + 200);
}
static uint32_t icu_loc_ctor(CPU *c, const uint32_t *a) { (void)a; loc_set(c->ecx, "en", "US"); return c->ecx; }
static uint32_t icu_loc_ctor4(CPU *c, const uint32_t *a)    /* (langue, pays, variante, mots-clés) */
{ loc_set(c->ecx, A(0) ? STR(A(0)) : "", A(1) ? STR(A(1)) : ""); return c->ecx; }
static uint32_t icu_loc_dtor(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }
static uint32_t icu_loc_ddtor(CPU *c, const uint32_t *a) { if (A(0) & 1) sc2_free(c->ecx); return c->ecx; }
static uint32_t icu_loc_assign(CPU *c, const uint32_t *a)
{ uint32_t s = A(0); loc_set(c->ecx, (const char *)GPTR(s + 4), (const char *)GPTR(s + 22)); return c->ecx; }
static uint32_t icu_loc_classid(CPU *c, const uint32_t *a) { (void)c; (void)a; return g_loc_classid; }
static uint32_t icu_loc_clone(CPU *c, const uint32_t *a)
{ (void)a; uint32_t n = sc2_alloc(360); loc_set(n, (const char *)GPTR(c->ecx + 4), (const char *)GPTR(c->ecx + 22)); return n; }
static uint32_t icu_loc_defaut(CPU *c, const uint32_t *a)
{ (void)c; (void)a; if (!g_loc_defaut) { g_loc_defaut = sc2_alloc(360); loc_set(g_loc_defaut, "en", "US"); } return g_loc_defaut; }
static uint32_t icu_loc_setdefaut(CPU *c, const uint32_t *a) { (void)c; if (A(1)) wr32(A(1), 0); return 0; }
/* Services non fournis : U_UNSUPPORTED_ERROR (16), objet NULL */
static uint32_t icu_non_fourni(CPU *c, const uint32_t *a)    /* (const Locale&, UErrorCode&) */
{
    if (A(1)) wr32(A(1), 16);
    sc2_trace("[ICU] NumberFormat non fourni depuis 0x%08X → U_UNSUPPORTED_ERROR", rd32(c->esp));
    return 0;
}
static uint32_t icu_bi(CPU *c, const uint32_t *a, uint32_t genre, const char *nom)
{
    if (A(1)) wr32(A(1), 0);
    uint32_t o = sc2_icu_breakiterator(genre);
    sc2_trace("[ICU] BreakIterator::create%sInstance depuis 0x%08X → BreakIterator factice 0x%08X", nom, rd32(c->esp), o);
    return o;
}
static uint32_t icu_bi_car(CPU *c, const uint32_t *a) { return icu_bi(c, a, 0, "Character"); }
static uint32_t icu_bi_mot(CPU *c, const uint32_t *a) { return icu_bi(c, a, 1, "Word"); }
static uint32_t icu_bi_ligne(CPU *c, const uint32_t *a) { return icu_bi(c, a, 2, "Line"); }
static uint32_t icu_bi_phrase(CPU *c, const uint32_t *a) { return icu_bi(c, a, 3, "Sentence"); }
static uint32_t icu_nul(CPU *c, const uint32_t *a) { (void)a; sc2_trace("[ICU] DateFormat non fourni depuis 0x%08X → NULL", rd32(c->esp)); return 0; }

/* ---- d3d9.dll ------------------------------------------------------------------- */
static uint32_t p_Direct3DCreate9(CPU *c, const uint32_t *a) { (void)c; return sc2_d3d9_create(A(0)); }
static uint32_t p_d3dx_info(CPU *c, const uint32_t *a) { return sc2_d3dx_image_info(c, a); }
static uint32_t p_d3dx_tex(CPU *c, const uint32_t *a)  { return sc2_d3dx_create_texture(c, a, 3); }
static uint32_t p_d3dx_cube(CPU *c, const uint32_t *a) { return sc2_d3dx_create_texture(c, a, 5); }
static uint32_t p_d3dx_vol(CPU *c, const uint32_t *a)  { return sc2_d3dx_create_texture(c, a, 4); }
static uint32_t p_d3dx_check(CPU *c, const uint32_t *a) { return sc2_d3dx_check_tex(c, a); }
static uint32_t p_d3dx_compile(CPU *c, const uint32_t *a) { return sc2_d3dx_compile(c, a); }
static uint32_t p_d3dx_ctab(CPU *c, const uint32_t *a) { return sc2_d3dx_constant_table(c, a); }
static uint32_t p_d3dx_load_surf_mem(CPU *c, const uint32_t *a) { return sc2_d3dx_load_surface_memory(c, a); }   /* passe 9h */
static uint32_t p_d3dx_load_surf_fichier(CPU *c, const uint32_t *a)   /* (dst, pal, rect, src, n, srect, filtre, ck, *info) */
{
    uint32_t b[3] = { A(3), A(4), A(8) };
    return sc2_d3dx_image_info(c, b);
}
static uint32_t p_d3dx_matrix_inverse(CPU *c, const uint32_t *a)        /* (out, *det, m) → out ou NULL */
{
    (void)c; float m[16], inv[16]; memcpy(m, GPTR(A(2)), 64);
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (A(1)) memcpy(GPTR(A(1)), &det, 4);
    if (det == 0.f) return 0;
    for (int i = 0; i < 16; i++) inv[i] /= det;
    memcpy(GPTR(A(0)), inv, 64);
    return A(0);
}
static uint32_t g_vs_profil, g_ps_profil;
static uint32_t p_d3dx_vs_profile(CPU *c, const uint32_t *a)
{ (void)c; (void)a; if (!g_vs_profil) { g_vs_profil = sc2_alloc(8); strcpy((char *)GPTR(g_vs_profil), "vs_3_0"); } return g_vs_profil; }
static uint32_t p_d3dx_ps_profile(CPU *c, const uint32_t *a)
{ (void)c; (void)a; if (!g_ps_profil) { g_ps_profil = sc2_alloc(8); strcpy((char *)GPTR(g_ps_profil), "ps_3_0"); } return g_ps_profil; }
static uint32_t p_d3dx_echec(CPU *c, const uint32_t *a)
{ sc2_log("[D3DX] fonction non fournie (sauvegarde/compilation) depuis 0x%08X → D3DERR_INVALIDCALL", rd32(c->esp)); (void)a; return 0x8876086Cu; }
/* DirectDraw (sonde de mémoire vidéo de 0x00A00FC0) : absent, le jeu a un repli. */
static uint32_t p_ddraw_absent(CPU *c, const uint32_t *a)
{
    if (A(1)) wr32(A(1), 0);
    sc2_log("[DDRAW] DirectDrawCreateEx depuis 0x%08X → DDERR_NODIRECTDRAWHW (pas de DirectDraw)", rd32(c->esp));
    return 0x88760233u;
}

/* longjmp invité : retour au SC2_SETJMP3 correspondant (runtime/sc2_runtime.c). Le
 * _setjmp3 de l'IAT n'est jamais appelé : lift-boot.py a réécrit ses appels. */
static uint32_t p_longjmp(CPU *c, const uint32_t *a) { sc2_longjmp(c, A(0), A(1)); }
static uint32_t p_setjmp_direct(CPU *c, const uint32_t *a)
{ (void)a; sc2_stop(14, "_setjmp3 appelé par un site NON réécrit (appelant 0x%08X) : relancer lift-boot.py", rd32(c->esp)); }

static const imp_def g_p4[] = {
    /* MSVCR80 */
    { "_create_locale", 2, CDECL, p__create_locale }, { "_strtod_l", 3, CDECL, p__strtod_l },
    { "longjmp", 2, CDECL, p_longjmp }, { "_setjmp3", 2, CDECL, p_setjmp_direct },
    { "qsort", 4, CDECL, p_qsort }, { "sscanf", 2, CDECL, p_sscanf }, { "sscanf_s", 2, CDECL, p_sscanf_s },
    /* ADVAPI32 */
    { "InitializeSecurityDescriptor", 2, STD, p_InitializeSecurityDescriptor },
    { "SetSecurityDescriptorDacl", 4, STD, p_ret1 },
    { "ConvertStringSecurityDescriptorToSecurityDescriptorW", 4, STD, p_ConvertStringSD },
    { "SetEntriesInAclW", 4, STD, p_SetEntriesInAclW }, { "BuildExplicitAccessWithNameW", 5, STD, p_ret0 },
    { "GetNamedSecurityInfoW", 8, STD, p_GetNamedSecurityInfoW }, { "SetNamedSecurityInfoW", 7, STD, p_ret0 },
    { "RegQueryInfoKeyW", 12, STD, p_reg_absent }, { "RegEnumKeyExA", 8, STD, p_reg_fin },
    /* KERNEL32 */
    { "LocalFree", 1, STD, p_ret0 }, { "LocalLock", 1, STD, p_arg0 }, { "LocalUnlock", 1, STD, p_ret1 },
    { "LocalSize", 1, STD, p_size }, { "LocalFlags", 1, STD, p_ret0 },
    { "GlobalAlloc", 2, STD, p_alloc }, { "GlobalLock", 1, STD, p_arg0 }, { "GlobalUnlock", 1, STD, p_ret1 },
    { "GlobalSize", 1, STD, p_size }, { "GlobalFlags", 1, STD, p_ret0 },
    { "SleepEx", 2, STD, p_SleepEx }, { "GetProcessId", 1, STD, p_GetProcessId },
    { "DuplicateHandle", 7, STD, p_DuplicateHandle },
    { "FormatMessageW", 7, STD, p_ret0 }, { "FormatMessageA", 7, STD, p_ret0 },
    { "GetLocaleInfoA", 4, STD, p_GetLocaleInfoA },
    { "SystemTimeToFileTime", 2, STD, p_SystemTimeToFileTime }, { "FileTimeToSystemTime", 2, STD, p_FileTimeToSystemTime },
    { "FileTimeToLocalFileTime", 2, STD, p_FileTimeToLocalFileTime },
    { "GetFileTime", 4, STD, p_GetFileTime }, { "SetFileTime", 4, STD, p_ret1 },
    { "GetDiskFreeSpaceW", 5, STD, p_GetDiskFreeSpaceW }, { "SetFileAttributesW", 2, STD, p_ret1 },
    { "GetLogicalDriveStringsW", 2, STD, p_GetLogicalDriveStringsW }, { "QueryDosDeviceW", 3, STD, p_ret0 },
    { "GetShortPathNameW", 3, STD, p_GetShortPathNameW },
    { "GetProcessWorkingSetSize", 3, STD, p_GetProcessWorkingSetSize },
    /* instantanés / contrôle des threads : refusés (le jeu a un repli) */
    { "CreateToolhelp32Snapshot", 2, STD, p_invalid }, { "Thread32First", 2, STD, p_ret0 },
    { "Thread32Next", 2, STD, p_ret0 }, { "OpenThread", 3, STD, p_ret0 },
    { "SuspendThread", 1, STD, p_invalid }, { "GetThreadContext", 2, STD, p_ret0 },
    { "GetThreadSelectorEntry", 3, STD, p_ret0 },
    /* USER32 : fenêtre factice */
    { "RegisterClassExW", 1, STD, p_RegisterClassExW }, { "RegisterClassExA", 1, STD, p_RegisterClassExA },
    { "UnregisterClassW", 2, STD, p_ret1 }, { "UnregisterClassA", 2, STD, p_ret1 },
    { "CreateWindowExW", 12, STD, p_CreateWindowExW }, { "CreateWindowExA", 12, STD, p_CreateWindowExA },
    { "DefWindowProcW", 4, STD, p_DefWindowProc }, { "DefWindowProcA", 4, STD, p_DefWindowProc },
    { "DestroyWindow", 1, STD, p_ret1 }, { "ShowWindow", 2, STD, p_ret0 }, { "UpdateWindow", 1, STD, p_ret1 },
    { "SetWindowPos", 7, STD, p_SetWindowPos }, { "MoveWindow", 6, STD, p_MoveWindow },
    { "GetClientRect", 2, STD, p_GetClientRect }, { "GetWindowRect", 2, STD, p_GetWindowRect },
    { "AdjustWindowRectEx", 4, STD, p_AdjustWindowRectEx },
    { "SetWindowLongW", 3, STD, p_SetWindowLong }, { "SetWindowLongA", 3, STD, p_SetWindowLong },
    { "GetWindowLongA", 2, STD, p_GetWindowLong },
    { "GetWindowPlacement", 2, STD, p_GetWindowPlacement }, { "SetWindowPlacement", 2, STD, p_ret1 },
    { "GetWindowInfo", 2, STD, p_GetWindowInfo },
    { "LoadCursorW", 2, STD, p_handle_fictif }, { "LoadCursorA", 2, STD, p_handle_fictif },
    { "LoadImageW", 6, STD, p_handle_fictif }, { "SetCursor", 1, STD, p_ret0 },
    { "GetSysColorBrush", 1, STD, p_handle_fictif }, { "GetSysColor", 1, STD, p_ret0 },
    { "GetDC", 1, STD, p_GetDC }, { "ReleaseDC", 2, STD, p_ret1 }, { "GetDesktopWindow", 0, STD, p_GetDesktopWindow },
    { "SetForegroundWindow", 1, STD, p_ret1 }, { "SetActiveWindow", 1, STD, p_fen_principale },
    { "SetFocus", 1, STD, p_fen_principale }, { "GetFocus", 0, STD, p_fen_principale },
    { "GetActiveWindow", 0, STD, p_fen_principale }, { "GetParent", 1, STD, p_ret0 }, { "GetWindow", 2, STD, p_ret0 },
    { "IsWindowVisible", 1, STD, p_ret1 }, { "IsWindowEnabled", 1, STD, p_ret1 }, { "EnableWindow", 2, STD, p_ret0 },
    { "InvalidateRect", 3, STD, p_ret1 }, { "SetWindowTextA", 2, STD, p_ret1 },
    { "EnumDisplayMonitors", 4, STD, p_EnumDisplayMonitors }, { "GetMonitorInfoW", 2, STD, p_GetMonitorInfoW },
    { "SystemParametersInfoW", 4, STD, p_SystemParametersInfoW },
    { "GetCursorPos", 1, STD, p_GetCursorPos }, { "SetCursorPos", 2, STD, p_ret1 }, { "ClipCursor", 1, STD, p_ret1 },
    { "ClientToScreen", 2, STD, p_ret1 }, { "ScreenToClient", 2, STD, p_ret1 },
    { "SetCapture", 1, STD, p_ret0 }, { "ReleaseCapture", 0, STD, p_ret1 },
    { "GetKeyboardLayout", 1, STD, p_GetKeyboardLayout }, { "GetKeyboardLayoutList", 2, STD, p_GetKeyboardLayoutList },
    { "GetKeyboardLayoutNameA", 1, STD, p_GetKeyboardLayoutNameA }, { "ActivateKeyboardLayout", 2, STD, p_GetKeyboardLayout },
    { "GetKeyboardState", 1, STD, p_GetKeyboardState }, { "MapVirtualKeyW", 2, STD, p_ret0 },
    { "MapVirtualKeyExW", 3, STD, p_ret0 }, { "ToUnicodeEx", 7, STD, p_ret0 },
    { "SetWindowsHookExW", 4, STD, p_handle_fictif }, { "UnhookWindowsHookEx", 1, STD, p_ret1 },
    { "CallNextHookEx", 4, STD, p_ret0 },
    { "SetTimer", 4, STD, p_ret1 }, { "KillTimer", 2, STD, p_ret1 }, { "GetMessageTime", 0, STD, p_GetMessageTime },
    { "PeekMessageW", 5, STD, p_PeekMessage }, { "TranslateMessage", 1, STD, p_ret0 },
    { "DispatchMessageW", 1, STD, p_ret0 }, { "MsgWaitForMultipleObjects", 5, STD, p_MsgWait },
    /* GDI32 */
    { "GetDeviceCaps", 2, STD, p_GetDeviceCaps }, { "GetStockObject", 1, STD, p_handle_fictif },
    { "DeleteObject", 1, STD, p_ret1 }, { "DeleteDC", 1, STD, p_ret1 },
    { "CreateCompatibleDC", 1, STD, p_GetDC }, { "SelectObject", 2, STD, p_handle_fictif },
    { "CreateSolidBrush", 1, STD, p_handle_fictif }, { "CreateFontIndirectA", 1, STD, p_handle_fictif },
    { "SetBkMode", 2, STD, p_ret1 }, { "SetTextColor", 2, STD, p_ret0 }, { "SetBkColor", 2, STD, p_ret0 },
    { "GetTextMetricsA", 2, STD, p_GetTextMetricsA }, { "GetTextFaceA", 3, STD, p_GetTextFaceA },
    { "BitBlt", 9, STD, p_ret1 }, { "CreateDIBSection", 6, STD, p_CreateDIBSection },
    /* IMM32 : pas d'IME */
    { "ImmGetContext", 1, STD, p_ret0 }, { "ImmReleaseContext", 2, STD, p_ret1 },
    { "ImmAssociateContext", 2, STD, p_ret0 }, { "ImmAssociateContextEx", 3, STD, p_ret1 },
    { "ImmGetOpenStatus", 1, STD, p_ret0 }, { "ImmSetOpenStatus", 2, STD, p_ret1 },
    { "ImmGetConversionStatus", 3, STD, p_ret0 }, { "ImmSetConversionStatus", 3, STD, p_ret1 },
    { "ImmNotifyIME", 4, STD, p_ret1 }, { "ImmGetProperty", 2, STD, p_ret0 }, { "ImmGetVirtualKey", 1, STD, p_ret0 },
    { "ImmIsUIMessageW", 4, STD, p_ret0 }, { "ImmGetCompositionStringW", 4, STD, p_ret0 },
    { "ImmSetCompositionStringW", 6, STD, p_ret0 }, { "ImmGetCandidateListW", 4, STD, p_ret0 },
    { "ImmSetCompositionWindow", 2, STD, p_ret1 }, { "ImmSetCandidateWindow", 2, STD, p_ret1 },
    { "ImmGetIMEFileNameA", 3, STD, p_ret0 },
    /* COMCTL32, PSAPI */
    { "InitCommonControlsEx", 1, STD, p_ret1 }, { "_TrackMouseEvent", 1, STD, p_ret1 },
    { "GetProcessMemoryInfo", 3, STD, p_GetProcessMemoryInfo }, { "GetModuleInformation", 4, STD, p_GetModuleInformation },
    { "GetModuleBaseNameW", 4, STD, p_GetModuleBaseNameW }, { "EnumProcessModules", 4, STD, p_EnumProcessModules },
    { "GetMappedFileNameW", 4, STD, p_ret0 },
    /* WINMM : pas de mixer ; DSOUND : pas de pilote (énumérations vides) */
    { "mixerGetNumDevs", 0, STD, p_ret0 }, { "mixerOpen", 5, STD, p_mm_nodriver },
    { "mixerGetDevCapsW", 3, STD, p_mm_nodriver }, { "mixerGetLineInfoW", 3, STD, p_mm_nodriver },
    { "mixerGetLineControlsW", 3, STD, p_mm_nodriver }, { "mixerGetControlDetailsW", 3, STD, p_mm_nodriver },
    { "mixerSetControlDetails", 3, STD, p_mm_nodriver }, { "mixerClose", 1, STD, p_ret0 },
    { "DSOUND.dll#3", 2, STD, p_ret0 }, { "DSOUND.dll#8", 2, STD, p_ret0 },      /* DirectSound(Capture)EnumerateW */
    { "DSOUND.dll#9", 2, STD, p_e_fail },                                        /* GetDeviceID */
    { "DSOUND.dll#11", 3, STD, p_ds_nodriver }, { "DSOUND.dll#12", 3, STD, p_ds_nodriver },  /* …Create8 */
    /* SHELL32 */
    { "ShellExecuteExW", 1, STD, p_ShellExecuteExW }, { "ShellExecuteW", 6, STD, p_ShellExecuteW },
    { "FindExecutableW", 3, STD, p_ret0 },
    /* icuuc44 (cdecl, U_EXPORT2) */
    { "u_setMemoryFunctions_44", 5, CDECL, p_u_setMemoryFunctions }, { "u_init_44", 1, CDECL, p_u_init },
    { "u_cleanup_44", 0, CDECL, p_ret0 },
    /* ICU C++ (thiscall : this dans ecx, l'appelé dépile ; statiques : cdecl) */
    { "??0UnicodeString@icu_44@@QAE@XZ", 0, STD, icu_us_ctor }, { "??0UnicodeString@icu_44@@QAE@PB_W@Z", 1, STD, icu_us_ctor_w },
    { "??1UnicodeString@icu_44@@UAE@XZ", 0, STD, icu_us_dtor },
    { "icu:UnicodeString::`scalar deleting destructor'", 1, STD, icu_us_ddtor },
    { "??4UnicodeString@icu_44@@QAEAAV01@ABV01@@Z", 1, STD, icu_us_assign },
    { "?setTo@UnicodeString@icu_44@@QAEAAV12@ABV12@@Z", 1, STD, icu_us_assign },
    { "?getTerminatedBuffer@UnicodeString@icu_44@@QAEPB_WXZ", 0, STD, icu_us_terminated },
    { "?toLower@UnicodeString@icu_44@@QAEAAV12@ABVLocale@2@@Z", 1, STD, icu_us_lower },
    { "?toUpper@UnicodeString@icu_44@@QAEAAV12@ABVLocale@2@@Z", 1, STD, icu_us_upper },
    { "?extractBetween@UnicodeString@icu_44@@UBEXHHAAV12@@Z", 3, STD, icu_us_extract },
    { "?handleReplaceBetween@UnicodeString@icu_44@@UAEXHHABV12@@Z", 3, STD, icu_us_replace },
    { "?copy@UnicodeString@icu_44@@UAEXHHH@Z", 3, STD, icu_us_copy },
    { "?hasMetaData@UnicodeString@icu_44@@UBECXZ", 0, STD, p_ret0 },
    { "?clone@UnicodeString@icu_44@@UBEPAVReplaceable@2@XZ", 0, STD, icu_us_clone },
    { "?getLength@UnicodeString@icu_44@@MBEHXZ", 0, STD, icu_us_length },
    { "?getCharAt@UnicodeString@icu_44@@MBE_WH@Z", 1, STD, icu_us_charat },
    { "?getChar32At@UnicodeString@icu_44@@MBEHH@Z", 1, STD, icu_us_charat },
    { "?getDynamicClassID@UnicodeString@icu_44@@UBEPAXXZ", 0, STD, icu_us_classid },
    { "??2UMemory@icu_44@@SAPAXI@Z", 1, CDECL, icu_new }, { "??3UMemory@icu_44@@SAXPAX@Z", 1, CDECL, icu_delete },
    { "??0Locale@icu_44@@QAE@XZ", 0, STD, icu_loc_ctor }, { "??0Locale@icu_44@@QAE@PBD000@Z", 4, STD, icu_loc_ctor4 },
    { "??1Locale@icu_44@@UAE@XZ", 0, STD, icu_loc_dtor },
    { "icu:Locale::`scalar deleting destructor'", 1, STD, icu_loc_ddtor },
    { "icu:Locale::getDynamicClassID", 0, STD, icu_loc_classid }, { "icu:Locale::clone", 0, STD, icu_loc_clone },
    { "??4Locale@icu_44@@QAEAAV01@ABV01@@Z", 1, STD, icu_loc_assign },
    { "?getDefault@Locale@icu_44@@SAABV12@XZ", 0, CDECL, icu_loc_defaut },
    { "?setDefault@Locale@icu_44@@SAXABV12@AAW4UErrorCode@@@Z", 2, CDECL, icu_loc_setdefaut },
    { "?createCharacterInstance@BreakIterator@icu_44@@SAPAV12@ABVLocale@2@AAW4UErrorCode@@@Z", 2, CDECL, icu_bi_car },
    { "?createLineInstance@BreakIterator@icu_44@@SAPAV12@ABVLocale@2@AAW4UErrorCode@@@Z", 2, CDECL, icu_bi_ligne },
    { "?createSentenceInstance@BreakIterator@icu_44@@SAPAV12@ABVLocale@2@AAW4UErrorCode@@@Z", 2, CDECL, icu_bi_phrase },
    { "?createWordInstance@BreakIterator@icu_44@@SAPAV12@ABVLocale@2@AAW4UErrorCode@@@Z", 2, CDECL, icu_bi_mot },
    { "?createInstance@NumberFormat@icu_44@@SAPAV12@ABVLocale@2@AAW4UErrorCode@@@Z", 2, CDECL, icu_non_fourni },
    { "?createCurrencyInstance@NumberFormat@icu_44@@SAPAV12@ABVLocale@2@AAW4UErrorCode@@@Z", 2, CDECL, icu_non_fourni },
    { "?createPercentInstance@NumberFormat@icu_44@@SAPAV12@ABVLocale@2@AAW4UErrorCode@@@Z", 2, CDECL, icu_non_fourni },
    { "?createDateInstance@DateFormat@icu_44@@SAPAV12@W4EStyle@12@ABVLocale@2@@Z", 2, CDECL, icu_nul },
    { "?createTimeInstance@DateFormat@icu_44@@SAPAV12@W4EStyle@12@ABVLocale@2@@Z", 2, CDECL, icu_nul },
    { "?createDateTimeInstance@DateFormat@icu_44@@SAPAV12@W4EStyle@12@0ABVLocale@2@@Z", 3, CDECL, icu_nul },
    /* fmodex (stdcall) */
    { "_FMOD_Memory_Initialize@24", 6, STD, p_ret0 }, { "_FMOD_Memory_GetStats@12", 3, STD, p_FMOD_Memory_GetStats },
    { "_FMOD_System_Create@4", 1, STD, p_FMOD_System_Create },
    /* Battle.net factice (servi par GetProcAddress, hors IAT) */
    { "#1 Init", 0, CDECL, p_bnet_init }, { "#2 Shutdown", 0, CDECL, p_ret0 },
    { "#3 Create", 3, CDECL, p_bnet_create }, { "#4 Destroy", 1, CDECL, p_ret0 },
    /* d3d9.dll */
    { "Direct3DCreate9", 1, STD, p_Direct3DCreate9 },
    { "DirectDrawCreateEx", 4, STD, p_ddraw_absent },
    /* d3dx9_41 (stdcall) : images DDS */
    { "D3DXGetImageInfoFromFileInMemory", 3, STD, p_d3dx_info },
    { "D3DXCreateTextureFromFileInMemoryEx", 15, STD, p_d3dx_tex },
    { "D3DXCreateCubeTextureFromFileInMemoryEx", 14, STD, p_d3dx_cube },
    { "D3DXCreateVolumeTextureFromFileInMemoryEx", 16, STD, p_d3dx_vol },
    { "D3DXCheckTextureRequirements", 7, STD, p_d3dx_check },
    { "D3DXLoadSurfaceFromFileInMemory", 9, STD, p_d3dx_load_surf_fichier },
    { "D3DXLoadSurfaceFromMemory", 10, STD, p_d3dx_load_surf_mem }, { "D3DXMatrixInverse", 3, STD, p_d3dx_matrix_inverse },
    { "D3DXGetVertexShaderProfile", 1, STD, p_d3dx_vs_profile }, { "D3DXGetPixelShaderProfile", 1, STD, p_d3dx_ps_profile },
    { "D3DXSaveSurfaceToFileInMemory", 5, STD, p_d3dx_echec }, { "D3DXSaveTextureToFileInMemory", 4, STD, p_d3dx_echec },
    { "D3DXSaveTextureToFileW", 4, STD, p_d3dx_echec }, { "D3DXCompileShader", 10, STD, p_d3dx_compile },
    { "D3DXGetShaderConstantTableEx", 3, STD, p_d3dx_ctab },
    { "D3DPERF_BeginEvent", 2, STD, p_ret0 }, { "D3DPERF_EndEvent", 0, STD, p_ret0 },
};

const imp_def *sc2_iat_p4_find(const char *nom)
{
    const imp_def *audio = sc2_audio_find ? sc2_audio_find(nom) : NULL;   /* passe 5e : FMOD Ex */
    if (audio) return audio;
    const imp_def *entree = sc2_input_find ? sc2_input_find(nom) : NULL;   /* passe 5f : entrées */
    if (entree) return entree;
    for (unsigned k = 0; k < sizeof g_p4 / sizeof g_p4[0]; k++)
        if (!strcmp(g_p4[k].nom, nom)) return &g_p4[k];
    return NULL;
}
