/* sc2_android_main.c — l'activité Android de SC2 Partout (passe 7a) : NativeActivity + native_app_glue.
 *
 *   android_main (fil de la glue)   fenêtre, tactile (→ sc2_input, passe 7d), touche retour, événements
 *   fil « sc2-jeu » (16 Mo de pile)  zone invitée décalée, puis main() de recomp-sc2 tel quel
 *   fil « sc2-log »                  stdout/stderr du runtime → logcat (tag SC2Partout) + fichier
 *
 * Données : SC2_INSTALL = /sdcard/SC2Partout/install si lisible (accès « tous les fichiers »),
 * sinon <stockage externe de l'app>/install (/sdcard/Android/data/<paquet>/files/install, sans
 * permission). Cf. docs/PIPELINE-ANDROID.md.
 *
 * Affichage v1 : le moteur rend dans son back buffer Vulkan (backend 5c, ici sur Adreno) ; après
 * chaque Present, le back buffer est relu et copié dans la fenêtre (ANativeWindow, RGBA 8888,
 * géométrie à bandes noires : c'est le compositeur qui met à l'échelle de l'écran).
 */
#define _GNU_SOURCE
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "android_native_app_glue.h"
#include "sc2_android.h"
#include "sc2_audio_android.h"
#include <jni.h>
#include "sc2_geo.h"
#include "sc2_gestes.h"   /* 21/10 : 1 doigt = clic gauche, 2 doigts = clic droit */
#include "sc2_phases.h"
#include "sc2_documents.h"  /* V1.5 : semis de Variables.txt commun (recomp-sc2/runtime/sc2_documents.c) */
#include "sc2_trace_present.h"
#include "sc2_vars_regle.h"  /* 23/10 : les réglages d'un run de test ne fuient plus vers /sdcard */
/* passe 9s : runtime/video_amc.c (option CMake SC2_VIDEO_AMC) — symboles faibles : absents si l'option est OFF */
int  video_amc_entree(void) __attribute__((weak));
void video_amc_bilan(char *o, size_t cap) __attribute__((weak));
#include "sc2_entrees_bt.h"                            /* passe 8h */
#ifdef SC2_SWAPCHAIN
#include "sc2_vk_presente.h"
#endif

#define TAG "SC2Partout"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static struct android_app *g_app;
static char g_exe[1024], g_install[768], g_journal[768];

/* ---- stdout/stderr → logcat -------------------------------------------------------------- */

static int g_tube[2] = { -1, -1 };
static atomic_int g_log_actif;
static FILE *g_fjournal;

/* Passe 7r : phases du run (sc2_phases.h) repérées dans le journal du runtime → cadence PAR PHASE. Une
 * ligne « [PHASE] menu à 41.2 s (frame affichée 230) ; titre : 228 images en 30.5 s = 7.5 images/s » à
 * chaque passage, le nom de la phase dans chaque ligne [FPS], et « [BILAN] [FPS] phase … » à la sortie. */
static sc2_phases g_ph;
static pthread_mutex_t g_ph_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_ph_pret;
static atomic_uint g_frames_affichees;
static struct timespec g_t_lancement;

static double depuis_lancement(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - g_t_lancement.tv_sec) + (double)(t.tv_nsec - g_t_lancement.tv_nsec) / 1e9;
}

/* ligne du runtime (fil_log) ou frame affichée (k > 0, sc2a_frame) */
static void phases_suivre(const char *ligne, unsigned k)
{
    if (!atomic_load(&g_ph_pret)) return;
    char s[256]; int change;
    pthread_mutex_lock(&g_ph_mu);
    double t = depuis_lancement();
    change = ligne ? sc2_phases_ligne(&g_ph, ligne, t, atomic_load(&g_frames_affichees)) > 0
                   : sc2_phases_frame(&g_ph, t, k) > 0;
    if (change) sc2_phases_texte_passage(&g_ph, s, sizeof s);
    pthread_mutex_unlock(&g_ph_mu);
    if (change) {
        __android_log_write(ANDROID_LOG_INFO, TAG, s);
        if (g_fjournal) { fputs(s, g_fjournal); fputc('\n', g_fjournal); }
    }
}

static const char *phase_courante(void)
{
    return atomic_load(&g_ph_pret) ? g_ph.nom[g_ph.courante] : "?";
}

static void phases_bilan(void)
{
    if (!atomic_load(&g_ph_pret)) return;
    char b[SC2_PHASES_MAX + 2][160];
    pthread_mutex_lock(&g_ph_mu);
    int m = sc2_phases_bilan(&g_ph, depuis_lancement(), atomic_load(&g_frames_affichees), b, SC2_PHASES_MAX + 2);
    pthread_mutex_unlock(&g_ph_mu);
    for (int i = 0; i < m; i++) {
        __android_log_write(ANDROID_LOG_INFO, TAG, b[i]);
        if (g_fjournal) { fputs(b[i], g_fjournal); fputc('\n', g_fjournal); }
    }
}

static void *fil_log(void *p)
{
    (void)p;
    char tampon[4096]; size_t n = 0;
    for (;;) {
        ssize_t k = read(g_tube[0], tampon + n, sizeof tampon - 1 - n);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; break; }
        atomic_store(&g_log_actif, 1);
        n += (size_t)k;
        char *debut = tampon, *nl;
        while ((nl = memchr(debut, '\n', n - (size_t)(debut - tampon)))) {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, TAG, debut);
            phases_suivre(debut, 0);
            if (g_fjournal) { fputs(debut, g_fjournal); fputc('\n', g_fjournal); }
            debut = nl + 1;
        }
        n -= (size_t)(debut - tampon);
        memmove(tampon, debut, n);
        if (n == sizeof tampon - 1) { tampon[n] = 0; __android_log_write(ANDROID_LOG_INFO, TAG, tampon); n = 0; }
        if (g_fjournal) fflush(g_fjournal);
        atomic_store(&g_log_actif, 0);
    }
    return NULL;
}

/* atexit : le bilan du runtime part par stdout juste avant exit() — on le laisse arriver. */
static void vider_log(void)
{
    fflush(stdout); fflush(stderr);
    for (int i = 0; i < 200; i++) {
        int reste = 0;
        if (ioctl(g_tube[0], FIONREAD, &reste) || (!reste && !atomic_load(&g_log_actif))) break;
        usleep(5000);
    }
    phases_bilan();
    if (g_fjournal) fflush(g_fjournal);
}

/* 21/10 : exit(), lancé par le fil du jeu, exécute aussi les finaliseurs statiques de libart, qui avortent (« destroying
 * mutex with owner or contenders », SIGABRT : Fold v24a, 1 sortie sur ≈ 20) quand un fil Java tient l'un de ses verrous.
 * Enregistré AVANT tous les autres gestionnaires, celui-ci passe en DERNIER des nôtres (ordre inverse d'atexit) —
 * bilans écrits, journal vidé — et termine par _exit : les finaliseurs d'ART ne tournent jamais. */
static void sortie_sans_finaliseurs(void) { fflush(NULL); _exit(0); }

static void rediriger_sorties(void)
{
    atexit(sortie_sans_finaliseurs);
    if (pipe(g_tube)) return;
    dup2(g_tube[1], 1); dup2(g_tube[1], 2);
    setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    g_fjournal = g_journal[0] ? fopen(g_journal, "w") : NULL;
    pthread_t t; pthread_create(&t, NULL, fil_log, NULL); pthread_detach(t);
    atexit(vider_log);
}

/* ---- configuration ----------------------------------------------------------------------- */

/* Fichier sc2.env (KEY=VALEUR par ligne, # commentaire) : SC2_ECRAN, SC2_FRAMES_MAX, SC2_WATCHDOG… */
static void lire_env(const char *chemin)
{
    FILE *f = fopen(chemin, "r");
    if (!f) return;
    char l[1024];
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        char *eg = strchr(l, '=');
        if (l[0] == '#' || !eg) continue;
        *eg = 0;
        setenv(l, eg + 1, 1);
        LOGI("%s : %s=%s", chemin, l, eg + 1);
    }
    fclose(f);
}

static int lisible(const char *install)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/Versions/Base15405/SC2.exe", install);
    return access(p, R_OK) == 0;
}

/* Asset de l'APK → fichier (le runtime lit ses listes par fopen). 0 = OK. */
static int extraire_asset(const char *nom, const char *dest)
{
    AAsset *a = AAssetManager_open(g_app->activity->assetManager, nom, AASSET_MODE_STREAMING);
    if (!a) return -1;
    FILE *f = fopen(dest, "wb");
    char b[4096]; int k;
    while (f && (k = AAsset_read(a, b, sizeof b)) > 0) fwrite(b, 1, (size_t)k, f);
    AAsset_close(a);
    return f && !fclose(f) ? 0 : -1;
}

/* ---- Variables.txt persistant ------------------------------------------------------------ */

/* sc2-c (le C:\ émulé) est effacé à chaque lancement (passe 6a) : le Variables.txt du jeu — réglages
 * graphiques, son… écrit sous C:\Users\sc2\Documents\StarCraft II\ — serait perdu. À chaque lancement :
 * sauvegarde de celui du run précédent vers /sdcard/SC2Partout/Variables.txt (AVANT l'effacement), puis
 * semis d'un préréglage dans le C:\ neuf (APRÈS) : SC2_VARIABLES s'il est posé, sinon le fichier de
 * /sdcard, sinon (vague 0 bis, 22/10) le préréglage intégré « fold ». SC2_VARIABLES_FIGE=1 : le préréglage
 * de /sdcard est figé, le jeu ne l'écrase plus. Le semis lui-même est commun au banc Mac depuis V1.5
 * (sc2_variables_semer, recomp-sc2/runtime/sc2_documents.c). */
#define VARS_TXT_SD "/sdcard/SC2Partout/Variables.txt"

/* Préréglage « fold » = ULTRA (décision du mainteneur le 23/10, après S1 : l'Ultra tient le plafond de 60 images/s sur
 * TRaynor01 ; vague 0 bis du 22/10 : Medium, textures High), semé quand
 * l'utilisateur n'a encore aucun Variables.txt. Copie conforme de scripts/prereglages/fold.txt (clés de réglage
 * écrites par le jeu lui-même, aucun contenu du jeu) : scripts/banc-options.sh --controle vérifie l'égalité. */
static const char VARS_FOLD[] =
/* VARS_FOLD début (généré depuis scripts/prereglages/fold.txt) */
"alphadepthWrite=1\r\n"
"aolight=1\r\n"
"creepnormalmap=1\r\n"
"creepQuality=3\r\n"
"creepreflection=1\r\n"
"creepselfshadow=1\r\n"
"creeptrans=1\r\n"
"deff=1\r\n"
"deffspec=1\r\n"
"depthDisp=1\r\n"
"dof=1\r\n"
"fogvolume=1\r\n"
"foliagedensity=0.750000\r\n"
"foliagequality=3\r\n"
"gamehdr8bit=0\r\n"
"GraphicsOptionEffectsDetail=3\r\n"
"GraphicsOptionLightingQuality=3\r\n"
"GraphicsOptionModelQuality=2\r\n"
"GraphicsOptionMovies=3\r\n"
"GraphicsOptionOverallQualityVer7[5]=3\r\n"
"GraphicsOptionPhysicsQuality=3\r\n"
"GraphicsOptionPortraits=2\r\n"
"GraphicsOptionPostProcessing=3\r\n"
"GraphicsOptionReflections=2\r\n"
"GraphicsOptionShaderDetail=3\r\n"
"GraphicsOptionShadowQuality=3\r\n"
"GraphicsOptionTerrainQuality=3\r\n"
"GraphicsOptionTextureQuality[2]=3\r\n"
"halo=1\r\n"
"hdr=1\r\n"
"hdr8bit=0\r\n"
"lightingLevel=3\r\n"
"lightmap=0\r\n"
"lightmapcastshadows=1\r\n"
"localao=1\r\n"
"localight=1\r\n"
"localShadows=1\r\n"
"lowQualityMovies=0\r\n"
"normalmap=1\r\n"
"parallax=1\r\n"
"parallaxshadow=1\r\n"
"particlelod=3\r\n"
"particleobjects=1.000000\r\n"
"particleterrain=1.000000\r\n"
"physicsdensity=16\r\n"
"pixellight=1\r\n"
"portraits3d=1\r\n"
"preBlendedCreep=0\r\n"
"ribbonlod=3\r\n"
"sampleFOW=1\r\n"
"shadowmapsize=2048\r\n"
"shadows=1\r\n"
"simplifiedCloaking=0\r\n"
"simplifiedShaders=0\r\n"
"soft=1\r\n"
"softshadows=1\r\n"
"softshadowTaps=12\r\n"
"spec=1\r\n"
"splatlod=3\r\n"
"targettexeldensity=10.600000\r\n"
"TerrainTextureHiResCacheSize=24\r\n"
"TerrainTextureLowResCacheSize=50\r\n"
"TerrainTextureSize=1024\r\n"
"texQualityLevel=3\r\n"
"texSpace=1024\r\n"
"textureBasedFOW=1\r\n"
"transparentshadows=1\r\n"
"treadlod=3\r\n"
"useLowqualitymodels=0\r\n"
"watercaustics=1\r\n"
"waterflipbook=0\r\n"
"waterreflection=1\r\n"
"waterrendertargetformat=3\r\n"
"waterrendertargetsize=1024\r\n"
"watershadow=1\r\n"
/* VARS_FOLD fin */
;

/* Copie par blocs src → dst (dst créé/écrasé). 0 = OK, -1 = échec (journalisé par l'appelant). */
static int copier_fichier(const char *src, const char *dst)
{
    FILE *f = fopen(src, "rb");
    if (!f) return -1;
    FILE *g = fopen(dst, "wb");
    int ok = g != NULL;
    char b[4096]; size_t k;
    while (ok && (k = fread(b, 1, sizeof b, f)) > 0) ok = fwrite(b, 1, k, g) == k;
    ok = ok && !ferror(f);
    if (g && fclose(g)) ok = 0;                      /* fermé même après un échec d'écriture */
    fclose(f);
    return ok ? 0 : -1;
}

/* Vrai si le fichier contient « GraphicsOption » (section des réglages graphiques du jeu : écarte un
 * fichier quelconque posé par l'utilisateur à la place). ≤ 64 Kio, lu en une fois. */
static int contient_graphicsoption(const char *chemin)
{
    static char b[64 * 1024 + 1];                    /* une fois au lancement, avant le fil du jeu */
    FILE *f = fopen(chemin, "rb");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return strstr(b, "GraphicsOption") != NULL;
}

/* Étape 1 (avant l'effacement de sc2-c) : les réglages changés en jeu au run précédent → /sdcard, par
 * fichier temporaire + rename (un /sdcard à moitié écrit ne doit jamais passer pour un réglage). Un refus
 * (figé, absent, taille hors bornes, sans « GraphicsOption », run de test précédent, /sdcard plus récent)
 * est journalisé et n'arrête pas le lancement. Les deux derniers protègent les réglages de l'utilisateur :
 * le marqueur .sc2-run-de-test du sc2-c (23/10 : l'Ultra de l'étape 4 de la séance du Fold avait remplacé
 * son Variables.txt) et un Variables.txt posé par le lanceur Java entre deux runs. La règle est pure
 * (sc2_vars_regle.h, testée sur l'hôte) : sauver_vars ne fait que la nourrir et journaliser. */
static void sauver_vars(const char *src, const char *dst, const char *marqueur)
{
    const char *f = getenv("SC2_VARIABLES_FIGE");
    struct stat st;
    int src_ok = !stat(src, &st) && S_ISREG(st.st_mode);
    struct stat md;
    int marque_ok = !stat(marqueur, &md) && S_ISREG(md.st_mode);
    struct stat dd;
    int dst_ok = !stat(dst, &dd) && S_ISREG(dd.st_mode);
    int d = sc2_vars_decision(f && !strcmp(f, "1"), src_ok, src_ok ? (long)st.st_size : 0,
                              src_ok && contient_graphicsoption(src), marque_ok, dst_ok,
                              src_ok ? (long long)st.st_mtime : 0, dst_ok ? (long long)dd.st_mtime : 0);
    switch (d) {
    case SC2_VARS_FIGE:
        LOGI("[VARS] SC2_VARIABLES_FIGE=1 : réglages du jeu NON sauvegardés (préréglage de %s figé)", dst);
        return;
    case SC2_VARS_ABSENT:
        LOGI("[VARS] pas de réglages à sauvegarder (%s absent)", src);
        return;
    case SC2_VARS_TAILLE:
        LOGI("[VARS] réglages NON sauvegardés (%s : %ld o)", src, (long)st.st_size);
        return;
    case SC2_VARS_SANS_GRAPHICSOPTION:
        LOGI("[VARS] réglages NON sauvegardés (%s sans « GraphicsOption »)", src);
        return;
    case SC2_VARS_RUN_DE_TEST: {
        char raison[128] = "";
        FILE *m = fopen(marqueur, "r");               /* 1re ligne du marqueur : la raison du run de test */
        if (m) { if (fgets(raison, sizeof raison, m)) raison[strcspn(raison, "\r\n")] = 0; fclose(m); }
        LOGI("[VARS] réglages NON sauvegardés : run de test au lancement précédent (%s)",
             raison[0] ? raison : sc2_vars_decision_texte(d));
        return;
    }
    case SC2_VARS_DST_PLUS_RECENT:
        LOGI("[VARS] réglages NON sauvegardés : /sdcard plus récent que ceux du run précédent (posés par le lanceur ?)");
        return;
    }
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    if (copier_fichier(src, tmp) || rename(tmp, dst)) {
        LOGE("[VARS] sauvegarde des réglages en échec : %s → %s (%s)", src, dst, strerror(errno));
        unlink(tmp);
        return;
    }
    LOGI("[VARS] réglages du jeu sauvegardés → %s (%ld o)", dst, (long)st.st_size);
}

/* Étape 2 (après l'effacement de sc2-c et setenv("SC2_ROOT")) : le préréglage est semé là où le jeu le lit —
 * C:\Users\sc2\Documents\StarCraft II\Variables.txt, soit $SC2_ROOT/Users/sc2/Documents/StarCraft II/
 * Variables.txt (hpath() de recomp-sc2/runtime/sc2_iat.c) — par le runtime commun (V1.5 : sc2_variables_semer,
 * qui a remplacé la copie locale ; même point d'appel, mêmes bornes de 1 o à 64 Kio). Source : SC2_VARIABLES,
 * à défaut /sdcard/SC2Partout/Variables.txt, à défaut (vague 0 bis) le préréglage intégré « fold » ;
 * SC2_VARIABLES=0 : aucun semis, réglages détectés par le jeu. Un SC2_VARIABLES inutilisable n'est PAS
 * remplacé par « fold » : réglages par défaut du jeu, comme avant. Les réglages de l'utilisateur (/sdcard)
 * passent avant « fold » : lastDeviceId y est aligné sur la carte annoncée pour qu'ils soient conservés. */
static void semer_vars(void)
{
    const char *r = getenv("SC2_ROOT"), *src = getenv("SC2_VARIABLES"), *f = getenv("SC2_VARIABLES_FIGE");
    const char *racine = r && *r ? r : "sc2-c";
    int fige = f && !strcmp(f, "1");
    struct stat st;
    if (src && !strcmp(src, "0")) {
        LOGI("[VARS] SC2_VARIABLES=0 : aucun Variables.txt semé (réglages détectés par le jeu)");
        return;
    }
    if (src && *src) { sc2_variables_semer(racine, src, fige); return; }
    if (!stat(VARS_TXT_SD, &st) && S_ISREG(st.st_mode) && st.st_size >= 1 && st.st_size <= SC2_VARIABLES_MAX) {
        sc2_variables_semer(racine, VARS_TXT_SD, fige);
        return;
    }
    LOGI("[VARS] aucun Variables.txt fourni (%s absent) : préréglage intégré « fold »", VARS_TXT_SD);
    sc2_variables_semer_texte(racine, VARS_FOLD, "le préréglage intégré « fold » (Ultra)", fige);
}

static int rm_un(const char *p, const struct stat *st, int type, struct FTW *f)
{ (void)st; (void)type; (void)f; return remove(p); }

static int configurer(void)
{
    const char *ext = g_app->activity->externalDataPath, *intr = g_app->activity->internalDataPath;
    char p[1024], marqueur[1100];
    mkdir(ext, 0755);
    snprintf(g_journal, sizeof g_journal, "%s/sc2-partout.log", ext);
    rediriger_sorties();
    lire_env("/sdcard/SC2Partout/sc2.env");
    snprintf(p, sizeof p, "%s/sc2.env", ext); lire_env(p);
    sc2_phases_init(&g_ph, getenv("SC2_PHASES"));
    for (int i = 2; i < g_ph.n; i++) LOGI("[PHASE] règle : %s = « %s »", g_ph.nom[i], g_ph.motif[i]);
    atomic_store(&g_ph_pret, 1);

    const char *choix[3] = { getenv("SC2_INSTALL"), "/sdcard/SC2Partout/install", NULL };
    char app_install[768]; snprintf(app_install, sizeof app_install, "%s/install", ext); choix[2] = app_install;
    g_install[0] = 0;
    for (int i = 0; i < 3 && !g_install[0]; i++)
        if (choix[i] && *choix[i] && lisible(choix[i])) snprintf(g_install, sizeof g_install, "%s", choix[i]);
    if (!g_install[0]) {
        LOGE("données introuvables : ni /sdcard/SC2Partout/install (accès « tous les fichiers » accordé ?) ni %s "
             "ne contiennent Versions/Base15405/SC2.exe — cf. docs/PIPELINE-ANDROID.md §4", app_install);
        return -1;
    }
    setenv("SC2_INSTALL", g_install, 1);
    snprintf(g_exe, sizeof g_exe, "%s/Versions/Base15405/SC2.exe", g_install);
    /* Variables.txt persistant : les réglages du run précédent sont sortis de sc2-c AVANT son effacement
     * (le marqueur .sc2-run-de-test y est lu s'il y est : run de test précédent → rien de recopié). */
    snprintf(p, sizeof p, "%s/sc2-c/Users/sc2/Documents/StarCraft II/Variables.txt", intr);
    snprintf(marqueur, sizeof marqueur, "%s/sc2-c/" SC2_VARS_MARQUE_TEST, intr);
    sauver_vars(p, VARS_TXT_SD, marqueur);
    /* Racine C:\ neuve à chaque lancement (passe 6a : les runtimeCache~NN.bin accumulés font
     * déborder la pile hôte au démarrage). */
    snprintf(p, sizeof p, "%s/sc2-c", intr);
    nftw(p, rm_un, 16, FTW_DEPTH | FTW_PHYS);
    mkdir(intr, 0755);
    setenv("SC2_ROOT", p, 1);
    /* Passe 10t : Banks/ et Saves/ (progression, sauvegardes) persistants — défaut /sdcard/
     * SC2Partout/StarCraft II ; sc2.env (lu plus haut) ou l'environnement peuvent poser
     * SC2_DOCUMENTS autre part, valeur vide = pas de persistance. */
    if (!getenv("SC2_DOCUMENTS")) setenv("SC2_DOCUMENTS", "/sdcard/SC2Partout/StarCraft II", 1);
    /* Vague 0 bis : profil GPU annoncé au jeu (V2.5, lu par recomp-sc2/runtime/sc2_d3d9.c ; sans effet tant que le
     * runtime ne le lit pas) — « dx10 » par défaut sur Android ; « historique » (8600 GT, 512 Mo) dans sc2.env pour
     * l'ancien comportement. AVANT semer_vars : le lastDeviceId du Variables.txt semé est aligné sur le DeviceId de la
     * carte annoncée (sc2_variables_device_id) — ajouté au préréglage « fold », remplacé dans celui de /sdcard écrit
     * par le jeu sous la 8600 GT (1026 → 1507) —, sinon le jeu re-détecte ses réglages et écrase ceux de l'utilisateur.
     * Suppose que sc2_d3d9.c annonce bien ce profil (fusion de caps-etats AVANT ce changement). */
    if (!getenv("SC2_GPU_PROFIL")) setenv("SC2_GPU_PROFIL", "dx10", 1);
    semer_vars();                                    /* préréglage → C:\ neuf (SC2_VARIABLES, /sdcard, sinon « fold ») */
    /* Run de test (23/10) : marqueur dans le sc2-c NEUF — le lancement suivant le lira dans sauver_vars
     * (ci-dessus) avant d'effacer sc2-c, et ne recopiera pas les réglages de ce run vers /sdcard. */
    {
        const char *raison = sc2_vars_raison_test(getenv("SC2_BANC_ENTREES"), getenv("SC2_DOCUMENTS"),
                                                  getenv("SC2_VARIABLES"), getenv("SC2_VARIABLES_FIGE"));
        if (raison) {
            mkdir(p, 0755);                            /* sc2-c peut manquer (semis sans effet, ex. SC2_VARIABLES=0) */
            snprintf(marqueur, sizeof marqueur, "%s/" SC2_VARS_MARQUE_TEST, p);
            FILE *m = fopen(marqueur, "w");
            if (m) { fprintf(m, "%s\n", raison); fclose(m); }
            else LOGE("[VARS] marqueur de run de test non posé : %s (%s)", marqueur, strerror(errno));
            LOGI("[VARS] run de test (%s) : ses réglages ne seront pas recopiés dans /sdcard", raison);
        }
    }
    /* Passe 10s : cache de pipelines Vulkan HORS de sc2-c (effacé à chaque lancement), sinon les shaders sont
     * recompilés à chaque partie (322 pipelines, 3,5 s cumulées, jusqu'à 35 ms d'à-coup sur le Fold). */
    snprintf(p, sizeof p, "%s/vk-pipelines.bin", intr);
    if (!getenv("SC2_VK_CACHE_PIPELINES")) setenv("SC2_VK_CACHE_PIPELINES", p, 1);
    snprintf(p, sizeof p, "%s/frames", ext);
    if (!getenv("SC2_FRAMES_DIR")) setenv("SC2_FRAMES_DIR", p, 1);
    /* Vague 0 bis (22/10) : plus aucune capture PPM en jeu par défaut (à-coup de relecture toutes les 1,7 s,
     * 6-8 Go écrits par heure) ; les scripts de banc et du Fold posent SC2_FRAMES=1 explicitement (V0.1). */
    if (!getenv("SC2_FRAMES")) setenv("SC2_FRAMES", "0", 1);
    /* 21/10 : plein écran par défaut — « auto » = mode standard au rapport le plus proche de la fenêtre (lancer_jeu) :
     * 1280x1024 sur l'écran intérieur du Fold (1280x720 y laissait ≈ 37 % de bandes noires), 1280x720 sur l'extérieur.
     * SC2_ECRAN=1280x720 dans sc2.env pour l'ancien comportement (scénarios scriptés en coordonnées 1280x720). */
    if (!getenv("SC2_ECRAN")) setenv("SC2_ECRAN", "auto", 1);
#ifdef SC2_SWAPCHAIN
    /* APK « vk » (passe 7b) : swapchain par défaut ; SC2_SWAPCHAIN=0 dans sc2.env → copie CPU */
    if (!getenv("SC2_SWAPCHAIN")) setenv("SC2_SWAPCHAIN", "1", 1);
#endif
    /* Initialiseurs stricts (recomp-sc2/inits-stricts.txt, init Storm → allocateur global) : comme
     * le run de référence Linux. Embarqué dans l'APK (assets/), posé dans le stockage interne. */
    snprintf(p, sizeof p, "%s/inits-stricts.txt", intr);
    if (!getenv("SC2_STRICT_INITS") && !extraire_asset("inits-stricts.txt", p)) setenv("SC2_STRICT_INITS", p, 1);
    /* Passe 7l : polices système de repli (Liberation, équivalents métriques d'Arial/Times/Courier), embarquées
     * dans l'APK (assets/polices, scripts/build-apk.sh) : posées dans le stockage interne ; le runtime les lie
     * dans C:\Windows\Fonts (CSIDL_FONTS) quand le moteur y cherche arial.ttf. SC2_POLICES=0 : aucune. */
    if (!getenv("SC2_POLICES")) {
        static const char *const pol[] = { "LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf", "LiberationSans-Italic.ttf",
                                           "LiberationSans-BoldItalic.ttf", "LiberationSerif-Regular.ttf", "LiberationMono-Regular.ttf" };
        char q[1200], a[96];
        int n = 0;
        snprintf(p, sizeof p, "%s/polices", intr);
        mkdir(p, 0755);
        for (size_t i = 0; i < sizeof pol / sizeof pol[0]; i++) {
            snprintf(a, sizeof a, "polices/%s", pol[i]); snprintf(q, sizeof q, "%s/%s", p, pol[i]);
            if (!extraire_asset(a, q)) n++;
        }
        if (n) setenv("SC2_POLICES", p, 1);
        printf("[ANDROID] polices système de repli : %d posées dans %s\n", n, p);
    }
    if (chdir(intr)) LOGE("chdir(%s) : %s", intr, strerror(errno));
    printf("[ANDROID] SC2_INSTALL=%s SC2_ROOT=%s SC2_DOCUMENTS=%s SC2_FRAMES_DIR=%s SC2_FRAMES=%s SC2_GPU_PROFIL=%s SC2_ECRAN=%s "
           "SC2_STRICT_INITS=%s\n", g_install, getenv("SC2_ROOT"), getenv("SC2_DOCUMENTS"), getenv("SC2_FRAMES_DIR"),
           getenv("SC2_FRAMES"), getenv("SC2_GPU_PROFIL"), getenv("SC2_ECRAN"),
           getenv("SC2_STRICT_INITS") ? getenv("SC2_STRICT_INITS") : "(aucun)");
    return 0;
}

/* ---- fenêtre ----------------------------------------------------------------------------- */

static pthread_mutex_t g_fen_mu = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *g_fen;
static uint32_t g_jw = 1280, g_jh = 720;            /* back buffer du jeu */
static int32_t g_bw, g_bh, g_ox, g_oy;              /* tampon de fenêtre (bandes noires comprises) */
static int32_t g_fw, g_fh;                          /* fenêtre en pixels physiques */
static int g_geo_ok;
/* Passe 9d : compteurs de la copie CPU (sous g_fen_mu ; lus au bilan de sortie). Un écran noir se lit ainsi :
 * tampons postés > 0 → le chemin CPU livre à SurfaceFlinger ; échecs de verrou > 0 → ANativeWindow_lock refuse ;
 * fenêtres perdues > 0 et Present sans fenêtre > 0 (accroches) → le jeu a rendu sans fenêtre (écran éteint…). */
static uint64_t g_postes, g_echecs_verrou, g_echecs_post, g_fen_perdues, g_geo_vide;
/* Passe 10a (sous g_fen_mu), remis à zéro à chaque INIT_WINDOW : refus du verrou sur CETTE fenêtre, géométrie
 * reposée après le 3e (une fois par fenêtre : jamais de boucle), 1er tampon posté sur cette fenêtre. Globaux :
 * géométries reposées, tampons de format inattendu (postés sans image plutôt qu'écrits hors du tampon). */
static unsigned g_verrou_ko_fen; static int g_repose_fen, g_poste_fen;
static uint64_t g_reposes, g_format_autre;

/* Passe 10a : appelé par sc2_vk_presente.c (fil du jeu) quand la swapchain bascule sur la copie CPU — la surface
 * Vulkan détruite, la fenêtre a perdu sa géométrie de tampon : peindre() la repose au prochain Present. */
void sc2a_geo_invalider(void)
{
    pthread_mutex_lock(&g_fen_mu); g_geo_ok = 0; g_verrou_ko_fen = 0; g_repose_fen = 0; pthread_mutex_unlock(&g_fen_mu);
}

static void geometrie(void)                         /* sous g_fen_mu */
{
    g_fw = ANativeWindow_getWidth(g_fen); g_fh = ANativeWindow_getHeight(g_fen);
    if (g_fw <= 0 || g_fh <= 0) {                   /* passe 9d : sinon peindre() ne verrouille jamais, sans trace */
        if (g_geo_vide++ < 3) LOGE("[AFFICHAGE] fenêtre de taille %dx%d : géométrie NON posée, rien ne sera peint "
                                   "(cas n°%llu ; suivants comptés)", g_fw, g_fh, (unsigned long long)g_geo_vide);
        return;
    }
    if ((int64_t)g_fw * g_jh >= (int64_t)g_fh * g_jw) {           /* fenêtre plus large : bandes verticales */
        g_bh = (int32_t)g_jh; g_bw = (int32_t)((int64_t)g_jh * g_fw / g_fh);
    } else {
        g_bw = (int32_t)g_jw; g_bh = (int32_t)((int64_t)g_jw * g_fh / g_fw);
    }
    if (g_bw < (int32_t)g_jw) g_bw = (int32_t)g_jw;
    if (g_bh < (int32_t)g_jh) g_bh = (int32_t)g_jh;
    g_ox = (g_bw - (int32_t)g_jw) / 2; g_oy = (g_bh - (int32_t)g_jh) / 2;
    int32_t rg = ANativeWindow_setBuffersGeometry(g_fen, g_bw, g_bh, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    if (rg) LOGE("[AFFICHAGE] ANativeWindow_setBuffersGeometry(%dx%d, RGBA_8888) : échec %d (%s)", g_bw, g_bh, rg, strerror(-rg));
    g_geo_ok = 1;
    LOGI("fenêtre %dx%d → tampon %dx%d, image du jeu %ux%u en (%d, %d)", g_fw, g_fh, g_bw, g_bh, g_jw, g_jh, g_ox, g_oy);
}

/* Passe 10a : tampon d'un autre format que RGBA_8888 / RGBX_8888 — la géométrie RGBA n'a pas pris (setBuffersGeometry
 * en échec : la fenêtre d'une NativeActivity est en RGB_565 par défaut). Avant, 4 octets par pixel étaient écrits
 * quel que soit le format : un tampon de 2 octets par pixel débordait de moitié (mémoire écrasée, run tué). */
#define FMT_BGRA_8888 5                              /* HAL_PIXEL_FORMAT_BGRA_8888 (absent des énumérations du NDK) */
static void peindre_converti(const ANativeWindow_Buffer *b, const uint8_t *rgba, uint32_t couleur)
{
    const int32_t f = b->format, opp = f == AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM ? 2 : f == AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM ? 3 : 4;
    for (int32_t y = 0; y < b->height; y++) {
        uint8_t *l = (uint8_t *)b->bits + (size_t)y * b->stride * opp;
        int32_t sy = y - g_oy;
        const uint32_t *s = rgba && sy >= 0 && sy < (int32_t)g_jh ? (const uint32_t *)rgba + (size_t)sy * g_jw : NULL;
        for (int32_t x = 0; x < b->width; x++) {
            int32_t sx = x - g_ox;
            uint32_t v = !s ? couleur : sx < 0 || sx >= (int32_t)g_jw ? 0xFF000000u : s[sx] | 0xFF000000u;
            uint32_t r = v & 0xFF, g = (v >> 8) & 0xFF, bl = (v >> 16) & 0xFF;
            if (opp == 2) ((uint16_t *)l)[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
            else if (opp == 3) { l[3 * x] = (uint8_t)r; l[3 * x + 1] = (uint8_t)g; l[3 * x + 2] = (uint8_t)bl; }
            else ((uint32_t *)l)[x] = (v & 0xFF00FF00u) | bl | (r << 16);                     /* BGRA */
        }
    }
}

/* Remplit la fenêtre : image du jeu (rgba != NULL) ou couleur unie (écran d'erreur). */
static void peindre(const uint8_t *rgba, uint32_t couleur)
{
    pthread_mutex_lock(&g_fen_mu);
    if (g_fen && !g_geo_ok) geometrie();
    ANativeWindow_Buffer b;
    int32_t rl = g_fen && g_geo_ok ? ANativeWindow_lock(g_fen, &b, NULL) : 1;
    if (rl < 0) {                                    /* passe 9d : refus du verrou — 3 premiers, puis par puissances de 10 */
        uint64_t k = ++g_echecs_verrou;
        if (k <= 3 || k == 10 || k == 100 || k == 1000 || k == 10000 || k == 100000)
            LOGE("[AFFICHAGE] ANativeWindow_lock : échec %d (%s) — tampon NON posté (échec n°%llu, %llu postés avant)",
                 rl, strerror(-rl), (unsigned long long)k, (unsigned long long)g_postes);
        /* passe 10a : au 3e refus sur CETTE fenêtre, la géométrie est reposée UNE fois, et le verrou réessayé aussitôt */
        if (++g_verrou_ko_fen >= 3 && !g_repose_fen) {
            g_repose_fen = 1; g_reposes++;
            int32_t rg = ANativeWindow_setBuffersGeometry(g_fen, g_bw, g_bh, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
            rl = ANativeWindow_lock(g_fen, &b, NULL);
            LOGE("[AFFICHAGE] verrou refusé %u fois sur cette fenêtre : géométrie %dx%d RGBA_8888 reposée (%d), nouvel essai du "
                 "verrou → %d (une seule fois par fenêtre)", g_verrou_ko_fen, g_bw, g_bh, rg, rl);
        }
    }
    if (!rl) {
        static int dit;                              /* passe 9d : le tampon réellement obtenu, une fois */
        if (!dit++) LOGI("[AFFICHAGE] 1er tampon de fenêtre verrouillé : %dx%d, stride %d, format %d (attendu %d = RGBA_8888)",
                         b.width, b.height, b.stride, b.format, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        if (b.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM || b.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM) {
            for (int32_t y = 0; y < b.height; y++) {
                uint32_t *l = (uint32_t *)b.bits + (size_t)y * b.stride;
                int32_t sy = y - g_oy;
                if (!rgba || sy < 0 || sy >= (int32_t)g_jh) { for (int32_t x = 0; x < b.width; x++) l[x] = couleur; continue; }
                const uint32_t *s = (const uint32_t *)rgba + (size_t)sy * g_jw;
                /* passe 10o : bornes calculées une fois par ligne (la 7a testait chaque pixel) ; le milieu est une boucle
                 * sans branche que le compilateur vectorise (NEON) */
                int32_t x0 = g_ox < 0 ? 0 : g_ox > b.width ? b.width : g_ox;
                int32_t x1 = g_ox + (int32_t)g_jw; x1 = x1 < x0 ? x0 : x1 > b.width ? b.width : x1;
                for (int32_t x = 0; x < x0; x++) l[x] = 0xFF000000u;
                const uint32_t *sm = s + (x0 - g_ox); uint32_t *lm = l + x0;
                for (int32_t x = 0, m = x1 - x0; x < m; x++) lm[x] = sm[x] | 0xFF000000u;   /* R,G,B,A octets */
                for (int32_t x = x1; x < b.width; x++) l[x] = 0xFF000000u;
            }
        } else if (b.format == AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM || b.format == AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM
                   || b.format == FMT_BGRA_8888) {
            if (!g_format_autre++) LOGE("[AFFICHAGE] tampon de fenêtre au format %d (pas RGBA_8888) : image convertie", b.format);
            peindre_converti(&b, rgba, couleur);
        } else if (!g_format_autre++)                /* format inconnu : rien n'est écrit (taille de pixel inconnue) */
            LOGE("[AFFICHAGE] tampon de fenêtre au format %d inconnu : posté SANS image (rien écrit hors du tampon)", b.format);
        int32_t rp = ANativeWindow_unlockAndPost(g_fen);
        if (rp) { if (g_echecs_post++ < 3) LOGE("[AFFICHAGE] ANativeWindow_unlockAndPost : échec %d (%s)", rp, strerror(-rp)); }
        else {
            if (!g_postes++) LOGI("[AFFICHAGE] 1er tampon posté à SurfaceFlinger (copie CPU)");
            /* passe 10a : la preuve que la présentation a repris d'elle-même sur une fenêtre revenue */
            if (!g_poste_fen++ && g_fen_perdues)
                LOGI("[AFFICHAGE] 1er tampon posté sur la fenêtre revenue (après %llu fenêtre(s) perdue(s), %llu tampons postés en tout)",
                     (unsigned long long)g_fen_perdues, (unsigned long long)g_postes);
        }
    }
    pthread_mutex_unlock(&g_fen_mu);
}

static atomic_int g_par_gpu;                         /* passe 7o : la dernière frame venait de la swapchain */
int  sc2a_frame_voulue(unsigned n) { (void)n; return g_fen != NULL; }

/* Passe 7o (perf Fold) : cadence affichée, une ligne logcat toutes les 5 s
 *   [FPS] 23.8 images/s sur 5.0 s (frames 1201→1320, swapchain fifo) ; moyenne depuis la 1re : 22.9
 * — à lire avec `adb logcat -s SC2Partout | grep FPS`. SC2_FPS_PERIODE=<s> change la période (0 : muet). */
static void compter_fps(unsigned n, int gpu)
{
    static struct timespec t0, tp; static unsigned n0, np; static double periode = -1;
    if (periode < 0) { const char *e = getenv("SC2_FPS_PERIODE"); periode = e ? atof(e) : 5.0; }
    if (periode <= 0) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    if (!n0) { t0 = tp = t; n0 = np = n; return; }
    double dt = (double)(t.tv_sec - tp.tv_sec) + (double)(t.tv_nsec - tp.tv_nsec) / 1e9;
    if (dt < periode) return;
    double tt = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
    /* passe 10o : la voie et sa cause (une copie CPU dit pourquoi), et le profil de la boucle (sc2a_profil_texte) */
    char voie[160], prof[200];
#ifdef SC2_SWAPCHAIN
    sc2v_etat_court(voie, sizeof voie);
#else
    snprintf(voie, sizeof voie, "APK sans swapchain (SC2_APK_SWAPCHAIN=0)");
#endif
    sc2a_profil_texte(prof, sizeof prof);
    LOGI("[FPS] %.1f images/s sur %.1f s (frames %u→%u, %s ; %s) ; moyenne depuis la 1re : %.1f ; phase %s ; %s", (n - np) / dt, dt,
         np, n, gpu ? "swapchain" : "copie CPU", voie, tt > 0 ? (n - n0) / tt : 0.0, phase_courante(), prof);
    tp = t; np = n;
}

void sc2a_frame(unsigned n, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    if (w != g_jw || h != g_jh) return;
    if (rgba) peindre(rgba, 0);                      /* NULL : déjà présentée par la swapchain (7b) */
    atomic_store(&g_par_gpu, rgba == NULL);
    unsigned k = atomic_fetch_add(&g_frames_affichees, 1) + 1;
    if (k == 1 || k == 10 || k % 600 == 0) LOGI("frame n°%u affichée à l'écran (%u affichées)", n, k);
    phases_suivre(NULL, k);
    compter_fps(n, rgba == NULL);
}

/* Toucher (pixels de fenêtre) → pixel CLIENT du jeu. Passe 7o : on inverse la géométrie du chemin qui a
 * présenté la DERNIÈRE frame (android/jni/sc2_geo.h), au lieu de la recalculer depuis la seule taille de
 * fenêtre (7d) — celle-ci supposait une échelle uniforme et tronquait : juste à l'échelle entière
 * (émulateur 2400x1080 ×1,5), faux d'1 à 2 px sur 50 à 83 % de l'image sur le Fold 8 (test-geo.c) :
 *   - swapchain : rectangle exact du blit (sc2v_geometrie + sc2_geo_ajuster), surface étirée à la fenêtre ;
 *   - copie CPU : image en (g_ox, g_oy) du tampon g_bw×g_bh, étiré par le compositeur à la fenêtre (ex.
 *     1280x720 en (160, 0) de 1600x720) ;
 *   - rien encore présenté : image centrée à son rapport (ce que fera la swapchain). */
static sc2_geo geo_courante(void)
{
    pthread_mutex_lock(&g_fen_mu);
    int32_t fw = g_fen ? ANativeWindow_getWidth(g_fen) : 0, fh = g_fen ? ANativeWindow_getHeight(g_fen) : 0;
    sc2_geo g = { 0 };
    if (fw > 0 && fh > 0) {
#ifdef SC2_SWAPCHAIN
        uint32_t sw, sh, dw, dh;
        barre_geo bg;                    /* 21/10 : rectangle réel du blit (image calée en haut si la barre est là) */
        if (atomic_load(&g_par_gpu) && !sc2v_geometrie(&sw, &sh, &dw, &dh) && sw == g_jw && sh == g_jh)
            g = !sc2v_barre_geo(&bg) ? sc2_geo_swapchain_rect(g_jw, g_jh, bg.jeu.x, bg.jeu.y, (uint32_t)bg.jeu.w,
                                                             (uint32_t)bg.jeu.h, dw, dh, (uint32_t)fw, (uint32_t)fh)
                                     : sc2_geo_swapchain(g_jw, g_jh, dw, dh, (uint32_t)fw, (uint32_t)fh);
        else
#endif
        if (!atomic_load(&g_par_gpu) && g_geo_ok && g_fw == fw && g_fh == fh)
            g = sc2_geo_tampon(g_jw, g_jh, (uint32_t)g_bw, (uint32_t)g_bh, g_ox, g_oy, (uint32_t)fw, (uint32_t)fh);
        else
            g = sc2_geo_swapchain(g_jw, g_jh, (uint32_t)fw, (uint32_t)fh, (uint32_t)fw, (uint32_t)fh);
    }
    pthread_mutex_unlock(&g_fen_mu);
    return g;
}

static void vers_jeu(float fx, float fy, int32_t *x, int32_t *y)
{
    sc2_geo g = geo_courante();
    if (!g.ok) { *x = (int32_t)fx; *y = (int32_t)fy; return; }
    int dedans = sc2_geo_vers_jeu(&g, fx, fy, x, y);
    static unsigned n;
    if (n++ < 40 || !dedans)   /* trace de la conversion (preuve émulateur) ; bande noire : toujours */
        LOGI("[TOUCHER] écran (%.1f, %.1f) → client (%d, %d)%s [image en (%.1f, %.1f) ×%.4f×%.4f]", fx, fy, *x, *y,
             dedans ? "" : " BANDE NOIRE (ramené au bord)", g.x0, g.y0, g.sx, g.sy);
}

/* ---- fil du jeu -------------------------------------------------------------------------- */

static atomic_int g_jeu_lance, g_jeu_fini;

void sc2a_avant_sortie(void)
{
    uint64_t inj, serv; sc2a_bilan_entrees(&inj, &serv);
    printf("[ANDROID] sortie : %u frames affichées, %llu événements injectés dans sc2_input (runtime 5f), %llu déplacements "
           "fusionnés ; les messages livrés au jeu sont les lignes [INPUT]\n",
           atomic_load(&g_frames_affichees), (unsigned long long)inj, (unsigned long long)serv);
    {   /* passe 9d : ce que la copie CPU a livré à la fenêtre, et ce que le jeu a rendu sans fenêtre */
        uint64_t sf, cr; sc2a_bilan_presents(&sf, &cr);
        pthread_mutex_lock(&g_fen_mu);
        printf("[ANDROID] affichage : %u Present ; copie CPU : tampons postés %llu / échecs de verrou %llu / échecs de post "
               "%llu / géométries vides %llu ; fenêtres perdues %llu ; Present sans fenêtre %llu ; cibles refusées %llu\n",
               sc2a_presents(), (unsigned long long)g_postes, (unsigned long long)g_echecs_verrou,
               (unsigned long long)g_echecs_post, (unsigned long long)g_geo_vide, (unsigned long long)g_fen_perdues,
               (unsigned long long)sf, (unsigned long long)cr);
        /* passe 10a : ligne à part — la ligne « affichage : » est lue telle quelle par scripts/verdict-fold.sh (9e) */
        printf("[ANDROID] robustesse : géométries reposées après refus du verrou %llu ; tampons d'un autre format que RGBA_8888 %llu\n",
               (unsigned long long)g_reposes, (unsigned long long)g_format_autre);
        pthread_mutex_unlock(&g_fen_mu);
    }
#ifdef SC2_SWAPCHAIN
    char b[640]; sc2v_bilan(b, sizeof b); printf("[ANDROID] %s\n", b);
#endif
    { char t[640]; sc2tp_bilan(t, sizeof t); if (t[0]) printf("[BILAN] [PRESENT] tout le run : %s\n", t); }   /* passe 7p3 */
    if (video_amc_bilan) { char t[256]; video_amc_bilan(t, sizeof t); printf("[BILAN] %s\n", t); }            /* passe 9s */
    { char t[320]; sc2bt_bilan(t, sizeof t); printf("[ANDROID] %s\n", t); }                                     /* passe 8h */
    sc2aa_arreter();                                 /* flux AAudio fermé, bilan [AUDIO] Android */
    ANativeActivity_finish(g_app->activity);
}

static void *fil_jeu(void *p)
{
    (void)p;
    if (sc2a_zone_init()) { atomic_store(&g_jeu_fini, 1); return NULL; }
    sc2a_entrees_activer();                          /* AVANT sc2_iat_install (cf. sc2_android_hooks.c) */
    sc2aa_demarrer();                                /* passe 7e : sortie AAudio, avant le 1er System::update */
    printf("[ANDROID] zone invitée : 4 Go réservés à %p (g_hote_delta) ; SC2.exe = %s\n", (void *)sc2a_zone(), g_exe);
    char *argv[] = { "recomp-sc2", g_exe, NULL };
    int r = sc2_poc_main(2, argv);                   /* ne revient qu'en cas d'échec de chargement */
    printf("[ANDROID] le runtime a rendu %d sans exit() (image introuvable ?)\n", r);
    atomic_store(&g_jeu_fini, 1);
    return NULL;
}

static void lancer_jeu(void)
{
    if (atomic_exchange(&g_jeu_lance, 1)) return;
    unsigned w, h;
    if (!strcmp(getenv("SC2_ECRAN"), "auto") && g_app && g_app->window) {
        /* passe 7o : mode standard du jeu au rapport le plus proche de la fenêtre (Fold intérieur → 1280x1024) */
        uint32_t aw, ah;
        sc2_geo_mode_auto((uint32_t)ANativeWindow_getWidth(g_app->window), (uint32_t)ANativeWindow_getHeight(g_app->window), &aw, &ah);
        char v[32]; snprintf(v, sizeof v, "%ux%u", aw, ah); setenv("SC2_ECRAN", v, 1);
        printf("[ANDROID] SC2_ECRAN=auto → %s (fenêtre %dx%d)\n", v, ANativeWindow_getWidth(g_app->window),
               ANativeWindow_getHeight(g_app->window));
    } else if (!strcmp(getenv("SC2_ECRAN"), "auto")) {          /* pas encore de fenêtre : repli explicite */
        setenv("SC2_ECRAN", "1280x720", 1);
        printf("[ANDROID] SC2_ECRAN=auto sans fenêtre au lancement → 1280x720\n");
    }
    if (sscanf(getenv("SC2_ECRAN"), "%ux%u", &w, &h) == 2) { g_jw = w; g_jh = h; }
    sc2a_taille_ecran(g_jw, g_jh);
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20);
    pthread_t t;
    if (pthread_create(&t, &at, fil_jeu, NULL)) { LOGE("pthread_create(fil du jeu) a échoué"); atomic_store(&g_jeu_fini, 1); }
    else { pthread_setname_np(t, "sc2-jeu"); pthread_detach(t); }
    pthread_attr_destroy(&at);
}

/* ---- événements -------------------------------------------------------------------------- */

static int g_config_ok = -1;
static struct timespec g_t_sortie;

/* 21/10 : plein écran immersif (fr.sc2partout.Immersif, fil de l'interface) — les barres système recouvraient la barre
 * de raccourcis. Classe chargée par le chargeur de l'activité (FindClass d'un fil natif ne voit pas les classes de l'app).
 * SC2_IMMERSIF=0 : rien. */
static void immersif(void)
{
    const char *e = getenv("SC2_IMMERSIF");
    if ((e && *e == '0') || !g_app || !g_app->activity) return;
    JavaVM *vm = g_app->activity->vm; JNIEnv *env;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return;
    jobject act = g_app->activity->clazz;
    jclass ca = (*env)->GetObjectClass(env, act);
    jmethodID gcl = (*env)->GetMethodID(env, ca, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject cl = (*env)->CallObjectMethod(env, act, gcl);
    jclass ccl = (*env)->FindClass(env, "java/lang/ClassLoader");
    jmethodID lc = (*env)->GetMethodID(env, ccl, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring nom = (*env)->NewStringUTF(env, "fr.sc2partout.Immersif");
    jclass ci = (jclass)(*env)->CallObjectMethod(env, cl, lc, nom);
    if ((*env)->ExceptionCheck(env) || !ci) {
        (*env)->ExceptionClear(env); LOGI("[IMMERSIF] classe fr.sc2partout.Immersif introuvable");
    } else {
        jmethodID ap = (*env)->GetStaticMethodID(env, ci, "appliquer", "(Landroid/app/Activity;)V");
        if (ap) (*env)->CallStaticVoidMethod(env, ci, ap, act);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); LOGI("[IMMERSIF] échec de l'appel"); }
        else { static int dit; if (!dit++) LOGI("[IMMERSIF] barres système masquées (plein écran immersif)"); }
    }
    (*env)->DeleteLocalRef(env, nom); (*env)->DeleteLocalRef(env, ca); (*env)->DeleteLocalRef(env, ccl);
    if (cl) (*env)->DeleteLocalRef(env, cl);
    if (ci) (*env)->DeleteLocalRef(env, ci);
    (*vm)->DetachCurrentThread(vm);
}

static void cmd(struct android_app *app, int32_t c)
{
    switch (c) {
    case APP_CMD_GAINED_FOCUS: immersif(); break;      /* 21/10 : après un retour d'arrière-plan ou un dialogue */
    case APP_CMD_PAUSE: case APP_CMD_RESUME: {     /* 21/10 : arrière-plan = jeu suspendu (SC2_PAUSE=0 : continue) */
        static int lu, coupe;
        if (!lu) { const char *e = getenv("SC2_PAUSE"); coupe = e && *e == '0'; lu = 1; }
        if (!coupe && atomic_load(&g_jeu_lance) && !atomic_load(&g_jeu_fini)) {
            LOGI("[PAUSE] %s", c == APP_CMD_PAUSE ? "activité en pause : suspension du jeu" : "activité reprise : le jeu repart");
            sc2a_suspendre(c == APP_CMD_PAUSE);
        }
        break;
    }
    case APP_CMD_INIT_WINDOW:
        immersif();                                    /* 21/10 */
#ifdef SC2_SWAPCHAIN
        /* la swapchain se crée sur le fil du jeu au prochain Present ; avant la 1re frame, rien ne
         * verrouille la fenêtre par CPU (sinon elle ne pourrait plus servir à Vulkan). Passe 10o : remise à la
         * swapchain AVANT que g_fen ne soit publiée, sinon la copie CPU (après une bascule) pourrait la verrouiller
         * la première et la rendre inutilisable pour Vulkan (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR). */
        if (g_config_ok == 0) sc2v_fenetre(app->window, 0, 0);
#endif
    {
        /* passe 10a : compteurs de CETTE fenêtre remis à zéro (refus du verrou, géométrie reposée, 1er tampon posté) */
        pthread_mutex_lock(&g_fen_mu); g_fen = app->window; g_geo_ok = 0; g_verrou_ko_fen = 0; g_repose_fen = 0; g_poste_fen = 0;
        uint64_t p = g_postes; pthread_mutex_unlock(&g_fen_mu);
        /* passe 9d : chaque (re)prise de fenêtre, datée par les compteurs du jeu */
        LOGI("[FENETRE] INIT_WINDOW %dx%d — %u Present vus, %u frames affichées, %llu tampons postés", ANativeWindow_getWidth(app->window),
             ANativeWindow_getHeight(app->window), sc2a_presents(), atomic_load(&g_frames_affichees), (unsigned long long)p);
    }
        if (g_config_ok == 0) {
            lancer_jeu();
            /* aide au test (adb shell input tap) : bouton du dialogue (767, 301) client 5f/7l, centre de l'image */
            sc2_geo g = geo_courante();
            int32_t bx, by, cx, cy;
            sc2_geo_vers_ecran(&g, 767, 301, &bx, &by); sc2_geo_vers_ecran(&g, (int32_t)g_jw / 2, (int32_t)g_jh / 2, &cx, &cy);
            LOGI("fenêtre %dx%d, image du jeu %ux%u en (%.1f, %.1f) ×%.4f ; bouton client (767, 301) → adb shell input tap %d %d ;"
                 " centre → adb shell input tap %d %d", ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window),
                 g_jw, g_jh, g.x0, g.y0, g.sx, bx, by, cx, cy);
        }
        else peindre(NULL, 0xFF202080u);           /* rouge sombre (ABGR) : données absentes */
        break;
    case APP_CMD_WINDOW_RESIZED: case APP_CMD_CONFIG_CHANGED:
        pthread_mutex_lock(&g_fen_mu); g_geo_ok = 0; pthread_mutex_unlock(&g_fen_mu);
        {   /* passe 9d : pliage / rotation (20 premiers) */
            static unsigned n;
            if (n++ < 20) LOGI("[FENETRE] %s %dx%d — %u Present vus", c == APP_CMD_WINDOW_RESIZED ? "WINDOW_RESIZED" : "CONFIG_CHANGED",
                               app->window ? ANativeWindow_getWidth(app->window) : 0, app->window ? ANativeWindow_getHeight(app->window) : 0,
                               sc2a_presents());
        }
#ifdef SC2_SWAPCHAIN
        sc2v_taille_changee();                       /* passe 7f : la swapchain suit le pliage / la rotation */
#endif
        break;
    case APP_CMD_TERM_WINDOW:
#ifdef SC2_SWAPCHAIN
        sc2v_fenetre(NULL, 0, 0);                    /* swapchain et surface détruites avant le retour */
#endif
    {
        pthread_mutex_lock(&g_fen_mu); g_fen = NULL; uint64_t k = ++g_fen_perdues, p = g_postes; pthread_mutex_unlock(&g_fen_mu);
        /* passe 9d : le jeu continue de rendre ; plus rien n'est relu ni posté jusqu'au prochain INIT_WINDOW */
        LOGI("[FENETRE] TERM_WINDOW (fenêtre perdue n°%llu) — %u Present vus, %u frames affichées, %llu tampons postés ; "
             "le rendu continue sans fenêtre", (unsigned long long)k, sc2a_presents(), atomic_load(&g_frames_affichees),
             (unsigned long long)p);
    }
        break;
    default: break;
    }
}

static void demander_sortie(const char *pourquoi)
{
    if (sc2a_sortie_demandee()) return;
    LOGI("sortie demandée (%s)", pourquoi);
    if (!atomic_load(&g_jeu_lance) || atomic_load(&g_jeu_fini)) { ANativeActivity_finish(g_app->activity); return; }
    sc2a_demander_sortie();
    clock_gettime(CLOCK_MONOTONIC, &g_t_sortie);
}

/* 21/10 : gestes tactiles — état (fil principal seulement) et livraison des actions au module d'entrées */
static gestes g_gestes;
static int32_t g_doigt0;
static void gestes_livrer(void)
{
    for (int k = 0; k < g_gestes.na; k++) {
        const geste_action *ac = &g_gestes.a[k];
        int32_t x, y;
        vers_jeu(ac->x, ac->y, &x, &y);
        int b = ac->bouton ? 2 : 0;                         /* sc2_input_bouton : 0 gauche, 2 droit */
        if (ac->type == GACT_POSITION) sc2a_deplacer(x, y);
        else sc2a_bouton(b, ac->type == GACT_APPUI, x, y);
        if (ac->bouton && ac->type == GACT_APPUI) LOGI("[GESTE] deux doigts → clic droit en (%d, %d)", x, y);
    }
    g_gestes.na = 0;
}

#ifdef SC2_SWAPCHAIN
/* 21/10 : barre de raccourcis (sc2_barre.h, dessinée par sc2_vk_presente.c) — un toucher la (ré)affiche ; un appui DANS
 * la barre envoie ses touches au jeu et tout le geste (jusqu'au dernier doigt levé) lui appartient. 1 = consommé. */
static int barre_entree(AInputEvent *e, int32_t a)
{
    static int geste_barre;
    if (geste_barre) { if (a == AMOTION_EVENT_ACTION_UP || a == AMOTION_EVENT_ACTION_CANCEL) geste_barre = 0; return 1; }
    if (a != AMOTION_EVENT_ACTION_DOWN) return 0;
    sc2v_barre_voulue(1);
    uint32_t sw, sh, dw, dh;
    pthread_mutex_lock(&g_fen_mu);
    int32_t fw = g_fen ? ANativeWindow_getWidth(g_fen) : 0, fh = g_fen ? ANativeWindow_getHeight(g_fen) : 0;
    pthread_mutex_unlock(&g_fen_mu);
    if (fw <= 0 || fh <= 0 || sc2v_geometrie(&sw, &sh, &dw, &dh)) return 0;
    int32_t sx = (int32_t)(AMotionEvent_getX(e, 0) * (float)dw / (float)fw);
    int32_t sy = (int32_t)(AMotionEvent_getY(e, 0) * (float)dh / (float)fh);
    uint16_t vk[8]; uint8_t bas[8];
    int n = sc2v_barre_appuyer(sx, sy, vk, bas, 8);
    if (n < 0) return 0;                                 /* hors de la barre : geste du jeu */
    geste_barre = 1;
    for (int k = 0; k < n; k++) sc2a_cle(vk[k], bas[k]);
    int t = 0; while (t < n - 1 && (vk[t] == 0x10 || vk[t] == 0x11 || vk[t] == 0x12)) t++;   /* touche hors modificateurs */
    if (n) LOGI("[BARRE] appui en (%d, %d) : VK 0x%02X, %d événement(s) clavier", sx, sy, vk[t], n);
    return 1;
}
#endif

static int32_t entree(struct android_app *app, AInputEvent *e)
{
    (void)app;
    int souris = (AInputEvent_getSource(e) & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        int32_t kc = AKeyEvent_getKeyCode(e), ka = AKeyEvent_getAction(e);
        /* Passe 8h : un clic DROIT non consommé devient « retour » (repli système du bouton secondaire) : ignoré,
         * le clic droit est déjà livré au jeu par l'événement souris. */
        if (kc == AKEYCODE_BACK && souris) return 1;
        /* Passe 8h : clavier physique (Bluetooth/USB) → VK Win32 ; une touche non traduite reste au système. */
        if (kc != AKEYCODE_BACK) {
            if (ka != AKEY_EVENT_ACTION_DOWN && ka != AKEY_EVENT_ACTION_UP) return 0;
            if (!atomic_load(&g_jeu_lance) || atomic_load(&g_jeu_fini)) return 0;
            int pris = sc2bt_cle(kc, ka == AKEY_EVENT_ACTION_DOWN, AKeyEvent_getRepeatCount(e));
#ifdef SC2_SWAPCHAIN
            if (pris) sc2v_barre_voulue(0);              /* 21/10 : clavier physique → la barre s'efface */
#endif
            return pris;
        }
        /* Passe 7d : retour = Échap pour le jeu (VK_ESCAPE enfoncée puis relâchée) ; appui LONG
         * (bouton retour maintenu, navigation à 3 boutons) = sortie propre. Fermer l'app depuis les
         * récents passe par onDestroy (bilan aussi). */
        static int long_appui;
        int32_t act = AKeyEvent_getAction(e);
        if (act == AKEY_EVENT_ACTION_DOWN) {
            if (AKeyEvent_getRepeatCount(e) == 0) long_appui = 0;
            if ((AKeyEvent_getFlags(e) & AKEY_EVENT_FLAG_LONG_PRESS) || AKeyEvent_getRepeatCount(e) > 0) long_appui = 1;
        } else if (act == AKEY_EVENT_ACTION_UP) {
            if (long_appui) demander_sortie("appui long sur retour");
            else if (atomic_load(&g_jeu_lance) && !atomic_load(&g_jeu_fini)) { sc2a_touche(0x1B); LOGI("retour → VK_ESCAPE"); }
            else demander_sortie("retour sans moteur");
            long_appui = 0;
        }
        return 1;
    }
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return 0;
    int32_t a = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    if (souris || AMotionEvent_getToolType(e, 0) == AMOTION_EVENT_TOOL_TYPE_MOUSE) {   /* passe 8h : vraie souris */
        int32_t x, y;
        vers_jeu(AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0), &x, &y);
        sc2bt_souris(a, AMotionEvent_getButtonState(e), x, y, AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_VSCROLL, 0),
                     AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HSCROLL, 0));
#ifdef SC2_SWAPCHAIN
        sc2v_barre_voulue(0);                            /* 21/10 : souris → la barre s'efface */
#endif
        return 1;
    }
    /* passe 9s : toucher pendant une cinématique plein écran (SC2_VIDEO_AMC=1) = Échap injectée ; le geste entier
     * (appui → relâche) est gardé ici, sinon son clic tomberait sur l'écran qui suit la cinématique */
    static int geste_garde;
    if (a == AMOTION_EVENT_ACTION_DOWN && video_amc_entree && video_amc_entree()) { geste_garde = 1; LOGI("toucher → saut de cinématique (VK_ESCAPE)"); }
    if (geste_garde) { if (a == AMOTION_EVENT_ACTION_UP || a == AMOTION_EVENT_ACTION_CANCEL) geste_garde = 0; return 1; }
#ifdef SC2_SWAPCHAIN
    if (barre_entree(e, a)) return 1;                    /* 21/10 : toucher dans la barre de raccourcis */
#endif
    /* 21/10 : gestes (sc2_gestes.h) — 1 doigt = clic gauche (glisser = cadre), 2 doigts = clic droit */
    int64_t t = AMotionEvent_getEventTime(e) / 1000000;
    size_t i = (size_t)((AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                        >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (a == AMOTION_EVENT_ACTION_DOWN) {
        g_doigt0 = AMotionEvent_getPointerId(e, 0);
        gestes_appui(&g_gestes, AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0), t);
    } else if (a == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        gestes_appui_autre(&g_gestes, AMotionEvent_getX(e, i), AMotionEvent_getY(e, i), t);
    } else if (a == AMOTION_EVENT_ACTION_POINTER_UP) {
        gestes_releve_autre(&g_gestes, AMotionEvent_getX(e, i), AMotionEvent_getY(e, i), t);
    } else if (a == AMOTION_EVENT_ACTION_MOVE) {
        size_t n = AMotionEvent_getPointerCount(e), k = 0;
        while (k < n && AMotionEvent_getPointerId(e, k) != g_doigt0) k++;
        if (k == n) return 1;                               /* le 1er doigt est déjà levé */
        gestes_bouge(&g_gestes, AMotionEvent_getX(e, k), AMotionEvent_getY(e, k), t);
    } else if (a == AMOTION_EVENT_ACTION_UP || a == AMOTION_EVENT_ACTION_CANCEL) {
        gestes_releve(&g_gestes, AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0), t, a == AMOTION_EVENT_ACTION_CANCEL);
    } else return 1;
    gestes_livrer();
    return 1;
}

void android_main(struct android_app *app)
{
    clock_gettime(CLOCK_MONOTONIC, &g_t_lancement);
    g_app = app;
    app->onAppCmd = cmd;
    app->onInputEvent = entree;
    g_config_ok = configurer();
    printf("[ANDROID] SC2 Partout — passe 7a, %s\n", g_config_ok ? "DONNÉES ABSENTES" : g_install);

    while (!app->destroyRequested) {
        int ev; struct android_poll_source *src;
        int delai = sc2a_sortie_demandee() ? 100 : -1;
        if (gestes_attend(&g_gestes)) delai = 16;          /* 21/10 : échéance d'un appui en attente */
        int r = ALooper_pollOnce(delai, NULL, &ev, (void **)&src);
        if (r >= 0 && src) src->process(app, src);
        if (gestes_attend(&g_gestes)) {
            struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
            gestes_tick(&g_gestes, (int64_t)tn.tv_sec * 1000 + tn.tv_nsec / 1000000);
            gestes_livrer();
        }
        if (atomic_load(&g_jeu_fini) && !sc2a_sortie_demandee()) demander_sortie("fin du runtime");
        if (sc2a_sortie_demandee() && atomic_load(&g_jeu_lance) && !atomic_load(&g_jeu_fini)) {
            struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
            if (t.tv_sec - g_t_sortie.tv_sec >= 3)     /* le fil du jeu n'a ni présenté ni pompé en 3 s */
                sc2a_sortir("fil de l'activité (le jeu n'a pas répondu en 3 s)");
        }
    }
    if (atomic_load(&g_jeu_lance) && !atomic_load(&g_jeu_fini)) sc2a_sortir("onDestroy");
}
