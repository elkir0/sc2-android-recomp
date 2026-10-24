/* test_config_mac.c — V3, lot M3 (22/10/2026) : configuration de l'app macOS en C pur (mac/sc2_config_mac.c).
 *
 * Tout se joue dans des dossiers mkdtemp (le dossier de support contient une espace, comme « Application Support »).
 * Les sept cas de la critique §3.4 M3 :
 *   1. l'environnement l'emporte sur sc2.env (lecture seule, puis par sc2m_configurer, au-dessus des défauts) ;
 *   2. la fusion de sc2.env ne réécrit que ses clés (.tmp puis rename : nouvel inode, l'ancien fichier intact) ;
 *   3. sc2-c est effacé sans suivre les liens Banks, Saves, polices ni un lien de dossier ; Variables.txt sauvé avant ;
 *   4. les défauts ne sont posés que s'ils manquent (une valeur vide reste vide) ;
 *   5. SC2_VK_LIB n'est posé que si <frameworks>/libMoltenVK.dylib existe ;
 *   6. sha256 : un SC2.exe creux de 16 449 536 o est refusé, la référence injectée est acceptée par le cache relu
 *      (aucun nouveau calcul), une mtime changée invalide le cache, une mauvaise taille est refusée sans calcul ;
 *   7. verrou d'instance (processus fils : les verrous POSIX sont par processus) : un second preneur attend puis
 *      échoue au délai, un troisième attend et l'obtient quand le premier sort.
 * En plus : install à choisir puis second appel, pack SPIR-V, journal (dup2, rotation), dossiers par défaut.
 * Sortie : une ligne PASS/FAIL par contrôle, « RÉSULTAT : N PASS, M FAIL », code 0 sans échec.
 */
#include "sc2_config_mac.h"
#include "sc2_documents.h"   /* SC2_VARIABLES_MAX défini = V1.5 présente (même règle que le module) */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) { g_pass++; printf("PASS  "); } else { g_fail++; printf("FAIL  "); } \
                              printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

/* sha256 de 16 449 536 octets nuls (head -c 16449536 /dev/zero | shasum -a 256) et de « abc » (FIPS 180-2). */
#define SHA_ZEROS "3cb263957bc7d6c6ea6c67f35b78305b28a538b275134074346605742ce94f87"
#define SHA_ABC   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

static char g_base[512];

/* ---- outils ------------------------------------------------------------------------------- */

static void creer_dossier(const char *chemin)
{
    char t[1400];
    snprintf(t, sizeof t, "%s", chemin);
    for (char *q = t + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
    mkdir(t, 0755);
}

static void parent(const char *f)
{
    char d[1400];
    snprintf(d, sizeof d, "%s", f);
    char *s = strrchr(d, '/');
    if (s) { *s = 0; creer_dossier(d); }
}

static void ecrire(const char *p, const char *contenu)
{
    parent(p);
    FILE *f = fopen(p, "wb");
    if (!f) { printf("      (échec d'écriture %s : %s)\n", p, strerror(errno)); return; }
    fputs(contenu, f);
    fclose(f);
}

static char *lire(const char *p, char *o, size_t cap)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    size_t n = fread(o, 1, cap - 1, f);
    fclose(f);
    o[n] = 0;
    return o;
}

static int existe_l(const char *p) { struct stat st; return !lstat(p, &st); }

static int egal_env(const char *nom, const char *attendu)
{
    const char *v = getenv(nom);
    if (!attendu) return v == NULL;
    return v && !strcmp(v, attendu);
}

/* chemin sous la base : chemin(o, "eff/support") */
static char *chemin(char *o, size_t cap, const char *rel) { snprintf(o, cap, "%s/%s", g_base, rel); return o; }

/* SC2.exe creux de la taille demandée sous <install>/Versions/Base15405/ */
static void exe_creux(const char *install, long long taille)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", install, SC2M_EXE_RELATIF);
    parent(p);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, taille)) printf("      (fichier creux %s : %s)\n", p, strerror(errno));
    if (fd >= 0) close(fd);
}

static const char *const VARIABLES[] = {
    "SC2_INSTALL", "SC2_EXE", "SC2_ROOT", "SC2_DOCUMENTS", "SC2_VARIABLES", "SC2_VARIABLES_FIGE",
    "SC2_VK_CACHE_PIPELINES", "SC2_FRAMES", "SC2_FRAMES_DIR", "SC2_SWAPCHAIN", "SC2_ECRAN", "SC2_POLICES",
    "SC2_STRICT_INITS", "SC2_RUN_SANS_BNET", "SC2_MANQUE_CONTINUE", "SC2_VK_LIB", "SC2_SPIRV_PACK", "SC2_MAC_SUPPORT",
    "SC2_MAC_JOURNAL", "SC2_ARGS", "SC2_TEST_ENV", "SC2_TEST_EGAL", "SC2_TEST_BLANC", "SC2_TEST_VIDE", "SC2_TEST_COMMENTE",
};
static void env_propre(void) { for (size_t i = 0; i < sizeof VARIABLES / sizeof *VARIABLES; i++) unsetenv(VARIABLES[i]); }

static void dossiers(sc2m_dossiers *d, const char *support, const char *ressources, const char *frameworks)
{
    memset(d, 0, sizeof *d);
    snprintf(d->support, sizeof d->support, "%s", support);
    snprintf(d->journaux, sizeof d->journaux, "%s/Logs", support);
    if (ressources) snprintf(d->ressources, sizeof d->ressources, "%s", ressources);
    if (frameworks) snprintf(d->frameworks, sizeof d->frameworks, "%s", frameworks);
}

/* Capture de stdout (le journal du module) dans un fichier, puis recopie en retrait. */
static int g_sauve = -1;
static char g_cap[1400];
static void capture_debut(void)
{
    fflush(stdout);
    chemin(g_cap, sizeof g_cap, "capture.txt");
    int fd = open(g_cap, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    g_sauve = dup(1);
    dup2(fd, 1);
    close(fd);
}
static char *capture_fin(char *o, size_t cap)
{
    fflush(stdout);
    dup2(g_sauve, 1);
    close(g_sauve);
    if (!lire(g_cap, o, cap)) o[0] = 0;
    for (char *l = o, *nl; *l; l = nl + 1) {
        nl = strchr(l, '\n');
        if (!nl) { printf("      %s\n", l); break; }
        printf("      %.*s\n", (int)(nl - l), l);
    }
    return o;
}

static int configurer(const sc2m_dossiers *d, sc2m_config *c, char *journal, size_t cap)
{
    capture_debut();
    int r = sc2m_configurer(d, c);
    capture_fin(journal, cap);
    return r;
}

static long ms_depuis(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)((t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000);
}

static void dormir_ms(long ms) { struct timespec z = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&z, NULL); }

/* ---- 1. l'environnement l'emporte sur sc2.env -------------------------------------------- */

static void cas_env(void)
{
    printf("--- 1. environnement > sc2.env > défauts\n");
    env_propre();
    char S[1400], f[1500], j[8192];
    chemin(S, sizeof S, "Application Support/env");
    snprintf(f, sizeof f, "%s/sc2.env", S);
    ecrire(f, "# commentaire\nSC2_TEST_ENV=fichier\nSC2_TEST_EGAL=a=b c\r\n  \tSC2_TEST_BLANC=x\nmauvaise ligne\n"
              "1ABC=z\n#SC2_TEST_COMMENTE=1\n\nSC2_TEST_VIDE=\n");
    setenv("SC2_TEST_ENV", "environnement", 1);
    capture_debut();
    int n = sc2m_env_lire(f);
    capture_fin(j, sizeof j);
    CHECK(n == 3, "sc2m_env_lire : 3 variables posées (obtenu %d)", n);
    CHECK(egal_env("SC2_TEST_ENV", "environnement"), "variable déjà dans l'environnement : non remplacée par sc2.env");
    CHECK(egal_env("SC2_TEST_EGAL", "a=b c"), "« = » et espace dans la valeur, CR final retiré (obtenu « %s »)",
          getenv("SC2_TEST_EGAL") ? getenv("SC2_TEST_EGAL") : "(nul)");
    CHECK(egal_env("SC2_TEST_BLANC", "x"), "blancs en tête de ligne ignorés");
    CHECK(egal_env("SC2_TEST_VIDE", ""), "valeur vide posée vide");
    CHECK(egal_env("SC2_TEST_COMMENTE", NULL) && !getenv("1ABC"), "commentaire et nom invalide ignorés");
    CHECK(strstr(j, "ignorée (l'environnement l'emporte") != NULL, "journal : priorité de l'environnement dite");
    snprintf(f, sizeof f, "%s/absent.env", S);
    CHECK(sc2m_env_lire(f) == 0, "sc2.env absent : 0");

    /* par l'entrée réelle : environnement > sc2.env > défaut */
    env_propre();
    chemin(S, sizeof S, "Application Support/env2");
    snprintf(f, sizeof f, "%s/sc2.env", S);
    ecrire(f, "SC2_ECRAN=1024x768\nSC2_FRAMES=0\n");
    setenv("SC2_FRAMES", "1", 1);
    sc2m_dossiers d;
    sc2m_config c;
    dossiers(&d, S, NULL, NULL);
    int r = configurer(&d, &c, j, sizeof j);
    CHECK(r == 1 && c.install_a_choisir, "sans install : rend 1, install à choisir (obtenu %d)", r);
    CHECK(egal_env("SC2_ECRAN", "1024x768"), "sc2.env l'emporte sur le défaut SC2_ECRAN=auto");
    CHECK(egal_env("SC2_FRAMES", "1"), "l'environnement (SC2_FRAMES=1) l'emporte sur sc2.env (0) et sur le défaut");
}

/* ---- 2. fusion de sc2.env ----------------------------------------------------------------- */

static void cas_fusion(void)
{
    printf("--- 2. fusion de sc2.env : seules ses clés changent, .tmp puis rename\n");
    env_propre();
    char S[1400], f[1500], tmp[1600], o[4096];
    chemin(S, sizeof S, "Application Support/fusion");
    snprintf(f, sizeof f, "%s/sc2.env", S);
    snprintf(tmp, sizeof tmp, "%s.tmp", f);
    const char *avant = "# réglages\nSC2_ECRAN=1280x720\n\nSC2_ARGS=-run Maps/Campaign/TStory01.SC2Map\n"
                        "#SC2_ARGS=commentée\nSC2_RETIREE=1\n  SC2_ARGS=doublon\nSC2_GARDEE=a=b\r\nsans-egal\n";
    const char *apres = "# réglages\nSC2_ECRAN=1280x720\n\nSC2_ARGS=-run Maps/Campaign/TRaynor01.SC2Map\n"
                        "#SC2_ARGS=commentée\nSC2_GARDEE=a=b\r\nsans-egal\nSC2_NOUVELLE=1\n";
    ecrire(f, avant);
    struct stat s0, s1;
    stat(f, &s0);
    int fd_ancien = open(f, O_RDONLY);
    const char *cles[] = { "SC2_ARGS", "SC2_RETIREE", "SC2_NOUVELLE", "SC2_ARGS" };
    const char *vals[] = { "-run Maps/Campaign/TRaynor01.SC2Map", NULL, "1", "ignorée (doublon de l'appel)" };
    int r = sc2m_env_ecrire(f, cles, vals, 4);
    CHECK(r == 0, "sc2m_env_ecrire rend 0 (obtenu %d)", r);
    CHECK(lire(f, o, sizeof o) && !strcmp(o, apres), "contenu fusionné exact (clé remplacée en place, doublon et clé "
          "retirée supprimés, nouvelle clé à la fin, le reste intact)");
    if (strcmp(o, apres)) printf("      obtenu :\n%s", o);
    stat(f, &s1);
    CHECK(s1.st_ino != s0.st_ino, "nouvel inode : écrit à côté puis renommé (inode %llu → %llu)",
          (unsigned long long)s0.st_ino, (unsigned long long)s1.st_ino);
    char b[4096] = "";
    ssize_t k = fd_ancien >= 0 ? pread(fd_ancien, b, sizeof b - 1, 0) : -1;
    if (k >= 0) b[k] = 0;
    CHECK(k > 0 && !strcmp(b, avant), "l'ancien fichier n'a jamais été réécrit en place");
    if (fd_ancien >= 0) close(fd_ancien);
    CHECK(!existe_l(tmp), "aucun .tmp laissé");
    const char *mauv[] = { "SC2 ARGS" }, *v1[] = { "x" };
    const char *bonne[] = { "SC2_ARGS" }, *v2[] = { "a\nSC2_INJECTEE=1" };
    CHECK(sc2m_env_ecrire(f, mauv, v1, 1) == -1 && sc2m_env_ecrire(f, bonne, v2, 1) == -1
          && lire(f, o, sizeof o) && !strcmp(o, apres), "clé invalide ou valeur à saut de ligne : refus, fichier intact");
    snprintf(f, sizeof f, "%s/neuf/sc2.env", S);
    const char *v3[] = { "-run Maps/Campaign/TRaynor01.SC2Map" };
    CHECK(sc2m_env_ecrire(f, bonne, v3, 1) == 0 && lire(f, o, sizeof o)
          && !strcmp(o, "SC2_ARGS=-run Maps/Campaign/TRaynor01.SC2Map\n"), "fichier absent : créé avec la clé");
    /* aller-retour : ce que la fusion écrit, sc2m_env_lire le relit */
    unsetenv("SC2_ARGS");
    sc2m_env_lire(f);
    CHECK(egal_env("SC2_ARGS", "-run Maps/Campaign/TRaynor01.SC2Map"), "aller-retour écriture → lecture");
    unsetenv("SC2_ARGS");
}

/* ---- 3. effacement de sc2-c sans suivre les liens, Variables.txt sauvé -------------------- */

static void cas_effacement(void)
{
    printf("--- 3. sc2-c effacé sans suivre les liens ; Variables.txt sauvé avant, SC2_VARIABLES posé après\n");
    env_propre();
    char S[1400], docs[1500], p[1600], q[1600], ext[1400], o[8192], j[16384];
    chemin(S, sizeof S, "Application Support/eff");
    chemin(ext, sizeof ext, "eff-ext/polices");
    snprintf(p, sizeof p, "%s/StarCraft II/Banks/TCampaign.SC2Bank", S);              ecrire(p, "progression");
    snprintf(p, sizeof p, "%s/StarCraft II/Saves/Unsaved/Liberation Day.SC2Save", S); ecrire(p, "sauvegarde");
    snprintf(p, sizeof p, "%s/LiberationSans-Regular.ttf", ext);                     ecrire(p, "police");
    snprintf(docs, sizeof docs, "%s/sc2-c/Users/sc2/Documents/StarCraft II", S);
    creer_dossier(docs);
    snprintf(p, sizeof p, "%s/Banks", docs); snprintf(q, sizeof q, "%s/StarCraft II/Banks", S); symlink(q, p);
    snprintf(p, sizeof p, "%s/Saves", docs); snprintf(q, sizeof q, "%s/StarCraft II/Saves", S); symlink(q, p);
    snprintf(p, sizeof p, "%s/Variables.txt", docs); ecrire(p, "[GraphicsOption]\nx=1\n");
    snprintf(p, sizeof p, "%s/sc2-c/Windows/Fonts/arial.ttf", S); parent(p);
    snprintf(q, sizeof q, "%s/LiberationSans-Regular.ttf", ext); symlink(q, p);
    snprintf(p, sizeof p, "%s/sc2-c/lien-dossier", S); symlink(ext, p);
    snprintf(p, sizeof p, "%s/sc2-c/ProgramData/Blizzard/runtimeCache~01.bin", S); ecrire(p, "cache");

    sc2m_dossiers d;
    sc2m_config c;
    dossiers(&d, S, NULL, NULL);
    configurer(&d, &c, j, sizeof j);
    snprintf(p, sizeof p, "%s/sc2-c", S);
    CHECK(!existe_l(p), "sc2-c effacé");
    CHECK(egal_env("SC2_ROOT", p), "SC2_ROOT=<support>/sc2-c");
    snprintf(p, sizeof p, "%s/StarCraft II/Banks/TCampaign.SC2Bank", S);
    CHECK(lire(p, o, sizeof o) && !strcmp(o, "progression"), "cible du lien Banks intacte");
    snprintf(p, sizeof p, "%s/StarCraft II/Saves/Unsaved/Liberation Day.SC2Save", S);
    CHECK(lire(p, o, sizeof o) && !strcmp(o, "sauvegarde"), "cible du lien Saves intacte");
    snprintf(p, sizeof p, "%s/LiberationSans-Regular.ttf", ext);
    CHECK(lire(p, o, sizeof o) && !strcmp(o, "police"), "cible des liens de police et de dossier intacte");
    snprintf(p, sizeof p, "%s/Variables.txt", S);
    CHECK(lire(p, o, sizeof o) && !strcmp(o, "[GraphicsOption]\nx=1\n"), "Variables.txt sauvé AVANT l'effacement");
    CHECK(egal_env("SC2_VARIABLES", p), "SC2_VARIABLES=<support>/Variables.txt (semé par le runtime de V1.5)");
#ifdef SC2_VARIABLES_MAX
    CHECK(strstr(j, "non semé (V1.5 absente)") == NULL, "V1.5 présente : pas de « non semé »");
#else
    CHECK(strstr(j, "[MAC] Variables.txt non semé (V1.5 absente)") != NULL, "V1.5 absente : « [MAC] Variables.txt non "
          "semé (V1.5 absente) » au journal");
#endif

    /* SC2_VARIABLES_FIGE=1 : la sauvegarde ne remplace pas le préréglage ; SC2_VARIABLES de l'environnement gardé */
    env_propre();
    snprintf(p, sizeof p, "%s/Variables.txt", docs); ecrire(p, "[GraphicsOption]\nx=2\n");
    setenv("SC2_VARIABLES_FIGE", "1", 1);
    setenv("SC2_VARIABLES", "/ailleurs/Variables.txt", 1);
    configurer(&d, &c, j, sizeof j);
    snprintf(p, sizeof p, "%s/Variables.txt", S);
    CHECK(lire(p, o, sizeof o) && !strcmp(o, "[GraphicsOption]\nx=1\n"), "SC2_VARIABLES_FIGE=1 : préréglage non remplacé");
    CHECK(egal_env("SC2_VARIABLES", "/ailleurs/Variables.txt"), "SC2_VARIABLES de l'environnement conservée");
    /* fichier sans « GraphicsOption » : non sauvé */
    env_propre();
    snprintf(p, sizeof p, "%s/Variables.txt", docs); ecrire(p, "n'importe quoi\n");
    configurer(&d, &c, j, sizeof j);
    snprintf(p, sizeof p, "%s/Variables.txt", S);
    CHECK(lire(p, o, sizeof o) && !strcmp(o, "[GraphicsOption]\nx=1\n"), "Variables.txt sans « GraphicsOption » : non sauvé");

    /* racine qui est elle-même un lien : le lien part, la cible reste ; refus des racines dangereuses */
    char lien[1400], cible[1400];
    chemin(lien, sizeof lien, "eff-lien/sc2-c");
    chemin(cible, sizeof cible, "eff-cible");
    snprintf(p, sizeof p, "%s/garde.txt", cible); ecrire(p, "garde");
    parent(lien); symlink(cible, lien);
    int r = sc2m_effacer_arbre(lien);
    CHECK(r == 0 && !existe_l(lien) && lire(p, o, sizeof o) && !strcmp(o, "garde"), "racine en lien : lien retiré, cible intacte");
    /* « lien/ » et « lien/. » : lstat suivrait le lien ; la racine est normalisée avant, la cible reste */
    char lr[1500];
    symlink(cible, lien);
    snprintf(lr, sizeof lr, "%s//", lien);
    r = sc2m_effacer_arbre(lr);
    CHECK(r == 0 && !existe_l(lien) && lire(p, o, sizeof o) && !strcmp(o, "garde"), "racine « lien// » : lien retiré, cible intacte");
    symlink(cible, lien);
    snprintf(lr, sizeof lr, "%s/", lien);
    r = sc2m_effacer_arbre(lr);
    CHECK(r == 0 && !existe_l(lien) && lire(p, o, sizeof o) && !strcmp(o, "garde"), "racine « lien/ » : lien retiré, cible intacte");
    symlink(cible, lien);
    snprintf(lr, sizeof lr, "%s/.", lien);
    r = sc2m_effacer_arbre(lr);
    CHECK(r == -1 && existe_l(lien) && lire(p, o, sizeof o) && !strcmp(o, "garde"), "racine « lien/. » : refus, lien et cible intacts");
    snprintf(lr, sizeof lr, "%s/./", lien);
    int r2 = sc2m_effacer_arbre(lr);
    snprintf(lr, sizeof lr, "%s/..", lien);
    int r3 = sc2m_effacer_arbre(lr);
    CHECK(r2 == -1 && r3 == -1 && existe_l(lien) && lire(p, o, sizeof o) && !strcmp(o, "garde"),
          "racine « lien/./ » ou « lien/.. » : refus, lien et cible intacts");
    /* un vrai dossier donné avec « / » final : effacé comme sans */
    snprintf(lr, sizeof lr, "%s/eff-dossier/sous/f.txt", g_base); ecrire(lr, "x");
    snprintf(lr, sizeof lr, "%s/eff-dossier/", g_base);
    r = sc2m_effacer_arbre(lr);
    CHECK(r == 0 && !existe_l(lr), "dossier « eff-dossier/ » : effacé");
    CHECK(sc2m_effacer_arbre("relatif/sc2-c") == -1 && sc2m_effacer_arbre("/") == -1 && sc2m_effacer_arbre("") == -1 &&
          sc2m_effacer_arbre("//") == -1 && sc2m_effacer_arbre("///") == -1 && sc2m_effacer_arbre("/.") == -1 &&
          sc2m_effacer_arbre("/..") == -1, "racine relative, « / », « // », « /. », « /.. » ou vide : refus");
    snprintf(p, sizeof p, "%s/n-existe-pas", g_base);
    CHECK(sc2m_effacer_arbre(p) == 0, "racine absente : 0");
}

/* ---- 4. défauts posés seulement s'ils manquent ; install à choisir puis second appel ------ */

static void cas_defauts(void)
{
    printf("--- 4. défauts posés seulement s'ils manquent\n");
    env_propre();
    char S[1400], R[1400], I[1400], p[1600], exe[1600], j[16384];
    chemin(S, sizeof S, "Application Support/defauts");
    chemin(R, sizeof R, "SC2Partout.app/Contents/Resources");
    chemin(I, sizeof I, "defauts-install/install");
    snprintf(p, sizeof p, "%s/polices/LiberationSans-Regular.ttf", R); ecrire(p, "police");
    snprintf(p, sizeof p, "%s/inits-stricts.txt", R);                  ecrire(p, "0x013C74C0\n");
    exe_creux(I, SC2M_EXE_TAILLE);
    snprintf(exe, sizeof exe, "%s/%s", I, SC2M_EXE_RELATIF);
    sc2m_install_reference(SHA_ZEROS);
    setenv("SC2_INSTALL", I, 1);
    setenv("SC2_ECRAN", "1280x720", 1);
    setenv("SC2_FRAMES", "1", 1);
    setenv("SC2_DOCUMENTS", "", 1);
    setenv("SC2_MANQUE_CONTINUE", "0", 1);
    sc2m_dossiers d;
    sc2m_config c;
    dossiers(&d, S, R, NULL);
    int r = configurer(&d, &c, j, sizeof j);
    CHECK(r == 0 && !c.install_a_choisir && !strcmp(c.exe, exe), "install conforme : rend 0, c.exe = %s", c.exe);
    CHECK(egal_env("SC2_INSTALL", I), "SC2_INSTALL gardée");
    CHECK(egal_env("SC2_ECRAN", "1280x720"), "SC2_ECRAN de l'environnement gardée (défaut auto non posé)");
    CHECK(egal_env("SC2_FRAMES", "1"), "SC2_FRAMES=1 gardée (défaut 0 non posé)");
    CHECK(egal_env("SC2_DOCUMENTS", ""), "SC2_DOCUMENTS vide gardée vide (pas de persistance voulue)");
    CHECK(egal_env("SC2_MANQUE_CONTINUE", "0"), "SC2_MANQUE_CONTINUE=0 gardée");
    snprintf(p, sizeof p, "%s/vk-pipelines.bin", S);
    CHECK(egal_env("SC2_VK_CACHE_PIPELINES", p), "défaut SC2_VK_CACHE_PIPELINES=<support>/vk-pipelines.bin");
    snprintf(p, sizeof p, "%s/frames", S);
    CHECK(egal_env("SC2_FRAMES_DIR", p), "défaut SC2_FRAMES_DIR=<support>/frames");
    CHECK(egal_env("SC2_SWAPCHAIN", "1"), "défaut SC2_SWAPCHAIN=1");
    CHECK(egal_env("SC2_RUN_SANS_BNET", "1"), "défaut SC2_RUN_SANS_BNET=1");
    CHECK(egal_env("SC2_EXE", exe), "défaut SC2_EXE=<install>/Versions/Base15405/SC2.exe");
    snprintf(p, sizeof p, "%s/polices", R);
    CHECK(egal_env("SC2_POLICES", p), "défaut SC2_POLICES=<ressources>/polices");
    snprintf(p, sizeof p, "%s/inits-stricts.txt", R);
    CHECK(egal_env("SC2_STRICT_INITS", p), "défaut SC2_STRICT_INITS=<ressources>/inits-stricts.txt");
    char cwd[1400] = "", rs[1400] = "";
    CHECK(getcwd(cwd, sizeof cwd) && realpath(S, rs) && !strcmp(cwd, rs), "chdir(<support>)");
    CHECK(strstr(j, "[MAC] défauts posés : ") != NULL && strstr(j, "[MAC] SC2_INSTALL=") != NULL, "journal : défauts et résumé");

    /* défauts absents tous, ressources sans polices, pas d'install : rien qui dépende de l'install */
    env_propre();
    chemin(S, sizeof S, "Application Support/defauts2");
    chemin(R, sizeof R, "Vide.app/Contents/Resources");
    creer_dossier(R);
    dossiers(&d, S, R, NULL);
    r = configurer(&d, &c, j, sizeof j);
    snprintf(p, sizeof p, "%s/StarCraft II", S);
    CHECK(r == 1 && c.install_a_choisir && strstr(c.motif, "SC2.exe absent") != NULL, "sans install : rend 1, motif "
          "« %s »", c.motif);
    CHECK(egal_env("SC2_DOCUMENTS", p) && egal_env("SC2_FRAMES", "0") && egal_env("SC2_ECRAN", "auto")
          && egal_env("SC2_MANQUE_CONTINUE", "1"), "défauts SC2_DOCUMENTS, SC2_FRAMES=0, SC2_ECRAN=auto, SC2_MANQUE_CONTINUE=1");
    CHECK(egal_env("SC2_EXE", NULL) && egal_env("SC2_INSTALL", NULL) && egal_env("SC2_SPIRV_PACK", NULL),
          "install à choisir : ni SC2_EXE, ni SC2_INSTALL, ni pack");
    CHECK(egal_env("SC2_POLICES", NULL) && egal_env("SC2_STRICT_INITS", NULL) && strstr(j, "polices introuvables"),
          "ressources sans polices : SC2_POLICES non posée, et dit");
    /* l'hôte choisit l'install (NSOpenPanel), la pose, puis rappelle : second appel complet */
    setenv("SC2_INSTALL", I, 1);
    r = configurer(&d, &c, j, sizeof j);
    CHECK(r == 0 && !c.install_a_choisir && egal_env("SC2_EXE", exe), "second appel après le choix : rend 0, SC2_EXE posée");
    sc2m_install_reference(NULL);
}

/* ---- 5. SC2_VK_LIB seulement si le fichier existe ---------------------------------------- */

static void cas_vk_lib(void)
{
    printf("--- 5. SC2_VK_LIB=<frameworks>/libMoltenVK.dylib seulement si le fichier existe\n");
    env_propre();
    char S[1400], F[1400], p[1600], j[16384];
    chemin(S, sizeof S, "Application Support/vk");
    chemin(F, sizeof F, "SC2Partout.app/Contents/Frameworks");
    creer_dossier(F);
    sc2m_dossiers d;
    sc2m_config c;
    dossiers(&d, S, NULL, F);
    configurer(&d, &c, j, sizeof j);
    CHECK(egal_env("SC2_VK_LIB", NULL), "Frameworks sans dylib : SC2_VK_LIB non posée");
    snprintf(p, sizeof p, "%s/libMoltenVK.dylib", F);
    ecrire(p, "dylib");
    env_propre();
    configurer(&d, &c, j, sizeof j);
    CHECK(egal_env("SC2_VK_LIB", p), "dylib présente : SC2_VK_LIB=%s", p);
    env_propre();
    setenv("SC2_VK_LIB", "/opt/ailleurs/libvulkan.1.dylib", 1);
    configurer(&d, &c, j, sizeof j);
    CHECK(egal_env("SC2_VK_LIB", "/opt/ailleurs/libvulkan.1.dylib"), "SC2_VK_LIB de l'environnement gardée");
    env_propre();
    dossiers(&d, S, NULL, NULL);
    configurer(&d, &c, j, sizeof j);
    CHECK(egal_env("SC2_VK_LIB", NULL), "hors paquet (frameworks vide) : SC2_VK_LIB non posée");
}

/* ---- 6. sha256 de SC2.exe et cache des vérifications ------------------------------------- */

static void cas_sha256(void)
{
    printf("--- 6. sha256 : fichier creux refusé, référence injectée acceptée par le cache relu\n");
    env_propre();
    char S[1400], I[1400], p[1600], o[8192], m[256], j[8192];
    chemin(p, sizeof p, "sha/abc.txt");
    ecrire(p, "abc");
    char hex[65] = "";
    CHECK(sc2m_sha256_fichier(p, hex) == 0 && !strcmp(hex, SHA_ABC), "sha256(« abc ») = ba7816bf… (obtenu %.8s…)", hex);
    chemin(S, sizeof S, "sha/Application Support");
    chemin(I, sizeof I, "sha/install");
    exe_creux(I, SC2M_EXE_TAILLE);
    sc2m_install_reference(NULL);
    int n0 = sc2m_sha_calculs();
    capture_debut();
    int r = sc2m_install_valider(I, S, m, sizeof m);
    capture_fin(j, sizeof j);
    CHECK(r == 2 && strstr(m, "sha256") && sc2m_sha_calculs() == n0 + 1, "SC2.exe creux de 16 449 536 o : refusé (rend %d, "
          "motif « %s »), 1 calcul", r, m);
    char refus[200];   /* public : la référence est celle donnée au build (SC2M_EXE_SHA256), pas une empreinte du jeu */
    snprintf(refus, sizeof refus, "[MAC] SC2.exe 16449536 o, sha256 3cb26395… NON conforme (référence %.8s…", SC2M_EXE_SHA256);
    CHECK(strstr(j, refus) != NULL, "journal du refus");
    CHECK(sc2m_install_reference("pas-un-sha") == -1, "référence invalide refusée");
    CHECK(sc2m_install_reference(SHA_ZEROS) == 0, "référence injectée");
    capture_debut();
    r = sc2m_install_valider(I, S, m, sizeof m);
    capture_fin(j, sizeof j);
    CHECK(r == 0 && sc2m_sha_calculs() == n0 + 1, "référence injectée : accepté, SANS nouveau calcul (cache relu)");
    CHECK(strstr(j, "[MAC] SC2.exe 16449536 o, sha256 3cb26395… conforme (cache)") != NULL, "journal : « … conforme (cache) »");
    snprintf(p, sizeof p, "%s/verifications.txt", S);
    char exe[1600];
    snprintf(exe, sizeof exe, "%s/%s", I, SC2M_EXE_RELATIF);
    CHECK(lire(p, o, sizeof o) && strstr(o, SHA_ZEROS "\t16449536\t") && strstr(o, exe), "verifications.txt : sha, taille, chemin");
    struct timespec ts[2] = { { 1700000000, 5 }, { 1700000000, 5 } };
    utimensat(AT_FDCWD, exe, ts, 0);
    r = sc2m_install_valider(I, S, m, sizeof m);
    CHECK(r == 0 && sc2m_sha_calculs() == n0 + 2, "mtime changée : cache invalidé, recalcul (%d calculs)", sc2m_sha_calculs() - n0);
    r = sc2m_install_valider(I, S, m, sizeof m);
    CHECK(r == 0 && sc2m_sha_calculs() == n0 + 2, "puis relu du cache");
    r = sc2m_install_valider(I, NULL, m, sizeof m);
    CHECK(r == 0 && sc2m_sha_calculs() == n0 + 3, "sans dossier de support : pas de cache, calcul");
    CHECK(sc2m_install_reference("3CB263957BC7D6C6EA6C67F35B78305B28A538B275134074346605742CE94F87") == 0
          && sc2m_install_valider(I, S, m, sizeof m) == 0, "référence en majuscules acceptée");
    sc2m_install_reference(NULL);
    r = sc2m_install_valider(I, S, m, sizeof m);
    CHECK(r == 2 && sc2m_sha_calculs() == n0 + 3, "référence d'origine rétablie : refusé, depuis le cache");
    char I2[1400];
    chemin(I2, sizeof I2, "sha/install-long");
    exe_creux(I2, SC2M_EXE_TAILLE + 1);
    r = sc2m_install_valider(I2, S, m, sizeof m);
    CHECK(r == 2 && strstr(m, "16449537") && sc2m_sha_calculs() == n0 + 3, "16 449 537 o : refusé sans calcul (« %s »)", m);
    chemin(I2, sizeof I2, "sha/install-absente");
    r = sc2m_install_valider(I2, S, m, sizeof m);
    CHECK(r == 1 && strstr(m, "SC2.exe absent"), "install absente : rend 1 (« %s »)", m);
}

/* ---- 7. verrou d'instance ----------------------------------------------------------------- */

static void cas_verrou(void)
{
    printf("--- 7. verrou d'instance : un second preneur attend puis échoue au délai\n");
    env_propre();
    char S[1400], p[1600], o[64];
    chemin(S, sizeof S, "Application Support/verrou");
    int pa[2], pc[2], pb[2];
    if (pipe(pa) || pipe(pc) || pipe(pb)) { CHECK(0, "pipe : %s", strerror(errno)); return; }
    fflush(stdout);
    pid_t a = fork();
    if (a == 0) {                                     /* A : tient le verrou 3 s */
        int r = sc2m_verrou_instance(S, 0);
        char ok = r == 0 ? 'A' : 'x';
        if (write(pa[1], &ok, 1) != 1) _exit(8);
        dormir_ms(3000);
        _exit(r == 0 ? 0 : 9);
    }
    char ok = 0;
    if (read(pa[0], &ok, 1) != 1) ok = 0;
    CHECK(ok == 'A', "premier preneur : verrou tenu");
    fflush(stdout);
    pid_t cc = fork();
    if (cc == 0) {                                    /* C : attend 1 s au plus, doit échouer */
        struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = sc2m_verrou_instance(S, 1);
        long dt = ms_depuis(&t0);
        if (write(pc[1], &dt, sizeof dt) != (ssize_t)sizeof dt) _exit(8);
        _exit(r == 1 ? 1 : 7);
    }
    fflush(stdout);
    pid_t b = fork();
    if (b == 0) {                                     /* B : attend 10 s au plus, l'obtient quand A sort */
        struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = sc2m_verrou_instance(S, 10);
        long dt = ms_depuis(&t0);
        int r2 = sc2m_verrou_instance(S, 0);          /* réentrant */
        if (write(pb[1], &dt, sizeof dt) != (ssize_t)sizeof dt) _exit(8);
        _exit(r == 0 && r2 == 0 ? 0 : 1);
    }
    int sa = -1, sc = -1, sb = -1;
    long dtc = -1, dtb = -1;
    waitpid(a, &sa, 0);
    waitpid(cc, &sc, 0);
    waitpid(b, &sb, 0);
    if (read(pc[0], &dtc, sizeof dtc) != (ssize_t)sizeof dtc) dtc = -1;
    if (read(pb[0], &dtb, sizeof dtb) != (ssize_t)sizeof dtb) dtb = -1;
    for (int i = 0; i < 2; i++) { close(pa[i]); close(pc[i]); close(pb[i]); }
    CHECK(WIFEXITED(sa) && WEXITSTATUS(sa) == 0, "premier preneur sorti normalement");
    CHECK(WIFEXITED(sc) && WEXITSTATUS(sc) == 1 && dtc >= 950 && dtc < 2900, "second preneur : a attendu puis a échoué "
          "au délai de 1 s (rend 1 après %ld ms)", dtc);
    CHECK(WIFEXITED(sb) && WEXITSTATUS(sb) == 0 && dtb >= 1500 && dtb < 9000, "troisième preneur : a attendu la sortie "
          "du premier puis l'a obtenu (après %ld ms), second appel réentrant", dtb);
    snprintf(p, sizeof p, "%s/.instance", S);
    CHECK(lire(p, o, sizeof o) && strtol(o, NULL, 10) == (long)b, "pid du détenteur écrit dans .instance (%ld)", (long)b);
}

/* ---- en plus : pack SPIR-V ---------------------------------------------------------------- */

static void cas_pack(void)
{
    printf("--- pack SPIR-V : choisi comme le runtime, contrôlé, jamais bloquant\n");
    env_propre();
    char S[1400], I[1400], p[1600], q[1600], j[16384];
    chemin(S, sizeof S, "Application Support/pack");
    chemin(I, sizeof I, "pack/install");
    exe_creux(I, SC2M_EXE_TAILLE);
    sc2m_install_reference(SHA_ZEROS);
    chemin(p, sizeof p, "pack/sc2-spirv.pak");
    ecrire(p, "faux pack");
    sc2m_dossiers d;
    sc2m_config c;
    dossiers(&d, S, NULL, NULL);
    setenv("SC2_INSTALL", I, 1);
    int r = configurer(&d, &c, j, sizeof j);
    CHECK(r == 0 && egal_env("SC2_SPIRV_PACK", NULL) && strstr(j, "NON conforme") && strstr(j, p),
          "pack à côté de l'install : contrôlé (NON conforme dit), SC2_SPIRV_PACK non posée (le runtime le trouve seul)");
    unlink(p);
    snprintf(q, sizeof q, "%s/sc2-spirv.pak", S);
    ecrire(q, "faux pack");
    env_propre();
    setenv("SC2_INSTALL", I, 1);
    configurer(&d, &c, j, sizeof j);
    CHECK(egal_env("SC2_SPIRV_PACK", q), "pack dans <support> : SC2_SPIRV_PACK=%s", q);
    env_propre();
    unlink(q);
    setenv("SC2_INSTALL", I, 1);
    configurer(&d, &c, j, sizeof j);
    CHECK(strstr(j, "[MAC] pack SPIR-V absent") != NULL && egal_env("SC2_SPIRV_PACK", NULL), "aucun pack : absent dit");
    sc2m_install_reference(NULL);
}

/* ---- en plus : journal --------------------------------------------------------------------- */

static int fils_journal(const char *chemin_j, const char *texte)
{
    fflush(stdout);
    pid_t f = fork();
    if (f == 0) {
        int r = sc2m_journal_ouvrir(chemin_j);
        printf("%s-stdout\n", texte);
        fprintf(stderr, "%s-stderr\n", texte);
        fflush(NULL);
        _exit(r ? 1 : 0);
    }
    int s = -1;
    waitpid(f, &s, 0);
    return WIFEXITED(s) ? WEXITSTATUS(s) : -1;
}

static void cas_journal(void)
{
    printf("--- journal : dup2 de stdout et stderr, rotation en .1\n");
    env_propre();
    char J[1400], J1[1500], o[4096];
    chemin(J, sizeof J, "journal/Logs/SC2Partout/sc2-partout.log");
    snprintf(J1, sizeof J1, "%s.1", J);
    CHECK(fils_journal(J, "premier") == 0 && lire(J, o, sizeof o) && strstr(o, "premier-stdout\n")
          && strstr(o, "premier-stderr\n") && strstr(o, "[MAC] journal : "), "stdout et stderr dans le fichier (dossier créé)");
    CHECK(fils_journal(J, "second") == 0 && lire(J1, o, sizeof o) && strstr(o, "premier-stdout")
          && lire(J, o, sizeof o) && strstr(o, "second-stdout") && !strstr(o, "premier-stdout") && strstr(o, "précédent"),
          "second lancement : l'ancien journal en .1");
    CHECK(sc2m_journal_ouvrir("-") == 0 && sc2m_journal_ouvrir(NULL) == 0, "« - » : sortie standard inchangée");
    sc2m_dossiers d;
    dossiers(&d, "/s", NULL, NULL);
    snprintf(d.journaux, sizeof d.journaux, "/j");
    CHECK(!strcmp(sc2m_journal_chemin(&d, o, sizeof o), "/j/sc2-partout.log"), "chemin par défaut <journaux>/sc2-partout.log");
    setenv("SC2_MAC_JOURNAL", "-", 1);
    CHECK(!strcmp(sc2m_journal_chemin(&d, o, sizeof o), "-"), "SC2_MAC_JOURNAL=- : « - »");
    unsetenv("SC2_MAC_JOURNAL");
}

/* ---- en plus : dossiers par défaut -------------------------------------------------------- */

static void cas_dossiers(void)
{
    printf("--- dossiers par défaut\n");
    env_propre();
    sc2m_dossiers d;
    char P[1400], p[1600], o[1600];
    const char *home = getenv("HOME");
    char home_sauve[1024];
    snprintf(home_sauve, sizeof home_sauve, "%s", home ? home : "");
    setenv("HOME", "/Users/essai", 1);
    CHECK(sc2m_dossiers_defaut(&d, NULL) == 0 && !strcmp(d.support, "/Users/essai/Library/Application Support/SC2Partout")
          && !strcmp(d.journaux, "/Users/essai/Library/Logs/SC2Partout") && !d.ressources[0] && !d.frameworks[0],
          "sans SC2_MAC_SUPPORT : ~/Library/Application Support/SC2Partout et ~/Library/Logs/SC2Partout");
    setenv("SC2_MAC_SUPPORT", "/tmp/essai/support/", 1);
    CHECK(sc2m_dossiers_defaut(&d, NULL) == 0 && !strcmp(d.support, "/tmp/essai/support")
          && !strcmp(d.journaux, "/tmp/essai/support/Logs"), "SC2_MAC_SUPPORT : support et journal isolés (« %s »)", d.support);
    char cwd[1024] = "";
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    setenv("SC2_MAC_SUPPORT", "relatif", 1);
    snprintf(o, sizeof o, "%s/relatif", cwd);
    CHECK(sc2m_dossiers_defaut(&d, NULL) == 0 && !strcmp(d.support, o), "SC2_MAC_SUPPORT relatif : rendu absolu");
    unsetenv("SC2_MAC_SUPPORT");
    chemin(P, sizeof P, "Dossiers.app");
    snprintf(p, sizeof p, "%s/Contents/Resources", P);  creer_dossier(p);
    snprintf(p, sizeof p, "%s/Contents/Frameworks", P); creer_dossier(p);
    CHECK(sc2m_dossiers_defaut(&d, P) == 0 && !strcmp(d.frameworks, p) && strstr(d.ressources, "/Contents/Resources"),
          "paquet : Resources et Frameworks retenus");
    chemin(P, sizeof P, "SansRien.app");
    creer_dossier(P);
    CHECK(sc2m_dossiers_defaut(&d, P) == 0 && !d.ressources[0] && !d.frameworks[0], "paquet sans Resources ni Frameworks : vides");
    if (home_sauve[0]) setenv("HOME", home_sauve, 1); else unsetenv("HOME");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *t = getenv("TMPDIR");
    char tmpdir[400];
    snprintf(tmpdir, sizeof tmpdir, "%s", t && *t ? t : "/tmp");
    for (size_t n = strlen(tmpdir); n > 1 && tmpdir[n - 1] == '/'; n--) tmpdir[n - 1] = 0;   /* TMPDIR finit par « / » */
    snprintf(g_base, sizeof g_base, "%s/test_config_mac.XXXXXX", tmpdir);
    if (!mkdtemp(g_base)) { printf("FAIL  mkdtemp %s : %s\n", g_base, strerror(errno)); return 1; }
    printf("dossier d'essai : %s ; V1.5 %s\n", g_base,
#ifdef SC2_VARIABLES_MAX
           "présente"
#else
           "absente"
#endif
    );
    cas_env();
    cas_fusion();
    cas_effacement();
    cas_defauts();
    cas_vk_lib();
    cas_sha256();
    cas_verrou();
    cas_pack();
    cas_journal();
    cas_dossiers();
    env_propre();
    if (chdir("/")) { }
    if (!g_fail) sc2m_effacer_arbre(g_base);
    else printf("dossier d'essai conservé : %s\n", g_base);
    printf("RÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
