/* sc2_attentes.c — SC2_ATTENTES=1 : OÙ le fil du jeu est-il bloqué ?
 *
 * Le fil du jeu consomme ≈ 15 ms de CPU pour ≈ 26 ms d'image : les ≈ 10 ms
 * restantes se passent dans les primitives d'attente du runtime (Sleep, SleepEx,
 * WaitForSingleObject(Ex), MsgWait, sections critiques, fences Vulkan). Ce
 * module mesure le temps BLOQUÉ — horloge murale, pas CPU — de chaque attente,
 * par fil et par cause, avec les sites d'appel invités (adresse de retour) :
 * une ligne par SC2_ATTENTES_PERIODE secondes (défaut 5) sur le fil du jeu,
 * un bilan par fil et par site à la sortie (atexit à la 1re activation).
 * Inactif tant que SC2_ATTENTES≠1 : sc2_att_debut rend 0 et l'appelant ne
 * mesure rien (motif `t0 = sc2_att_debut ? sc2_att_debut() : 0`).
 *
 * Inclus par sc2_iat.c après ses includes, comme sc2_entrees_banc.c l'est par
 * sc2_input_sim.c : aucune liste de build à toucher. Les symboles sont FAIBLES
 * pour les autres unités (sc2_d3d9v.c, lié seul dans test_d3d9v, doit lier sans
 * ce module) ; SC2_ATTENTES_IMPL, défini ci-dessous, les rend ici FORTS.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                       /* pthread_getname_np (glibc) ; sans effet macOS/bionic */
#endif
#define SC2_ATTENTES_IMPL
#include "sc2_attentes.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Tranches de durée DEMANDÉE (ms) : 0, 1, 2-9, 10-99, ≥100 — 0xFFFFFFFF
 * (INFINITE) tombe dans ≥100. CS et fence n'ont pas de délai : une seule
 * tranche (0). */
#define ATT_TRN        5
#define ATT_FILS_MAX   64                 /* au-delà : attentes comptées aux sites, plus par fil */
#define ATT_SITES_MAX  1024
#define ATT_SITES_TOP  15

static const char *att_causes[ATT_N]     = { "Sleep", "SleepEx", "Wait", "MsgWait", "CS", "fence" };
static const char *att_tranches[ATT_TRN] = { "0", "1", "2-9", "10-99", "≥100" };

/* Compteurs par (cause, tranche) — cumuls depuis le départ (n, ns) et fenêtre
 * courante (fn, fns), remise à zéro après chaque ligne périodique du fil du
 * jeu. Chaque fil n'écrit que les siens ; le bilan (atexit) lit ceux des
 * autres par atomiques relâchés. */
struct att_fil {
    char nom[32];                         /* pthread_getname_np, ou « fil n » si anonyme */
    int jeu;                              /* marqué par sc2_att_present (une fois par image) */
    uint64_t n[ATT_N][ATT_TRN], ns[ATT_N][ATT_TRN];
    uint64_t fn[ATT_N][ATT_TRN], fns[ATT_N][ATT_TRN];
    uint64_t fen_images, fen_mur0, fen_cpu0;             /* cadre de la fenêtre (fil du jeu) */
};
static struct att_fil g_fils[ATT_FILS_MAX];
static int g_n_fils;                      /* fils suivis (croît atomiquement, clampé à MAX) */
static _Thread_local int t_att_idx = -1;  /* slot du fil courant, attribué à sa 1re attente */

/* Sites d'appel invités : (cause, tranche, ret_va) → appels, ns. Table FIXE
 * sous mutex ; pleine → compteur « hors table » (l'attente reste comptée par fil). */
struct att_site { uint8_t cause, tr; uint32_t ret; uint64_t n, ns; };
static pthread_mutex_t g_sites_mu = PTHREAD_MUTEX_INITIALIZER;
static struct att_site g_sites[ATT_SITES_MAX];
static int g_sites_n;
static uint64_t g_hors_n, g_hors_ns;

/* Activation : SC2_ATTENTES lu UNE fois (double vérification sous mutex) ; la
 * période suit, et le bilan s'enregistre par atexit à la 1re activation. */
#define ATT_INACTIF 1
#define ATT_ACTIF   2
static pthread_mutex_t g_init_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_etat;                        /* 0 = env non lu, 1 = inactif, 2 = actif */
static double g_periode = 5.0;            /* SC2_ATTENTES_PERIODE (secondes) */

static int att_etat(void)                 /* sans effet de bord : pour le bilan d'atexit */
{
    return __atomic_load_n(&g_etat, __ATOMIC_ACQUIRE);
}

static int att_actif(void)                /* lit l'env une fois, enregistre le bilan si actif */
{
    int e = att_etat();
    if (e) return e;
    pthread_mutex_lock(&g_init_mu);
    e = att_etat();
    if (!e) {
        const char *v = getenv("SC2_ATTENTES");
        e = (v && strcmp(v, "1") == 0) ? ATT_ACTIF : ATT_INACTIF;
        if (e == ATT_ACTIF) {
            const char *p = getenv("SC2_ATTENTES_PERIODE");
            double s = p ? strtod(p, NULL) : 0.0;
            if (s > 0) g_periode = s;
            atexit(sc2_att_bilan);
        }
        __atomic_store_n(&g_etat, e, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_init_mu);
    return e;
}

static uint64_t att_ns(clockid_t horloge)
{
    struct timespec t;
    clock_gettime(horloge, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static struct att_fil *att_mon_fil(void)  /* slot du fil courant, attribué à la 1re attente */
{
    int i = t_att_idx;
    if (i >= 0) return &g_fils[i];
    i = __atomic_fetch_add(&g_n_fils, 1, __ATOMIC_RELAXED);
    if (i >= ATT_FILS_MAX) return NULL;   /* plus de slot : la cause reste comptée aux sites */
    t_att_idx = i;
    pthread_getname_np(pthread_self(), g_fils[i].nom, sizeof g_fils[i].nom);   /* vide si anonyme */
    if (!g_fils[i].nom[0]) snprintf(g_fils[i].nom, sizeof g_fils[i].nom, "fil %d", i + 1);
    return &g_fils[i];
}

static int att_tranche(int cause, uint32_t ms)
{
    if (cause == ATT_CS || cause == ATT_VK_FENCE) return 0;   /* pas de délai demandé */
    if (ms == 0) return 0;
    if (ms == 1) return 1;
    if (ms < 10) return 2;
    if (ms < 100) return 3;
    return 4;                             /* ≥ 100 ms, INFINITE (0xFFFFFFFF) compris */
}

static void att_site_noter(int cause, int tr, uint32_t ret, uint64_t bloq)
{
    int i;
    pthread_mutex_lock(&g_sites_mu);
    for (i = 0; i < g_sites_n; i++)
        if (g_sites[i].cause == (uint8_t)cause && g_sites[i].tr == (uint8_t)tr && g_sites[i].ret == ret)
            break;
    if (i < g_sites_n) { g_sites[i].n++; g_sites[i].ns += bloq; }
    else if (g_sites_n < ATT_SITES_MAX) {
        g_sites[g_sites_n].cause = (uint8_t)cause; g_sites[g_sites_n].tr = (uint8_t)tr;
        g_sites[g_sites_n].ret = ret;             g_sites[g_sites_n].n = 1;
        g_sites[g_sites_n].ns = bloq;             g_sites_n++;
    }
    else { g_hors_n++; g_hors_ns += bloq; }   /* table pleine : comptée, pas détaillée */
    pthread_mutex_unlock(&g_sites_mu);
}

/* Étiquette « Cause » ou « Cause(tranche) » — CS et fence n'ont pas de délai demandé. */
static void att_etiquette(char *b, size_t tb, int cause, int tr)
{
    if (cause == ATT_CS || cause == ATT_VK_FENCE) snprintf(b, tb, "%s", att_causes[cause]);
    else snprintf(b, tb, "%s(%s)", att_causes[cause], att_tranches[tr]);
}

/* Somme une matrice de compteurs (fenêtre propre au fil, ou cumuls lus par
 * atomiques relâchés : le bilan peut lire pendant qu'un autre fil attend). */
static uint64_t att_somme(const uint64_t m[ATT_N][ATT_TRN], int atomique)
{
    uint64_t s = 0;
    for (int i = 0; i < ATT_N; i++) for (int j = 0; j < ATT_TRN; j++)
        s += atomique ? __atomic_load_n(&m[i][j], __ATOMIC_RELAXED) : m[i][j];
    return s;
}

/* Détail « étiquette x ms k× · … » des (cause, tranche) non vides d'un fil.
 * fenetre : la fenêtre courante du fil du jeu — valeurs PAR IMAGE (k avec une
 * décimale), lue par le fil lui-même ; sinon cumuls depuis le départ pour le
 * bilan, lus par atomiques relâchés. */
static void att_detail(const struct att_fil *f, int fenetre, char *b, size_t tb)
{
    size_t o = 0;
    b[0] = '\0';
    for (int i = 0; i < ATT_N; i++) for (int j = 0; j < ATT_TRN; j++) {
        char et[32];
        uint64_t k, ns;
        int w;
        if (fenetre) { k = f->fn[i][j]; ns = f->fns[i][j]; }
        else {
            k = __atomic_load_n(&f->n[i][j], __ATOMIC_RELAXED);
            ns = __atomic_load_n(&f->ns[i][j], __ATOMIC_RELAXED);
        }
        if (!k) continue;
        att_etiquette(et, sizeof et, i, j);
        if (fenetre) {
            double img = (double)f->fen_images;
            w = snprintf(b + o, o < tb ? tb - o : 0, "%s%s %.1f ms %.1f×", o ? " · " : "", et,
                         (double)ns / 1e6 / img, (double)k / img);
        } else
            w = snprintf(b + o, o < tb ? tb - o : 0, "%s%s %.1f ms %llu×", o ? " · " : "", et,
                         (double)ns / 1e6, (unsigned long long)k);
        if (w < 0 || o + (size_t)w >= tb) { b[tb - 1] = '\0'; return; }   /* tronqué : arrêt propre */
        o += (size_t)w;
    }
}

uint64_t sc2_att_debut(void)
{
    if (att_actif() != ATT_ACTIF) return 0;
    (void)att_mon_fil();                  /* le slot du fil est attribué à sa 1re attente */
    return att_ns(CLOCK_MONOTONIC);
}

void sc2_att_fin(int cause, uint32_t ms_demandes, uint64_t t0, uint32_t ret_va)
{
    struct att_fil *f;
    uint64_t fin, bloq;
    int tr;
    if (!t0 || att_actif() != ATT_ACTIF || (unsigned)cause >= (unsigned)ATT_N) return;
    fin = att_ns(CLOCK_MONOTONIC);
    bloq = fin > t0 ? fin - t0 : 0;
    tr = att_tranche(cause, ms_demandes);
    att_site_noter(cause, tr, ret_va, bloq);
    f = att_mon_fil();
    if (f) {
        f->n[cause][tr]++; f->ns[cause][tr] += bloq;
        f->fn[cause][tr]++; f->fns[cause][tr] += bloq;
    }
}

void sc2_att_present(void)
{
    struct att_fil *f;
    uint64_t mur, cpu;
    if (att_actif() != ATT_ACTIF) return;
    f = att_mon_fil();
    if (!f) return;
    __atomic_store_n(&f->jeu, 1, __ATOMIC_RELAXED);   /* le bilan (atexit) le lit d'un autre fil */
    mur = att_ns(CLOCK_MONOTONIC);
    if (!f->fen_images) {                 /* 1re image d'une fenêtre : repères mur/CPU, rien à dire */
        f->fen_mur0 = mur;
        f->fen_cpu0 = att_ns(CLOCK_THREAD_CPUTIME_ID);
        f->fen_images = 1;
        return;
    }
    f->fen_images++;
    if (mur - f->fen_mur0 < (uint64_t)(g_periode * 1e9)) return;
    cpu = att_ns(CLOCK_THREAD_CPUTIME_ID);
    {
        char det[2048];
        double img = (double)f->fen_images;
        att_detail(f, 1, det, sizeof det);
        printf("[ATTENTES] fil du jeu : %llu images en %.2f s = %.1f ms/image, CPU %.1f ms/image, "
               "bloqué %.1f ms/image%s%s\n",
               (unsigned long long)f->fen_images, (double)(mur - f->fen_mur0) / 1e9,
               (double)(mur - f->fen_mur0) / 1e6 / img, (double)(cpu - f->fen_cpu0) / 1e6 / img,
               (double)att_somme(f->fns, 0) / 1e6 / img, det[0] ? " : " : "", det);
        fflush(stdout);
    }
    f->fen_images = 0;                    /* fenêtre suivante : repars remis à la prochaine image */
    memset(f->fn, 0, sizeof f->fn);
    memset(f->fns, 0, sizeof f->fns);
}

void sc2_att_bilan(void)
{
    int n, i, k;
    if (att_etat() != ATT_ACTIF) return;  /* jamais activé : rien à imprimer */
    n = __atomic_load_n(&g_n_fils, __ATOMIC_RELAXED);
    if (n > ATT_FILS_MAX) n = ATT_FILS_MAX;
    for (i = 0; i < n; i++) {
        struct att_fil *f = &g_fils[i];
        char det[2048];
        uint64_t appels = att_somme(f->n, 1);
        if (!appels && !__atomic_load_n(&f->jeu, __ATOMIC_RELAXED)) continue;   /* fil sans attente mesurée */
        att_detail(f, 0, det, sizeof det);
        printf("[BILAN] [ATTENTES] fil %d « %s »%s : bloqué %.1f ms : %s\n", i + 1, f->nom,
               __atomic_load_n(&f->jeu, __ATOMIC_RELAXED) ? " (fil du jeu)" : "",
               (double)att_somme(f->ns, 1) / 1e6, det);
    }
    fflush(stdout);
    {   /* sites d'appel : copie sous le verrou, classement (les plus coûteux en ns) hors du verrou */
        struct att_site l[ATT_SITES_MAX];
        uint64_t hn, hns;
        int ln;
        pthread_mutex_lock(&g_sites_mu);
        memcpy(l, g_sites, sizeof g_sites);
        ln = g_sites_n; hn = g_hors_n; hns = g_hors_ns;
        pthread_mutex_unlock(&g_sites_mu);
        if (!ln && !hn) return;
        printf("[BILAN] [ATTENTES] sites :");
        for (k = 0; k < ATT_SITES_TOP && k < ln; k++) {
            int m = k;
            char et[32];
            for (i = k + 1; i < ln; i++) if (l[i].ns > l[m].ns) m = i;
            if (m != k) { struct att_site e = l[k]; l[k] = l[m]; l[m] = e; }
            att_etiquette(et, sizeof et, l[k].cause, l[k].tr);
            printf("\n[BILAN] [ATTENTES] %s ret 0x%08X : %llu appels, %.1f ms, %.1f µs moyen", et,
                   l[k].ret, (unsigned long long)l[k].n, (double)l[k].ns / 1e6,
                   (double)l[k].ns / 1e3 / (double)l[k].n);
        }
        printf("\n");
        if (hn)
            printf("[BILAN] [ATTENTES] sites : table pleine, %llu appels (%.1f ms) hors table\n",
                   (unsigned long long)hn, (double)hns / 1e6);
        fflush(stdout);
    }
}
