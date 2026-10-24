/* test_d3d9_file.c — V5.1 (24/10) : file de libération des ressources D3D9 (runtime/sc2_d3d9_file.h). Pur : ni Vulkan,
 * ni mémoire invitée — des objets simulés (compte de références, genre, surfaces de niveaux, liaisons du device) et un
 * journal des appels oubli / rendu.
 *   1. délai : un objet tombé à zéro avant le Present f n'est rendu qu'au Present f + ZF_DELAI (≥ 2 frames complètes) ;
 *   2. liaisons : lié au device (étage de texture, flux, RT, indices, profondeur, back buffer) → gardé, puis rendu une
 *      fois délié ;
 *   3. repris (AddRef après zéro) → sorti de la file, jamais rendu ;
 *   4. conteneur dont une surface est tenue (−1) → sorti ; surface liée (−2) → gardé ; genre plus rendable → sorti ;
 *   5. ordre : l'oubli Vulkan de TOUS les prêts précède le premier rendu ; surfaces retirées avant leur conteneur ;
 *   6. file pleine : refus compté ;
 *   7. aucun objet rendu deux fois, aucun objet vivant rendu (100 000 événements tirés).
 * Sortie : « RÉSULTAT : x PASS, y FAIL », code 0 sans FAIL. */
#include "sc2_d3d9_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) { g_pass++; printf("PASS  "); } else { g_fail++; printf("FAIL  "); } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define NOBJ 64
typedef struct {
    uint32_t ref[NOBJ]; int genre[NOBJ], surf[NOBJ], en_file[NOBJ], rendus[NOBJ], surf_retirees[NOBJ];
    zf_liaisons l;
    char journal[4096]; int nj;                       /* « O<n> » oubli de n objets, « R<o> » rendu, « S<o> » surfaces */
    int oublis, rendus_tot, oubli_avant_rendu_ko, rendu_non_oublie;
    uint8_t oublie[NOBJ];
} monde;
static void noter(monde *m, const char *fmt, unsigned v)   /* journal borné (les tirages le dépassent : on cesse) */
{
    if (m->nj < (int)sizeof m->journal - 32) m->nj += snprintf(m->journal + m->nj, sizeof m->journal - (size_t)m->nj, fmt, v);
}
static uint32_t o_ref(void *x, uint32_t o) { return ((monde *)x)->ref[o]; }
static int o_genre(void *x, uint32_t o) { return ((monde *)x)->genre[o]; }
static int o_surfaces(void *x, uint32_t o, int retirer)
{
    monde *m = x;
    if (retirer) { m->surf_retirees[o]++; noter(m, "S%u ", o); return 0; }
    return m->surf[o];
}
static int o_lie(void *x, uint32_t o) { return zf_lie(&((monde *)x)->l, o); }
static void o_hors_file(void *x, uint32_t o) { ((monde *)x)->en_file[o] = 0; }
static void o_oublier(void *x, const uint32_t *objs, int n)
{
    monde *m = x; m->oublis++;
    for (int i = 0; i < n; i++) m->oublie[objs[i]] = 1;
    noter(m, "O%u ", (unsigned)n);
}
static void o_rendre(void *x, uint32_t o)
{
    monde *m = x;
    if (!m->oublie[o]) m->rendu_non_oublie++;
    m->rendus[o]++; m->rendus_tot++; m->en_file[o] = 0;
    noter(m, "R%u ", o);
}
static const zf_ops OPS = { o_ref, o_genre, o_surfaces, o_lie, o_hors_file, o_oublier, o_rendre };

static zf_entree g_f[NOBJ]; static uint32_t g_nf; static zf_compteurs g_c;
static void raz(monde *m) { memset(m, 0, sizeof *m); memset(g_f, 0, sizeof g_f); g_nf = 0; memset(&g_c, 0, sizeof g_c); }
static void mettre(monde *m, uint32_t o, uint32_t frame)   /* comme zomb_mettre : marque « en file » à l'appelant */
{
    if (!m->en_file[o] && zf_mettre(g_f, &g_nf, NOBJ, o, frame, &g_c)) m->en_file[o] = 1;
}
static uint32_t present(monde *m, uint32_t n)              /* comme zombies_traiter */
{
    uint32_t prets[NOBJ];
    uint32_t np = zf_trier(g_f, &g_nf, n, &OPS, m, prets, &g_c);
    zf_rendre(prets, np, &OPS, m, &g_c);
    return np;
}

int main(void)
{
    static monde m;
    /* 1. délai */
    raz(&m); m.genre[5] = ZF_TAMPON; mettre(&m, 5, 10);
    int au = -1;
    for (uint32_t n = 10; n < 20 && au < 0; n++) if (present(&m, n)) au = (int)n;
    CHECK(au == (int)(10 + ZF_DELAI) && ZF_DELAI >= 3 && m.rendus[5] == 1,
          "délai : tombé au Present 10, rendu au Present %d (attendu %u = 10 + ZF_DELAI, ≥ 2 frames complètes)", au, 10 + ZF_DELAI);
    /* 2. liaisons : chaque emplacement du device garde l'objet */
    const char *noms[6] = { "étage de texture 7", "flux 3", "RT 1", "indices", "profondeur", "back buffer" };
    int garde_ok = 1;
    for (int k = 0; k < 6; k++) {
        raz(&m); m.genre[9] = k == 0 ? ZF_CONTENEUR : k == 1 || k == 3 ? ZF_TAMPON : ZF_SURFACE;
        if (k == 0) m.l.tex[7] = 9; else if (k == 1) m.l.flux[3] = 9; else if (k == 2) m.l.rt[1] = 9;
        else if (k == 3) m.l.indices = 9; else if (k == 4) m.l.ds = 9; else m.l.backbuf = 9;
        mettre(&m, 9, 0);
        for (uint32_t n = 0; n < 12; n++) present(&m, n);
        int garde = !m.rendus[9] && g_nf == 1 && g_c.lies > 0;
        memset(&m.l, 0, sizeof m.l);                  /* délié : rendu au Present suivant */
        present(&m, 12);
        if (!(garde && m.rendus[9] == 1 && !g_nf)) { garde_ok = 0; printf("      échec : %s\n", noms[k]); }
    }
    CHECK(garde_ok, "liaisons : lié (étage, flux, RT, indices, profondeur, back buffer) → gardé en file ; délié → rendu");
    /* 3. repris */
    raz(&m); m.genre[3] = ZF_CONTENEUR; mettre(&m, 3, 0); m.ref[3] = 1;
    for (uint32_t n = 0; n < 8; n++) present(&m, n);
    CHECK(!m.rendus[3] && !g_nf && !m.en_file[3] && g_c.reprises == 1, "repris (AddRef après zéro) : sorti de la file, jamais rendu");
    /* 4. surfaces de niveaux */
    raz(&m); m.genre[4] = ZF_CONTENEUR; m.surf[4] = -1; mettre(&m, 4, 0);
    for (uint32_t n = 0; n < 8; n++) present(&m, n);
    int tenue = !m.rendus[4] && !g_nf && g_c.retenus == 1;
    m.surf[4] = 0; mettre(&m, 4, 8);                  /* la surface rendue à zéro remet le conteneur en file */
    for (uint32_t n = 8; n < 16; n++) present(&m, n);
    CHECK(tenue && m.rendus[4] == 1 && m.surf_retirees[4] == 1, "surface tenue : conteneur sorti ; remis en file → rendu avec ses surfaces");
    raz(&m); m.genre[6] = ZF_CONTENEUR; m.surf[6] = -2; mettre(&m, 6, 0);
    for (uint32_t n = 0; n < 8; n++) present(&m, n);
    int liee = !m.rendus[6] && g_nf == 1;
    m.surf[6] = 0; present(&m, 8);
    CHECK(liee && m.rendus[6] == 1, "surface liée au device : conteneur gardé en file, rendu une fois déliée");
    raz(&m); m.genre[7] = ZF_AUTRE; mettre(&m, 7, 0);
    for (uint32_t n = 0; n < 8; n++) present(&m, n);
    CHECK(!m.rendus[7] && !g_nf, "genre plus rendable (conteneur marqué « jamais ») : sorti sans rendu");
    /* 5. ordre oubli → rendus, surfaces avant conteneur */
    raz(&m);
    for (uint32_t o = 10; o < 14; o++) { m.genre[o] = o == 10 ? ZF_CONTENEUR : ZF_TAMPON; mettre(&m, o, 0); }
    uint32_t np = present(&m, ZF_DELAI);
    CHECK(np == 4 && m.oublis == 1 && !strncmp(m.journal, "O4 S10 R10 ", 11) && !m.rendu_non_oublie,
          "ordre : UN oubli Vulkan des 4 prêts, puis surfaces du conteneur, puis rendus (journal « %s»)", m.journal);
    /* 6. file pleine */
    raz(&m); int refus = 0;
    for (uint32_t o = 0; o < NOBJ; o++) { m.genre[o] = ZF_TAMPON; mettre(&m, o, 0); }
    zf_entree tmp; uint32_t nplein = NOBJ;
    refus = !zf_mettre(&tmp, &nplein, NOBJ, 1, 0, &g_c) && g_c.pleine == 1;
    CHECK(refus && g_nf == NOBJ, "file pleine : refus compté, rien d'écrit");
    /* 7. tirages : jamais un objet vivant ni lié rendu, jamais deux fois, tout finit rendu */
    raz(&m); srand(0x24102026u);
    int vivant_rendu = 0, double_rendu = 0;
    for (uint32_t n = 0; n < 100000; n++) {
        uint32_t o = (uint32_t)rand() % NOBJ;
        if (!m.genre[o]) m.genre[o] = 1 + rand() % 3;
        switch (rand() % 6) {
        case 0: if (!m.rendus[o]) { m.ref[o] = 0; mettre(&m, o, n); } break;         /* Release → 0 */
        case 1: if (!m.rendus[o] && m.en_file[o] && rand() % 4 == 0) m.ref[o] = 1; break;   /* repris */
        case 2: m.l.tex[rand() % 20] = rand() % 2 ? o : 0; break;
        case 3: m.l.flux[rand() % 16] = rand() % 2 ? o : 0; break;
        default: break;
        }
        uint32_t prets[NOBJ], k = zf_trier(g_f, &g_nf, n, &OPS, &m, prets, &g_c);
        for (uint32_t i = 0; i < k; i++) {
            if (m.ref[prets[i]] || zf_lie(&m.l, prets[i])) vivant_rendu++;
            if (m.rendus[prets[i]]) double_rendu++;
        }
        zf_rendre(prets, k, &OPS, &m, &g_c);
        for (uint32_t i = 0; i < k; i++) { m.genre[prets[i]] = 0; m.rendus[prets[i]] = 0; m.oublie[prets[i]] = 0; }   /* adresse resservie */
    }
    CHECK(!vivant_rendu && !double_rendu && !m.rendu_non_oublie && m.rendus_tot > 1000,
          "100 000 événements tirés : %d rendus, 0 objet vivant ou lié rendu (%d), 0 double (%d), tous oubliés avant (%d)",
          m.rendus_tot, vivant_rendu, double_rendu, m.rendu_non_oublie);
    printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
