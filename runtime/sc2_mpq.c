/* sc2_mpq.c — passe 6a : index StormLib des archives MPQ ouvertes par le moteur (cf. sc2_mpq.h).
 *
 * À l'ouverture : SFileOpenArchive (lecture seule) puis, pour chaque fichier connu du listfile
 * interne, sa position et sa taille compressée dans l'archive (SFileInfoByteOffset +
 * SFileInfoCompressedSize), triées par position. Une lecture brute du moteur est rapprochée de ces
 * intervalles : en-tête/tables MPQ, ou fichier interne nommé. Seule la 1re lecture de chaque
 * fichier est tracée (SC2_MPQ_TRACE=0 : aucune ; =2 : toutes les lectures).
 *
 * 24/10 — ALIAS « X.wav » → « X.wav_ » (port runtime de la passe 7z2 ; SC2_MPQ_ALIAS_WAV=0 le coupe, indépendant de
 * SC2_MPQ_TRACE). Les archives d'assets rebâties depuis le Tome n'ont les bruitages que sous « X.wav_ » (2 980 fichiers
 * dans les 4 archives Liberty) ; SoundData.xml demande « X.wav » : sans alias, aucun son « .wav » n'atteint createSound et
 * un CActorSound créé par déclencheur est détruit à sa création (« Trigger Error … Could not create actor » au pont de
 * l'Hyperion). À la montée d'une archive (« MPQ\x1A » en tête, v1 ou v2), son (listfile) est relu par StormLib ; s'il
 * nomme des « .wav_ », la table de hachage est lue sur disque, augmentée (sc2_mpq_alias.h : une entrée « X.wav » par
 * entrée vivante de « X.wav_ », seulement si « X.wav » est absent pour cette locale ; aucune entrée existante ne change)
 * puis rechiffrée ; quand le moteur lit cette plage par ReadFile, sc2_mpq_lu y recopie la table augmentée. Le fichier
 * sur disque n'est jamais modifié. Bilan : « [MPQ] alias .wav : N entrées dans K archives ».
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <strings.h>                                          /* V5.1 : strncasecmp (mem_carte) */
#include "StormLib.h"
#include "sc2_mpq.h"
#include "sc2_mpq_alias.h"

void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  sc2_trace_actif(void);                                   /* passe 10n : SC2_TRACE=1 */
#define sc2_trace(...) do { if (sc2_trace_actif()) sc2_log(__VA_ARGS__); } while (0)
/* V1.1 (22/10) : chaque fichier interne touché par une lecture, signalé au banc d'entrées (sc2_entrees_banc.c :
 * QUAND:CARTE, QUAND:STABLE) indépendamment du drapeau « 1re lecture » — sous g_mu ; référence faible */
void sc2_banc_mpq_lu(const char *nom_interne) __attribute__((weak));
/* V5.1 (24/10) : ligne [MEM] (sc2_runtime.c) à chaque CHANGEMENT de carte lue — DocumentHeader de Maps\…\<nom>.SC2Map,
 * une ligne par transition hub ↔ mission ; sous g_mu. Référence faible (tests sans le runtime). */
void sc2_mem_ligne(const char *motif) __attribute__((weak));
static void mem_carte(const char *nom)
{
    static char der[128];
    if (!sc2_mem_ligne || strncasecmp(nom, "Maps\\", 5)) return;
    for (const char *p = nom + 5; *p; p++) {
        if (strncasecmp(p, ".SC2Map\\", 8)) continue;
        if (strcasecmp(p + 8, "DocumentHeader")) return;
        const char *d = p;
        while (d > nom && d[-1] != '\\') d--;
        char c[128], m[160];
        snprintf(c, sizeof c, "%.*s", (int)(p - d), d);
        if (!strcasecmp(c, der)) return;
        snprintf(der, sizeof der, "%s", c); snprintf(m, sizeof m, "carte %s lue", c);
        sc2_mem_ligne(m);
        return;
    }
}

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

/* ---- 24/10 : alias « X.wav » → « X.wav_ » (cf. en-tête ; état sous g_mu) -------------------------------------------- */
typedef struct {
    char hote[512], win[256];
    uint32_t fh[16];            /* handles invités ouverts sur cette archive (0 = libre) */
    uint8_t *tab;               /* table de hachage augmentée, chiffrée, prête à servir (NULL : aucun alias) */
    uint64_t pos; uint32_t oct; /* plage de la table dans le fichier */
    int n; uint64_t servies;    /* alias posés ; lectures du moteur recouvrant la table */
} alias_arc;
static alias_arc g_al[32];
static int g_al_n, g_al_actif = -1;

static int alias_actif(void)
{
    if (g_al_actif < 0) { const char *e = getenv("SC2_MPQ_ALIAS_WAV"); g_al_actif = !(e && !strcmp(e, "0")); }
    return g_al_actif;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* (listfile) entier, terminé par 0 (StormLib rend FALSE + ERROR_HANDLE_EOF avec des octets sur la dernière lecture). */
static char *lire_listfile(HANDLE h, size_t *n)
{
    HANDLE f = NULL;
    *n = 0;
    if (!SFileOpenFileEx(h, "(listfile)", SFILE_OPEN_FROM_MPQ, &f)) return NULL;
    DWORD t = SFileGetFileSize(f, NULL), lu = 0, tot = 0;
    char *b = t != SFILE_INVALID_SIZE ? malloc((size_t)t + 1) : NULL;
    while (b && tot < t) {
        lu = 0;
        bool ok = SFileReadFile(f, b + tot, t - tot, &lu, NULL);
        tot += lu;
        if (!ok || !lu) break;
    }
    SFileCloseFile(f);
    if (b) { b[tot] = 0; *n = tot; }
    return b;
}
static int nomme_des_wav_(const char *b, size_t n)
{
    for (size_t i = 4; i < n; i++)
        if (b[i] == '_' && (i + 1 == n || b[i + 1] == '\r' || b[i + 1] == '\n' || b[i + 1] == ';')
            && !strncasecmp(b + i - 4, ".wav", 4)) return 1;
    return 0;
}
/* Construit la table augmentée de l'archive `a` (g_mu tenu ; une seule fois par archive). */
static void alias_construire(alias_arc *a)
{
    FILE *f = fopen(a->hote, "rb");
    if (!f) return;
    uint8_t hd[0x2C] = { 0 };
    int ok = fread(hd, 1, sizeof hd, f) == sizeof hd && !memcmp(hd, "MPQ\x1A", 4);
    uint32_t fmt = (uint32_t)hd[12] | (uint32_t)hd[13] << 8, taille = le32(hd + 0x18);
    uint64_t pos = le32(hd + 0x10) + (fmt == 1 ? (uint64_t)(hd[0x28] | hd[0x29] << 8) << 32 : 0);
    if (!ok || fmt > 1 || !taille || (taille & (taille - 1)) || taille > (1u << 20)) { fclose(f); return; }
    HANDLE h = NULL; size_t nl = 0; char *lst = NULL;
    if (SFileOpenArchive(a->hote, 0, MPQ_OPEN_READ_ONLY | MPQ_OPEN_NO_LISTFILE | MPQ_OPEN_NO_ATTRIBUTES, &h)) {
        lst = lire_listfile(h, &nl);
        SFileCloseArchive(h);
    }
    if (!lst || !nomme_des_wav_(lst, nl)) { free(lst); fclose(f); return; }
    mpq_hent *t = malloc((size_t)taille * sizeof *t);
    ok = t && !fseeko(f, (off_t)pos, SEEK_SET) && fread(t, sizeof *t, taille, f) == taille;
    fclose(f);
    if (!ok) { free(t); free(lst); return; }
    uint32_t cle = mpq_hache("(hash table)", 3);
    mpq_dechiffrer((uint32_t *)t, (size_t)taille * 4, cle);
    mpq_alias_bilan bl;
    int n = mpq_alias_wav(t, taille, lst, nl, &bl);
    free(lst);
    sc2_log("[MPQ] alias .wav : %s — %d entrée(s) « X.wav » ajoutée(s) pour %d « X.wav_ » du (listfile) (table de %u entrées ; "
            "%d « X.wav » déjà présent(s), %d absent(s) de la table, %d refusé(s) table pleine)", a->win, n, bl.noms, taille,
            bl.deja, bl.absents, bl.plein);
    if (!n) { free(t); return; }
    mpq_chiffrer((uint32_t *)t, (size_t)taille * 4, cle);
    a->tab = (uint8_t *)t; a->pos = pos; a->oct = taille * (uint32_t)sizeof *t; a->n = n;
}
static void alias_ouvert(uint32_t fh, const char *hote, const char *win)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_al_n; i++) if (!strcmp(g_al[i].hote, hote)) {          /* déjà examinée */
        int j = 0; while (j < 16 && g_al[i].fh[j]) j++;
        if (j < 16) g_al[i].fh[j] = fh;
        else if (g_al[i].tab) sc2_log("[MPQ] alias .wav : %s — plus de 16 handles ouverts, handle 0x%X servi SANS alias",
                                      g_al[i].win, fh);
        pthread_mutex_unlock(&g_mu); return;
    }
    if (g_al_n < 32) {
        alias_arc *a = &g_al[g_al_n++];
        memset(a, 0, sizeof *a);
        snprintf(a->hote, sizeof a->hote, "%s", hote);
        snprintf(a->win, sizeof a->win, "%s", court(win));
        a->fh[0] = fh;
        alias_construire(a);
    }
    pthread_mutex_unlock(&g_mu);
}
/* ReadFile du moteur sur `fh`, [off, off+n) déjà dans `tampon` : la partie qui recouvre une table augmentée est remplacée. */
static void alias_servir(uint32_t fh, uint64_t off, void *tampon, uint32_t n)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_al_n; i++) {
        alias_arc *a = &g_al[i];
        if (!a->tab || off >= a->pos + a->oct || off + n <= a->pos) continue;
        int j = 0; while (j < 16 && a->fh[j] != fh) j++;
        if (j == 16) continue;
        uint64_t d = off > a->pos ? off : a->pos, fin = off + n < a->pos + a->oct ? off + n : a->pos + a->oct;
        memcpy((uint8_t *)tampon + (d - off), a->tab + (d - a->pos), (size_t)(fin - d));
        if (!a->servies++)
            sc2_log("[MPQ] alias .wav : %s — table de hachage servie au moteur avec %d alias (off 0x%llX, %llu o)", a->win,
                    a->n, (unsigned long long)d, (unsigned long long)(fin - d));
    }
    pthread_mutex_unlock(&g_mu);
}

void sc2_mpq_ouvert(uint32_t fh, const char *hote, const char *win)
{
    if (!niveau() && !alias_actif()) return;
    FILE *f = fopen(hote, "rb");
    if (!f) return;
    unsigned char sig[16] = { 0 };
    size_t k = fread(sig, 1, 16, f);
    long taille = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    fclose(f);
    if (k < 4 || memcmp(sig, "MPQ", 3) || (sig[3] != 0x1A && sig[3] != 0x1B)) return;
    if (sig[3] == 0x1A && alias_actif()) alias_ouvert(fh, hote, win);    /* 24/10 : avant toute lecture du moteur */
    if (!niveau()) return;
    /* L3 (24/10) : StormLib (OpenArchiveFromStream, ~/tools/StormLib src/SFileOpenArchive.cpp:343) boucle SANS FIN
     * quand l'en-tête MPQ que désignent les données utilisateur (« MPQ\x1B », dwHeaderOffs en +8) n'a pas 0xD0 octets
     * devant lui avant la fin du fichier : la lecture échoue, le break ne sort que de la boucle interne et la recherche
     * reprend à l'offset 0. Cas réel : les sauvegardes ratées de 1 181 o (0x400 + MPQ de 157 o) — le dialogue « Save »
     * du menu F10 les liste et les ouvre, et le run se figeait (97 % CPU dans h_CreateFileW → sc2_mpq_ouvert). L'index
     * n'étant qu'un diagnostic, une telle archive n'est pas indexée. */
    if (sig[3] == 0x1B && k == 16) {
        uint32_t off = (uint32_t)sig[8] | (uint32_t)sig[9] << 8 | (uint32_t)sig[10] << 16 | (uint32_t)sig[11] << 24;
        if (taille < 0 || (uint64_t)off + 0xD0u > (uint64_t)taille) {
            sc2_log("[MPQ] %s : en-tête MPQ à 0x%X trop près de la fin (%ld o), archive non indexée (boucle StormLib)",
                    court(win), off, taille);
            return;
        }
    }

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

void sc2_mpq_lu(uint32_t fh, uint64_t off, void *tampon, uint32_t n)
{
    if (!n) return;
    if (g_al_n && tampon) alias_servir(fh, off, tampon, n);
    if (!g_niv) return;
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
        if (sc2_banc_mpq_lu) sc2_banc_mpq_lu(e->nom);
        mem_carte(e->nom);                                    /* V5.1 */
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
    for (int i = 0; i < g_al_n; i++) for (int j = 0; j < 16; j++) if (g_al[i].fh[j] == fh) g_al[i].fh[j] = 0;   /* 24/10 */
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
    if (!alias_actif()) { printf("[MPQ] alias .wav : coupé (SC2_MPQ_ALIAS_WAV=0)\n"); return; }
    int na = 0, k = 0; uint64_t sv = 0;
    for (int i = 0; i < g_al_n; i++) if (g_al[i].tab) { na += g_al[i].n; k++; sv += g_al[i].servies; }
    printf("[MPQ] alias .wav : %d entrées dans %d archives (%d archives MPQ examinées ; lectures du moteur servies avec "
           "alias : %llu)\n", na, k, g_al_n, (unsigned long long)sv);
}

/* ---- L3 (24/10) : carte de campagne servie depuis son archive de cartes (Load sans parent) ----------------------
 * Une carte de campagne n'existe pas sur disque : « Maps\Campaign\TRaynor02.SC2Map » est un DOSSIER de
 * Campaigns\Liberty.SC2Campaign\Base.SC2Maps (option mapArchivePath). Le lancement normal l'ouvre avec cette
 * archive pour parent (L_00AB3990 → L_00AB2450 → L_009902D0(…, parent)). Le Load d'une sauvegarde en mode histoire
 * ne passe pas par là : L_00B1DFF0 → L_00AB2D00 → L_009902D0 l'ouvre SANS parent, comme archive disque requise, et
 * l'échec part en erreur fatale 0x20010002 (rapport L3, § 4 ; cause exacte non tranchée). Contournement : le runtime
 * construit une fois, dans <SC2_ROOT>/cartes-campagne/, une archive MPQ v1 contenant les fichiers de ce dossier
 * (préfixe retiré, contenu recopié tel quel, sans compression), et la sert au chemin demandé. */
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
static pthread_mutex_t g_cartes_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char rel[512], hote[1024]; } g_cartes[16];
static int g_n_cartes;

static void mkdirs_hote(const char *chemin)
{
    char t[1024]; snprintf(t, sizeof t, "%s", chemin);
    for (char *q = t + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
}
/* Copie dans `dest` les fichiers de `src` dont le nom commence par `prefixe` (avec « \ » final). Rend le nombre de
 * fichiers copiés (0 : dossier absent de l'archive), -1 en cas d'échec d'écriture. */
static int copier_dossier(HANDLE src, const char *prefixe, const char *dest, uint64_t *octets)
{
    size_t lp = strlen(prefixe);
    char masque[600]; snprintf(masque, sizeof masque, "%s*", prefixe);
    char **noms = NULL; int n = 0, cap = 0;
    SFILE_FIND_DATA fd;
    HANDLE hf = SFileFindFirstFile(src, masque, &fd, NULL);
    for (bool ok = hf != NULL; ok; ok = SFileFindNextFile(hf, &fd)) {
        if (strncasecmp(fd.cFileName, prefixe, lp) || !fd.cFileName[lp]) continue;
        if (n == cap) { cap = cap ? 2 * cap : 64; noms = realloc(noms, (size_t)cap * sizeof *noms); }
        noms[n++] = strdup(fd.cFileName);
    }
    if (hf) SFileFindClose(hf);
    if (!n) { free(noms); return 0; }
    mkdirs_hote(dest); unlink(dest);
    HANDLE dst = NULL; int copies = 0;
    if (!SFileCreateArchive(dest, MPQ_CREATE_ARCHIVE_V1 | MPQ_CREATE_LISTFILE | MPQ_CREATE_ATTRIBUTES,
                            (DWORD)(n + 16 < 64 ? 64 : n + 16), &dst)) copies = -1;
    for (int i = 0; i < n && copies >= 0; i++) {
        HANDLE f = NULL;
        if (!SFileOpenFileEx(src, noms[i], SFILE_OPEN_FROM_MPQ, &f)) continue;
        DWORD haut = 0, t = SFileGetFileSize(f, &haut), lu = 0;
        unsigned char *b = malloc(t ? t : 1);
        bool ok = b && (t == 0 || (SFileReadFile(f, b, t, &lu, NULL) && lu == t));
        SFileCloseFile(f);
        HANDLE g = NULL;
        if (ok && SFileCreateFile(dst, noms[i] + lp, 0, t, 0, MPQ_FILE_REPLACEEXISTING, &g)) {
            if (t) ok = SFileWriteFile(g, b, t, 0);
            ok = SFileFinishFile(g) && ok;
        } else ok = false;
        free(b);
        if (ok) { copies++; *octets += t; } else copies = -1;
    }
    if (dst && !SFileCloseArchive(dst)) copies = -1;
    for (int i = 0; i < n; i++) free(noms[i]);
    free(noms);
    if (copies < 0) unlink(dest);
    return copies;
}

int sc2_mpq_carte_campagne(const char *install, const char *rel, const char *cache, char *o, size_t cap, uint32_t appelant)
{
    pthread_mutex_lock(&g_cartes_mu);
    for (int i = 0; i < g_n_cartes; i++)
        if (!strcasecmp(g_cartes[i].rel, rel)) {
            snprintf(o, cap, "%s", g_cartes[i].hote);
            pthread_mutex_unlock(&g_cartes_mu);
            return 1;
        }
    char prefixe[600]; size_t k = 0;                          /* « /Maps/Campaign/X.SC2Map » → « Maps\Campaign\X.SC2Map\ » */
    for (const char *p = rel + (rel[0] == '/'); *p && k + 2 < sizeof prefixe; p++) prefixe[k++] = *p == '/' ? '\\' : *p;
    prefixe[k++] = '\\'; prefixe[k] = 0;
    char camp[1024]; snprintf(camp, sizeof camp, "%s/Campaigns", install);
    DIR *d = opendir(camp); struct dirent *e; int rendu = 0;
    while (d && !rendu && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 12 || strcasecmp(e->d_name + l - 12, ".SC2Campaign")) continue;
        char dc[1024]; snprintf(dc, sizeof dc, "%s/%s", camp, e->d_name);
        DIR *d2 = opendir(dc); struct dirent *e2;
        while (d2 && !rendu && (e2 = readdir(d2))) {
            size_t l2 = strlen(e2->d_name);
            if (l2 < 8 || strcasecmp(e2->d_name + l2 - 8, ".SC2Maps")) continue;
            char pack[1200]; snprintf(pack, sizeof pack, "%s/%s", dc, e2->d_name);
            HANDLE h = NULL;
            if (!SFileOpenArchive(pack, 0, MPQ_OPEN_READ_ONLY, &h)) continue;
            char dest[1200]; snprintf(dest, sizeof dest, "%s%s", cache, rel[0] == '/' ? rel : "/");
            if (rel[0] != '/') strncat(dest, rel, sizeof dest - strlen(dest) - 1);
            uint64_t oct = 0;
            int nf = copier_dossier(h, prefixe, dest, &oct);
            SFileCloseArchive(h);
            if (nf == 0) continue;
            if (nf < 0) {
                sc2_log("[MPQ] carte de campagne %s trouvée dans Campaigns/%s/%s mais l'archive synthétique n'a pas pu être "
                        "écrite (%s)", prefixe, e->d_name, e2->d_name, dest);
                rendu = -1; break;
            }
            sc2_log("[MPQ] carte de campagne servie depuis Campaigns/%s/%s (Load sans parent) : %s → archive synthétique "
                    "de %d fichiers, %llu o (%s), appelant 0x%08X", e->d_name, e2->d_name, prefixe, nf,
                    (unsigned long long)oct, dest, appelant);
            if (g_n_cartes < 16) {
                snprintf(g_cartes[g_n_cartes].rel, sizeof g_cartes[0].rel, "%s", rel);
                snprintf(g_cartes[g_n_cartes].hote, sizeof g_cartes[0].hote, "%s", dest);
                g_n_cartes++;
            }
            snprintf(o, cap, "%s", dest);
            rendu = 1;
        }
        if (d2) closedir(d2);
    }
    if (d) closedir(d);
    pthread_mutex_unlock(&g_cartes_mu);
    return rendu > 0;
}
