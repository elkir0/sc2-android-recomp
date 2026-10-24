/* sc2_mac_evenements.c — rejeu SC2_MAC_EVENEMENTS de l'app Mac, cœur en C pur (V3, lot M4, 22/10).
 * Grammaire, contrat et mode d'emploi : sc2_mac_evenements.h. Aucun état global, aucun appel AppKit, runtime ou getenv. */
#include "sc2_mac_evenements.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ==== outils ======================================================================================================= */
static int min_ascii(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int egal_sans_casse(const char *a, size_t na, const char *b)
{
    size_t nb = strlen(b);
    if (na != nb) return 0;
    for (size_t i = 0; i < na; i++)
        if (min_ascii((unsigned char)a[i]) != min_ascii((unsigned char)b[i])) return 0;
    return 1;
}
/* colonne (en caractères UTF-8, 1 = le premier) de p dans la ligne qui commence en deb */
static int colonne(const char *deb, const char *p)
{
    int c = 1;
    for (; deb < p; deb++)
        if (((unsigned char)*deb & 0xC0) != 0x80) c++;
    return c;
}
static int hexa(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = min_ascii(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static int utf8_ecrire(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (char)(0x80 | (cp >> 6 & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
/* une séquence UTF-8 valide (ni surlongue, ni surrogat, ≤ U+10FFFF) : octets lus, 0 si invalide */
static int utf8_lire(const char *p, const char *fin, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)p;
    if (p >= fin) return 0;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    int n = u[0] >= 0xF0 && u[0] <= 0xF4 ? 4 : u[0] >= 0xE0 ? 3 : u[0] >= 0xC2 && u[0] < 0xE0 ? 2 : 0;
    if (!n || fin - p < n) return 0;
    uint32_t v = u[0] & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) return 0;
        v = v << 6 | (u[i] & 0x3F);
    }
    if ((n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10FFFF)) || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return n;
}
static int32_t arrondi(double v) { return (int32_t)(v >= 0 ? v + 0.5 : v - 0.5); }   /* = banc_arrondi (V1.1) */

/* entier décimal ou 0x… (signe - admis) : 1 lu, 0 syntaxe, -1 hors de [min, max] */
static int lire_entier(const char *p, int n, long long min, long long max, long long *v)
{
    int neg = 0, base = 10;
    if (n > 0 && *p == '-') { neg = 1; p++; n--; }
    if (n > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; n -= 2; }
    if (n <= 0) return 0;
    long long x = 0;
    for (int i = 0; i < n; i++) {
        int d = hexa((unsigned char)p[i]);
        if (d < 0 || d >= base) return 0;
        if (x > 1000000000000LL) return -1;
        x = x * base + d;
    }
    if (neg) x = -x;
    if (x < min || x > max) return -1;
    *v = x;
    return 1;
}
/* réel « [-]chiffres[.chiffres] » (point décimal quelle que soit la locale) : 1 lu, 0 sinon */
static int lire_reel(const char *p, int n, double *v)
{
    int i = 0, neg = 0, chiffres = 0;
    double x = 0, f = 0.1;
    if (i < n && (p[i] == '-' || p[i] == '+')) neg = p[i++] == '-';
    for (; i < n && p[i] >= '0' && p[i] <= '9'; i++, chiffres++) { x = x * 10 + (p[i] - '0'); if (x > 1e12) return 0; }
    if (i < n && p[i] == '.')
        for (i++; i < n && p[i] >= '0' && p[i] <= '9'; i++, chiffres++) { x += (p[i] - '0') * f; f /= 10; }
    if (i != n || !chiffres) return 0;
    *v = neg ? -x : x;
    return 1;
}

/* ==== lexique ====================================================================================================== */
#define JETONS_MAX 24
typedef struct {
    const char *p; int n;             /* texte brut du mot (guillemets compris) */
    int col;                          /* colonne de son premier caractère */
    int chaine;                       /* entre guillemets : v = valeur décodée, nv octets */
    char v[SC2EN_TEXTE_MAX]; int nv;
} jeton;

typedef struct {
    sc2en_script *s;
    const char *src; int ligne;
    const char *deb; int col_fin;     /* début de la ligne ; colonne juste après son dernier caractère */
    char *err; size_t errcap, errlen; int nerr;
    int voie; uint32_t ref_w, ref_h; int ancre_ref;   /* directives en vigueur */
    uint32_t tenus;                   /* modificateurs tenus par MODS (bits de côté, caps, fn) */
    int isole_ligne, plein;
    sc2en_quand quand; int col_quand; /* condition de la ligne */
    const char *cmd; int col_cmd, sous;
} ctx;

static void erreur(ctx *c, int col, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void erreur(ctx *c, int col, const char *fmt, ...)
{
    c->nerr++;
    if (!c->err || c->errlen + 1 >= c->errcap) return;
    char m[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    size_t reste = c->errcap - c->errlen;
    int k = snprintf(c->err + c->errlen, reste, "%s:%d:%d: %s\n", c->src, c->ligne, col, m);
    if (k > 0) c->errlen += (size_t)k < reste ? (size_t)k : reste - 1;
}

/* découpe [l, fin) en mots ; rend leur nombre, ou -1 (erreur déjà nommée) */
static int decouper(ctx *c, const char *l, const char *fin, jeton *t)
{
    int n = 0;
    const char *p = l;
    for (;;) {
        while (p < fin && (*p == ' ' || *p == '\t')) p++;
        if (p >= fin || *p == '#') break;
        if (n == JETONS_MAX) { erreur(c, colonne(c->deb, p), "trop de mots sur la ligne (%d au plus)", JETONS_MAX); return -1; }
        jeton *j = &t[n];
        j->p = p; j->col = colonne(c->deb, p); j->chaine = 0; j->nv = 0; j->v[0] = 0;
        if (*p == '"') {
            j->chaine = 1;
            p++;
            for (;;) {
                if (p >= fin) { erreur(c, j->col, "guillemet non fermé"); return -1; }
                if (*p == '"') { p++; break; }
                char u[4];
                int k;
                if (*p == '\\') {
                    const char *e = p++;
                    uint32_t cp = 0;
                    if (p >= fin) { erreur(c, colonne(c->deb, e), "échappement incomplet en fin de ligne"); return -1; }
                    switch (*p) {
                    case '"': cp = '"'; p++; break;
                    case '\\': cp = '\\'; p++; break;
                    case 'n': cp = '\n'; p++; break;
                    case 'r': cp = '\r'; p++; break;
                    case 't': cp = '\t'; p++; break;
                    case 'e': cp = 0x1B; p++; break;
                    case 'u': case 'U': {
                        int nh = *p == 'u' ? 4 : 8;
                        p++;
                        for (int h = 0; h < nh; h++, p++) {
                            if (p >= fin || hexa((unsigned char)*p) < 0) {
                                erreur(c, colonne(c->deb, e), "échappement \\%c : %d chiffres hexadécimaux attendus",
                                       nh == 4 ? 'u' : 'U', nh);
                                return -1;
                            }
                            cp = cp * 16 + (uint32_t)hexa((unsigned char)*p);
                        }
                        if (!cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                            erreur(c, colonne(c->deb, e), "échappement : point de code U+%04X invalide", (unsigned)cp);
                            return -1;
                        }
                        break;
                    }
                    default:
                        erreur(c, colonne(c->deb, e), "échappement inconnu (\\\" \\\\ \\n \\r \\t \\e \\uXXXX \\UXXXXXXXX)");
                        return -1;
                    }
                    k = utf8_ecrire(cp, u);
                } else {
                    uint32_t cp;
                    k = utf8_lire(p, fin, &cp);
                    if (!k) { erreur(c, colonne(c->deb, p), "UTF-8 invalide dans la chaîne"); return -1; }
                    memcpy(u, p, (size_t)k);
                    p += k;
                }
                if (j->nv + k >= SC2EN_TEXTE_MAX) {
                    erreur(c, j->col, "chaîne trop longue (%d octets au plus)", SC2EN_TEXTE_MAX - 1);
                    return -1;
                }
                memcpy(j->v + j->nv, u, (size_t)k);
                j->nv += k;
            }
            j->v[j->nv] = 0;
            if (p < fin && *p != ' ' && *p != '\t') {
                erreur(c, colonne(c->deb, p), "blanc attendu après la chaîne");
                return -1;
            }
        } else {
            while (p < fin && *p != ' ' && *p != '\t') {
                if (*p == '"') { erreur(c, colonne(c->deb, p), "guillemet au milieu d'un mot"); return -1; }
                p++;
            }
        }
        j->n = (int)(p - j->p);
        n++;
    }
    return n;
}

static int est(const jeton *t, const char *k) { return !t->chaine && egal_sans_casse(t->p, (size_t)t->n, k); }
static int prefixe(const jeton *t, const char *k)
{
    size_t n = strlen(k);
    return !t->chaine && (size_t)t->n >= n && egal_sans_casse(t->p, n, k);
}

/* ==== modificateurs et clavier ===================================================================================== */
static const struct { const char *nom; uint32_t m; } NOMS_MODS[] = {
    { "cmd", SC2EN_MOD_LCMD }, { "lcmd", SC2EN_MOD_LCMD }, { "rcmd", SC2EN_MOD_RCMD },
    { "shift", SC2EN_MOD_LSHIFT }, { "lshift", SC2EN_MOD_LSHIFT }, { "rshift", SC2EN_MOD_RSHIFT },
    { "alt", SC2EN_MOD_LALT }, { "opt", SC2EN_MOD_LALT }, { "lalt", SC2EN_MOD_LALT }, { "lopt", SC2EN_MOD_LALT },
    { "ralt", SC2EN_MOD_RALT }, { "ropt", SC2EN_MOD_RALT },
    { "ctrl", SC2EN_MOD_LCTRL }, { "lctrl", SC2EN_MOD_LCTRL }, { "rctrl", SC2EN_MOD_RCTRL },
    { "caps", SC2EN_MOD_CAPS }, { "fn", SC2EN_MOD_FN },
};
/* touches de modificateur : bit, keyCode (kVK_Command…), nom au journal ; ordre des flagsChanged de MODS */
static const struct { uint32_t m; uint16_t kvk; const char *nom; } TOUCHES_MODS[] = {
    { SC2EN_MOD_LCMD, 0x37, "cmd" },     { SC2EN_MOD_RCMD, 0x36, "rcmd" },
    { SC2EN_MOD_LSHIFT, 0x38, "shift" }, { SC2EN_MOD_RSHIFT, 0x3C, "rshift" },
    { SC2EN_MOD_LALT, 0x3A, "alt" },     { SC2EN_MOD_RALT, 0x3D, "ralt" },
    { SC2EN_MOD_LCTRL, 0x3B, "ctrl" },   { SC2EN_MOD_RCTRL, 0x3E, "rctrl" },
    { SC2EN_MOD_CAPS, 0x39, "caps" },    { SC2EN_MOD_FN, 0x3F, "fn" },
};
#define NB(t) ((int)(sizeof(t) / sizeof((t)[0])))

uint32_t sc2en_drapeaux(uint32_t m)
{
    uint32_t f = m;
    if (m & (SC2EN_MOD_LSHIFT | SC2EN_MOD_RSHIFT)) f |= SC2EN_MOD_SHIFT;
    if (m & (SC2EN_MOD_LCTRL | SC2EN_MOD_RCTRL)) f |= SC2EN_MOD_CTRL;
    if (m & (SC2EN_MOD_LALT | SC2EN_MOD_RALT)) f |= SC2EN_MOD_ALT;
    if (m & (SC2EN_MOD_LCMD | SC2EN_MOD_RCMD)) f |= SC2EN_MOD_CMD;
    return f;
}

/* « a,b,… » de [p, p+n) (dans le mot t) → bits ; 0 + erreur nommée si un nom est inconnu */
static int lire_mods(ctx *c, const jeton *t, const char *p, int n, uint32_t *m)
{
    *m = 0;
    if (n <= 0) { erreur(c, colonne(c->deb, p), "modificateurs attendus (cmd, ctrl, alt, shift, caps, fn ; r… à droite)"); return 0; }
    const char *fin = p + n;
    while (p <= fin) {
        const char *q = p;
        while (q < fin && *q != ',') q++;
        int trouve = 0;
        for (int i = 0; i < NB(NOMS_MODS); i++)
            if (egal_sans_casse(p, (size_t)(q - p), NOMS_MODS[i].nom)) { *m |= NOMS_MODS[i].m; trouve = 1; break; }
        if (!trouve) {
            erreur(c, colonne(c->deb, p), "modificateur « %.*s » inconnu (cmd, ctrl, alt, opt, shift, caps, fn ; r… à droite)",
                   (int)(q - p), p);
            return 0;
        }
        p = q + 1;
    }
    (void)t;
    return 1;
}

/* disposition US (ANSI) : caractère → kVK, Maj */
static const struct { char c; uint8_t kvk, shift; } US[] = {
    { 'a', 0x00, 0 }, { 'b', 0x0B, 0 }, { 'c', 0x08, 0 }, { 'd', 0x02, 0 }, { 'e', 0x0E, 0 }, { 'f', 0x03, 0 },
    { 'g', 0x05, 0 }, { 'h', 0x04, 0 }, { 'i', 0x22, 0 }, { 'j', 0x26, 0 }, { 'k', 0x28, 0 }, { 'l', 0x25, 0 },
    { 'm', 0x2E, 0 }, { 'n', 0x2D, 0 }, { 'o', 0x1F, 0 }, { 'p', 0x23, 0 }, { 'q', 0x0C, 0 }, { 'r', 0x0F, 0 },
    { 's', 0x01, 0 }, { 't', 0x11, 0 }, { 'u', 0x20, 0 }, { 'v', 0x09, 0 }, { 'w', 0x0D, 0 }, { 'x', 0x07, 0 },
    { 'y', 0x10, 0 }, { 'z', 0x06, 0 },
    { '1', 0x12, 0 }, { '2', 0x13, 0 }, { '3', 0x14, 0 }, { '4', 0x15, 0 }, { '5', 0x17, 0 }, { '6', 0x16, 0 },
    { '7', 0x1A, 0 }, { '8', 0x1C, 0 }, { '9', 0x19, 0 }, { '0', 0x1D, 0 },
    { '!', 0x12, 1 }, { '@', 0x13, 1 }, { '#', 0x14, 1 }, { '$', 0x15, 1 }, { '%', 0x17, 1 }, { '^', 0x16, 1 },
    { '&', 0x1A, 1 }, { '*', 0x1C, 1 }, { '(', 0x19, 1 }, { ')', 0x1D, 1 },
    { '-', 0x1B, 0 }, { '_', 0x1B, 1 }, { '=', 0x18, 0 }, { '+', 0x18, 1 }, { '[', 0x21, 0 }, { '{', 0x21, 1 },
    { ']', 0x1E, 0 }, { '}', 0x1E, 1 }, { '\\', 0x2A, 0 }, { '|', 0x2A, 1 }, { ';', 0x29, 0 }, { ':', 0x29, 1 },
    { '\'', 0x27, 0 }, { '"', 0x27, 1 }, { ',', 0x2B, 0 }, { '<', 0x2B, 1 }, { '.', 0x2F, 0 }, { '>', 0x2F, 1 },
    { '/', 0x2C, 0 }, { '?', 0x2C, 1 }, { '`', 0x32, 0 }, { '~', 0x32, 1 },
    { ' ', 0x31, 0 }, { '\t', 0x30, 0 }, { '\n', 0x24, 0 }, { '\r', 0x24, 0 },
};
int sc2en_qwerty(uint32_t cp, uint16_t *kvk, int *shift)
{
    int maj = cp >= 'A' && cp <= 'Z';
    uint32_t q = maj ? cp + 32 : cp;
    for (int i = 0; i < NB(US); i++)
        if ((uint32_t)(unsigned char)US[i].c == q) {
            if (kvk) *kvk = US[i].kvk;
            if (shift) *shift = maj || US[i].shift;
            return 1;
        }
    return 0;
}

/* ==== analyse ====================================================================================================== */
static sc2en_evt *ajouter(ctx *c, sc2en_type type)
{
    sc2en_script *s = c->s;
    if (s->n >= SC2EN_EVT_MAX) {
        if (!c->plein) erreur(c, c->col_cmd, "plus de %d événements", SC2EN_EVT_MAX);
        c->plein = 1;
        return NULL;
    }
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 64;
        sc2en_evt *q = realloc(s->evt, (size_t)nc * sizeof *q);
        if (!q) { erreur(c, c->col_cmd, "mémoire épuisée"); return NULL; }
        s->evt = q;
        s->cap = nc;
    }
    sc2en_evt *e = &s->evt[s->n++];
    memset(e, 0, sizeof *e);
    e->ligne = c->ligne;
    e->colonne = c->col_cmd;
    e->sous = c->sous++;
    e->commande = c->cmd;
    if (e->sous == 0) e->quand = c->quand;
    e->type = type;
    e->voie = c->voie;
    e->drapeaux = sc2en_drapeaux(c->tenus);
    if (e->voie == SC2EN_VOIE_FILE) s->n_file++;
    return e;
}

static int trop(ctx *c, const jeton *t, int n, int attendu)
{
    if (n <= attendu) return 0;
    erreur(c, t[attendu].col, "%s : argument en trop « %.*s »", c->cmd, t[attendu].n, t[attendu].p);
    return 1;
}
static int lire_01(ctx *c, const jeton *t, int n, int *v)
{
    long long x;
    if (n < 2) { erreur(c, c->col_fin, "%s : 0 ou 1 attendu", c->cmd); return 0; }
    if (trop(c, t, n, 2)) return 0;
    if (t[1].chaine || lire_entier(t[1].p, t[1].n, 0, 1, &x) != 1) {
        erreur(c, t[1].col, "%s : 0 ou 1 attendu, lu « %.*s »", c->cmd, t[1].n, t[1].p);
        return 0;
    }
    *v = (int)x;
    return 1;
}

static int lire_coord(ctx *c, const jeton *t, int n, int *i, sc2en_coord *o)
{
    memset(o, 0, sizeof *o);
    if (*i >= n) { erreur(c, c->col_fin, "%s : coordonnée attendue (j X Y, v X Y ou f FX FY)", c->cmd); return 0; }
    const jeton *r = &t[*i];
    char rep = est(r, "j") ? 'j' : est(r, "v") ? 'v' : est(r, "f") ? 'f' : 0;
    if (!rep) { erreur(c, r->col, "%s : repère « %.*s » inconnu (j, v ou f)", c->cmd, r->n, r->p); return 0; }
    if (n - *i < 3) { erreur(c, c->col_fin, "%s : « %c » : deux nombres attendus", c->cmd, rep); return 0; }
    const jeton *a[2] = { &t[*i + 1], &t[*i + 2] };
    double v[2];
    for (int k = 0; k < 2; k++) {
        if (rep == 'j') {
            long long x;
            int r2 = a[k]->chaine ? 0 : lire_entier(a[k]->p, a[k]->n, -65536, 65536, &x);
            if (r2 != 1) {
                erreur(c, a[k]->col, "%s : j : pixel client entier attendu%s, lu « %.*s »", c->cmd,
                       r2 < 0 ? " (−65536 à 65536)" : "", a[k]->n, a[k]->p);
                return 0;
            }
            v[k] = (double)x;
        } else {
            if (a[k]->chaine || !lire_reel(a[k]->p, a[k]->n, &v[k])) {
                erreur(c, a[k]->col, "%s : %c : nombre attendu, lu « %.*s »", c->cmd, rep, a[k]->n, a[k]->p);
                return 0;
            }
            if (rep == 'f' && (v[k] < 0 || v[k] > 1)) {
                erreur(c, a[k]->col, "%s : f : fraction hors de [0, 1] « %.*s »", c->cmd, a[k]->n, a[k]->p);
                return 0;
            }
            if (rep == 'v' && fabs(v[k]) > 100000) {
                erreur(c, a[k]->col, "%s : v : point hors de ±100000 « %.*s »", c->cmd, a[k]->n, a[k]->p);
                return 0;
            }
        }
    }
    o->repere = rep;
    o->x = v[0];
    o->y = v[1];
    *i += 3;
    int ancre = 0;
    if (*i < n && !t[*i].chaine && t[*i].n > 1 && t[*i].p[0] == '@') {
        const jeton *m = &t[*i];
        if (rep != 'j') {
            erreur(c, m->col, "%s : ancre « %.*s » sans objet hors d'une coordonnée j", c->cmd, m->n, m->p);
            return 0;
        }
        const char *a1 = m->p + 1;
        size_t n1 = (size_t)m->n - 1;
        ancre = egal_sans_casse(a1, n1, "ui") ? SC2EN_ANCRE_UI : egal_sans_casse(a1, n1, "scene") ? SC2EN_ANCRE_SCENE
              : egal_sans_casse(a1, n1, "lin") ? SC2EN_ANCRE_LIN : egal_sans_casse(a1, n1, "brut") ? SC2EN_ANCRE_BRUT : 0;
        if (!ancre) {
            erreur(c, m->col, "%s : ancre « %.*s » inconnue (@ui, @scene, @lin, @brut)", c->cmd, m->n, m->p);
            return 0;
        }
        if (ancre != SC2EN_ANCRE_BRUT && !c->ref_w) {
            erreur(c, m->col, "%s : ancre « %.*s » sans REF (pixels bruts)", c->cmd, m->n, m->p);
            return 0;
        }
        (*i)++;
    }
    o->ref_w = c->ref_w;
    o->ref_h = c->ref_h;
    o->ancre = rep != 'j' || !c->ref_w ? SC2EN_ANCRE_BRUT : ancre ? ancre : c->ancre_ref;
    return 1;
}

static void cmd_souris(ctx *c, const jeton *t, int n, sc2en_type type, int clic)
{
    int i = 1, b = 0;
    if (type != SC2EN_DEPLACER) {
        if (n < 2) { erreur(c, c->col_fin, "%s : bouton attendu (g, d ou m)", c->cmd); return; }
        if (est(&t[1], "g")) b = 0;
        else if (est(&t[1], "d")) b = 1;
        else if (est(&t[1], "m")) b = 2;
        else { erreur(c, t[1].col, "%s : bouton « %.*s » inconnu (g, d ou m)", c->cmd, t[1].n, t[1].p); return; }
        i = 2;
    }
    sc2en_coord pos;
    if (!lire_coord(c, t, n, &i, &pos)) return;
    uint32_t mods = 0;
    int vu = 0;
    for (; i < n; i++) {
        if (prefixe(&t[i], "mods=") && !vu) {
            if (!lire_mods(c, &t[i], t[i].p + 5, t[i].n - 5, &mods)) return;
            vu = 1;
        } else {
            erreur(c, t[i].col, "%s : argument « %.*s » inconnu ou en trop", c->cmd, t[i].n, t[i].p);
            return;
        }
    }
    for (int k = 0; k <= clic; k++) {
        sc2en_evt *e = ajouter(c, k ? SC2EN_HAUT : type);
        if (!e) return;
        e->pos = pos;
        e->bouton = b;
        e->drapeaux = sc2en_drapeaux(c->tenus | mods);
        if (pos.repere == 'j') c->s->n_j++;
    }
}

static void cmd_touche(ctx *c, const jeton *t, int n, int bas, int haut)
{
    long long kv;
    if (n < 2) { erreur(c, c->col_fin, "%s : kvk attendu (0 à 0x7F)", c->cmd); return; }
    int r = t[1].chaine ? 0 : lire_entier(t[1].p, t[1].n, 0, 0x7F, &kv);
    if (r != 1) {
        erreur(c, t[1].col, "%s : kvk%s attendu (0 à 0x7F), lu « %.*s »", c->cmd, r < 0 ? " dans la plage" : "", t[1].n, t[1].p);
        return;
    }
    const jeton *car = NULL;
    uint32_t mods = 0;
    int vu_mods = 0, rep = 0;
    for (int i = 2; i < n; i++) {
        if (t[i].chaine && !car) car = &t[i];
        else if (prefixe(&t[i], "mods=") && !vu_mods) {
            if (!lire_mods(c, &t[i], t[i].p + 5, t[i].n - 5, &mods)) return;
            vu_mods = 1;
        } else if (est(&t[i], "rep") && bas && !rep) rep = 1;
        else { erreur(c, t[i].col, "%s : argument « %.*s » inconnu ou en trop", c->cmd, t[i].n, t[i].p); return; }
    }
    for (int k = 0; k < 2; k++) {
        if (!(k ? haut : bas)) continue;
        sc2en_evt *e = ajouter(c, k ? SC2EN_TOUCHE_HAUT : SC2EN_TOUCHE_BAS);
        if (!e) return;
        e->kvk = (uint16_t)kv;
        e->repetition = k ? 0 : rep;
        e->drapeaux = sc2en_drapeaux(c->tenus | mods);
        if (car) memcpy(e->texte, car->v, (size_t)car->nv + 1);
    }
}

static void cmd_mods(ctx *c, const jeton *t, int n)
{
    if (n < 2) { erreur(c, c->col_fin, "MODS : liste attendue (cmd,shift… ou -)"); return; }
    if (trop(c, t, n, 2)) return;
    uint32_t nouveau = 0;
    if (!est(&t[1], "-")) {
        if (t[1].chaine) { erreur(c, t[1].col, "MODS : liste attendue (cmd,shift… ou -), lu une chaîne"); return; }
        if (!lire_mods(c, &t[1], t[1].p, t[1].n, &nouveau)) return;
    }
    uint32_t etat = c->tenus;
    int k = 0;
    for (int passe = 0; passe < 2; passe++)               /* relâchées d'abord, puis enfoncées */
        for (int i = 0; i < NB(TOUCHES_MODS); i++) {
            uint32_t m = TOUCHES_MODS[i].m;
            int change = passe == 0 ? (etat & m) && !(nouveau & m) : !(etat & m) && (nouveau & m);
            if (!change) continue;
            etat ^= m;
            sc2en_evt *e = ajouter(c, SC2EN_MODIFICATEURS);
            if (!e) return;
            e->kvk = TOUCHES_MODS[i].kvk;
            e->drapeaux = sc2en_drapeaux(etat);
            k++;
        }
    if (!k && !ajouter(c, SC2EN_JALON)) return;
    c->tenus = nouveau;
}

static void cmd_texte(ctx *c, const jeton *t, int n)
{
    if (n < 2 || !t[1].chaine) {
        erreur(c, n < 2 ? c->col_fin : t[1].col, "TEXTE : chaîne entre guillemets attendue");
        return;
    }
    long long pas = -1;
    if (n >= 3) {
        if (!prefixe(&t[2], "pas=") || lire_entier(t[2].p + 4, t[2].n - 4, 0, 60000, &pas) != 1) {
            erreur(c, t[2].col, "TEXTE : « pas=MS » attendu (0 à 60000), lu « %.*s »", t[2].n, t[2].p);
            return;
        }
        if (trop(c, t, n, 3)) return;
    }
    if (!t[1].nv) { erreur(c, t[1].col, "TEXTE : chaîne vide"); return; }
    const char *p = t[1].v, *fin = t[1].v + t[1].nv;
    for (int k = 1; p < fin; k++) {                       /* validation complète avant d'ajouter quoi que ce soit */
        uint32_t cp;
        int l = utf8_lire(p, fin, &cp);
        if (!l || !sc2en_qwerty(cp, NULL, NULL)) {
            erreur(c, t[1].col, "TEXTE : caractère n°%d « %.*s » sans touche US (TOUCHE kvk \"car\")", k, l ? l : 1, p);
            return;
        }
        p += l;
    }
    for (p = t[1].v; p < fin;) {
        uint32_t cp;
        uint16_t kvk;
        int maj, l = utf8_lire(p, fin, &cp);
        sc2en_qwerty(cp, &kvk, &maj);
        for (int k = 0; k < 2; k++) {
            int premier = c->sous == 0;
            sc2en_evt *e = ajouter(c, k ? SC2EN_TOUCHE_HAUT : SC2EN_TOUCHE_BAS);
            if (!e) return;
            if (!k && !premier) {
                e->quand.type = pas >= 0 ? SC2EN_MS_APRES : SC2EN_PRESENTS_APRES;
                e->quand.v = pas >= 0 ? (double)pas : 1;
            }
            e->kvk = kvk;
            e->drapeaux = sc2en_drapeaux(c->tenus | (maj ? SC2EN_MOD_LSHIFT : 0));
            if (cp == '\n') strcpy(e->texte, "\r");             /* Retour : characters = « \r » */
            else { memcpy(e->texte, p, (size_t)l); e->texte[l] = 0; }
        }
        p += l;
    }
}

static void commande(ctx *c, const jeton *t, int n)
{
    static const char *const NOMS[] = {
        "REF", "VOIE", "ISOLE", "MOVE", "DOWN", "UP", "DRAG", "CLIC", "MOLETTE", "TOUCHE", "TOUCHE_BAS", "TOUCHE_HAUT",
        "MODS", "TEXTE", "FOCUS", "OCCULTE", "MINIATURISER", "RESTAURER", "PLEIN_ECRAN", "TAILLE", "FERMER", "QUITTER",
        "SIGNAL", "ATTENDRE_LIGNE", "JOURNAL",
    };
    const jeton *k = &t[0];
    c->col_cmd = k->col;
    c->sous = 0;
    c->cmd = NULL;
    for (int i = 0; i < NB(NOMS); i++)
        if (est(k, NOMS[i])) { c->cmd = NOMS[i]; break; }
    if (!c->cmd) { erreur(c, k->col, "commande inconnue « %.*s »", k->n, k->p); return; }
    const char *m = c->cmd;

    /* ---- directives ---- */
    if (!strcmp(m, "REF") || !strcmp(m, "VOIE") || !strcmp(m, "ISOLE")) {
        if (c->quand.type != SC2EN_TOUT_DE_SUITE) {
            erreur(c, c->col_quand, "condition sans objet devant la directive %s", m);
            return;
        }
        if (!strcmp(m, "REF")) {
            if (n < 2) { erreur(c, c->col_fin, "REF : LxH attendu (ou « - »)"); return; }
            if (est(&t[1], "-")) {
                if (trop(c, t, n, 2)) return;
                c->ref_w = c->ref_h = 0;
                c->ancre_ref = SC2EN_ANCRE_UI;
                return;
            }
            const jeton *d = &t[1];
            int x = -1;
            for (int i = 0; i < d->n && !d->chaine; i++)
                if (d->p[i] == 'x' || d->p[i] == 'X') { x = i; break; }
            long long L, H;
            if (x < 0 || lire_entier(d->p, x, 320, 8192, &L) != 1 || lire_entier(d->p + x + 1, d->n - x - 1, 200, 8192, &H) != 1) {
                erreur(c, d->col, "REF : LxH attendu (320×200 à 8192×8192), lu « %.*s »", d->n, d->p);
                return;
            }
            int ancre = SC2EN_ANCRE_UI;
            if (n >= 3) {
                ancre = est(&t[2], "ui") ? SC2EN_ANCRE_UI : est(&t[2], "scene") ? SC2EN_ANCRE_SCENE
                      : est(&t[2], "lin") ? SC2EN_ANCRE_LIN : 0;
                if (!ancre) { erreur(c, t[2].col, "REF : ancre « %.*s » inconnue (ui, scene ou lin)", t[2].n, t[2].p); return; }
                if (trop(c, t, n, 3)) return;
            }
            c->ref_w = (uint32_t)L;
            c->ref_h = (uint32_t)H;
            c->ancre_ref = ancre;
            c->s->ref_w = c->ref_w;
            c->s->ref_h = c->ref_h;
        } else if (!strcmp(m, "VOIE")) {
            if (n < 2) { erreur(c, c->col_fin, "VOIE : directe ou file attendu"); return; }
            if (trop(c, t, n, 2)) return;
            if (est(&t[1], "directe")) c->voie = SC2EN_VOIE_DIRECTE;
            else if (est(&t[1], "file")) c->voie = SC2EN_VOIE_FILE;
            else erreur(c, t[1].col, "VOIE : « %.*s » inconnue (directe ou file)", t[1].n, t[1].p);
        } else {
            int b;
            if (!lire_01(c, t, n, &b)) return;
            if (c->s->isole >= 0) { erreur(c, k->col, "ISOLE déjà donné ligne %d", c->isole_ligne); return; }
            c->s->isole = b;
            c->isole_ligne = c->ligne;
        }
        return;
    }

    /* ---- souris, clavier ---- */
    if (!strcmp(m, "MOVE")) { cmd_souris(c, t, n, SC2EN_DEPLACER, 0); return; }
    if (!strcmp(m, "DOWN")) { cmd_souris(c, t, n, SC2EN_BAS, 0); return; }
    if (!strcmp(m, "UP")) { cmd_souris(c, t, n, SC2EN_HAUT, 0); return; }
    if (!strcmp(m, "DRAG")) { cmd_souris(c, t, n, SC2EN_GLISSER, 0); return; }
    if (!strcmp(m, "CLIC")) { cmd_souris(c, t, n, SC2EN_BAS, 1); return; }
    if (!strcmp(m, "TOUCHE")) { cmd_touche(c, t, n, 1, 1); return; }
    if (!strcmp(m, "TOUCHE_BAS")) { cmd_touche(c, t, n, 1, 0); return; }
    if (!strcmp(m, "TOUCHE_HAUT")) { cmd_touche(c, t, n, 0, 1); return; }
    if (!strcmp(m, "MODS")) { cmd_mods(c, t, n); return; }
    if (!strcmp(m, "TEXTE")) { cmd_texte(c, t, n); return; }
    if (!strcmp(m, "MOLETTE")) {
        double dy, dx = 0;
        int i = 2, precis = 0;
        if (n < 2) { erreur(c, c->col_fin, "MOLETTE : DY attendu"); return; }
        if (t[1].chaine || !lire_reel(t[1].p, t[1].n, &dy) || fabs(dy) > 100000) {
            erreur(c, t[1].col, "MOLETTE : DY attendu (±100000), lu « %.*s »", t[1].n, t[1].p);
            return;
        }
        if (i < n && !est(&t[i], "precis")) {
            if (t[i].chaine || !lire_reel(t[i].p, t[i].n, &dx) || fabs(dx) > 100000) {
                erreur(c, t[i].col, "MOLETTE : DX (±100000) ou « precis » attendu, lu « %.*s »", t[i].n, t[i].p);
                return;
            }
            i++;
        }
        if (i < n && est(&t[i], "precis")) { precis = 1; i++; }
        if (trop(c, t, n, i)) return;
        sc2en_evt *e = ajouter(c, SC2EN_MOLETTE);
        if (e) { e->dy = dy; e->dx = dx; e->precis = precis; }
        return;
    }

    /* ---- fenêtre, cycle de vie ---- */
    if (!strcmp(m, "FOCUS") || !strcmp(m, "OCCULTE")) {
        int b;
        if (!lire_01(c, t, n, &b)) return;
        sc2en_evt *e = ajouter(c, !strcmp(m, "FOCUS") ? SC2EN_FOCUS : SC2EN_OCCULTE);
        if (e) e->valeur = b;
        return;
    }
    if (!strcmp(m, "MINIATURISER") || !strcmp(m, "RESTAURER") || !strcmp(m, "FERMER") || !strcmp(m, "QUITTER")) {
        if (trop(c, t, n, 1)) return;
        ajouter(c, !strcmp(m, "MINIATURISER") ? SC2EN_MINIATURISER : !strcmp(m, "RESTAURER") ? SC2EN_RESTAURER
                 : !strcmp(m, "FERMER") ? SC2EN_FERMER : SC2EN_QUITTER);
        return;
    }
    if (!strcmp(m, "PLEIN_ECRAN")) {
        int b = -1;
        if (n >= 2 && !lire_01(c, t, n, &b)) return;
        sc2en_evt *e = ajouter(c, SC2EN_PLEIN_ECRAN);
        if (e) e->valeur = b;
        return;
    }
    if (!strcmp(m, "TAILLE")) {
        long long w, h;
        const jeton *a = n >= 2 ? &t[1] : NULL;
        int ok;
        if (!a) { erreur(c, c->col_fin, "TAILLE : W H (ou WxH) attendu, en points"); return; }
        if (n == 2) {                                     /* WxH */
            int x = -1;
            for (int i = 0; i < a->n && !a->chaine; i++)
                if (a->p[i] == 'x' || a->p[i] == 'X') { x = i; break; }
            ok = x > 0 && lire_entier(a->p, x, 100, 16384, &w) == 1 && lire_entier(a->p + x + 1, a->n - x - 1, 100, 16384, &h) == 1;
            if (!ok) { erreur(c, a->col, "TAILLE : W H ou WxH attendu (100 à 16384 points), lu « %.*s »", a->n, a->p); return; }
        } else {
            if (trop(c, t, n, 3)) return;
            for (int k2 = 1; k2 <= 2; k2++) {
                long long *d = k2 == 1 ? &w : &h;
                if (t[k2].chaine || lire_entier(t[k2].p, t[k2].n, 100, 16384, d) != 1) {
                    erreur(c, t[k2].col, "TAILLE : %s attendu (100 à 16384 points), lu « %.*s »", k2 == 1 ? "W" : "H",
                           t[k2].n, t[k2].p);
                    return;
                }
            }
        }
        sc2en_evt *e = ajouter(c, SC2EN_TAILLE);
        if (e) { e->l = (double)w; e->h = (double)h; }
        return;
    }
    if (!strcmp(m, "SIGNAL")) {
        int sig = 0;
        if (n < 2) { erreur(c, c->col_fin, "SIGNAL : TERM ou INT attendu"); return; }
        if (trop(c, t, n, 2)) return;
        if (est(&t[1], "TERM") || est(&t[1], "SIGTERM")) sig = SIGTERM;
        else if (est(&t[1], "INT") || est(&t[1], "SIGINT")) sig = SIGINT;
        else { erreur(c, t[1].col, "SIGNAL : « %.*s » inconnu (TERM ou INT)", t[1].n, t[1].p); return; }
        sc2en_evt *e = ajouter(c, SC2EN_SIGNAL);
        if (e) e->valeur = sig;
        return;
    }

    /* ---- rejeu ---- */
    if (!strcmp(m, "ATTENDRE_LIGNE")) {
        long long d = SC2EN_DELAI_DEFAUT_MS;
        if (n < 2 || !t[1].chaine || !t[1].nv) {
            erreur(c, n < 2 ? c->col_fin : t[1].col, "ATTENDRE_LIGNE : motif non vide entre guillemets attendu");
            return;
        }
        if (n >= 3) {
            if (t[2].chaine || lire_entier(t[2].p, t[2].n, 1, 3600000, &d) != 1) {
                erreur(c, t[2].col, "ATTENDRE_LIGNE : délai en ms attendu (1 à 3600000), lu « %.*s »", t[2].n, t[2].p);
                return;
            }
            if (trop(c, t, n, 3)) return;
        }
        sc2en_evt *e = ajouter(c, SC2EN_ATTENDRE_LIGNE);
        if (e) { memcpy(e->texte, t[1].v, (size_t)t[1].nv + 1); e->delai_ms = (uint32_t)d; }
        return;
    }
    if (!strcmp(m, "JOURNAL")) {
        if (n < 2 || !t[1].chaine) { erreur(c, n < 2 ? c->col_fin : t[1].col, "JOURNAL : texte entre guillemets attendu"); return; }
        if (trop(c, t, n, 2)) return;
        sc2en_evt *e = ajouter(c, SC2EN_JOURNAL);
        if (e) memcpy(e->texte, t[1].v, (size_t)t[1].nv + 1);
        return;
    }
}

/* une ligne [l, fin) : condition éventuelle, puis la commande */
static void traiter_ligne(ctx *c, const char *l, const char *fin)
{
    jeton t[JETONS_MAX];
    c->deb = l;
    c->col_fin = colonne(l, fin);
    int n = decouper(c, l, fin, t);
    if (n <= 0) return;
    int i = 0;
    c->quand.type = SC2EN_TOUT_DE_SUITE;
    c->quand.v = 0;
    c->col_quand = 0;
    if (!t[0].chaine && t[0].n >= 2 && t[0].p[0] == '@' && min_ascii((unsigned char)t[0].p[1]) == 'p') {
        const char *a;
        int na, col;
        c->col_quand = t[0].col;
        if (t[0].n > 2) { a = t[0].p + 2; na = t[0].n - 2; col = t[0].col + 2; i = 1; }
        else if (n >= 2 && !t[1].chaine) { a = t[1].p; na = t[1].n; col = t[1].col; i = 2; }
        else { erreur(c, n >= 2 ? t[1].col : c->col_fin, "« @p » : numéro de Present attendu (« @p N » ou « @p +N »)"); return; }
        int rel = na > 0 && a[0] == '+';
        long long v;
        if (lire_entier(a + rel, na - rel, 0, 1000000000, &v) != 1) {
            erreur(c, col, "« @p » : numéro de Present attendu (« @p N » ou « @p +N »), lu « %.*s »", na, a);
            return;
        }
        c->quand.type = rel ? SC2EN_PRESENTS_APRES : SC2EN_AU_PRESENT;
        c->quand.v = (double)v;
    } else if (!t[0].chaine && t[0].p[0] == '+') {
        const char *a = t[0].p + 1;
        int na = t[0].n - 1;
        double mult = 1, ms;
        if (na > 2 && egal_sans_casse(a + na - 2, 2, "ms")) na -= 2;
        else if (na > 1 && min_ascii((unsigned char)a[na - 1]) == 's') { na -= 1; mult = 1000; }
        if (!na || a[0] == '-' || a[0] == '+' || !lire_reel(a, na, &ms) || ms * mult > 86400000.0) {
            erreur(c, t[0].col, "délai « %.*s » invalide (+MS, +MSms ou +Ss, 24 h au plus)", t[0].n, t[0].p);
            return;
        }
        c->quand.type = SC2EN_MS_APRES;
        c->quand.v = ms * mult;
        c->col_quand = t[0].col;
        i = 1;
    }
    if (i >= n) { erreur(c, c->col_fin, "commande attendue après la condition"); return; }
    commande(c, t + i, n - i);
}

int sc2en_analyser(const char *texte, size_t n, int separateur_pv, const char *source, sc2en_script *s,
                   char *err, size_t errcap)
{
    memset(s, 0, sizeof *s);
    s->isole = -1;
    snprintf(s->source, sizeof s->source, "%s", source && *source ? source : "?");
    if (err && errcap) err[0] = 0;
    ctx c;
    memset(&c, 0, sizeof c);
    c.s = s;
    c.src = s->source;
    c.err = errcap ? err : NULL;
    c.errcap = errcap;
    c.ancre_ref = SC2EN_ANCRE_UI;
    if (!texte) n = 0;
    const char *p = texte, *fin = texte + n;
    if (n >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;      /* BOM */
    while (p < fin) {
        const char *q = p;
        int dans = 0, comm = 0, debut_mot = 1;
        while (q < fin && *q != '\n') {                 /* fin de l'élément : « \n », ou « ; » hors guillemets */
            char ch = *q;
            if (dans) {
                if (ch == '\\' && q + 1 < fin && q[1] != '\n') q++;
                else if (ch == '"') dans = 0;
            } else {
                if (separateur_pv && ch == ';') break;
                if (!comm) {
                    if (ch == '"') dans = 1;
                    else if (ch == '#' && debut_mot) comm = 1;
                }
            }
            debut_mot = !dans && (ch == ' ' || ch == '\t');
            q++;
        }
        c.ligne++;
        const char *e = q;
        if (e > p && e[-1] == '\r') e--;
        traiter_ligne(&c, p, e);
        p = q + 1;
    }
    s->lignes = c.ligne;
    return c.nerr;
}

int sc2en_charger(const char *spec, const char *nom_texte, sc2en_script *s, char *err, size_t errcap)
{
    if (!spec) spec = "";
    int fichier = spec[0] == '@' && !(min_ascii((unsigned char)spec[1]) == 'p' && (spec[2] == ' ' || spec[2] == '\t' || spec[2] == '+'));
    if (!fichier) return sc2en_analyser(spec, strlen(spec), 1, nom_texte, s, err, errcap);
    const char *chemin = spec + 1;
    memset(s, 0, sizeof *s);
    s->isole = -1;
    snprintf(s->source, sizeof s->source, "%s", chemin);
    if (err && errcap) err[0] = 0;
    FILE *f = fopen(chemin, "rb");
    if (!f) {
        if (err && errcap) snprintf(err, errcap, "%s:0:0: scénario illisible (%s)\n", chemin, strerror(errno));
        return 1;
    }
    char *buf = malloc(SC2EN_FICHIER_MAX + 1);
    size_t lu = buf ? fread(buf, 1, SC2EN_FICHIER_MAX + 1, f) : 0;
    int erreur_lecture = !buf || ferror(f);
    fclose(f);
    if (erreur_lecture || lu > SC2EN_FICHIER_MAX) {
        if (err && errcap)
            snprintf(err, errcap, "%s:0:0: %s\n", chemin, !buf ? "mémoire épuisée" : lu > SC2EN_FICHIER_MAX
                     ? "scénario trop grand (1 Mio au plus)" : "erreur de lecture");
        free(buf);
        return 1;
    }
    int r = sc2en_analyser(buf, lu, 0, chemin, s, err, errcap);
    free(buf);
    return r;
}

void sc2en_liberer(sc2en_script *s)
{
    if (!s) return;
    free(s->evt);
    s->evt = NULL;
    s->n = s->cap = 0;
}

int sc2en_isole(const sc2en_script *s, const char *env, const char **pourquoi)
{
    const char *w;
    int v;
    if (env && (!strcmp(env, "0") || !strcmp(env, "1"))) { v = env[0] == '1'; w = "variable"; }
    else if (s && s->isole >= 0) { v = s->isole; w = "scénario"; }
    else { v = 1; w = "défaut"; }
    if (pourquoi) *pourquoi = w;
    return v;
}

int sc2en_resumer(const sc2en_script *s, int isole, const char *pourquoi, char *o, size_t cap)
{
    char ref[48];
    if (s->ref_w) snprintf(ref, sizeof ref, "REF %ux%u", s->ref_w, s->ref_h);
    else snprintf(ref, sizeof ref, "sans REF");
    return snprintf(o, cap, "scénario %s : %d lignes, %d événements (j : %d, VOIE file : %d), %s, ISOLE %d (%s)",
                    s->source, s->lignes, s->n, s->n_j, s->n_file, ref, isole, pourquoi ? pourquoi : "?");
}

/* ==== coordonnées ================================================================================================== */
void sc2en_ref_vers_client(uint32_t L, uint32_t H, int ancre, int32_t x, int32_t y, uint32_t w, uint32_t h,
                           int32_t *ox, int32_t *oy)
{
    if (!L || !H || !w || !h || ancre == SC2EN_ANCRE_BRUT) { *ox = x; *oy = y; return; }
    double LL = L, HH = H, l = w, hh = h, X, Y;
    if (ancre == SC2EN_ANCRE_LIN) { X = x * l / LL; Y = y * hh / HH; }
    else if (ancre == SC2EN_ANCRE_SCENE) { double s = l / LL; X = l / 2 + (x - LL / 2) * s; Y = hh / 2 + (y - HH / 2) * s; }
    else {                                                /* ui : boîte 4:3 en haut, centrée (V1.1, M_UI) */
        double hb = HH < LL * 0.75 ? HH : LL * 0.75, hb2 = hh < l * 0.75 ? hh : l * 0.75, s = hb2 / hb;
        X = l / 2 + (x - LL / 2) * s;
        Y = y * s;
    }
    *ox = arrondi(X);
    *oy = arrondi(Y);
}

static int dans_vue(const sc2en_vue *v, int32_t px, int32_t py)
{
    return !v->fw || !v->fh || (px >= 0 && py >= 0 && (uint32_t)px < v->fw && (uint32_t)py < v->fh);
}

int sc2en_vers_vue(const sc2en_coord *c, const sc2en_vue *v, sc2en_point *pt)
{
    memset(pt, 0, sizeof *pt);
    pt->cx = pt->cy = -1;
    if (!c || !v || !c->repere || !(v->echelle > 0)) return -1;
    double e = v->echelle;
    if (c->repere == 'j') {
        if (!v->geo.ok) return -1;
        int32_t cx, cy;
        sc2en_ref_vers_client(c->ref_w, c->ref_h, c->ancre, (int32_t)c->x, (int32_t)c->y, v->geo.jw, v->geo.jh, &cx, &cy);
        sc2_geo_vers_ecran(&v->geo, cx, cy, &pt->px, &pt->py);
        pt->vx = pt->px / e;
        pt->vy = pt->py / e;
        pt->cx = cx;
        pt->cy = cy;
        pt->dedans = cx >= 0 && cy >= 0 && (uint32_t)cx < v->geo.jw && (uint32_t)cy < v->geo.jh && dans_vue(v, pt->px, pt->py);
        return pt->dedans;
    }
    if (c->repere == 'f') {
        if (!v->fw || !v->fh) return -1;
        double px = floor(c->x * v->fw), py = floor(c->y * v->fh);
        if (px > v->fw - 1.0) px = v->fw - 1.0;
        if (py > v->fh - 1.0) py = v->fh - 1.0;
        pt->px = (int32_t)px;
        pt->py = (int32_t)py;
        pt->vx = px / e;
        pt->vy = py / e;
    } else {
        pt->vx = c->x;
        pt->vy = c->y;
        pt->px = (int32_t)floor(c->x * e);
        pt->py = (int32_t)floor(c->y * e);
    }
    if (v->geo.ok) {
        int32_t x, y;
        int d = sc2_geo_vers_jeu(&v->geo, pt->vx * e, pt->vy * e, &x, &y);
        pt->cx = x;
        pt->cy = y;
        pt->dedans = d && dans_vue(v, pt->px, pt->py);
    }
    return pt->dedans;
}

int sc2en_vue_vers_jeu(const sc2en_vue *v, double vx, double vy, int32_t *x, int32_t *y)
{
    if (!v || !v->geo.ok || !(v->echelle > 0)) return -1;
    return sc2_geo_vers_jeu(&v->geo, vx * v->echelle, vy * v->echelle, x, y);
}

/* ==== ordonnancement =============================================================================================== */
void sc2en_demarrer(sc2en_rejeu *r, const sc2en_script *s, sc2en_instant t0,
                    int (*ligne_vue)(void *ctx, const char *motif), void *ctx)
{
    memset(r, 0, sizeof *r);
    r->s = s;
    r->t_prec = t0.t_ms;
    r->p_prec = t0.presents;
    r->ligne_vue = ligne_vue;
    r->ctx = ctx;
}

int sc2en_avancer(sc2en_rejeu *r, sc2en_instant t, const sc2en_evt **e, double *echeance_ms)
{
    const sc2en_evt *rien = NULL;
    double ech_rien = -1;
    if (!e) e = &rien;
    if (!echeance_ms) echeance_ms = &ech_rien;
    *e = NULL;
    *echeance_ms = -1;
    if (!r || !r->s) return SC2EN_FINI;
    if (r->final) { if (r->final == SC2EN_DELAI) *e = &r->s->evt[r->i]; return r->final; }
    if (r->attente) {
        const sc2en_evt *a = &r->s->evt[r->i];
        if (r->ligne_vue && r->ligne_vue(r->ctx, a->texte) > 0) {
            r->attente = 0;
            r->t_prec = t.t_ms;
            r->p_prec = t.presents;
            r->i++;
        } else if (t.t_ms - r->attente_debut >= a->delai_ms) {
            r->attente = 0;
            r->final = SC2EN_DELAI;
            *e = a;
            return SC2EN_DELAI;
        } else {
            double fin = r->attente_debut + a->delai_ms, scrut = t.t_ms + SC2EN_SCRUTATION_MS;
            *echeance_ms = fin < scrut ? fin : scrut;
            return SC2EN_RIEN;
        }
    }
    if (r->i >= r->s->n) { r->final = SC2EN_FINI; return SC2EN_FINI; }
    const sc2en_evt *x = &r->s->evt[r->i];
    int du;
    switch (x->quand.type) {
    case SC2EN_AU_PRESENT: du = (double)t.presents >= x->quand.v; break;
    case SC2EN_PRESENTS_APRES: du = (double)(t.presents - r->p_prec) >= x->quand.v; break;
    case SC2EN_MS_APRES:
        du = t.t_ms >= r->t_prec + x->quand.v;
        if (!du) *echeance_ms = r->t_prec + x->quand.v;
        break;
    default: du = 1; break;
    }
    if (du && x->pos.repere == 'j' && !t.geo_ok) du = 0;  /* j attend la 1re géométrie : revenir au prochain Present */
    if (!du) return SC2EN_RIEN;
    r->t_prec = t.t_ms;
    r->p_prec = t.presents;
    if (x->type == SC2EN_ATTENDRE_LIGNE) { r->attente = 1; r->attente_debut = t.t_ms; }
    else r->i++;
    *e = x;
    return SC2EN_EXECUTER;
}

double sc2en_horodatage(int rang) { return SC2EN_MARQUEUR + (rang > 0 ? rang : 0); }
int sc2en_synthetique(double horodatage) { return horodatage >= SC2EN_MARQUEUR; }

/* ==== journal ====================================================================================================== */
static size_t ajout(char *o, size_t cap, size_t k, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t ajout(char *o, size_t cap, size_t k, const char *fmt, ...)
{
    if (k + 1 >= cap) return k;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(o + k, cap - k, fmt, ap);
    va_end(ap);
    if (r < 0) return k;
    return k + (size_t)r < cap ? k + (size_t)r : cap - 1;
}
/* texte UTF-8 entre guillemets, contrôles et zone privée (touches de fonction U+F7xx) échappés */
static size_t ajout_chaine(char *o, size_t cap, size_t k, const char *s)
{
    const char *fin = s + strlen(s);
    k = ajout(o, cap, k, "\"");
    while (s < fin) {
        uint32_t cp;
        int l = utf8_lire(s, fin, &cp);
        if (!l) { k = ajout(o, cap, k, "\\x%02X", (unsigned char)*s); s++; continue; }
        if (cp == '"' || cp == '\\') k = ajout(o, cap, k, "\\%c", (char)cp);
        else if (cp == '\n') k = ajout(o, cap, k, "\\n");
        else if (cp == '\r') k = ajout(o, cap, k, "\\r");
        else if (cp == '\t') k = ajout(o, cap, k, "\\t");
        else if (cp == 0x1B) k = ajout(o, cap, k, "\\e");
        else if (cp < 0x20 || cp == 0x7F || (cp >= 0xE000 && cp <= 0xF8FF)) k = ajout(o, cap, k, "\\u%04X", (unsigned)cp);
        else k = ajout(o, cap, k, "%.*s", l, s);
        s += l;
    }
    return ajout(o, cap, k, "\"");
}
static size_t ajout_mods(char *o, size_t cap, size_t k, uint32_t d)
{
    int premier = 1;
    for (int i = 0; i < NB(TOUCHES_MODS); i++)
        if (d & TOUCHES_MODS[i].m) { k = ajout(o, cap, k, "%s%s", premier ? " mods=" : ",", TOUCHES_MODS[i].nom); premier = 0; }
    return k;
}
static size_t ajout_coord(char *o, size_t cap, size_t k, const sc2en_coord *c)
{
    static const char *const ANCRES[] = { "", "ui", "scene", "lin", "brut" };
    if (c->repere == 'j') {
        k = ajout(o, cap, k, "j %d %d", (int)c->x, (int)c->y);
        if (c->ref_w) k = ajout(o, cap, k, " @%s (REF %ux%u)", ANCRES[c->ancre >= 1 && c->ancre <= 4 ? c->ancre : 0], c->ref_w, c->ref_h);
    } else if (c->repere == 'v') k = ajout(o, cap, k, "v %.2f %.2f", c->x, c->y);
    else if (c->repere == 'f') k = ajout(o, cap, k, "f %.4f %.4f", c->x, c->y);
    return k;
}

int sc2en_decrire(const sc2en_evt *e, const sc2en_point *pt, unsigned presents, double t_s, char *o, size_t cap)
{
    static const char *const BOUTONS = "gdm";
    size_t k = 0;
    if (!o || !cap) return 0;
    o[0] = 0;
    k = ajout(o, cap, k, "l.%d %s → ", e->ligne, e->commande ? e->commande : "?");
    switch (e->type) {
    case SC2EN_DEPLACER: case SC2EN_BAS: case SC2EN_HAUT: case SC2EN_GLISSER:
        k = ajout(o, cap, k, "%s", e->type == SC2EN_DEPLACER ? "MOVE " : e->type == SC2EN_BAS ? "DOWN "
                  : e->type == SC2EN_HAUT ? "UP " : "DRAG ");
        if (e->type != SC2EN_DEPLACER) k = ajout(o, cap, k, "%c ", BOUTONS[e->bouton >= 0 && e->bouton <= 2 ? e->bouton : 0]);
        k = ajout_coord(o, cap, k, &e->pos);
        k = ajout_mods(o, cap, k, e->drapeaux);
        break;
    case SC2EN_MOLETTE:
        k = ajout(o, cap, k, "MOLETTE dy %g dx %g %s", e->dy, e->dx, e->precis ? "points" : "crans");
        k = ajout_mods(o, cap, k, e->drapeaux);
        break;
    case SC2EN_TOUCHE_BAS: case SC2EN_TOUCHE_HAUT:
        k = ajout(o, cap, k, "%s 0x%02X ", e->type == SC2EN_TOUCHE_BAS ? "TOUCHE_BAS" : "TOUCHE_HAUT", e->kvk);
        k = ajout_chaine(o, cap, k, e->texte);
        k = ajout_mods(o, cap, k, e->drapeaux);
        if (e->repetition) k = ajout(o, cap, k, " rep");
        break;
    case SC2EN_MODIFICATEURS:
        {
            size_t k0 = (k = ajout(o, cap, k, "flagsChanged 0x%02X, tenus :", e->kvk));
            k = ajout_mods(o, cap, k, e->drapeaux);
            if (k == k0) k = ajout(o, cap, k, " aucun");
        }
        break;
    case SC2EN_FOCUS: k = ajout(o, cap, k, "FOCUS %d", e->valeur); break;
    case SC2EN_OCCULTE: k = ajout(o, cap, k, "OCCULTE %d", e->valeur); break;
    case SC2EN_MINIATURISER: k = ajout(o, cap, k, "MINIATURISER"); break;
    case SC2EN_RESTAURER: k = ajout(o, cap, k, "RESTAURER"); break;
    case SC2EN_PLEIN_ECRAN:
        k = e->valeur < 0 ? ajout(o, cap, k, "PLEIN_ECRAN (bascule)") : ajout(o, cap, k, "PLEIN_ECRAN %d", e->valeur);
        break;
    case SC2EN_TAILLE: k = ajout(o, cap, k, "TAILLE %gx%g points", e->l, e->h); break;
    case SC2EN_FERMER: k = ajout(o, cap, k, "FERMER"); break;
    case SC2EN_QUITTER: k = ajout(o, cap, k, "QUITTER"); break;
    case SC2EN_SIGNAL: k = ajout(o, cap, k, "SIGNAL %s", e->valeur == SIGTERM ? "TERM" : e->valeur == SIGINT ? "INT" : "?"); break;
    case SC2EN_ATTENDRE_LIGNE:
        k = ajout(o, cap, k, "ATTENDRE_LIGNE ");
        k = ajout_chaine(o, cap, k, e->texte);
        k = ajout(o, cap, k, " (%u ms au plus)", e->delai_ms);
        break;
    case SC2EN_JOURNAL: k = ajout(o, cap, k, "JOURNAL "); k = ajout_chaine(o, cap, k, e->texte); break;
    case SC2EN_JALON: k = ajout(o, cap, k, "jalon"); break;
    }
    if (pt && e->pos.repere) {
        k = ajout(o, cap, k, " → v %.2f %.2f (px %d %d, client ", pt->vx, pt->vy, pt->px, pt->py);
        k = pt->cx >= 0 || pt->cy >= 0 ? ajout(o, cap, k, "%d %d", pt->cx, pt->cy) : ajout(o, cap, k, "?");
        k = ajout(o, cap, k, "%s)", pt->dedans ? "" : ", hors image");
    }
    k = ajout(o, cap, k, " — Present n°%u, t=%.3f s", presents, t_s);
    return (int)k;
}

/* ==== suivi du journal ============================================================================================= */
/* Le même fichier que fd (écriture seule), rouvert en lecture : F_GETPATH sous macOS, /proc/self/fd sous Linux ; le
 * double doit être le même fichier (st_dev, st_ino). Rend le nouveau fd, ou -1. */
static int rouvrir_en_lecture(int fd, const struct stat *st)
{
    int r = -1;
#ifdef F_GETPATH
    char p[MAXPATHLEN];
    if (fcntl(fd, F_GETPATH, p) == 0) r = open(p, O_RDONLY | O_CLOEXEC);
#else
    char p[64];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    r = open(p, O_RDONLY | O_CLOEXEC);
#endif
    if (r < 0) return -1;
    struct stat s2;
    if (fstat(r, &s2) != 0 || s2.st_dev != st->st_dev || s2.st_ino != st->st_ino) { close(r); return -1; }
    return r;
}

int sc2en_suivi_fd(sc2en_suivi *j, int fd, long long depart)
{
    struct stat st;
    memset(j, 0, sizeof *j);
    j->fd = -1;
    if (fd < 0 || fstat(fd, &st) != 0) { j->erreur = fd < 0 ? EBADF : errno; errno = j->erreur; return -1; }
    if (!S_ISREG(st.st_mode)) { j->erreur = ESPIPE; errno = ESPIPE; return -1; }
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) { j->erreur = errno; return -1; }
    if ((fl & O_ACCMODE) == O_WRONLY) {                    /* « > fichier », « >> fichier » : pread y rendrait EBADF */
        int r = rouvrir_en_lecture(fd, &st);
        if (r < 0) { j->erreur = EBADF; errno = EBADF; return -1; }
        j->fd = r;
        j->possede = 1;
    } else
        j->fd = fd;
    j->pos = depart < 0 ? (long long)st.st_size : depart;
    return 0;
}

int sc2en_suivi_ouvrir(sc2en_suivi *j, const char *chemin, long long depart)
{
    if (!chemin || !strcmp(chemin, "-")) return sc2en_suivi_fd(j, 1, depart);
    int fd = open(chemin, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        memset(j, 0, sizeof *j);
        j->fd = -1;
        j->erreur = e;
        return -1;
    }
    if (sc2en_suivi_fd(j, fd, depart)) {
        int e = j->erreur;
        close(fd);
        j->erreur = e;
        errno = e;
        return -1;
    }
    j->possede = 1;
    return 0;
}

static int contient(const char *b, size_t n, const char *m, size_t mn)
{
    if (n < mn) return 0;
    for (size_t i = 0; i + mn <= n; i++)
        if (b[i] == m[0] && !memcmp(b + i, m, mn)) return 1;
    return 0;
}

/* ligne écrite par le rejeu lui-même (« [MACEV] » en tête : sc2_log n'ajoute aucun préfixe aux lignes du jeu) */
static int ligne_du_rejeu(const char *b, size_t n)
{
    static const char P[] = "[MACEV]";
    return n >= sizeof P - 1 && !memcmp(b, P, sizeof P - 1);
}

int sc2en_suivi_chercher(sc2en_suivi *j, const char *motif)
{
    char buf[32768];
    if (!j || j->fd < 0 || !motif || !*motif) return -1;
    size_t mn = strlen(motif);
    for (int tour = 0; tour < 256; tour++) {               /* au plus 8 Mio examinés par appel */
        ssize_t n = pread(j->fd, buf, sizeof buf, (off_t)j->pos);
        if (n < 0) {
            if (errno == EINTR) continue;
            j->erreur = errno;
            return -1;
        }
        j->erreur = 0;
        if (n == 0) return 0;
        size_t deb = 0;
        for (size_t k = 0; k < (size_t)n; k++)
            if (buf[k] == '\n') {
                /* deb = 0 avec milieu : fin d'une ligne géante entamée (son sort est déjà décidé) ; sinon une ligne
                 * entière, vue depuis son début */
                int saute = !deb && j->milieu ? j->saute : ligne_du_rejeu(buf + deb, k - deb);
                j->milieu = j->saute = 0;
                if (!saute && contient(buf + deb, k - deb, motif, mn)) { j->pos += (long long)k + 1; return 1; }
                deb = k + 1;
            }
        if (!deb) {                                        /* aucune fin de ligne dans ce qui est lu */
            if ((size_t)n < sizeof buf) return 0;          /* ligne en cours d'écriture : on y reviendra */
            if (!j->milieu) { j->milieu = 1; j->saute = ligne_du_rejeu(buf, (size_t)n); }   /* ligne géante */
            if (j->saute) { j->pos += n; continue; }
            if (contient(buf, (size_t)n, motif, mn)) {     /* vue par morceaux ; elle ne sert qu'une fois : */
                j->pos += n;                               /* son reste est sauté */
                j->saute = 1;
                return 1;
            }
            j->pos += (long long)((size_t)n - (mn - 1));   /* chevauchement de mn − 1 octets */
            continue;
        }
        j->pos += (long long)deb;
        if ((size_t)n < sizeof buf) return 0;
    }
    return 0;
}

int sc2en_suivi_rappel(void *suivi, const char *motif) { return sc2en_suivi_chercher((sc2en_suivi *)suivi, motif) > 0; }

void sc2en_suivi_fermer(sc2en_suivi *j)
{
    if (!j) return;
    if (j->possede && j->fd >= 0) close(j->fd);
    j->fd = -1;
    j->possede = 0;
}
