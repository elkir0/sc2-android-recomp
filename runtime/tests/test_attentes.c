/* test_attentes.c — SC2_ATTENTES : temps bloqué par fil et par cause (21/10).
 *
 * Le module (runtime/sc2_attentes.c) est inclus par sc2_iat.c ; ici il est
 * compilé SEUL (comme test_entrees_banc compile sc2_entrees_banc.c), ses
 * états static vivant dans la même unité de traduction. Cas 1 (actif) :
 * SC2_ATTENTES=1, période 0,05 s, deux fils — le « jeu » présente des images
 * en boucle (sc2_att_present, fence après chaque image), le « dormeur »
 * enchaîne sc2_att_debut/usleep/sc2_att_fin de causes, tranches et ret_va
 * distincts. La sortie part dans un fichier temporaire (stdout détourné par
 * dup2 puis restauré : les diagnostics du test restent au terminal) : la
 * ligne périodique [ATTENTES] du fil du jeu doit y être, le bilan par fil et
 * les sites aussi, et les ns cumulés couvrir au moins la somme des usleep
 * (une attente ne peut durer que plus que demandé). Cas 2 (fils forkés) :
 * sans SC2_ATTENTES, sc2_att_debut rend 0 et ni la ligne ni le bilan
 * n'impriment quoi que ce soit.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sc2_attentes.c"

#define PERIODE_TEST "0.05"              /* 50 ms : une fenêtre toutes les ~4 images de 12 ms */

static uint64_t g_dorme_ns;              /* somme demandée par le dormeur (usleep en ns, atomique par commodité) */

static void *fil_dormeur(void *arg)
{
    (void)arg;
    /* Six attentes de (cause, tranche, ret_va) distincts : Sleep(0/1/2-9/10-99… tranches),
     * SleepEx(1), Wait(INFINITE → ≥100), CS (tranche unique). ATT_CS passe par le même
     * sc2_att_fin — pas besoin d'un vrai mutex contesté pour le module. */
    static const struct { int cause; uint32_t ms; uint32_t ret; } cas[] = {
        { ATT_SLEEP, 1, 0x00A01000u }, { ATT_SLEEP, 2, 0x00A01100u }, { ATT_SLEEP, 50, 0x00A01200u },
        { ATT_SLEEPEX, 1, 0x00A02000u }, { ATT_WAIT, 0xFFFFFFFFu, 0x00A03000u }, { ATT_CS, 0, 0x00A04000u },
    };
    for (int t = 0; t < 4; t++) for (unsigned i = 0; i < sizeof cas / sizeof cas[0]; i++) {
        uint32_t ns = cas[i].ms == 0xFFFFFFFFu ? 1000000u : cas[i].ms * 1000000u;   /* INFINITE : 1 ms au test */
        uint64_t t0 = sc2_att_debut();
        usleep(ns / 1000);
        if (t0) sc2_att_fin(cas[i].cause, cas[i].ms, t0, cas[i].ret);
        __atomic_fetch_add(&g_dorme_ns, ns, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void *fil_jeu(void *arg)
{
    (void)arg;
    for (int i = 0; i < 40; i++) {       /* ~40 images de ~12 ms : plusieurs fenêtres de 50 ms */
        uint64_t t0 = sc2_att_debut();
        usleep(3000);
        if (t0) sc2_att_fin(ATT_VK_FENCE, 0, t0, 0);   /* le fil du jeu attend sa fence après chaque image */
        sc2_att_present();
    }
    return NULL;
}

/* Cherche une sous-chaîne ligne à ligne dans un fichier ; rend 1 si présente. */
static int fichier_contient(const char *chemin, const char *aiguille)
{
    FILE *f = fopen(chemin, "r");
    char l[4096];
    int trouve = 0;
    if (!f) return 0;
    while (fgets(l, sizeof l, f))
        if (strstr(l, aiguille)) { trouve = 1; break; }
    fclose(f);
    return trouve;
}

int main(void)
{
    char tmp[128], tmp2[128];
    int st, ec = 0;
    pid_t pid;

    snprintf(tmp, sizeof tmp, "/tmp/test_attentes_actif.%d", (int)getpid());
    snprintf(tmp2, sizeof tmp2, "/tmp/test_attentes_inactif.%d", (int)getpid());

    /* ---- cas 2 D'ABORD (fork) : sans SC2_ATTENTES, début rend 0 et RIEN n'est imprimé ---- */
    pid = fork();
    if (pid == 0) {                      /* fils : env nettoyé AVANT la 1re activation (getenv lu une fois) */
        FILE *f = freopen(tmp2, "w", stdout);
        if (!f) _exit(2);
        unsetenv("SC2_ATTENTES");
        if (sc2_att_debut()) _exit(1);   /* inactif : jamais de t0 */
        sc2_att_fin(ATT_SLEEP, 1, 0, 0x00A05000u);   /* t0 nul : ignoré, aucun compteur bougé */
        sc2_att_present();
        sc2_att_bilan();
        fflush(stdout);
        _exit(0);                        /* _exit : pas de bilan atexit hérité du père */
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("ÉCHEC : cas inactif sort en %d\n", st);
        return 1;
    }
    if (access(tmp2, F_OK) != 0) {        /* le fils a bien créé son fichier de sortie (vide) */
        printf("ÉCHEC : pas de sortie inactive %s\n", tmp2);
        return 1;
    }
    if (fichier_contient(tmp2, "[ATTENTES]") || fichier_contient(tmp2, "[BILAN]")) {
        printf("ÉCHEC : le cas inactif a imprimé (voir %s)\n", tmp2);
        unlink(tmp2);
        return 1;
    }
    unlink(tmp2);

    /* ---- cas 1 : deux fils actifs, stdout détourné vers tmp puis restauré ---- */
    setenv("SC2_ATTENTES", "1", 1);
    setenv("SC2_ATTENTES_PERIODE", PERIODE_TEST, 1);
    {
        int sauv = dup(1);
        FILE *out = freopen(tmp, "w", stdout);
        pthread_t jeu, dormeur;
        if (!out || sauv < 0) { printf("ÉCHEC : détournement de stdout\n"); return 1; }
        pthread_create(&jeu, NULL, fil_jeu, NULL);
        pthread_create(&dormeur, NULL, fil_dormeur, NULL);
        pthread_join(jeu, NULL);
        pthread_join(dormeur, NULL);
        sc2_att_bilan();
        fflush(stdout);
        dup2(sauv, 1);                   /* diagnostics de retour au terminal */
        close(sauv);
    }

    if (!fichier_contient(tmp, "[ATTENTES] fil du jeu :")) {
        printf("ÉCHEC : pas de ligne [ATTENTES] fil du jeu dans %s\n", tmp);
        ec = 1;
    }
    if (!ec && !fichier_contient(tmp, "[BILAN] [ATTENTES] fil ")) {
        printf("ÉCHEC : pas de bilan par fil dans %s\n", tmp);
        ec = 1;
    }
    if (!ec && !fichier_contient(tmp, "ret 0x00A01000")) {
        printf("ÉCHEC : pas de site ret 0x00A01000 (Sleep 1 ms) dans %s\n", tmp);
        ec = 1;
    }
    if (!ec) {                           /* ns cumulés (tous fils) ≥ somme demandée par le dormeur */
        uint64_t attendu = __atomic_load_n(&g_dorme_ns, __ATOMIC_RELAXED);
        int n = __atomic_load_n(&g_n_fils, __ATOMIC_RELAXED);
        uint64_t s = 0;
        if (n > ATT_FILS_MAX) n = ATT_FILS_MAX;
        for (int i = 0; i < n; i++) s += att_somme(g_fils[i].ns, 1);
        if (s < attendu) {
            printf("ÉCHEC : %llu ns cumulés < %llu ns dormus\n", (unsigned long long)s,
                   (unsigned long long)attendu);
            ec = 1;
        }
    }
    if (ec) { printf("ÉCHEC : sortie conservée dans %s\n", tmp); return 1; }
    unlink(tmp);
    printf("test_attentes : OK\n");
    return 0;                            /* le bilan atexit réimprime au terminal : sans conséquence */
}
