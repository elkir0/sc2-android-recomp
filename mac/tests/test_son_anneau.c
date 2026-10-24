/* mac/tests/test_son_anneau.c — V3, lot M6 (23/10/2026) : anneau SPSC et purge du son Mac.
 *
 * Le test INCLUT android/jni/sc2_audio_android.c (même unité de compilation : ses static — produire,
 * consommer, purger, ms_en_frames, anneau_saut — deviennent atteignables) mais ne crée JAMAIS d'unité
 * CoreAudio : pure arithmétique de l'anneau, sans périphérique. Couverture exigée par le lot :
 *   - aller-retour FIFO y compris rebouclage de l'anneau (indice qui repasse par zéro) ;
 *   - débordement à CAP (les frames qui ne tiennent pas sont jetées, comptées) ;
 *   - purge : au-dessus de 150 ms (SC2_MAC_SON_LATENCE_MS par défaut), redescendre à 60 ms ;
 *   - invariant : reçues = jouées + purgées + débordements + résidu (résidu = g_ecrit − g_lu) ;
 *   - consommateur concurrent : le motif reste intact tant que l'index est publié APRÈS la copie.
 *
 * SC2_ANNEAU_BOGUE=1 (test_anneau_bogue, binaire à sortie inversée dans v3/son.cmake) : le producteur
 * publie g_ecrit AVANT la copie (bogue injecté exigé par le lot) — le fil consommateur lit alors des
 * cases non encore écrites et le motif se déchire : ce binaire doit ÉCHOUER.
 */
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sc2_audio_android.c"                       /* l'anneau, ses compteurs, purger() — branche __APPLE__ */

void sc2_log(const char *fmt, ...)                   /* exigé par le .c ; jamais appelé sans périphérique */
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

#ifdef SC2_ANNEAU_BOGUE
/* Même produit que produire(), sauf que l'index est publié AVANT la copie (fenêtre élargie par usleep). */
static void produire_bogue(const int16_t *pcm, size_t n, void *ctx)
{
    (void)ctx;
    atomic_fetch_add_explicit(&g_recues, n, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_livraisons, 1, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&g_ecrit, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&g_lu, memory_order_acquire);
    uint64_t libre = CAP - (w - r);
    if (n > libre) { atomic_fetch_add_explicit(&g_debord, n - libre, memory_order_relaxed); n = (size_t)libre; }
    uint32_t i = (uint32_t)(w & (CAP - 1)), k = (uint32_t)(n < CAP - i ? n : CAP - i);
    atomic_store_explicit(&g_ecrit, w + n, memory_order_release);   /* BOGUE : publié avant la copie */
    usleep(2000);                                                   /* le consommateur entre ici */
    memcpy(&g_anneau[i * 2], pcm, (size_t)k * 4);
    if (n > k) memcpy(&g_anneau[0], pcm + k * 2, (n - k) * 4);
    if (n) atomic_store_explicit(&g_amorce, 1, memory_order_relaxed);
}
#define produire produire_bogue                      /* toutes les sections y passent : seules les
                                                      * concurrentes voient le bogue (séquentiel = copie finie) */
#endif

static int g_pass, g_echec;
#define CHECK(cond, ...) do { if (cond) { g_pass++; printf("PASS  "); } else { g_echec++; printf("FAIL  "); } \
    printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

/* motif du cadre f : G = 3f+1, D = 5f+2 (modulo 2^16) — jamais nul, jamais périodique d'un tour d'anneau
 * (3×CAP ≢ 0 mod 65536) : une case reluée d'avant ou jamais écrite ne peut pas passer pour la bonne. */
static int16_t echant_g(uint64_t f) { return (int16_t)(f * 3 + 1); }
static int16_t echant_d(uint64_t f) { return (int16_t)(f * 5 + 2); }

static int16_t tampon[8192 * 2], sortie[8192 * 2];
static uint64_t g_verifiees;                         /* frames passées au crible */
static long g_ecarts;                                /* frames non conformes au motif */

static void motif(uint64_t base, size_t n)
{
    for (size_t j = 0; j < n; j++) { tampon[j * 2] = echant_g(base + j); tampon[j * 2 + 1] = echant_d(base + j); }
}

static void verifier(uint64_t attendu, uint32_t n)
{
    for (uint32_t j = 0; j < n; j++)
        if (sortie[2 * j] != echant_g(attendu + j) || sortie[2 * j + 1] != echant_d(attendu + j)) {
            if (g_ecarts == 0)
                printf("  premier écart : frame %llu attendu (%d,%d), reçu (%d,%d)\n",
                       (unsigned long long)(attendu + j), echant_g(attendu + j), echant_d(attendu + j),
                       sortie[2 * j], sortie[2 * j + 1]);
            g_ecarts++;
        }
    g_verifiees += n;
}

static void drainer(uint64_t *attendu)               /* vide l'anneau en vérifiant chaque frame */
{
    for (;;) {
        uint64_t dispo = atomic_load(&g_ecrit) - atomic_load(&g_lu);
        if (!dispo) return;
        uint32_t c = dispo > 4096 ? 4096 : (uint32_t)dispo;
        consommer(sortie, c);
        verifier(*attendu, c);
        *attendu += c;
    }
}

/* consommateur concurrent : tire dès que l'anneau contient une frame, au motif près */
static _Atomic int g_fin_fil, g_err_conc;
static _Atomic uint64_t g_verif_conc;
static void *fil_conso(void *p)
{
    uint64_t attendu = *(uint64_t *)p, verif = 0;
    int erreurs = 0;
    int16_t s[997 * 2];
    while (!atomic_load(&g_fin_fil)) {
        uint64_t w = atomic_load_explicit(&g_ecrit, memory_order_acquire);
        uint64_t r = atomic_load_explicit(&g_lu, memory_order_relaxed);
        if (w == r) { sched_yield(); continue; }
        uint32_t n = (uint32_t)(w - r < 997 ? w - r : 997);
        consommer(s, n);
        for (uint32_t j = 0; j < n && !erreurs; j++)
            if (s[2 * j] != echant_g(attendu + j) || s[2 * j + 1] != echant_d(attendu + j)) erreurs = 1;
        attendu += n; verif += n;
        if (erreurs) break;                          /* inutile d'insister : le motif est déchiré */
    }
    atomic_store(&g_verif_conc, verif);
    atomic_store(&g_err_conc, erreurs);
    return NULL;
}

int main(void)
{
    g_freq = 44100;                                  /* purger() compte en frames via g_freq */

    /* 1. pur calcul : seuil 150 ms, cible 60 ms ------------------------------------------------ */
    CHECK(ms_en_frames(60, 44100) == 2646 && ms_en_frames(150, 44100) == 6615,
           "ms → frames : 60 ms = %llu, 150 ms = %llu à 44100 Hz",
           (unsigned long long)ms_en_frames(60, 44100), (unsigned long long)ms_en_frames(150, 44100));
    CHECK(anneau_saut(5000, 6615, 2646) == 0 && anneau_saut(6615, 6615, 2646) == 0,
           "sous le seuil de 150 ms : aucune purge");
    CHECK(anneau_saut(6616, 6615, 2646) == 3970 && anneau_saut(7000, 6615, 2646) == 4354,
           "au-dessus : purge jusqu'à 60 ms (6616 → saut %llu, 7000 → saut %llu)",
           (unsigned long long)anneau_saut(6616, 6615, 2646), (unsigned long long)anneau_saut(7000, 6615, 2646));

    /* 2. aller-retour FIFO, y compris rebouclage (≈ 1 M de frames = 30 tours d'anneau) ------------ */
    uint32_t graine = 20261023;
    uint64_t attendu = atomic_load(&g_lu);
    for (int it = 0; it < 400; it++) {
        uint32_t n = 1 + (graine = graine * 1664525u + 1013904223u) % 5000u;
        if (atomic_load(&g_ecrit) - atomic_load(&g_lu) + n > CAP - 16) drainer(&attendu);
        motif(atomic_load(&g_ecrit), n);
        produire(tampon, n, NULL);
        if (it % 3) {                                /* deux fois sur trois, consommation partielle */
            uint64_t dispo = atomic_load(&g_ecrit) - atomic_load(&g_lu);
            uint32_t c = 1 + (graine = graine * 1664525u + 1013904223u) % 4000u;
            if (c > dispo) c = (uint32_t)dispo;
            consommer(sortie, c);
            verifier(attendu, c);
            attendu += c;
        }
    }
    drainer(&attendu);
    CHECK(g_ecarts == 0, "aller-retour FIFO sur 400 livraisons (rebouclages inclus) : %llu frames vérifiées, %ld écarts",
          (unsigned long long)g_verifiees, g_ecarts);
    CHECK(atomic_load(&g_debord) == 0, "aucun débordement tant que l'anneau n'est pas plein");
    CHECK(attendu == atomic_load(&g_ecrit), "tout produit est consommé (FIFO exact)");

    /* 3. débordement à CAP : l'anneau plein jette les NOUVELLES frames (comptées), garde les anciennes */
    motif(atomic_load(&g_ecrit), 4096);
    for (int k = 0; k < 8; k++) { produire(tampon, 4096, NULL); motif(atomic_load(&g_ecrit), 4096); }
    CHECK(atomic_load(&g_ecrit) - atomic_load(&g_lu) == CAP, "anneau plein : %u frames en attente", CAP);
    uint64_t d0 = atomic_load(&g_debord);
    produire(tampon, 1000, NULL);                    /* ne tient pas : jeté, compté */
    CHECK(atomic_load(&g_debord) - d0 == 1000, "débordement compté : %llu frames jetées",
          (unsigned long long)(atomic_load(&g_debord) - d0));
    CHECK(atomic_load(&g_ecrit) - atomic_load(&g_lu) == CAP, "la capacité ne dépasse jamais CAP");
    drainer(&attendu);
    CHECK(g_ecarts == 0, "les frames gardées sont les anciennes (les nouvelles sont jetées) : %ld écarts", g_ecarts);

    /* 4. purge : au-dessus de 150 ms → 60 ms ; en dessous, rien ---------------------------------- */
    uint64_t p0 = atomic_load(&g_purgees), k0 = atomic_load(&g_purges);
    motif(atomic_load(&g_ecrit), 7000); produire(tampon, 7000, NULL);
    uint64_t saut = purger();
    CHECK(saut == 4354 && atomic_load(&g_purgees) - p0 == 4354 && atomic_load(&g_purges) - k0 == 1,
          "7000 frames (> 150 ms) : purge unique de %llu frames → reste %llu",
          (unsigned long long)saut, (unsigned long long)(atomic_load(&g_ecrit) - atomic_load(&g_lu)));
    CHECK(atomic_load(&g_ecrit) - atomic_load(&g_lu) == 2646, "résidu ramené à 60 ms (2646 frames)");
    CHECK(purger() == 0 && atomic_load(&g_purges) == k0 + 1, "au seuil ou dessous : aucune nouvelle purge");
    motif(atomic_load(&g_ecrit), 3000); produire(tampon, 3000, NULL);
    CHECK(purger() == 0, "5646 frames (< 6615) : rien à purger");
    motif(atomic_load(&g_ecrit), 2000); produire(tampon, 2000, NULL);
    CHECK(purger() == 5000, "7646 frames : purge de 5000 → 60 ms");
    CHECK(atomic_load(&g_ecrit) - atomic_load(&g_lu) == 2646, "résidu de nouveau à 60 ms");

    /* 5. invariant : reçues = jouées + purgées + débordements + résidu --------------------------- */
    uint64_t residu = atomic_load(&g_ecrit) - atomic_load(&g_lu);
    CHECK(atomic_load(&g_recues) ==
          atomic_load(&g_jouees) + atomic_load(&g_purgees) + atomic_load(&g_debord) + residu,
          "invariant : reçues %llu = jouées %llu + purgées %llu + débordements %llu + résidu %llu",
          (unsigned long long)atomic_load(&g_recues), (unsigned long long)atomic_load(&g_jouees),
          (unsigned long long)atomic_load(&g_purgees), (unsigned long long)atomic_load(&g_debord),
          (unsigned long long)residu);

    /* 6. ligne de bilan (format, sans périphérique : jamais ouverte) ------------------------------ */
    char ligne[700];
    sortie_bilan(ligne, sizeof ligne);
    CHECK(strstr(ligne, "CoreAudio AUHAL") && strstr(ligne, "RMS du rappel") && strlen(ligne) < sizeof ligne,
          "ligne de bilan : %.120s…", ligne);

    /* 7. consommateur concurrent : le motif tient tant que l'index suit la copie ------------------ */
    pthread_t fil;
    attendu = atomic_load(&g_lu);                    /* les purges ont sauté des frames jamais consommées */
    drainer(&attendu);                               /* rend l'anneau vide : g_lu == g_ecrit */
    uint64_t base = atomic_load(&g_ecrit);
    g_err_conc = 0; g_verif_conc = 0; g_fin_fil = 0;
    CHECK(pthread_create(&fil, NULL, fil_conso, &base) == 0, "fil consommateur lancé");
    uint64_t produites = 0;
    for (int r = 0; r < 80; r++) {
        int garde = 0;                               /* borné : en mode bogue, le fil s'arrête sur l'écart */
        while (atomic_load(&g_ecrit) - atomic_load(&g_lu) > CAP - 4000 && garde++ < 200000) sched_yield();
        if (garde > 200000) break;
        motif(atomic_load(&g_ecrit), 3000);
        produire(tampon, 3000, NULL);
        produites += 3000;
    }
    atomic_store(&g_fin_fil, 1);
    pthread_join(fil, NULL);
    attendu = atomic_load(&g_lu);                    /* le fil s'est arrêté où il a voulu : on reprend là */
    drainer(&attendu);
    CHECK(atomic_load(&g_err_conc) == 0, "consommation concurrente : motif intact (%llu frames au fil)",
          (unsigned long long)atomic_load(&g_verif_conc));
    CHECK(atomic_load(&g_lu) - base == produites, "rien de perdu ni rejoué : %llu frames produites, %llu consommées",
          (unsigned long long)produites, (unsigned long long)(atomic_load(&g_lu) - base));

    /* invariant final, après tout */
    residu = atomic_load(&g_ecrit) - atomic_load(&g_lu);
    CHECK(atomic_load(&g_recues) ==
          atomic_load(&g_jouees) + atomic_load(&g_purgees) + atomic_load(&g_debord) + residu,
          "invariant final : reçues %llu = jouées %llu + purgées %llu + débordements %llu + résidu %llu",
          (unsigned long long)atomic_load(&g_recues), (unsigned long long)atomic_load(&g_jouees),
          (unsigned long long)atomic_load(&g_purgees), (unsigned long long)atomic_load(&g_debord),
          (unsigned long long)residu);

    printf("RÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_echec);
    fflush(stdout);
    return g_echec ? 1 : 0;
}
