/* test_documents.c — passe 10t : Banks/ et Saves/ persistants (runtime/sc2_documents.c).
 *
 * Tout se joue sur des dossiers temporaires (mkdtemp) : racine neuve → deux liens
 * corrects et cibles créées ; second appel → inchangé (idempotent) ; vrai dossier
 * préexistant → fusion (fichier absent repris, fichier persistant plus récent
 * conservé, fichier local plus récent qui l'emporte, sous-dossier recursé), puis
 * lien ; lien vers ailleurs → remplacé ; écriture d'un fichier À TRAVERS le lien →
 * présent dans le persistant ; effacement de la racine par nftw(FTW_DEPTH |
 * FTW_PHYS), comme Android → le persistant est intact.
 * V1.5 (22/10) : semis de Variables.txt — SC2_VARIABLES absente ou « 0 » : rien ; posée : semé
 * sous racine/Users/sc2/Documents/StarCraft II, sans .tmp laissé ; point d'appel commun une seule
 * fois par processus ; présent et non figé : conservé ; figé : réécrit ; sources refusées (vide,
 * > 64 Kio, absente, dossier) sans toucher l'existant ; préréglage en mémoire (« fold » d'Android) ;
 * lastDeviceId aligné sur le DeviceId du profil GPU : ajouté à une source qui ne l'a pas, remplacé s'il
 * diffère (Variables.txt de /sdcard du Fold écrit sous la 8600 GT, semé en dx10), sinon copie octet pour octet.
 */
#define _GNU_SOURCE
#include "sc2_documents.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass, g_fail;
#define CHECK(cond, msg) do { if (cond) { g_pass++; printf("PASS  %s\n", msg); } \
                              else { g_fail++; printf("FAIL  %s\n", msg); } } while (0)

/* sc2_log du runtime, remplacé ici : les messages [DOCS] défilent avec le test. */
void sc2_log(const char *fmt, ...)
{
    va_list ap;
    fputs("      ", stdout);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n');
}

/* mkdir -p local (le module le fait chez lui, le test prépare ses fixtures). */
static void creer_dossier(const char *chemin)
{
    char t[1400];
    snprintf(t, sizeof t, "%s", chemin);
    for (char *q = t + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
    mkdir(t, 0755);
}

/* Écrit un fichier (parents NON créés) et pose sa mtime : les fusions comparent les mtime. */
static void ecrire(const char *p, const char *contenu, time_t mtime)
{
    FILE *f = fopen(p, "wb");
    if (!f) { printf("      (échec d'écriture %s : %s)\n", p, strerror(errno)); return; }
    fputs(contenu, f);
    fclose(f);
    if (mtime) {
        struct timespec ts[2] = { { mtime, 0 }, { mtime, 0 } };
        utimensat(AT_FDCWD, p, ts, 0);
    }
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

static int rm_un(const char *p, const struct stat *st, int type, struct FTW *f)
{ (void)st; (void)type; (void)f; return remove(p); }

/* mtime en secondes : st_mtim (POSIX 2008) sauf macOS (st_mtimespec). */
#ifdef __APPLE__
#define MTIME_S(stv) ((stv).st_mtimespec.tv_sec)
#else
#define MTIME_S(stv) ((stv).st_mtim.tv_sec)
#endif

/* Vrai si « lien » est un lien symbolique pointant exactement vers realpath(cible). */
static int lien_vers(const char *lien, const char *cible)
{
    char c[1024], abs[1024];
    ssize_t n = readlink(lien, c, sizeof c - 1);
    if (n <= 0 || !realpath(cible, abs)) return 0;
    c[n] = 0;
    return strcmp(c, abs) == 0;
}

int main(void)
{
    char base[] = "/tmp/sc2docs-test-XXXXXX";
    if (!mkdtemp(base)) { perror("mkdtemp"); return 1; }

    static const char sc2[] = "Users/sc2/Documents/StarCraft II";
    char racine[1400], racine2[1400], racine3[1400], racine4[1400], persist[1400], autre[1400];
    snprintf(racine,  sizeof racine,  "%s/racine",  base);
    snprintf(racine2, sizeof racine2, "%s/racine2", base);
    snprintf(racine3, sizeof racine3, "%s/racine3", base);
    snprintf(racine4, sizeof racine4, "%s/racine4", base);
    snprintf(persist, sizeof persist, "%s/persist", base);
    snprintf(autre,   sizeof autre,   "%s/autre-persist", base);

    char p_banks[1500], p_saves[1500], k_banks[1400], k_saves[1400];
    snprintf(p_banks, sizeof p_banks, "%s/%s/Banks", racine, sc2);
    snprintf(p_saves, sizeof p_saves, "%s/%s/Saves", racine, sc2);
    snprintf(k_banks, sizeof k_banks, "%s/Banks", persist);
    snprintf(k_saves, sizeof k_saves, "%s/Saves", persist);

    /* ---- racine neuve : deux liens corrects, cibles créées -------------------------------- */
    CHECK(sc2_documents_preparer(racine, persist) == 0, "racine neuve : retour 0");
    CHECK(lien_vers(p_banks, k_banks), "Banks : lien symbolique vers $SC2_DOCUMENTS/Banks");
    CHECK(lien_vers(p_saves, k_saves), "Saves : lien symbolique vers $SC2_DOCUMENTS/Saves");
    struct stat st;
    CHECK(!stat(k_banks, &st) && S_ISDIR(st.st_mode) && !stat(k_saves, &st) && S_ISDIR(st.st_mode),
          "cibles persistantes Banks et Saves créées (dossiers)");

    /* Le jeu écrit SA banque dans C:\…\Banks\ — à travers le lien. */
    char bank_locale[1500], bank_persist[1400], lu[512];
    snprintf(bank_locale,  sizeof bank_locale,  "%s/TCampaign.SC2Bank", p_banks);
    snprintf(bank_persist, sizeof bank_persist, "%s/TCampaign.SC2Bank", k_banks);
    ecrire(bank_locale, "<Bank mission=3/>", 0);

    /* ---- second appel : rien ne bouge ------------------------------------------------------ */
    char avant[1024] = "";
    ssize_t n = readlink(p_banks, avant, sizeof avant - 1);
    if (n > 0) avant[n] = 0;
    CHECK(sc2_documents_preparer(racine, persist) == 0, "second appel : retour 0 (idempotent)");
    char apres[1024] = "";
    n = readlink(p_banks, apres, sizeof apres - 1);
    if (n > 0) apres[n] = 0;
    CHECK(!strcmp(avant, apres), "second appel : le lien Banks est inchangé");
    CHECK(lire(bank_persist, lu, sizeof lu) && !strcmp(lu, "<Bank mission=3/>"),
          "écriture à travers le lien : le fichier est dans le persistant");

    /* ---- vrai dossier préexistant : fusion, puis lien -------------------------------------- */
    char l2b[1500], l2s[1500];
    snprintf(l2b, sizeof l2b, "%s/%s/Banks", racine2, sc2);
    snprintf(l2s, sizeof l2s, "%s/%s/Saves", racine2, sc2);
    creer_dossier(l2b);
    creer_dossier(l2s); creer_dossier(autre);          /* autre : cible d'un lien alien, plus bas */
    char l2u[1600];
    snprintf(l2u, sizeof l2u, "%s/Unsaved", l2s); creer_dossier(l2u);

    time_t T1 = 1000000000, T2 = T1 + 1000, T3 = T1 + 2000;
    char f[1600];
    snprintf(f, sizeof f, "%s/Recent.SC2Bank", k_banks);        ecrire(f, "persist", T2);
    snprintf(f, sizeof f, "%s/LocalRecent.SC2Bank", k_banks);   ecrire(f, "persist-vieux", T1);
    snprintf(f, sizeof f, "%s/Recent.SC2Bank", l2b);            ecrire(f, "local-vieux", T1);
    snprintf(f, sizeof f, "%s/Nouveau.SC2Bank", l2b);           ecrire(f, "local", T3);
    snprintf(f, sizeof f, "%s/LocalRecent.SC2Bank", l2b);       ecrire(f, "local-recent", T3);
    snprintf(f, sizeof f, "%s/Liberation Day Victory.SC2Save", l2u); ecrire(f, "sauvegarde", T3);

    CHECK(sc2_documents_preparer(racine2, persist) == 0, "vrai dossier préexistant : retour 0");
    snprintf(f, sizeof f, "%s/Nouveau.SC2Bank", k_banks);
    CHECK(lire(f, lu, sizeof lu) && !strcmp(lu, "local"),
          "fusion : fichier local absent du persistant repris");
    snprintf(f, sizeof f, "%s/Recent.SC2Bank", k_banks);
    CHECK(lire(f, lu, sizeof lu) && !strcmp(lu, "persist"),
          "fusion : fichier persistant plus récent conservé (non écrasé)");
    snprintf(f, sizeof f, "%s/LocalRecent.SC2Bank", k_banks);
    CHECK(lire(f, lu, sizeof lu) && !strcmp(lu, "local-recent"),
          "fusion : fichier local plus récent l'emporte");
    snprintf(f, sizeof f, "%s/Unsaved/Liberation Day Victory.SC2Save", k_saves);
    CHECK(lire(f, lu, sizeof lu) && !strcmp(lu, "sauvegarde"),
          "fusion : sous-dossier (Saves/Unsaved) recursé et repris");
    CHECK(lien_vers(l2b, k_banks), "fusion : le vrai dossier est remplacé par le lien");
    snprintf(f, sizeof f, "%s/Nouveau.SC2Bank", k_banks);
    CHECK(!stat(f, &st) && MTIME_S(st) == T3, "reprise : mtime du fichier repris conservée");

    /* ---- lien vers ailleurs : remplacé, cible étrangère intacte ---------------------------- */
    char a_banks[1400], l3b[1500];
    snprintf(a_banks, sizeof a_banks, "%s/Banks", autre);
    creer_dossier(a_banks);
    snprintf(f, sizeof f, "%s/Marqueur.txt", a_banks); ecrire(f, "autre", 0);
    snprintf(l3b, sizeof l3b, "%s/%s/Banks", racine3, sc2);
    creer_dossier(l3b);                                /* le parent seulement */
    rmdir(l3b);                                        /* creer_dossier a fait Banks : on l'ôte */
    char abs_autre[1024];
    if (realpath(a_banks, abs_autre)) symlink(abs_autre, l3b);
    CHECK(sc2_documents_preparer(racine3, persist) == 0 && lien_vers(l3b, k_banks),
          "lien vers ailleurs : remplacé par le bon lien");
    snprintf(f, sizeof f, "%s/Marqueur.txt", a_banks);
    CHECK(lire(f, lu, sizeof lu) && !strcmp(lu, "autre"), "lien vers ailleurs : la cible étrangère est intacte");

    /* ---- effacement de la racine comme Android (nftw FTW_DEPTH | FTW_PHYS) ---------------- */
    nftw(racine, rm_un, 16, FTW_DEPTH | FTW_PHYS);
    snprintf(f, sizeof f, "%s/Nouveau.SC2Bank", k_banks);
    CHECK(lire(f, lu, sizeof lu) && !strcmp(lu, "local") && lire(bank_persist, lu, sizeof lu)
              && !strcmp(lu, "<Bank mission=3/>"),
          "effacement de la racine (nftw, comme Android) : le persistant est intact");
    CHECK(lstat(racine, &st) != 0 && errno == ENOENT, "effacement de la racine : la racine a bien disparu");

    /* ---- échec journalisé : persistant impossible à créer --------------------------------- */
    snprintf(f, sizeof f, "%s/un-fichier", base); ecrire(f, "x", 0);
    CHECK(sc2_documents_preparer(racine4, f) == -1,
          "persistant impossible à créer (un fichier à la place) : retour -1");

    /* ---- V1.5 : Variables.txt (SC2_VARIABLES, SC2_VARIABLES_FIGE) ------------------------- */
    static char grand[SC2_VARIABLES_MAX + 2];
    char rv[1400], rv2[1400], vt[1600], vt2[1600], src[1400], gros[1400], vide[1400];
    /* source écrite par le jeu sous la 8600 GT (lastDeviceId=1026) : copiée telle quelle en historique, alignée en dx10 */
    static const char ultra[] = "GraphicsOptionTextureQuality[2]=3\r\nlastDeviceId=1026\r\ntexQualityLevel=3\r\n";
    char nue[1400], sansfin[1400], deux[1400], attendu[256];
    snprintf(rv,   sizeof rv,   "%s/racine-vars",  base);
    snprintf(rv2,  sizeof rv2,  "%s/racine-vars2", base);
    snprintf(vt,   sizeof vt,   "%s/%s/Variables.txt", rv, sc2);
    snprintf(vt2,  sizeof vt2,  "%s/%s/Variables.txt", rv2, sc2);
    snprintf(src,  sizeof src,  "%s/ultra.txt", base);
    snprintf(gros, sizeof gros, "%s/trop-gros.txt", base);
    snprintf(vide, sizeof vide, "%s/vide.txt", base);
    ecrire(src, ultra, 0);
    snprintf(nue, sizeof nue, "%s/nue.txt", base);
    snprintf(sansfin, sizeof sansfin, "%s/sans-fin.txt", base);
    ecrire(nue, "GraphicsOptionTextureQuality[2]=3\r\ntexQualityLevel=3\r\n", 0);
    ecrire(sansfin, "texQualityLevel=3\nxlastDeviceId=7", 0);
    snprintf(deux, sizeof deux, "%s/deux.txt", base);
    ecrire(deux, "lastDeviceId=\nhalo=1\r\nlastDeviceId=1026", 0);
    ecrire(vide, "", 0);
    memset(grand, 'x', sizeof grand - 1);              /* 64 Kio + 1 */
    ecrire(gros, grand, 0);

    unsetenv("SC2_VARIABLES"); unsetenv("SC2_VARIABLES_FIGE"); unsetenv("SC2_GPU_PROFIL");
    CHECK(sc2_variables_depuis_env(rv) == 0 && lstat(vt, &st) != 0, "SC2_VARIABLES absente : rien n'est semé (retour 0)");
    setenv("SC2_VARIABLES", "0", 1);
    CHECK(sc2_variables_depuis_env(rv) == 0 && lstat(vt, &st) != 0, "SC2_VARIABLES=0 : rien n'est semé");
    setenv("SC2_VARIABLES", src, 1);
    CHECK(sc2_variables_depuis_env(rv) == 1, "SC2_VARIABLES=<fichier> : semé (retour 1), dossiers créés");
    CHECK(lire(vt, lu, sizeof lu) && !strcmp(lu, ultra),
          "semis : racine/Users/sc2/Documents/StarCraft II/Variables.txt identique à la source");
    snprintf(f, sizeof f, "%s.tmp", vt);
    CHECK(lstat(f, &st) != 0, "semis : aucun Variables.txt.tmp laissé");
    setenv("SC2_VARIABLES_FIGE", "1", 1);
    ecrire(vt, "ecrit-par-le-jeu\n", 0);
    CHECK(sc2_variables_depuis_env(rv) == 0 && lire(vt, lu, sizeof lu) && !strcmp(lu, "ecrit-par-le-jeu\n"),
          "point d'appel commun : une seule fois par processus (hôte Android déjà passé : rien n'est refait)");

    CHECK(sc2_variables_semer(rv2, src, 0) == 1 && lire(vt2, lu, sizeof lu) && !strcmp(lu, ultra), "absent : semé");
    ecrire(vt2, "ecrit-par-le-jeu\n", 0);
    CHECK(sc2_variables_semer(rv2, src, 0) == 0 && lire(vt2, lu, sizeof lu) && !strcmp(lu, "ecrit-par-le-jeu\n"),
          "présent, non figé : conservé (retour 0), réglages du jeu intacts");
    CHECK(sc2_variables_semer(rv2, src, 1) == 1 && lire(vt2, lu, sizeof lu) && !strcmp(lu, ultra),
          "présent, figé : réécrit depuis la source (retour 1)");
    ecrire(vt2, "ecrit-par-le-jeu\n", 0);
    CHECK(sc2_variables_semer(rv2, gros, 1) == -1 && lire(vt2, lu, sizeof lu) && !strcmp(lu, "ecrit-par-le-jeu\n"),
          "source > 64 Kio : refusée (retour -1), Variables.txt intact même figé");
    CHECK(sc2_variables_semer(rv2, vide, 1) == -1, "source vide (0 o) : refusée");
    snprintf(f, sizeof f, "%s/absent.txt", base);
    CHECK(sc2_variables_semer(rv2, f, 1) == -1, "source absente : refusée");
    CHECK(sc2_variables_semer(rv2, base, 1) == -1, "source dossier (pas un fichier ordinaire) : refusée");
    /* lastDeviceId : sans la clé, le jeu re-détecte ses réglages → ajoutée, DeviceId de la carte annoncée */
    unsetenv("SC2_GPU_PROFIL");
    CHECK(sc2_variables_device_id() == 0x0402 && sc2_variables_semer(rv2, nue, 1) == 1 && lire(vt2, lu, sizeof lu)
              && !strcmp(lu, "GraphicsOptionTextureQuality[2]=3\r\ntexQualityLevel=3\r\nlastDeviceId=1026\r\n"),
          "source sans lastDeviceId, profil historique : « lastDeviceId=1026 » (8600 GT) ajouté en fin");
    setenv("SC2_GPU_PROFIL", "dx10", 1);
    CHECK(sc2_variables_device_id() == 0x05E3 && sc2_variables_semer(rv2, nue, 1) == 1 && lire(vt2, lu, sizeof lu)
              && !strcmp(lu, "GraphicsOptionTextureQuality[2]=3\r\ntexQualityLevel=3\r\nlastDeviceId=1507\r\n"),
          "SC2_GPU_PROFIL=dx10 : « lastDeviceId=1507 » (GTX 285) ajouté");
    /* revue V1.5 : le Variables.txt de /sdcard du Fold, écrit par le jeu sous la 8600 GT (1026), semé sous dx10 →
     * aligné sur 1507, sinon le jeu re-détecte (Ultra en dx10) et écrase les réglages de l'utilisateur */
    CHECK(sc2_variables_semer(rv2, src, 1) == 1 && lire(vt2, lu, sizeof lu)
              && !strcmp(lu, "GraphicsOptionTextureQuality[2]=3\r\nlastDeviceId=1507\r\ntexQualityLevel=3\r\n"),
          "source avec lastDeviceId=1026 (écrite par le jeu sous la 8600 GT), dx10 : remplacé par 1507, reste intact");
    CHECK(sc2_variables_semer(rv2, deux, 1) == 1 && lire(vt2, lu, sizeof lu)
              && !strcmp(lu, "lastDeviceId=1507\nhalo=1\r\nlastDeviceId=1507"),
          "dx10 : chaque ligne lastDeviceId alignée (valeur vide, dernière ligne sans fin), fins de ligne d'origine");
    setenv("SC2_GPU_PROFIL", "historique", 1);
    CHECK(sc2_variables_semer(rv2, src, 1) == 1 && lire(vt2, lu, sizeof lu) && !strcmp(lu, ultra),
          "source avec lastDeviceId=1026, historique : copiée octet pour octet (canon.txt du canon épinglé)");
    setenv("SC2_GPU_PROFIL", "inconnu", 1);
    CHECK(sc2_variables_semer(rv2, sansfin, 1) == 1 && lire(vt2, lu, sizeof lu)
              && !strcmp(lu, "texQualityLevel=3\nxlastDeviceId=7\r\nlastDeviceId=1026\r\n"),
          "profil inconnu → historique ; sans fin de ligne : saut ajouté ; « xlastDeviceId » n'est pas la clé");
    unsetenv("SC2_GPU_PROFIL");
    snprintf(attendu, sizeof attendu, "GraphicsOptionModelQuality=2\nlastDeviceId=1026\r\n");
    CHECK(sc2_variables_semer_texte(rv2, "GraphicsOptionModelQuality=2\n", "texte", 1) == 1
              && lire(vt2, lu, sizeof lu) && !strcmp(lu, attendu),
          "préréglage en mémoire, figé : écrit, lastDeviceId ajouté");
    CHECK(sc2_variables_semer_texte(rv2, "GraphicsOptionModelQuality=0\n", "texte", 0) == 0
              && lire(vt2, lu, sizeof lu) && !strcmp(lu, attendu),
          "préréglage en mémoire, présent et non figé : conservé");
    CHECK(sc2_variables_semer_texte(rv2, "", "texte", 1) == -1, "préréglage en mémoire vide : refusé");
    unsetenv("SC2_VARIABLES"); unsetenv("SC2_VARIABLES_FIGE");

    printf("RÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    nftw(base, rm_un, 16, FTW_DEPTH | FTW_PHYS);
    return g_fail ? 1 : 0;
}
