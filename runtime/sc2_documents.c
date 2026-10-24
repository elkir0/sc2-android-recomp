/* sc2_documents.c — passe 10t : progression de campagne et sauvegardes persistantes.
 *
 * Le jeu écrit sa progression dans C:\Users\sc2\Documents\StarCraft II\Banks\ (banques
 * XML, ex. TCampaign.SC2Bank) et ses sauvegardes dans …\Saves\ (ex. Saves/Unsaved/
 * Liberation Day Victory.SC2Save). Ces dossiers vivent dans le C:\ émulé ($SC2_ROOT,
 * « sc2-c ») qu'Android efface à chaque lancement (passe 6a) : la progression serait
 * perdue. SC2_DOCUMENTS désigne un dossier hôte persistant : au démarrage, avant tout
 * code du jeu, Banks et Saves deviennent des liens symboliques vers $SC2_DOCUMENTS/
 * Banks et /Saves — les écritures du jeu atterrissent dans le persistant, et
 * l'effacement de sc2-c (nftw FTW_PHYS, qui ne suit pas les liens) retire les liens,
 * pas les cibles. Un vrai dossier préexistant (banc Mac : sc2-c conservé d'un run à
 * l'autre) est d'abord FUSIONNÉ dans le persistant, fichier par fichier, sans écraser
 * un fichier persistant plus récent (mtime : le plus récent gagne, chaque reprise
 * journalisée), puis le dossier est retiré et le lien posé. Sans SC2_DOCUMENTS :
 * rien ne change (le point d'appel ne nous invoque même pas).
 *
 * V1.5 (22/10) : Variables.txt (réglages graphiques et sonores du jeu, même dossier
 * StarCraft II\) n'est PAS un lien : c'est un fichier semé au lancement. Le semis
 * vivait côté Android (semer_vars de sc2_android_main.c) ; il est ici pour servir aussi
 * au banc Mac : SC2_VARIABLES=<fichier> le copie s'il est absent, SC2_VARIABLES_FIGE=1
 * le réécrit à chaque lancement (préréglages de scripts/prereglages/, canon épinglé).
 * Le jeu RE-DÉTECTE ses réglages (et écrase ceux semés) quand lastDeviceId manque ou n'est pas le
 * DeviceId de la carte annoncée (constaté le 22/10 : préréglages sans la clé → Medium, textures Low,
 * pour les 4) : le semis ALIGNE la clé sur la carte annoncée (sc2_variables_device_id) — ajoutée si elle
 * manque (préréglages), remplacée si elle diffère (Variables.txt écrit par le jeu sous un autre profil, ex.
 * celui de /sdcard du Fold, écrit sous la 8600 GT : 1026 → 1507 en dx10) : une source semée l'est pour être
 * appliquée, pas re-détectée.
 * La sauvegarde des réglages changés en jeu (sauver_vars) reste propre à Android.
 */
#define _GNU_SOURCE
#include "sc2_documents.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* sc2_log (sc2_host.h), déclaré sans tirer le modèle CPU : ce module n'est que
 * filesystem, et le test le lie sans le reste du runtime. */
void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* timespec d'accès/de modification : st_atim/st_mtim (POSIX 2008) sauf macOS (st_*timespec). */
#ifdef __APPLE__
#define DOCS_ATIM(stv) (stv).st_atimespec
#define DOCS_MTIM(stv) (stv).st_mtimespec
#else
#define DOCS_ATIM(stv) (stv).st_atim
#define DOCS_MTIM(stv) (stv).st_mtim
#endif

static const char *const docs_sous[] = { "Banks", "Saves" };
/* $SC2_ROOT/Users/sc2/Documents/StarCraft II — là où le jeu lit et écrit Banks/,
 * Saves/ et Variables.txt (cf. hpath() de sc2_iat.c, semer_vars() côté Android). */
static const char docs_dossier[] = "Users/sc2/Documents/StarCraft II";

/* mkdir récursif (chaque composant en 0755, EEXIST toléré). 0 = OK, -1 = échec. */
static int docs_mkdir_p(const char *chemin)
{
    char t[1024];
    if (snprintf(t, sizeof t, "%s", chemin) >= (int)sizeof t) return -1;
    for (char *q = t + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
    return mkdir(t, 0755) && errno != EEXIST ? -1 : 0;
}

static int docs_rm_un(const char *p, const struct stat *st, int type, struct FTW *f)
{ (void)st; (void)type; (void)f; return remove(p); }

/* Copie src → dst (dst créé/écrasé), mtime de src conservée : les fusions suivantes
 * comparent un historique fidèle. 0 = OK, -1 = échec (raison chez l'appelant). */
static int docs_copier(const char *src, const char *dst, const struct stat *st)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    int ok = out != NULL;
    char b[8192]; size_t k;
    while (ok && (k = fread(b, 1, sizeof b, in)) > 0) ok = fwrite(b, 1, k, out) == k;
    ok = ok && !ferror(in);
    if (out && fclose(out)) ok = 0;                    /* fermé même après un échec d'écriture */
    fclose(in);
    if (ok) {
        struct timespec ts[2] = { DOCS_ATIM((*st)), DOCS_MTIM((*st)) };
        utimensat(AT_FDCWD, dst, ts, 0);
    }
    return ok ? 0 : -1;
}

/* Fusionne le vrai dossier local src dans dst (persistant) : un fichier absent du
 * persistant, ou local plus récent, est repris (journalisé) ; un fichier persistant
 * plus récent ou aussi récent est conservé tel quel. 0 = OK, -1 = au moins un échec. */
static int docs_fusionner(const char *src, const char *dst)
{
    DIR *d = opendir(src);
    if (!d) { sc2_log("[DOCS] lecture de %s impossible (%s)", src, strerror(errno)); return -1; }
    struct dirent *e;
    int echecs = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[1024], dd[1024];
        if (snprintf(s, sizeof s, "%s/%s", src, e->d_name) >= (int)sizeof s
            || snprintf(dd, sizeof dd, "%s/%s", dst, e->d_name) >= (int)sizeof dd) continue;
        struct stat st;
        if (lstat(s, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            mkdir(dd, 0755);
            echecs += docs_fusionner(s, dd) != 0;
        } else if (S_ISLNK(st.st_mode)) {              /* rare dans une banque : recréé tel quel */
            char c[1024];
            ssize_t n = readlink(s, c, sizeof c - 1);
            if (n > 0) { c[n] = 0; unlink(dd); if (symlink(c, dd)) echecs++; }
        } else if (S_ISREG(st.st_mode)) {
            struct stat dp;
            if (!lstat(dd, &dp) && S_ISREG(dp.st_mode) && DOCS_MTIM(dp).tv_sec >= DOCS_MTIM(st).tv_sec) {
                sc2_log("[DOCS] %s : fichier persistant plus récent conservé", dd);
                continue;
            }
            if (docs_copier(s, dd, &st)) {
                sc2_log("[DOCS] reprise de %s impossible (%s)", s, strerror(errno));
                echecs++;
            } else
                sc2_log("[DOCS] %s repris dans le persistant", s);
        }
    }
    closedir(d);
    return echecs ? -1 : 0;
}

int sc2_documents_preparer(const char *racine, const char *persistant)
{
    sc2_log("[DOCS] progression et sauvegardes persistantes : %s (Banks, Saves)", persistant);
    if (docs_mkdir_p(persistant)) {
        sc2_log("[DOCS] création du persistant %s impossible (%s) — poursuite SANS persistance",
                persistant, strerror(errno));
        return -1;
    }
    int echecs = 0;
    for (size_t i = 0; i < sizeof docs_sous / sizeof docs_sous[0]; i++) {
        const char *sous = docs_sous[i];
        char cible[1024], locale[1150], parent[1150], abs[PATH_MAX];   /* realpath : PATH_MAX (FORTIFY Android) */
        if (snprintf(cible, sizeof cible, "%s/%s", persistant, sous) >= (int)sizeof cible
            || snprintf(locale, sizeof locale, "%s/%s/%s", racine, docs_dossier, sous) >= (int)sizeof locale
            || snprintf(parent, sizeof parent, "%s/%s", racine, docs_dossier) >= (int)sizeof parent) {
            sc2_log("[DOCS] chemin trop long pour %s", sous);
            echecs++;
            continue;
        }
        if (docs_mkdir_p(cible) || !realpath(cible, abs)) {
            sc2_log("[DOCS] création du persistant %s impossible (%s)", cible, strerror(errno));
            echecs++;
            continue;
        }
        docs_mkdir_p(parent);                          /* StarCraft II\ doit exister pour y poser le lien */
        struct stat st;
        if (!lstat(locale, &st)) {
            if (S_ISLNK(st.st_mode)) {
                char cur[PATH_MAX];
                ssize_t n = readlink(locale, cur, sizeof cur - 1);
                if (n > 0) {
                    cur[n] = 0;
                    if (!strcmp(cur, abs)) continue;   /* déjà le bon lien : rien à faire */
                }
                unlink(locale);                        /* lien vers ailleurs : remplacé */
            } else if (S_ISDIR(st.st_mode)) {
                /* Vrai dossier (ancien sc2-c non effacé, ex. banc Mac) : fusionné, puis retiré
                 * par le même nftw que l'effacement Android — les cibles, elles, restent. */
                sc2_log("[DOCS] %s est un vrai dossier : fusion dans %s, puis lien", locale, cible);
                echecs += docs_fusionner(locale, cible) != 0;
                nftw(locale, docs_rm_un, 16, FTW_DEPTH | FTW_PHYS);
            } else
                unlink(locale);                        /* fichier isolé à la place du dossier : remplacé */
        }
        if (symlink(abs, locale)) {
            sc2_log("[DOCS] pose du lien %s → %s impossible (%s) — poursuite SANS persistance",
                    locale, abs, strerror(errno));
            echecs++;
        }
    }
    return echecs ? -1 : 0;
}

/* ---- V1.5 : Variables.txt ------------------------------------------------------------------ */

static int g_vars_fait;                                /* un semis a été tenté dans ce processus */

/* DeviceId de la carte annoncée au jeu. Définition FAIBLE, copie de la table de sc2_d3d9.c (profil « dx10 » →
 * 0x05E3, tout le reste → 0x0402 historique), qui sert au test hôte et tant que sc2_d3d9.c ne l'expose pas : une
 * définition forte « unsigned sc2_gpu_device_id(void) { return gpu_profil_courant()->device; } » dans sc2_d3d9.c
 * (fusion de caps-etats) la remplace à l'édition de liens, et la table n'existe plus qu'à un endroit. */
__attribute__((weak)) unsigned sc2_gpu_device_id(void)
{
    const char *p = getenv("SC2_GPU_PROFIL");
    return p && !strcmp(p, "dx10") ? 0x05E3u : 0x0402u;
}

unsigned sc2_variables_device_id(void) { return sc2_gpu_device_id(); }

/* Écrit le texte (n octets) dans f, lastDeviceId ALIGNÉ sur dev : chaque ligne « lastDeviceId=… » (en début de texte
 * ou après un \n ; « xlastDeviceId » n'en est pas une) reçoit dev, sa fin de ligne d'origine gardée ; sans la clé,
 * « lastDeviceId=<dev> » ajoutée en fin (CRLF, comme les écrit le jeu). *etat : 0 = déjà alignée (texte écrit tel
 * quel), 1 = ajoutée, 2 = remplacée (avant = première valeur d'origine). *ecrit = octets écrits. 1 = OK, 0 = échec. */
static int vars_ecrire(FILE *f, const char *t, size_t n, unsigned dev, int *etat, char *avant, size_t navant,
                       size_t *ecrit)
{
    static const char cle[] = "lastDeviceId=";
    const size_t nc = sizeof cle - 1;
    char val[16], ajout[48];
    size_t nv = (size_t)snprintf(val, sizeof val, "%u", dev), deb = 0, tot = 0;
    int vue = 0, remplacee = 0;
    avant[0] = 0;
    for (size_t i = 0; i + nc <= n; i++) {
        if ((i && t[i - 1] != '\n') || memcmp(t + i, cle, nc)) continue;
        size_t a = i + nc, b = a;
        while (b < n && t[b] != '\r' && t[b] != '\n') b++;
        if (!vue) snprintf(avant, navant, "%.*s", (int)(b - a), t + a);
        vue = 1;
        if (b - a == nv && !memcmp(t + a, val, nv)) continue;          /* déjà la valeur de la carte annoncée */
        remplacee = 1;
        if (fwrite(t + deb, 1, a - deb, f) != a - deb || fwrite(val, 1, nv, f) != nv) return 0;
        tot += a - deb + nv;
        deb = b;                                                       /* reprise à la fin de ligne d'origine */
    }
    if (fwrite(t + deb, 1, n - deb, f) != n - deb) return 0;
    tot += n - deb;
    if (!vue) {
        size_t na = (size_t)snprintf(ajout, sizeof ajout, "%slastDeviceId=%s\r\n", t[n - 1] == '\n' ? "" : "\r\n", val);
        if (fwrite(ajout, 1, na, f) != na) return 0;
        tot += na;
    }
    *etat = !vue ? 1 : remplacee ? 2 : 0;
    *ecrit = tot;
    return 1;
}

/* Écrit Variables.txt depuis le texte (n octets) : présent et non figé → conservé ; sinon .tmp puis rename (un
 * Variables.txt à moitié écrit ne doit jamais être lu), lastDeviceId aligné sur la carte annoncée (vars_ecrire). */
static int vars_poser(const char *racine, const char *texte, size_t n, const char *origine, int fige)
{
    char d[1024], v[1100], tmp[1120], avant[24];
    if (snprintf(d, sizeof d, "%s/%s", racine, docs_dossier) >= (int)sizeof d
        || snprintf(v, sizeof v, "%s/Variables.txt", d) >= (int)sizeof v
        || snprintf(tmp, sizeof tmp, "%s.tmp", v) >= (int)sizeof tmp) {
        sc2_log("[VARS] chemin trop long sous %s : Variables.txt NON semé", racine);
        return -1;
    }
    struct stat st;
    if (!fige && !lstat(v, &st)) {
        sc2_log("[VARS] %s présent : conservé (SC2_VARIABLES_FIGE=1 pour le réécrire à chaque lancement)", v);
        return 0;
    }
    unsigned dev = sc2_variables_device_id();
    int etat = 0;
    size_t ecrit = 0;
    int ok = !docs_mkdir_p(d);
    if (ok) {
        FILE *f = fopen(tmp, "wb");
        ok = f && vars_ecrire(f, texte, n, dev, &etat, avant, sizeof avant, &ecrit);
        if (f && fclose(f)) ok = 0;
    }
    if (!ok || rename(tmp, v)) {
        sc2_log("[VARS] semis de %s → %s en échec (%s)", origine, v, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (etat == 2)
        sc2_log("[VARS] lastDeviceId %s → %u : réglages conservés (sinon le jeu les re-détecte pour la carte annoncée)",
                avant[0] ? avant : "(vide)", dev);
    sc2_log("[VARS] Variables.txt semé depuis %s (%zu o%s%s)", origine, ecrit,
            etat == 1 ? ", lastDeviceId de la carte annoncée ajouté" : "", fige ? ", figé : réécrit à chaque lancement" : "");
    return 1;
}

int sc2_variables_semer(const char *racine, const char *source, int fige)
{
    g_vars_fait = 1;
    static char buf[SC2_VARIABLES_MAX + 1];
    size_t n = 0;                                      /* lu : n > 64 Kio ⇔ source trop grande */
    struct stat st;
    FILE *f;
    if (source && *source && !stat(source, &st) && S_ISREG(st.st_mode) && (f = fopen(source, "rb"))) {
        n = fread(buf, 1, sizeof buf, f);
        fclose(f);
    }
    if (n < 1 || n > SC2_VARIABLES_MAX) {
        sc2_log("[VARS] %s refusé (absent, pas un fichier ordinaire, ou hors de 1 o à 64 Kio) : réglages par défaut "
                "du jeu", source && *source ? source : "(source vide)");
        return -1;
    }
    return vars_poser(racine, buf, n, source, fige);
}

int sc2_variables_semer_texte(const char *racine, const char *texte, const char *origine, int fige)
{
    g_vars_fait = 1;
    size_t n = texte ? strlen(texte) : 0;
    if (n < 1 || n > SC2_VARIABLES_MAX) {
        sc2_log("[VARS] %s refusé (%zu o, hors de 1 o à 64 Kio) : réglages par défaut du jeu", origine, n);
        return -1;
    }
    return vars_poser(racine, texte, n, origine, fige);
}

int sc2_variables_depuis_env(const char *racine)
{
    if (g_vars_fait) return 0;                         /* l'hôte (Android) a déjà semé : même ordre qu'avant */
    const char *src = getenv("SC2_VARIABLES"), *f = getenv("SC2_VARIABLES_FIGE");
    int fige = f && !strcmp(f, "1");
    if (!src || !*src || !strcmp(src, "0")) {
        if (fige) sc2_log("[VARS] SC2_VARIABLES_FIGE=1 sans SC2_VARIABLES : rien à semer (réglages du jeu)");
        return 0;
    }
    return sc2_variables_semer(racine, src, fige);
}
