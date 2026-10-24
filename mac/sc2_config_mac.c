/* mac/sc2_config_mac.c — configuration de l'app macOS, en C pur (V3, lot M3, 22/10/2026). Contrat : sc2_config_mac.h.
 *
 * Repris d'android/jni/sc2_android_main.c (lire_env l. 170-184, copier_fichier l. 215-227, mkdir_p l. 230-237,
 * sauver_vars l. 255-276, effacement de sc2-c l. 329-334, configurer l. 301-380). Ce fichier appartient à V1.5 et
 * n'est pas touché : la mise en commun viendra avec M12. Différences voulues avec Android :
 *   - sc2.env ne remplace pas l'environnement (setenv(…, 0)) : un essai passe ses variables, un vieux sc2.env ne les
 *     écrase pas ;
 *   - le semis de Variables.txt n'est pas recopié : SC2_VARIABLES, semé par le runtime de V1.5 ;
 *   - SC2.exe est contrôlé par sa taille ET son sha256 (Android : taille seule, en Java) ;
 *   - le pack SPIR-V est contrôlé par son sha256 (journal seul, jamais bloquant).
 */
#include "sc2_config_mac.h"
/* Passe 10t, et V1.5 : celle-ci y définit SC2_VARIABLES_MAX. Sa présence dit que le runtime sème SC2_VARIABLES. */
#include "sc2_documents.h"

#include <CommonCrypto/CommonDigest.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VARS_MAX (64 * 1024)                 /* bornes de sauver_vars et de V1.5 (SC2_VARIABLES_MAX) */
#define DOCS_SC2 "Users/sc2/Documents/StarCraft II"   /* C:\Users\sc2\Documents\StarCraft II (hpath, sc2_iat.c) */

/* ---- outils ------------------------------------------------------------------------------- */

static void jrn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void jrn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void motif_poser(char *m, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void motif_poser(char *m, size_t cap, const char *fmt, ...)
{
    if (!m || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, cap, fmt, ap);
    va_end(ap);
}

/* snprintf qui dit la troncature : 1 = trop long (o vidé), 0 = écrit. */
static int fmt_chemin(char *o, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fmt_chemin(char *o, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) { if (cap) o[0] = 0; return 1; }
    return 0;
}

static double maintenant(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int est_fichier(const char *p) { struct stat st; return p && !stat(p, &st) && S_ISREG(st.st_mode); }
static int est_dossier(const char *p) { struct stat st; return p && !stat(p, &st) && S_ISDIR(st.st_mode); }

/* mkdir récursif (chaque composant en 0755). 0 = OK, -1 = échec au composant final. */
static int mkdir_p(const char *chemin)
{
    char t[1200];
    if (fmt_chemin(t, sizeof t, "%s", chemin) || !t[0]) return -1;
    size_t n = strlen(t);
    while (n > 1 && t[n - 1] == '/') t[--n] = 0;
    for (char *q = t + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
    return mkdir(t, 0755) && errno != EEXIST ? -1 : 0;
}

/* Dossier parent d'un chemin de fichier, créé. */
static void mkdir_parent(const char *fichier)
{
    char d[1200];
    if (fmt_chemin(d, sizeof d, "%s", fichier)) return;
    char *s = strrchr(d, '/');
    if (s && s != d) { *s = 0; mkdir_p(d); }
}

/* Nom de variable : [A-Za-z_][A-Za-z0-9_]* sur n octets. */
static int nom_valide(const char *k, size_t n)
{
    if (!n || !(isalpha((unsigned char)k[0]) || k[0] == '_')) return 0;
    for (size_t i = 1; i < n; i++)
        if (!(isalnum((unsigned char)k[i]) || k[i] == '_')) return 0;
    return 1;
}

static const char *ou_aucun(const char *nom) { const char *v = getenv(nom); return v ? v : "(aucun)"; }

/* Copie par blocs src → dst (dst créé/écrasé). 0 = OK, -1 = échec (journalisé par l'appelant). */
static int copier_fichier(const char *src, const char *dst)
{
    FILE *f = fopen(src, "rb");
    if (!f) return -1;
    FILE *g = fopen(dst, "wb");
    int ok = g != NULL;
    char b[8192];
    size_t k;
    while (ok && (k = fread(b, 1, sizeof b, f)) > 0) ok = fwrite(b, 1, k, g) == k;
    ok = ok && !ferror(f);
    if (g && fclose(g)) ok = 0;
    fclose(f);
    return ok ? 0 : -1;
}

/* ---- sc2.env ------------------------------------------------------------------------------ */

int sc2m_env_lire(const char *fichier)
{
    FILE *f = fopen(fichier, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        jrn("[MAC] %s illisible : %s", fichier, strerror(errno));
        return -1;
    }
    char l[4096];
    int n = 0, no = 0;
    while (fgets(l, sizeof l, f)) {
        no++;
        size_t len = strlen(l);
        if (len == sizeof l - 1 && l[len - 1] != '\n' && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') { }
            jrn("[MAC] %s:%d : ligne de plus de %zu octets, ignorée", fichier, no, sizeof l - 2);
            continue;
        }
        l[strcspn(l, "\r\n")] = 0;
        char *k = l;
        while (*k == ' ' || *k == '\t') k++;
        if (!*k || *k == '#') continue;
        char *eg = strchr(k, '=');
        if (!eg || !nom_valide(k, (size_t)(eg - k))) {
            jrn("[MAC] %s:%d : ligne ignorée (NOM=VALEUR attendu) : %s", fichier, no, k);
            continue;
        }
        *eg = 0;
        const char *v = eg + 1, *deja = getenv(k);
        if (deja) { jrn("[MAC] sc2.env : %s=%s ignorée (l'environnement l'emporte : %s)", k, v, deja); continue; }
        if (setenv(k, v, 0) == 0) { n++; jrn("[MAC] sc2.env : %s=%s", k, v); }
    }
    fclose(f);
    return n;
}

/* Index de la clé que la ligne [p, p+n) affecte parmi cles (première occurrence), -1 sinon. */
static int cle_de_ligne(const char *p, size_t n, const char *const *cles, int nc)
{
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i >= n || p[i] == '#') return -1;
    const char *eg = memchr(p + i, '=', n - i);
    if (!eg) return -1;
    size_t lk = (size_t)(eg - (p + i));
    if (!nom_valide(p + i, lk)) return -1;
    for (int j = 0; j < nc; j++)
        if (strlen(cles[j]) == lk && !memcmp(cles[j], p + i, lk)) return j;
    return -1;
}

/* Première occurrence de cles[j] dans cles (doublons dans l'appel : la première gagne). */
static int premiere(const char *const *cles, int j)
{
    for (int i = 0; i < j; i++) if (!strcmp(cles[i], cles[j])) return i;
    return j;
}

int sc2m_env_ecrire(const char *fichier, const char *const *cles, const char *const *vals, int n)
{
    enum { NMAX = 64, TAILLE_MAX = 1 << 20 };
    if (!fichier || !*fichier || n < 0 || n > NMAX || (n && (!cles || !vals))) { errno = EINVAL; return -1; }
    for (int i = 0; i < n; i++)
        if (!cles[i] || !nom_valide(cles[i], strlen(cles[i])) || (vals[i] && strpbrk(vals[i], "\r\n"))) {
            jrn("[MAC] sc2.env : écriture refusée (clé « %s » ou sa valeur invalide)", cles[i] ? cles[i] : "(nulle)");
            errno = EINVAL;
            return -1;
        }
    char *old = NULL;
    size_t nold = 0;
    FILE *f = fopen(fichier, "rb");
    if (f) {
        old = malloc(TAILLE_MAX + 1);
        if (!old) { fclose(f); return -1; }
        nold = fread(old, 1, TAILLE_MAX + 1, f);
        int err = ferror(f);
        fclose(f);
        if (err || nold > TAILLE_MAX) {
            jrn("[MAC] sc2.env : %s illisible ou de plus de %d octets, non réécrit", fichier, TAILLE_MAX);
            free(old);
            return -1;
        }
    } else if (errno != ENOENT) {
        jrn("[MAC] sc2.env : %s illisible (%s), non réécrit", fichier, strerror(errno));
        return -1;
    }
    char tmp[1200];
    if (fmt_chemin(tmp, sizeof tmp, "%s.tmp", fichier)) { free(old); errno = ENAMETOOLONG; return -1; }
    mkdir_parent(fichier);
    FILE *g = fopen(tmp, "wb");
    if (!g) { jrn("[MAC] sc2.env : %s : %s", tmp, strerror(errno)); free(old); return -1; }
    int fait[NMAX] = { 0 };
    for (const char *p = old, *fin_tout = old + nold; p && p < fin_tout; ) {
        const char *nl = memchr(p, '\n', (size_t)(fin_tout - p));
        size_t L = nl ? (size_t)(nl - p) : (size_t)(fin_tout - p);
        int i = cle_de_ligne(p, L, cles, n);
        if (i < 0) { fwrite(p, 1, L, g); fputc('\n', g); }
        else {
            if (!fait[i] && vals[i]) fprintf(g, "%s=%s\n", cles[i], vals[i]);
            fait[i] = 1;
        }
        p += L + (nl ? 1 : 0);
    }
    for (int i = 0; i < n; i++)
        if (premiere(cles, i) == i && !fait[i] && vals[i]) { fprintf(g, "%s=%s\n", cles[i], vals[i]); fait[i] = 1; }
    free(old);
    int ok = !ferror(g) && !fflush(g) && !fsync(fileno(g));
    if (fclose(g)) ok = 0;
    if (!ok || rename(tmp, fichier)) {
        jrn("[MAC] sc2.env : écriture de %s en échec (%s) ; fichier d'origine intact", fichier, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---- sha256 et cache des vérifications ---------------------------------------------------- */

static int g_calculs;
static char g_ref_exe[65] = SC2M_EXE_SHA256;

int sc2m_sha_calculs(void) { return g_calculs; }

int sc2m_install_reference(const char *sha256_hex)
{
    if (!sha256_hex) { snprintf(g_ref_exe, sizeof g_ref_exe, "%s", SC2M_EXE_SHA256); return 0; }
    if (strlen(sha256_hex) != 64) return -1;
    char t[65];
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)sha256_hex[i])) return -1;
        t[i] = (char)tolower((unsigned char)sha256_hex[i]);
    }
    t[64] = 0;
    memcpy(g_ref_exe, t, sizeof t);
    return 0;
}

int sc2m_sha256_fichier(const char *chemin, char hex[65])
{
    enum { BLOC = 1 << 20 };
    int fd = open(chemin, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    unsigned char *b = malloc(BLOC);
    if (!b) { close(fd); return -1; }
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    int ok = 1;
    for (;;) {
        ssize_t k = read(fd, b, BLOC);
        if (k > 0) CC_SHA256_Update(&ctx, b, (CC_LONG)k);
        else if (k == 0) break;
        else if (errno != EINTR) { ok = 0; break; }
    }
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(md, &ctx);
    free(b);
    close(fd);
    if (!ok) return -1;
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
    hex[64] = 0;
    g_calculs++;
    return 0;
}

/* Une ligne du cache : « sha \t taille \t s.ns \t chemin ». 1 = analysée. */
static int cache_ligne(char *l, char **sha, long long *taille, long long *s, long *ns, char **chemin)
{
    l[strcspn(l, "\n")] = 0;
    char *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL, *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
    if (!t3 || t1 - l != 64) return 0;
    *t1 = *t2 = *t3 = 0;
    char *fin;
    *taille = strtoll(t1 + 1, &fin, 10);
    if (*fin) return 0;
    *s = strtoll(t2 + 1, &fin, 10);
    if (*fin != '.') return 0;
    *ns = strtol(fin + 1, &fin, 10);
    if (*fin) return 0;
    *sha = l;
    *chemin = t3 + 1;
    return 1;
}

static int cache_lire(const char *support, const char *chemin, const struct stat *st, char hex[65])
{
    char p[1200];
    if (!support || !*support || fmt_chemin(p, sizeof p, "%s/verifications.txt", support)) return 0;
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char l[1400];
    int trouve = 0;
    while (fgets(l, sizeof l, f)) {
        char *sha, *ch;
        long long taille, s;
        long ns;
        if (!cache_ligne(l, &sha, &taille, &s, &ns, &ch)) continue;
        if (!strcmp(ch, chemin) && taille == (long long)st->st_size
            && s == (long long)st->st_mtimespec.tv_sec && ns == st->st_mtimespec.tv_nsec) {
            memcpy(hex, sha, 64);
            hex[64] = 0;
            trouve = 1;
        }
    }
    fclose(f);
    return trouve;
}

/* Remplace l'entrée de chemin (32 entrées au plus, les plus récentes), .tmp puis rename. Échec : journal seul. */
static void cache_ecrire(const char *support, const char *chemin, const struct stat *st, const char *hex)
{
    enum { NMAX = 32 };
    char p[1200], tmp[1220];
    if (!support || !*support || fmt_chemin(p, sizeof p, "%s/verifications.txt", support)
        || fmt_chemin(tmp, sizeof tmp, "%s.tmp", p)) return;
    static char gardees[NMAX][1400];
    int ng = 0;
    FILE *f = fopen(p, "r");
    if (f) {
        char l[1400], c[1400];
        while (fgets(l, sizeof l, f)) {
            memcpy(c, l, sizeof c);
            char *sha, *ch;
            long long taille, s;
            long ns;
            if (!cache_ligne(c, &sha, &taille, &s, &ns, &ch) || !strcmp(ch, chemin)) continue;
            if (ng == NMAX - 1) { memmove(gardees[0], gardees[1], sizeof gardees[0] * (NMAX - 2)); ng--; }
            l[strcspn(l, "\n")] = 0;
            snprintf(gardees[ng++], sizeof gardees[0], "%s", l);
        }
        fclose(f);
    }
    mkdir_p(support);
    FILE *g = fopen(tmp, "w");
    if (!g) { jrn("[MAC] cache des vérifications %s : %s", tmp, strerror(errno)); return; }
    for (int i = 0; i < ng; i++) fprintf(g, "%s\n", gardees[i]);
    fprintf(g, "%s\t%lld\t%lld.%09ld\t%s\n", hex, (long long)st->st_size, (long long)st->st_mtimespec.tv_sec,
            (long)st->st_mtimespec.tv_nsec, chemin);
    int ok = !ferror(g);
    if (fclose(g)) ok = 0;
    if (!ok || rename(tmp, p)) { jrn("[MAC] cache des vérifications %s non écrit : %s", p, strerror(errno)); unlink(tmp); }
}

/* sha256 d'un fichier, par le cache si taille et mtime tiennent. source : « cache » ou « calculé en N ms ».
 * 0 = connu, -1 = illisible. */
static int sha_avec_cache(const char *chemin, const struct stat *st, const char *support, char hex[65],
                          char *source, size_t cap)
{
    if (cache_lire(support, chemin, st, hex)) { snprintf(source, cap, "cache"); return 0; }
    double t0 = maintenant();
    if (sc2m_sha256_fichier(chemin, hex)) return -1;
    snprintf(source, cap, "calculé en %.0f ms", (maintenant() - t0) * 1000.0);
    cache_ecrire(support, chemin, st, hex);
    return 0;
}

int sc2m_install_valider(const char *install, const char *support, char *motif, size_t cap)
{
    char exe[1100];
    if (!install || !*install) { motif_poser(motif, cap, "aucune install désignée"); return 1; }
    if (fmt_chemin(exe, sizeof exe, "%s/%s", install, SC2M_EXE_RELATIF)) {
        motif_poser(motif, cap, "chemin de l'install trop long");
        return 1;
    }
    struct stat st;
    if (stat(exe, &st) || !S_ISREG(st.st_mode)) {
        motif_poser(motif, cap, "SC2.exe absent : %s", exe);
        return 1;
    }
    if ((long long)st.st_size != SC2M_EXE_TAILLE) {
        motif_poser(motif, cap, "SC2.exe de %lld o au lieu de %lld o : %s", (long long)st.st_size, SC2M_EXE_TAILLE, exe);
        jrn("[MAC] %s", motif);
        return 2;
    }
    char hex[65], source[64];
    if (sha_avec_cache(exe, &st, support, hex, source, sizeof source)) {
        motif_poser(motif, cap, "SC2.exe illisible : %s (%s)", exe, strerror(errno));
        jrn("[MAC] %s", motif);
        return 1;
    }
    if (strcmp(hex, g_ref_exe)) {
        motif_poser(motif, cap, "SC2.exe de sha256 %.8s… au lieu de %.8s… : %s", hex, g_ref_exe, exe);
        jrn("[MAC] SC2.exe %lld o, sha256 %.8s… NON conforme (référence %.8s…, %s) : %s",
            (long long)st.st_size, hex, g_ref_exe, source, exe);
        return 2;
    }
    jrn("[MAC] SC2.exe %lld o, sha256 %.8s… conforme (%s) : %s", (long long)st.st_size, hex, source, exe);
    return 0;
}

/* ---- effacement de sc2-c ------------------------------------------------------------------ */

static int g_rm_echecs;

static int rm_un(const char *p, const struct stat *st, int type, struct FTW *f)
{
    (void)st; (void)f;
    /* FTW_PHYS : un lien est FTW_SL (jamais suivi) et part par unlink ; un dossier, vidé d'abord (FTW_DEPTH). */
    int r = (type == FTW_DP || type == FTW_DNR) ? rmdir(p) : unlink(p);
    if (r && errno != ENOENT) g_rm_echecs++;
    return 0;
}

int sc2m_effacer_arbre(const char *racine_brute)
{
    if (!racine_brute || racine_brute[0] != '/') { errno = EINVAL; return -1; }
    /* lstat("…/lien/") et lstat("…/lien/.") résolvent le lien (S_ISDIR vrai : nftw viderait la cible), et « // »
     * passerait pour un dossier quelconque. Les « / » finaux sont donc retirés, et un reste vide (« / »), ou fini par
     * « . » ou « .. », est refusé : le dernier composant est alors un vrai nom, que lstat ne suit pas. */
    char racine[PATH_MAX];
    size_t n = strlen(racine_brute);
    if (n >= sizeof racine) { errno = ENAMETOOLONG; return -1; }
    memcpy(racine, racine_brute, n + 1);
    while (n > 0 && racine[n - 1] == '/') racine[--n] = 0;
    const char *dernier = strrchr(racine, '/');
    dernier = dernier ? dernier + 1 : racine;
    if (!n || !strcmp(dernier, ".") || !strcmp(dernier, "..")) { errno = EINVAL; return -1; }
    struct stat st;
    if (lstat(racine, &st)) return errno == ENOENT ? 0 : -1;
    /* Mesuré le 22/10 (macOS 27) : nftw(FTW_PHYS) SUIT une racine qui est un lien (il vide la cible, puis échoue sur
     * le lien). Les liens sous la racine, eux, ne sont pas suivis. Une racine qui n'est pas un vrai dossier part donc
     * par unlink, sans parcours. */
    if (!S_ISDIR(st.st_mode)) return unlink(racine) && errno != ENOENT ? 1 : 0;
    g_rm_echecs = 0;
    if (nftw(racine, rm_un, 32, FTW_DEPTH | FTW_PHYS)) return -1;
    return g_rm_echecs;
}

/* ---- Variables.txt ------------------------------------------------------------------------ */

/* Vrai si le fichier (≤ 64 Kio) contient « GraphicsOption » : écarte un fichier quelconque (sauver_vars d'Android). */
static int contient_graphicsoption(const char *chemin)
{
    char *b = malloc(VARS_MAX + 1);
    FILE *f = b ? fopen(chemin, "rb") : NULL;
    if (!f) { free(b); return 0; }
    size_t n = fread(b, 1, VARS_MAX, f);
    fclose(f);
    b[n] = 0;
    int r = strstr(b, "GraphicsOption") != NULL;
    free(b);
    return r;
}

static int vars_acceptable(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISREG(st.st_mode) && st.st_size >= 1 && st.st_size <= VARS_MAX;
}

/* Avant l'effacement de sc2-c : les réglages changés en jeu au lancement précédent → <support>/Variables.txt, par
 * .tmp puis rename. Un refus est journalisé et n'arrête pas le lancement (sauver_vars, sc2_android_main.c:255). */
static void sauver_vars(const char *src, const char *dst)
{
    const char *fige = getenv("SC2_VARIABLES_FIGE");
    if (fige && !strcmp(fige, "1")) {
        jrn("[VARS] SC2_VARIABLES_FIGE=1 : réglages du jeu NON sauvegardés (préréglage de %s figé)", dst);
        return;
    }
    struct stat st;
    if (stat(src, &st) || !S_ISREG(st.st_mode)) { jrn("[VARS] pas de réglages à sauvegarder (%s absent)", src); return; }
    if (st.st_size < 1 || st.st_size > VARS_MAX) {
        jrn("[VARS] réglages NON sauvegardés (%s : %lld o)", src, (long long)st.st_size);
        return;
    }
    if (!contient_graphicsoption(src)) { jrn("[VARS] réglages NON sauvegardés (%s sans « GraphicsOption »)", src); return; }
    char tmp[1200];
    if (fmt_chemin(tmp, sizeof tmp, "%s.tmp", dst)) return;
    if (copier_fichier(src, tmp) || rename(tmp, dst)) {
        jrn("[VARS] sauvegarde des réglages en échec : %s → %s (%s)", src, dst, strerror(errno));
        unlink(tmp);
        return;
    }
    jrn("[VARS] réglages du jeu sauvegardés → %s (%lld o)", dst, (long long)st.st_size);
}

/* ---- dossiers, journal, verrou ------------------------------------------------------------ */

int sc2m_dossiers_defaut(sc2m_dossiers *d, const char *paquet)
{
    memset(d, 0, sizeof *d);
    const char *home = getenv("HOME");
    if (!home || !*home) { struct passwd *pw = getpwuid(getuid()); home = pw && pw->pw_dir && *pw->pw_dir ? pw->pw_dir : NULL; }
    const char *s = getenv("SC2_MAC_SUPPORT");
    if (s && *s) {
        if (s[0] == '/') { if (fmt_chemin(d->support, sizeof d->support, "%s", s)) return -1; }
        else {
            char cwd[1024];
            if (!getcwd(cwd, sizeof cwd) || fmt_chemin(d->support, sizeof d->support, "%s/%s", cwd, s)) return -1;
        }
        size_t n = strlen(d->support);
        while (n > 1 && d->support[n - 1] == '/') d->support[--n] = 0;
        /* essai isolé : le journal par défaut suit le dossier de support, le vrai journal n'est jamais tourné */
        if (fmt_chemin(d->journaux, sizeof d->journaux, "%s/Logs", d->support)) return -1;
    } else {
        if (!home) return -1;
        if (fmt_chemin(d->support, sizeof d->support, "%s/Library/Application Support/SC2Partout", home)
            || fmt_chemin(d->journaux, sizeof d->journaux, "%s/Library/Logs/SC2Partout", home)) return -1;
    }
    if (paquet && *paquet) {
        char p[1100];
        if (!fmt_chemin(p, sizeof p, "%s/Contents/Resources", paquet) && est_dossier(p))
            snprintf(d->ressources, sizeof d->ressources, "%s", p);
        if (!fmt_chemin(p, sizeof p, "%s/Contents/Frameworks", paquet) && est_dossier(p))
            snprintf(d->frameworks, sizeof d->frameworks, "%s", p);
    }
    return 0;
}

const char *sc2m_journal_chemin(const sc2m_dossiers *d, char *o, size_t cap)
{
    const char *j = getenv("SC2_MAC_JOURNAL");
    if (j && *j) snprintf(o, cap, "%s", j);
    else if (fmt_chemin(o, cap, "%s/sc2-partout.log", d->journaux)) snprintf(o, cap, "-");
    return o;
}

int sc2m_journal_ouvrir(const char *chemin)
{
    if (!chemin || !*chemin || !strcmp(chemin, "-")) return 0;
    char ancien[1200];
    if (fmt_chemin(ancien, sizeof ancien, "%s.1", chemin)) { jrn("[MAC] journal : chemin trop long"); return -1; }
    mkdir_parent(chemin);
    int tourne = !rename(chemin, ancien);
    int fd = open(chemin, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (fd < 0) { jrn("[MAC] journal %s : %s — sortie standard conservée", chemin, strerror(errno)); return -1; }
    fflush(stdout);
    fflush(stderr);
    if (dup2(fd, 1) < 0 || dup2(fd, 2) < 0) {
        int e = errno;
        if (fd > 2) close(fd);
        jrn("[MAC] journal %s : dup2 : %s", chemin, strerror(e));
        return -1;
    }
    if (fd > 2) close(fd);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (tourne) jrn("[MAC] journal : %s (précédent : %s)", chemin, ancien);
    else jrn("[MAC] journal : %s", chemin);
    return 0;
}

static int g_fd_instance = -1;

static long pid_lu(int fd)
{
    char b[32];
    ssize_t k = pread(fd, b, sizeof b - 1, 0);
    if (k <= 0) return 0;
    b[k] = 0;
    return strtol(b, NULL, 10);
}

int sc2m_verrou_instance(const char *support, int attente_s)
{
    if (g_fd_instance >= 0) return 0;
    char p[1100];
    if (!support || !*support || fmt_chemin(p, sizeof p, "%s/.instance", support)) return -1;
    mkdir_p(support);
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { jrn("[MAC] verrou d'instance %s : %s", p, strerror(errno)); return -1; }
    double t0 = maintenant();
    int attendu = 0;
    for (;;) {
        if (lseek(fd, 0, SEEK_SET) == 0 && lockf(fd, F_TLOCK, 0) == 0) break;
        if (errno != EAGAIN && errno != EACCES) {
            jrn("[MAC] verrou d'instance %s : %s", p, strerror(errno));
            close(fd);
            return -1;
        }
        if (!attendu && attente_s > 0)
            jrn("[MAC] verrou d'instance %s tenu par le pid %ld : attente (au plus %d s)", p, pid_lu(fd), attente_s);
        attendu = 1;
        if (maintenant() - t0 >= attente_s) {
            jrn("[MAC] une autre instance (pid %ld) tient %s depuis plus de %d s : abandon", pid_lu(fd), p, attente_s);
            close(fd);
            return 1;
        }
        struct timespec z = { 0, 50 * 1000 * 1000 };
        nanosleep(&z, NULL);
    }
    char b[32];
    int n = snprintf(b, sizeof b, "%ld\n", (long)getpid());
    if (ftruncate(fd, 0) || pwrite(fd, b, (size_t)n, 0) != n) jrn("[MAC] verrou d'instance %s : pid non écrit (%s)", p, strerror(errno));
    g_fd_instance = fd;
    if (attendu) jrn("[MAC] instance précédente terminée après %.1f s", maintenant() - t0);
    else jrn("[MAC] verrou d'instance tenu : %s (pid %ld)", p, (long)getpid());
    return 0;
}

/* ---- configurer() ------------------------------------------------------------------------- */

/* Défaut : posé seulement si la variable manque (une valeur vide est une valeur). Ajouté à la liste du journal. */
static void defaut(const char *nom, const char *val, char *poses, size_t cap)
{
    if (getenv(nom) || !val) return;
    setenv(nom, val, 1);
    size_t n = strlen(poses);
    snprintf(poses + n, cap - n, "%s%s", n ? " " : "", nom);
}

/* Pack SPIR-V : choisi comme le runtime le chercherait (sc2_spv_pack.c:98-110), contrôlé, journalisé. */
static void pack_spirv(const char *install, const char *support)
{
    char p[1100];
    const char *e = getenv("SC2_SPIRV_PACK");
    if (e && *e) {
        if (fmt_chemin(p, sizeof p, "%s", e)) return;
        if (!est_fichier(p)) {
            jrn("[MAC] pack SPIR-V absent : %s (SC2_SPIRV_PACK) : des shaders peuvent manquer", p);
            return;
        }
    } else {
        char cote[1100], ici[1100];
        const char *s = strrchr(install, '/');   /* même calcul que chemin_env() du runtime */
        if (!s || fmt_chemin(cote, sizeof cote, "%.*s/sc2-spirv.pak", (int)(s - install), install)) cote[0] = 0;
        if (fmt_chemin(ici, sizeof ici, "%s/sc2-spirv.pak", support)) ici[0] = 0;
        if (cote[0] && est_fichier(cote)) snprintf(p, sizeof p, "%s", cote);         /* trouvé seul par le runtime */
        else if (ici[0] && est_fichier(ici)) { snprintf(p, sizeof p, "%s", ici); setenv("SC2_SPIRV_PACK", ici, 1); }
        else {
            jrn("[MAC] pack SPIR-V absent (ni %s ni %s) : des shaders peuvent manquer", cote[0] ? cote : "(?)", ici);
            return;
        }
    }
    struct stat st;
    char hex[65], source[64];
    if (stat(p, &st) || sha_avec_cache(p, &st, support, hex, source, sizeof source)) {
        jrn("[MAC] pack SPIR-V : %s illisible (%s)", p, strerror(errno));
        return;
    }
    if (!strcmp(hex, SC2M_PACK_SHA256)) jrn("[MAC] pack SPIR-V : %s conforme (sha256 %.8s…, %s)", p, hex, source);
    else jrn("[MAC] pack SPIR-V : %s NON conforme (sha256 %.8s…, référence %.8s…, %s) : utilisé tel quel", p, hex,
             SC2M_PACK_SHA256, source);
}

int sc2m_configurer(const sc2m_dossiers *d, sc2m_config *c)
{
    memset(c, 0, sizeof *c);
    const char *S = d->support;
    if (S[0] != '/' || !S[1]) {
        motif_poser(c->motif, sizeof c->motif, "dossier de support « %s » : chemin absolu attendu", S);
        jrn("[MAC] configuration : %s", c->motif);
        return -1;
    }
    if (mkdir_p(S)) {
        motif_poser(c->motif, sizeof c->motif, "création de %s : %s", S, strerror(errno));
        jrn("[MAC] configuration : %s", c->motif);
        return -1;
    }
    char p[1200], vars_src[1300], vars_dst[1200], racine[1200], q[1300];
    if (fmt_chemin(p, sizeof p, "%s/sc2.env", S) || fmt_chemin(vars_dst, sizeof vars_dst, "%s/Variables.txt", S)
        || fmt_chemin(racine, sizeof racine, "%s/sc2-c", S)
        || fmt_chemin(vars_src, sizeof vars_src, "%s/%s/Variables.txt", racine, DOCS_SC2)) {
        motif_poser(c->motif, sizeof c->motif, "chemin du dossier de support trop long");
        return -1;
    }

    /* 1. sc2.env : l'environnement du processus l'emporte */
    sc2m_env_lire(p);

    /* 2. install */
    const char *e = getenv("SC2_INSTALL");
    int trop = 0;
    if (e && *e && e[0] == '/') trop = fmt_chemin(c->install, sizeof c->install, "%s", e);
    else if (e && *e) trop = fmt_chemin(c->install, sizeof c->install, "%s/%s", S, e);
    else trop = fmt_chemin(c->install, sizeof c->install, "%s/install", S);
    if (trop || fmt_chemin(c->exe, sizeof c->exe, "%s/%s", c->install, SC2M_EXE_RELATIF)) {
        motif_poser(c->motif, sizeof c->motif, "chemin de l'install trop long");
        return -1;
    }
    if (sc2m_install_valider(c->install, S, c->motif, sizeof c->motif)) {
        c->install_a_choisir = 1;
        jrn("[MAC] install à choisir : %s", c->motif);
    } else setenv("SC2_INSTALL", c->install, 1);

    /* 3. réglages du jeu sauvés AVANT l'effacement */
    sauver_vars(vars_src, vars_dst);

    /* 4. C:\ neuf à chaque lancement (passe 6a) ; SC2_ROOT imposé : seul <support>/sc2-c est jamais effacé */
    int k = sc2m_effacer_arbre(racine);
    if (k) jrn("[MAC] %s : effacement incomplet (%d entrée(s) restée(s))", racine, k);
    e = getenv("SC2_ROOT");
    if (e && strcmp(e, racine)) jrn("[MAC] SC2_ROOT=%s ignorée : le C:\\ de l'app est %s", e, racine);
    setenv("SC2_ROOT", racine, 1);
    e = getenv("SC2_VARIABLES");
    if (e) jrn("[MAC] Variables.txt : SC2_VARIABLES=%s (environnement ou sc2.env)", e);
    else if (vars_acceptable(vars_dst)) {
        setenv("SC2_VARIABLES", vars_dst, 1);
        jrn("[MAC] Variables.txt : SC2_VARIABLES=%s (réglages du lancement précédent)", vars_dst);
    } else jrn("[MAC] Variables.txt : aucun dans %s (réglages par défaut du jeu)", S);
#ifndef SC2_VARIABLES_MAX
    jrn("[MAC] Variables.txt non semé (V1.5 absente)");
#endif

    /* 5. défauts */
    char poses[512] = "";
    if (!fmt_chemin(q, sizeof q, "%s/StarCraft II", S)) defaut("SC2_DOCUMENTS", q, poses, sizeof poses);
    if (!fmt_chemin(q, sizeof q, "%s/vk-pipelines.bin", S)) defaut("SC2_VK_CACHE_PIPELINES", q, poses, sizeof poses);
    defaut("SC2_FRAMES", "0", poses, sizeof poses);
    if (!fmt_chemin(q, sizeof q, "%s/frames", S)) defaut("SC2_FRAMES_DIR", q, poses, sizeof poses);
    defaut("SC2_SWAPCHAIN", "1", poses, sizeof poses);
    defaut("SC2_ECRAN", "auto", poses, sizeof poses);
    defaut("SC2_RUN_SANS_BNET", "1", poses, sizeof poses);
    defaut("SC2_MANQUE_CONTINUE", "1", poses, sizeof poses);
    if (!c->install_a_choisir) {
        defaut("SC2_EXE", c->exe, poses, sizeof poses);
        e = getenv("SC2_EXE");
        if (e && strcmp(e, c->exe))
            jrn("[MAC] SC2_EXE=%s ≠ %s : le jeu reçoit l'exécutable contrôlé de l'install (argv[1])", e, c->exe);
    }
    char pol[1100] = "", ini[1100] = "";
    if (d->ressources[0]) {
        if (!fmt_chemin(q, sizeof q, "%s/polices", d->ressources) && est_dossier(q)) snprintf(pol, sizeof pol, "%s", q);
        if (!fmt_chemin(q, sizeof q, "%s/inits-stricts.txt", d->ressources) && est_fichier(q)) snprintf(ini, sizeof ini, "%s", q);
    }
#ifdef SC2_MAC_DEPOT
    else {   /* hors paquet (développement) : le dépôt désigné à la compilation */
        if (!fmt_chemin(q, sizeof q, "%s/android/assets/polices", SC2_MAC_DEPOT) && est_dossier(q)) snprintf(pol, sizeof pol, "%s", q);
        if (!fmt_chemin(q, sizeof q, "%s/recomp-sc2/inits-stricts.txt", SC2_MAC_DEPOT) && est_fichier(q)) snprintf(ini, sizeof ini, "%s", q);
    }
#endif
    if (pol[0]) defaut("SC2_POLICES", pol, poses, sizeof poses);
    else if (!getenv("SC2_POLICES"))
        jrn("[MAC] polices introuvables (%s/polices) : SC2_POLICES non posée — le lanceur plante vers la frame 40",
            d->ressources[0] ? d->ressources : "(hors paquet)");
    if (ini[0]) defaut("SC2_STRICT_INITS", ini, poses, sizeof poses);

    /* 6. MoltenVK embarqué */
    if (d->frameworks[0] && !fmt_chemin(q, sizeof q, "%s/libMoltenVK.dylib", d->frameworks) && est_fichier(q))
        defaut("SC2_VK_LIB", q, poses, sizeof poses);
    if (poses[0]) jrn("[MAC] défauts posés : %s", poses);

    /* 7. pack SPIR-V */
    if (!c->install_a_choisir) pack_spirv(c->install, S);

    /* 8. dossier courant et résumé */
    if (chdir(S)) {
        motif_poser(c->motif, sizeof c->motif, "chdir(%s) : %s", S, strerror(errno));
        jrn("[MAC] configuration : %s", c->motif);
        return -1;
    }
    jrn("[MAC] SC2_INSTALL=%s SC2_ROOT=%s SC2_DOCUMENTS=%s SC2_FRAMES_DIR=%s SC2_ECRAN=%s SC2_VK_LIB=%s SC2_POLICES=%s "
        "SC2_STRICT_INITS=%s SC2_VARIABLES=%s", c->install_a_choisir ? "(à choisir)" : c->install, ou_aucun("SC2_ROOT"),
        ou_aucun("SC2_DOCUMENTS"), ou_aucun("SC2_FRAMES_DIR"), ou_aucun("SC2_ECRAN"), ou_aucun("SC2_VK_LIB"),
        ou_aucun("SC2_POLICES"), ou_aucun("SC2_STRICT_INITS"), ou_aucun("SC2_VARIABLES"));
    return c->install_a_choisir ? 1 : 0;
}
