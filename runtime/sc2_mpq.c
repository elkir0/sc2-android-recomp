/* sc2_mpq.c — passe 6a : index StormLib des archives MPQ ouvertes par le moteur (cf. sc2_mpq.h).
 *
 * À l'ouverture : SFileOpenArchive (lecture seule) puis, pour chaque fichier connu du listfile
 * interne, sa position et sa taille compressée dans l'archive (SFileInfoByteOffset +
 * SFileInfoCompressedSize), triées par position. Une lecture brute du moteur est rapprochée de ces
 * intervalles : en-tête/tables MPQ, ou fichier interne nommé. Seule la 1re lecture de chaque
 * fichier est tracée (SC2_MPQ_TRACE=0 : aucune ; =2 : toutes les lectures).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "StormLib.h"
#include "sc2_mpq.h"

void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  sc2_trace_actif(void);                                   /* passe 10n : SC2_TRACE=1 */
#define sc2_trace(...) do { if (sc2_trace_actif()) sc2_log(__VA_ARGS__); } while (0)

typedef struct { uint64_t pos, fin; char *nom; uint32_t taille; uint8_t lu; } mpq_ent;
typedef struct {
    uint32_t fh[8];             /* handles invités ouverts sur cette archive (0 = libre) */
    char win[256], hote[512];
    HANDLE hmpq;
    mpq_ent *e; int n;
    uint64_t base;              /* position de l'en-tête MPQ dans le fichier */
    uint64_t tables_deb, tables_fin;
    int distincts, ouvertures; uint64_t octets_lus, lectures; int tables_vues, hors_index;
} mpq_arc;

static mpq_arc g_arc[32];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_niv = -1;

static mpq_arc *par_fh(uint32_t fh)
{
    for (int i = 0; i < 32; i++) for (int j = 0; j < 8; j++) if (g_arc[i].hmpq && g_arc[i].fh[j] == fh) return &g_arc[i];
    return NULL;
}
static int niveau(void)
{
    if (g_niv < 0) { const char *e = getenv("SC2_MPQ_TRACE"); g_niv = e ? atoi(e) : 1; }
    return g_niv;
}
static int par_pos(const void *a, const void *b)
{
    const mpq_ent *x = a, *y = b;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}
static uint64_t info64(HANDLE h, SFileInfoClass k)
{
    ULONGLONG v = 0; DWORD need = 0;
    return SFileGetFileInfo(h, k, &v, sizeof v, &need) ? (uint64_t)v : 0;
}
static uint32_t info32(HANDLE h, SFileInfoClass k)
{
    DWORD v = 0, need = 0;
    return SFileGetFileInfo(h, k, &v, sizeof v, &need) ? (uint32_t)v : 0;
}
/* Nom affiché d'une archive : chemin Windows à partir de « StarCraft II\ ». */
static const char *court(const char *w)
{
    const char *p = strstr(w, "StarCraft II\\");
    return p ? p + 13 : w;
}

void sc2_mpq_ouvert(uint32_t fh, const char *hote, const char *win)
{
    if (!niveau()) return;
    FILE *f = fopen(hote, "rb");
    if (!f) return;
    unsigned char sig[4] = { 0 };
    size_t k = fread(sig, 1, 4, f);
    fclose(f);
    if (k != 4 || memcmp(sig, "MPQ", 3) || (sig[3] != 0x1A && sig[3] != 0x1B)) return;

    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < 32; i++) if (g_arc[i].hmpq && !strcmp(g_arc[i].hote, hote)) {   /* déjà indexée */
        for (int j = 0; j < 8; j++) if (!g_arc[i].fh[j]) { g_arc[i].fh[j] = fh; break; }
        g_arc[i].ouvertures++;
        pthread_mutex_unlock(&g_mu); return;
    }
    pthread_mutex_unlock(&g_mu);
    HANDLE h = NULL;
    if (!SFileOpenArchive(hote, 0, MPQ_OPEN_READ_ONLY, &h)) {
        sc2_log("[MPQ] %s : signature MPQ mais StormLib refuse l'archive (err %u)", court(win), (unsigned)SErrGetLastError());
        return;
    }
    pthread_mutex_lock(&g_mu);
    mpq_arc *a = NULL;
    for (int i = 0; i < 32; i++) if (!g_arc[i].hmpq) { a = &g_arc[i]; break; }
    if (!a) { pthread_mutex_unlock(&g_mu); SFileCloseArchive(h); return; }
    memset(a, 0, sizeof *a);
    a->fh[0] = fh; a->hmpq = h; a->ouvertures = 1;
    snprintf(a->win, sizeof a->win, "%s", court(win));
    snprintf(a->hote, sizeof a->hote, "%s", hote);
    a->base = info64(h, SFileMpqHeaderOffset);
    uint32_t nfich = info32(h, SFileMpqNumberOfFiles);
    int cap = nfich ? (int)nfich + 16 : 1024;
    a->e = calloc((size_t)cap, sizeof *a->e);

    SFILE_FIND_DATA fd;
    HANDLE hf = SFileFindFirstFile(h, "*", &fd, NULL);
    for (bool ok = hf != NULL; ok; ok = SFileFindNextFile(hf, &fd)) {
        HANDLE hfi = NULL;
        if (!SFileOpenFileEx(h, fd.cFileName, SFILE_OPEN_FROM_MPQ, &hfi)) continue;
        uint64_t pos = info64(hfi, SFileInfoByteOffset);
        SFileCloseFile(hfi);
        if (a->n == cap) { cap *= 2; a->e = realloc(a->e, (size_t)cap * sizeof *a->e); }
        mpq_ent *e = &a->e[a->n++];
        e->pos = a->base + pos; e->fin = e->pos + (fd.dwCompSize ? fd.dwCompSize : 1);
        e->taille = fd.dwFileSize; e->nom = strdup(fd.cFileName); e->lu = 0;
    }
    if (hf) SFileFindClose(hf);
    qsort(a->e, (size_t)a->n, sizeof *a->e, par_pos);
    /* Tables de l'archive (hash/bloc/HET/BET) : après les données en v1-v3 ; on retient le plus
     * petit et le plus grand des décalages connus pour classer les lectures « tables ». */
    uint64_t t[] = { info64(h, SFileMpqHashTableOffset), info64(h, SFileMpqBlockTableOffset),
                     info64(h, SFileMpqHetTableOffset), info64(h, SFileMpqBetTableOffset),
                     info64(h, SFileMpqHiBlockTableOffset) };
    a->tables_deb = UINT64_MAX; a->tables_fin = 0;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i]) {
        if (a->base + t[i] < a->tables_deb) a->tables_deb = a->base + t[i];
        if (a->base + t[i] > a->tables_fin) a->tables_fin = a->base + t[i];
    }
    uint32_t fmt = 0; DWORD need = 0;
    SFileGetFileInfo(h, SFileMpqHeader, NULL, 0, &need);
    if (need >= 14) {
        unsigned char hd[256] = { 0 };
        if (SFileGetFileInfo(h, SFileMpqHeader, hd, need < sizeof hd ? need : sizeof hd, &need))
            fmt = (uint32_t)hd[12] | (uint32_t)hd[13] << 8;
    }
    sc2_log("[MPQ] MONTÉE par le moteur : %s — format v%u, secteur %u o, %d fichiers indexés (StormLib)",
            a->win, fmt + 1, info32(h, SFileMpqSectorSize), a->n);
    pthread_mutex_unlock(&g_mu);
}

void sc2_mpq_lu(uint32_t fh, uint64_t off, uint32_t n)
{
    if (!g_niv || !n) return;
    pthread_mutex_lock(&g_mu);
    mpq_arc *a = par_fh(fh);
    if (!a) { pthread_mutex_unlock(&g_mu); return; }
    a->lectures++; a->octets_lus += n;
    uint64_t fin = off + n;
    if (off < a->base + 0x100) {                              /* en-tête MPQ */
        if (!a->tables_vues++) sc2_trace("[MPQ] %s : en-tête lu par le moteur (off 0x%llX, %u o)", a->win,
                                       (unsigned long long)off, n);
        pthread_mutex_unlock(&g_mu); return;
    }
    /* 1er fichier dont la fin dépasse off (recherche binaire sur pos, puis recul d'un cran) */
    int lo = 0, hi = a->n;
    while (lo < hi) { int m = (lo + hi) / 2; if (a->e[m].pos <= off) lo = m + 1; else hi = m; }
    int i = lo > 0 ? lo - 1 : 0, touche = 0;
    for (; i < a->n && a->e[i].pos < fin; i++) {
        mpq_ent *e = &a->e[i];
        if (e->fin <= off) continue;
        touche = 1;
        /* passe 10n : sans SC2_TRACE, seules les 1res lectures qui servent de jalons aux phases Android
         * (android/jni/sc2_phases.h, motifs par défaut « ui_camapaignmenu » et « → Maps\ ») restent écrites. */
        if ((!e->lu || niveau() >= 2) && (sc2_trace_actif() || niveau() >= 2
             || (!e->lu && (strstr(e->nom, "ui_camapaignmenu") || !strncmp(e->nom, "Maps\\", 5)))))
            sc2_log("[MPQ] %s → %s (%u o)%s", a->win, e->nom, e->taille, e->lu ? "" : " — 1re lecture");
        if (!e->lu) { e->lu = 1; a->distincts++; }
    }
    if (!touche) {
        if (off >= a->tables_deb && a->tables_deb != UINT64_MAX) {
            if (a->tables_vues++ < 4) sc2_trace("[MPQ] %s : tables hash/bloc lues (off 0x%llX, %u o)", a->win,
                                              (unsigned long long)off, n);
        } else if (a->hors_index++ < 4)
            sc2_log("[MPQ] %s : lecture hors index (off 0x%llX, %u o) — fichier absent du listfile", a->win,
                    (unsigned long long)off, n);
    }
    pthread_mutex_unlock(&g_mu);
}

void sc2_mpq_ferme(uint32_t fh)
{
    pthread_mutex_lock(&g_mu);
    mpq_arc *a = par_fh(fh);                         /* l'index est gardé (réouverture, bilan) */
    if (a) for (int j = 0; j < 8; j++) if (a->fh[j] == fh) a->fh[j] = 0;
    pthread_mutex_unlock(&g_mu);
}

void sc2_mpq_bilan(void)
{
    int n = 0;
    for (int i = 0; i < 32; i++) {
        mpq_arc *a = &g_arc[i];
        if (!a->hmpq) continue;
        if (!n++) printf("[BILAN] archives MPQ montées par le moteur (index StormLib) :\n");
        printf("    %-52s %5d / %5d fichiers lus, %llu lectures, %.1f Mo, %d ouverture(s)\n", a->win, a->distincts, a->n,
               (unsigned long long)a->lectures, a->octets_lus / 1048576.0, a->ouvertures);
    }
    if (!n) printf("[BILAN] aucune archive MPQ ouverte par le moteur\n");
}
