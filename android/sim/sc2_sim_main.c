/* sc2_sim_main.c — banc Linux/qemu de la plate-forme Android (passe 7a), sans téléphone.
 *
 * Exerce, sur aarch64-linux-gnu sous qemu-user, EXACTEMENT les accroches de l'APK
 * (jni/sc2_android_hooks.c, jni/sc2_ucontext*.{c,S} : fibres sans la glibc) et le même schéma de
 * fils que jni/sc2_android_main.c :
 *   - un faux tas ART occupe 0x12C00000..0x32C00000 AVANT le démarrage (« dalvik-main space »
 *     d'un processus d'app, 512 Mo) : l'arène à adresse fixe de Linux (0x08000000..0x38000000)
 *     y serait impossible ;
 *   - le jeu tourne sur un fil de 16 Mo de pile, zone invitée décalée (g_hote_delta) ;
 *   - le fil principal joue l'activité : touchers (appui/relâche) injectés pendant le run, puis
 *     sortie demandée (sc2a_demander_sortie, appui long sur retour dans l'APK) à la frame SIM_SORTIE
 *     → bilan → exit(0). Passe 7d : touchers et Échap passent par l'API runtime sc2_input (5f) ;
 *     SIM_ECHAP=<frame> injecte VK_ESCAPE, SIM_ACTIVER_TARD=1 active les entrées après la 1re frame.
 * Ce qui serait affiché est compté (frames non noires) et une frame est écrite en PPM.
 *
 *   SC2_INSTALL=… qemu-aarch64 … ./sc2-sim [SC2.exe]
 *   SIM_SORTIE=<n> (défaut 300)  SIM_PPM=<fichier> (frame 200)
 *   passe 7o : SIM_FENETRE=<w>x<h> (fenêtre simulée de l'appareil, ex. 2400x1080 l'émulateur M4) et
 *   SIM_TAPS="<frame>:<x>,<y>;…" (cible en pixels CLIENT) : chaque cible est convertie en pixel FENÊTRE
 *   (ce que taperait `adb shell input tap`), puis ramenée en client par la conversion de l'APK
 *   (sc2_geo.h, géométrie de la copie CPU : tampon aux proportions de la fenêtre) et injectée comme un
 *   doigt : appui à <frame>, relâche 3 frames plus tard. Remplace les touchers automatiques.
 *   passe 7b, build -DSC2_SWAPCHAIN=ON : SC2_SWAPCHAIN=1 [SC2_SWAPCHAIN_CONTROLE=1] — présentation par
 *   swapchain dans une fenêtre X 2316x1920 (VK_KHR_xcb_surface, DISPLAY = Xvfb), relue pour contrôle.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "sc2_android.h"
#include "sc2_geo.h"
#include "sc2_audio_android.h"
#include "sc2_trace_present.h"
#ifdef SC2_SWAPCHAIN
#include "sc2_vk_presente.h"
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static char g_exe[1024];
static uint32_t g_w = 1280, g_h = 720;
static atomic_uint g_n, g_non_noires, g_derniere;
static unsigned g_sortie = 300;
static double g_moy_max;

int sc2a_frame_voulue(unsigned n) { (void)n; return 1; }

static atomic_uint g_gpu;
static struct timespec g_t1, g_tn;                /* passe 7c : cadence de la boucle (frame 1 → dernière) */
static void horodater(unsigned n)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    if (n == 1) g_t1 = t;
    g_tn = t;
}
void sc2a_frame(unsigned n, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    horodater(n);
    if (!rgba) {                                  /* présentée par la swapchain (passe 7b) */
        atomic_fetch_add(&g_gpu, 1); atomic_fetch_add(&g_n, 1); atomic_store(&g_derniere, n);
        return;
    }
    uint64_t s = 0; size_t np = (size_t)w * h;
    for (size_t i = 0; i < np; i++) s += rgba[4 * i] + rgba[4 * i + 1] + rgba[4 * i + 2];
    double moy = (double)s / (3.0 * (double)np);
    if (moy > g_moy_max) g_moy_max = moy;
    if (s) atomic_fetch_add(&g_non_noires, 1);
    atomic_fetch_add(&g_n, 1); atomic_store(&g_derniere, n);
    if (n == 200 || n == 1) {
        const char *p = getenv("SIM_PPM");
        char chemin[512]; snprintf(chemin, sizeof chemin, "%s.%u.ppm", p ? p : "sim-frame", n);
        FILE *f = fopen(chemin, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (size_t i = 0; i < np; i++) fwrite(rgba + 4 * i, 1, 3, f);
            fclose(f);
        }
        printf("[SIM] frame n°%u relue par l'accroche Present : moyenne %.2f → %s\n", n, moy, chemin);
    }
}

void sc2a_avant_sortie(void)
{
    uint64_t inj, serv; sc2a_bilan_entrees(&inj, &serv);
    printf("[SIM] sortie : %u frames relues (comme pour l'écran), %u non noires (moyenne max %.2f), dernière n°%u ; "
           "%llu événements injectés dans sc2_input, %llu déplacements fusionnés (livrés au jeu : lignes [INPUT])\n",
           atomic_load(&g_n), atomic_load(&g_non_noires), g_moy_max, atomic_load(&g_derniere),
           (unsigned long long)inj, (unsigned long long)serv);
    double dt = (double)(g_tn.tv_sec - g_t1.tv_sec) + (double)(g_tn.tv_nsec - g_t1.tv_nsec) / 1e9;
    unsigned dn = atomic_load(&g_derniere);
    if (dn > 1 && dt > 0)
        printf("[SIM] cadence : frames 1 → %u en %.1f s, %.2f frames/s (relecture CPU comprise, sous qemu)\n", dn, dt, (dn - 1) / dt);
    sc2aa_arreter();
#ifdef SC2_SWAPCHAIN
    char b[256]; sc2v_bilan(b, sizeof b);
    printf("[SIM] %s (%u frames comptées comme présentées par le GPU)\n", b, atomic_load(&g_gpu));
#endif
    { char t[640]; sc2tp_bilan(t, sizeof t); if (t[0]) printf("[BILAN] [PRESENT] tout le run : %s\n", t); }   /* passe 7p3 */
    fflush(stdout);
}

static void *fil_jeu(void *p)
{
    (void)p;
    if (sc2a_zone_init()) exit(3);
    /* Passe 7d : activation des entrées runtime AVANT le boot (comme l'APK). SIM_ACTIVER_TARD=1 :
     * activation après la 1re frame — contre-épreuve (les imports de la pompe sont déjà résolus). */
    if (!getenv("SIM_ACTIVER_TARD")) sc2a_entrees_activer();
    sc2aa_demarrer();                             /* passe 7e : même sortie audio que l'APK (pseudo-AAudio) */
    printf("[SIM] zone invitée : 4 Go réservés à %p (g_hote_delta) ; SC2.exe = %s\n", (void *)sc2a_zone(), g_exe);
    char *argv[] = { "recomp-sc2", g_exe, NULL };
    int r = sc2_poc_main(2, argv);
    printf("[SIM] le runtime a rendu %d sans exit()\n", r);
    exit(4);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* faux tas ART : le bas des 4 Go n'est pas libre dans un processus d'app Android */
    void *art = mmap((void *)0x12C00000, 0x20000000, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    printf("[SIM] faux tas ART 0x12C00000..0x32C00000 : %s\n", art == (void *)0x12C00000 ? "posé" : "ÉCHEC");
    if (art != (void *)0x12C00000) return 3;
    const char *inst = getenv("SC2_INSTALL");
    if (argc > 1) snprintf(g_exe, sizeof g_exe, "%s", argv[1]);
    else snprintf(g_exe, sizeof g_exe, "%s/Versions/Base15405/SC2.exe", inst ? inst : ".");
    if (getenv("SIM_SORTIE")) g_sortie = (unsigned)atoi(getenv("SIM_SORTIE"));
    if (!getenv("SC2_ECRAN")) setenv("SC2_ECRAN", "1280x720", 1);
    sscanf(getenv("SC2_ECRAN"), "%ux%u", &g_w, &g_h);
    sc2a_taille_ecran(g_w, g_h);
#ifdef SC2_SWAPCHAIN
    /* fenêtre X (Xvfb) à la taille du grand écran du Fold, en paysage */
    {   /* passe 7p3 : SIM_SWAPCHAIN=<l>x<h> — taille de la fenêtre X du présentateur (défaut 2316x1920, Fold 8 ;
         * 2400x1080 = émulateur du retest M4) */
        unsigned sw = 2316, sh = 1920; const char *e = getenv("SIM_SWAPCHAIN");
        if (e && (sscanf(e, "%ux%u", &sw, &sh) != 2 || !sw || !sh)) { sw = 2316; sh = 1920; }
        sc2v_fenetre(NULL, sw, sh);
    }
#endif

    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16u << 20);
    pthread_t t;
    if (pthread_create(&t, &at, fil_jeu, NULL)) { perror("pthread_create"); return 3; }

    /* passe 7o : fenêtre simulée + touchers ciblés (SIM_FENETRE, SIM_TAPS) */
    uint32_t fw = 0, fh = 0; sc2_geo geo = { 0 };
    struct { unsigned f; int32_t x, y, ex, ey; int etat; } taps[16]; int ntaps = 0;
    if (getenv("SIM_FENETRE") && sscanf(getenv("SIM_FENETRE"), "%ux%u", &fw, &fh) == 2 && fw && fh) {
        int32_t bw, bh;                              /* le tampon de geometrie() de sc2_android_main.c */
        if ((int64_t)fw * g_h >= (int64_t)fh * g_w) { bh = (int32_t)g_h; bw = (int32_t)((int64_t)g_h * fw / fh); }
        else { bw = (int32_t)g_w; bh = (int32_t)((int64_t)g_w * fh / fw); }
        if (bw < (int32_t)g_w) bw = (int32_t)g_w;
        if (bh < (int32_t)g_h) bh = (int32_t)g_h;
        geo = sc2_geo_tampon(g_w, g_h, (uint32_t)bw, (uint32_t)bh, (bw - (int32_t)g_w) / 2, (bh - (int32_t)g_h) / 2, fw, fh);
        printf("[SIM] fenêtre simulée %ux%u : tampon %dx%d, image du jeu en (%.1f, %.1f) ×%.4f×%.4f de la fenêtre\n",
               fw, fh, bw, bh, geo.x0, geo.y0, geo.sx, geo.sy);
    }
    for (const char *t = getenv("SIM_TAPS"); t && *t && ntaps < 16; ) {
        unsigned f; int x, y, k = 0;
        if (sscanf(t, "%u:%d,%d%n", &f, &x, &y, &k) != 3) break;
        taps[ntaps].f = f; taps[ntaps].x = x; taps[ntaps].y = y; taps[ntaps].etat = 0; ntaps++;
        t += k; if (*t == ';') t++;
    }

    /* l'« activité » : touchers à intervalles, puis retour */
    unsigned prochain = ntaps ? 0xFFFFFFFFu : 50;
    for (;;) {
        usleep(20000);
        unsigned n = atomic_load(&g_derniere);
        for (int i = 0; i < ntaps; i++) {
            if (taps[i].etat == 0 && n >= taps[i].f) {
                int32_t ex = taps[i].x, ey = taps[i].y, cx, cy;
                if (geo.ok) sc2_geo_vers_ecran(&geo, taps[i].x, taps[i].y, &ex, &ey);   /* « adb shell input tap ex ey » */
                int dedans = geo.ok ? sc2_geo_vers_jeu(&geo, ex, ey, &cx, &cy) : (cx = ex, cy = ey, 1);
                taps[i].ex = cx; taps[i].ey = cy;
                sc2a_tactile(SC2A_APPUI, cx, cy);
                printf("[SIM] [TOUCHER] frame %u : cible client (%d, %d) = écran (%d, %d) → client (%d, %d)%s — appui\n",
                       n, taps[i].x, taps[i].y, ex, ey, cx, cy, dedans ? "" : " BANDE NOIRE");
                taps[i].etat = 1; taps[i].f = n + 3;
            } else if (taps[i].etat == 1 && n >= taps[i].f) {
                sc2a_tactile(SC2A_LEVE, taps[i].ex, taps[i].ey);
                printf("[SIM] [TOUCHER] frame %u : relâche en client (%d, %d)\n", n, taps[i].ex, taps[i].ey);
                taps[i].etat = 2;
            }
        }
        if (n >= prochain && prochain < g_sortie) {
            int32_t x = (int32_t)(g_w / 2 + (prochain % 7) * 40), y = (int32_t)(g_h / 2);
            sc2a_tactile(SC2A_APPUI, x, y);
            sc2a_tactile(SC2A_BOUGE, x + 3, y + 2);
            sc2a_tactile(SC2A_LEVE, x + 3, y + 2);
            printf("[SIM] toucher injecté vers la frame %u en (%d, %d)\n", n, x, y);
            prochain += 50;
        }
        static int tard_fait, echap_fait;
        if (getenv("SIM_ACTIVER_TARD") && !tard_fait && n >= 1) {
            sc2a_entrees_activer(); tard_fait = 1;
            printf("[SIM] SIM_ACTIVER_TARD : sc2_input_activer() après la frame %u\n", n);
        }
        /* SIM_ECHAP=<frame> : la « touche retour » de l'APK (VK_ESCAPE enfoncée/relâchée) */
        if (getenv("SIM_ECHAP") && !echap_fait && n >= (unsigned)atoi(getenv("SIM_ECHAP"))) {
            sc2a_touche(0x1B); echap_fait = 1;
            printf("[SIM] retour → VK_ESCAPE injectée vers la frame %u\n", n);
        }
        if (n >= g_sortie && !sc2a_sortie_demandee()) {
            printf("[SIM] « touche retour » à la frame %u\n", n);
            sc2a_demander_sortie();
        }
    }
}
