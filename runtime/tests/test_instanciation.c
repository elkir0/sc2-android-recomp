/* test_instanciation.c — V2.2 (22/10) : plan d'instanciation (runtime/sc2_d3d9_inst.h) jugé contre une RÉFÉRENCE de la
 * lecture des sommets par D3D9. Pur : ni Vulkan, ni mémoire invitée.
 *
 *   - Référence D3D9 : l'attribut d'offset a du flux s, pour l'instance i et la position k de la liste d'index, lit
 *     l'élément i / max(d, 1) si le flux est en INSTANCEDATA | d, sinon BaseVertexIndex + idx[k] ; adresse
 *     off_flux + élément × stride + a dans le VB. Nombre d'instances : la valeur N d'INDEXEDDATA (1 sans INDEXEDDATA).
 *   - Implémentation jugée : le plan (inst_planifier, inst_plage, inst_borne) appliqué comme dessin_reel (masque
 *     g_freq_non1 de V1.3, même troncature à la fin du VB), puis la RÈGLE VULKAN : par sommet, élément
 *     idx[k] + vertexOffset (vertexOffset = −MinIndex) dans la copie ; par instance, élément i (firstInstance 0,
 *     diviseur 1). Une lecture hors de la copie est un échec (« lecture GPU hors plage »).
 *   - Cas fixes (forme SC2, plan neutre = canon, replis, INSTANCEDATA seul, troncature, stride 0) puis 200 000 cas
 *     tirés (graine imprimée). SC2_TEST_GRAINE=<graine d'un cas> rejoue ce seul cas en détail.
 * Sortie : « RÉSULTAT : x PASS, y FAIL », code 0 sans FAIL. Mutants : scripts/injecter-bugs-v22.sh. */
#include "sc2_d3d9_inst.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) { g_pass++; printf("PASS  "); } else { g_fail++; printf("FAIL  "); } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define NF_MAX 4
#define NA_MAX 3
#define NI_MAX 16
typedef struct { uint32_t a, sz; } attribut;
typedef struct {
    uint32_t s;                 /* numéro de flux D3D9 (0..15) */
    uint32_t freq;              /* D.freq[s] */
    uint32_t stride, off;       /* D.stream[s][2], D.stream[s][1] */
    uint32_t tvb;               /* O_W du VB (octets) */
    uint32_t nattr; attribut attr[NA_MAX];
} flux_cas;
typedef struct {
    uint32_t nflux; flux_cas f[NF_MAX];
    int32_t base; uint32_t minidx, nverts;
    uint32_t nidx, idx[NI_MAX];
} cas;
typedef struct {
    inst_plan p;
    uint32_t masque;
    int instancie;              /* d.instances posé (par_inst ≠ 0 ou n > 1) */
    uint32_t instances;         /* instanceCount Vulkan : 1 sans instanciation ; 0 = dessin supprimé (« vides ») */
    struct { int par_inst; uint64_t debut, octets, copie; } f[NF_MAX];
    int32_t vertex_offset;
} vk_dessin;

/* ---- implémentation jugée : la préparation de dessin_reel (sc2_d3d9.c), flux par flux ---------------------------- */
static void vk_preparer(const cas *c, vk_dessin *v, int neutre)
{
    uint32_t freq[16] = { 0 }, masque = 0;
    memset(v, 0, sizeof *v);
    for (uint32_t f = 0; f < c->nflux; f++) {
        freq[c->f[f].s] = c->f[f].freq;
        if (c->f[f].freq > 1) masque |= 1u << c->f[f].s;       /* v_SetStreamSourceFreq : g_freq_non1 (V1.3) */
    }
    v->masque = masque;
    if (neutre) v->p = (inst_plan){ 1, 0, 0, 0, INST_OK, 0 };  /* SC2_INSTANCES=0 : le dessin d'avant (témoin) */
    else v->p = inst_planifier(freq, masque);
    uint32_t ninst = v->p.n;
    uint32_t premier = (uint32_t)(c->base + (int32_t)c->minidx), nsommets = c->nverts;
    for (uint32_t f = 0; f < c->nflux; f++) {
        const flux_cas *x = &c->f[f];
        int par = (int)((v->p.par_inst >> x->s) & 1u);
        uint64_t debut, n, dispo = x->tvb > x->off ? x->tvb - x->off : 0;
        inst_plage(par, premier, nsommets, v->p.n, x->stride, &debut, &n);
        if (debut + n > dispo) {
            n = dispo > debut ? dispo - debut : 0;
            if (par) { uint32_t b = inst_borne(ninst, n, x->stride); if (b < ninst) ninst = b; }
        }
        v->f[f].par_inst = par; v->f[f].debut = debut; v->f[f].octets = n;
        v->f[f].copie = n ? n : 16;     /* sc2_d3d9v.c : un flux vide passe par l'anneau avec 16 octets (volatils_puiser) */
    }
    v->instancie = v->p.par_inst || v->p.n > 1;
    v->instances = v->instancie ? ninst : 1;
    v->vertex_offset = -(int32_t)c->minidx;
}
/* règle Vulkan : adresse (dans le VB) lue par l'attribut ; 0 si la lecture sort de la copie */
static int vk_adresse(const cas *c, const vk_dessin *v, uint32_t f, uint32_t i, uint32_t k, const attribut *at, uint64_t *adr)
{
    const flux_cas *x = &c->f[f];
    uint64_t e;
    if (v->f[f].par_inst) e = i;                                /* RATE_INSTANCE, diviseur 1, firstInstance 0 */
    else {
        int64_t ev = (int64_t)c->idx[k] + v->vertex_offset;     /* RATE_VERTEX : index + vertexOffset */
        if (ev < 0) return 0;
        e = (uint64_t)ev;
    }
    uint64_t q = e * x->stride + at->a;                         /* octet de la copie (liaison au début de la copie) */
    if (q + at->sz > v->f[f].copie) return 0;
    *adr = x->off + v->f[f].debut + q;
    return 1;
}

/* ---- référence D3D9 ---------------------------------------------------------------------------------------------- */
static uint64_t ref_adresse(const cas *c, uint32_t f, uint32_t i, uint32_t k, const attribut *at)
{
    const flux_cas *x = &c->f[f];
    uint64_t e;
    if (x->freq & D3DSSF_INSTANCEDATA) { uint32_t d = x->freq & D3DSSF_VALEUR; e = i / (d ? d : 1); }
    else e = (uint64_t)((int64_t)c->base + c->idx[k]);
    return x->off + e * x->stride + at->a;
}
static uint32_t ref_instances(const cas *c)
{
    for (uint32_t f = 0; f < c->nflux; f++)
        if ((c->f[f].freq & D3DSSF_INDEXEDDATA) && !(c->f[f].freq & D3DSSF_INSTANCEDATA)) {
            uint32_t v = c->f[f].freq & D3DSSF_VALEUR; return v ? v : 1;
        }
    return 1;
}
/* repli attendu (INST_*) : diviseur > 1, deux N différents, drapeaux doubles — la référence ne s'applique pas */
static uint32_t ref_repli(const cas *c)
{
    uint32_t n = 0, r = INST_OK;
    for (uint32_t s = 0; s < 16 && !r; s++)
        for (uint32_t f = 0; f < c->nflux; f++) {
            if (c->f[f].s != s || c->f[f].freq <= 1) continue;
            uint32_t q = c->f[f].freq, v = q & D3DSSF_VALEUR;
            if ((q & D3DSSF_INSTANCEDATA) && (q & D3DSSF_INDEXEDDATA)) r = INST_REPLI_DRAPEAUX;
            else if (q & D3DSSF_INSTANCEDATA) { if (v > 1) r = INST_REPLI_DIVISEUR; }
            else if (q & D3DSSF_INDEXEDDATA) { uint32_t m = v ? v : 1; if (n && m != n) r = INST_REPLI_N; if (m > n) n = m; }
        }
    return r;
}
/* instances entières que la référence peut lire dans le VB des flux d'instance (au-delà, D3D9 lirait hors du VB) */
static uint32_t ref_lisibles(const cas *c, uint32_t n)
{
    for (uint32_t f = 0; f < c->nflux; f++) {
        const flux_cas *x = &c->f[f];
        if (!(x->freq & D3DSSF_INSTANCEDATA) || !x->stride) continue;
        uint64_t dispo = x->tvb > x->off ? x->tvb - x->off : 0, k = dispo / x->stride;
        if (k < n) n = (uint32_t)k;
    }
    return n;
}

/* ---- juge : un cas → nombre d'écarts (message du premier dans err) ----------------------------------------------- */
static uint64_t g_lectures;
static int juger(const cas *c, int detail, char *err, size_t nerr)
{
    vk_dessin v; vk_preparer(c, &v, 0);
    uint32_t r = ref_repli(c);
    int ecarts = 0;
#define ECART(...) do { if (!ecarts++) snprintf(err, nerr, __VA_ARGS__); if (detail) { printf("      écart : "); \
                        printf(__VA_ARGS__); printf("\n"); } } while (0)
    if (detail)
        printf("      plan : n %u, par_inst 0x%X, indexes 0x%X, Σ %u, repli %u (vu 0x%X) ; masque 0x%X ; instances %u%s\n",
               v.p.n, v.p.par_inst, v.p.indexes, v.p.somme_idx, v.p.repli, v.p.vu, v.masque, v.instances,
               v.instancie ? " (instancié)" : "");
    uint32_t premier = (uint32_t)(c->base + (int32_t)c->minidx);
    if (r) {                                                   /* repli : plan neutre, le dessin d'avant */
        if (v.p.repli != r) ECART("repli %u attendu, %u obtenu", r, v.p.repli);
        if (v.p.n != 1 || v.p.par_inst || v.instancie) ECART("repli : plan non neutre (n %u, par_inst 0x%X)", v.p.n, v.p.par_inst);
        for (uint32_t f = 0; f < c->nflux; f++)
            if (v.f[f].debut != (uint64_t)premier * c->f[f].stride)
                ECART("repli : flux %u, début %" PRIu64 " ≠ formule d'avant", c->f[f].s, v.f[f].debut);
        return ecarts;
    }
    if (v.p.repli) ECART("repli %u inattendu", v.p.repli);
    if (!v.masque && (v.p.n != 1 || v.p.par_inst || v.p.indexes || v.p.somme_idx || v.instancie))
        ECART("fréquences dans {0, 1} : plan non neutre");
    uint32_t attendu = ref_lisibles(c, ref_instances(c));
    if (v.instances != attendu) ECART("instanceCount %u, référence %u", v.instances, attendu);
    uint32_t ni = v.instances < attendu ? v.instances : attendu;
    uint32_t ech[64], nech = 0;                                /* instances jugées : toutes jusqu'à 48, sinon un échantillon */
    if (ni <= 48) for (uint32_t i = 0; i < ni; i++) ech[nech++] = i;
    else {
        uint32_t h = (uint32_t)(c->base * 2654435761u) ^ c->nidx;
        for (uint32_t i = 0; i < 4; i++) { ech[nech++] = i; ech[nech++] = ni - 1 - i; }
        for (uint32_t i = 0; i < 24; i++) { h = h * 1103515245u + 12345u; ech[nech++] = (h >> 8) % ni; }
    }
    for (uint32_t q = 0; q < nech; q++)
        for (uint32_t k = 0; k < c->nidx; k++)
            for (uint32_t f = 0; f < c->nflux; f++)
                for (uint32_t a = 0; a < c->f[f].nattr; a++) {
                    const attribut *at = &c->f[f].attr[a];
                    uint64_t va = 0, ra = ref_adresse(c, f, ech[q], k, at);
                    g_lectures++;
                    if (ra + at->sz > c->f[f].tvb && c->f[f].stride)
                        ECART("générateur : la référence lit hors du VB (flux %u, instance %u)", c->f[f].s, ech[q]);
                    else if (!vk_adresse(c, &v, f, ech[q], k, at, &va))
                        ECART("lecture GPU hors plage : flux %u, instance %u, index n°%u, attribut +%u", c->f[f].s, ech[q], k, at->a);
                    else if (va != ra)
                        ECART("flux %u, instance %u, index n°%u, attribut +%u : Vulkan lit l'octet %" PRIu64 ", D3D9 %" PRIu64,
                              c->f[f].s, ech[q], k, at->a, va, ra);
                }
#undef ECART
    return ecarts;
}

/* ---- générateur ----------------------------------------------------------------------------------------------------- */
static uint64_t g_x;
static uint32_t alea(void) { g_x = g_x * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(g_x >> 33); }
static uint32_t tirer_n(void) { uint32_t u = alea() % 100; return u < 2 ? 0 : u < 10 ? 1 + alea() % 4 : 1 + alea() % 600; }
static void tirer(cas *c, uint64_t graine)
{
    g_x = graine;
    memset(c, 0, sizeof *c);
    c->nflux = 1 + alea() % NF_MAX;
    uint32_t pris = 0;
    for (uint32_t f = 0; f < c->nflux; f++) {
        flux_cas *x = &c->f[f];
        do x->s = alea() % 16; while (pris & (1u << x->s));
        pris |= 1u << x->s;
        x->stride = alea() % 50 ? 4 * (1 + alea() % 63) : 0;   /* 4 à 252 (MaxStreamStride 255), 2 % de stride 0 */
        x->off = 4 * (alea() % 1025);
        x->nattr = 1 + alea() % NA_MAX;
        for (uint32_t a = 0; a < x->nattr; a++) {
            uint32_t sz = 4 * (1 + alea() % 4), lim = x->stride ? x->stride : 16;
            if (sz > lim) sz = 4;
            x->attr[a] = (attribut){ 4 * (alea() % ((lim - sz) / 4 + 1)), sz };
        }
        x->freq = alea() % 2;                                   /* 0 (jamais posé) ou 1 : fréquence normale */
    }
    int g = alea() % 10 ? (int)(alea() % c->nflux) : -1;        /* flux de géométrie INDEXEDDATA | N (90 %) */
    int i = -1;
    if (c->nflux > 1 && alea() % 100 < 85) do i = (int)(alea() % c->nflux); while (i == g);
    else if (g < 0 && alea() % 2) i = (int)(alea() % c->nflux);  /* INSTANCEDATA sans INDEXEDDATA */
    uint32_t n = tirer_n();
    if (g >= 0) c->f[g].freq = D3DSSF_INDEXEDDATA | n;
    if (i >= 0) c->f[i].freq = D3DSSF_INSTANCEDATA | (alea() % 50 ? 1u : 0u);
    uint32_t u = alea() % 100;                                  /* 8 % : replis et fréquences anciennes */
    if (u < 2 && i >= 0) c->f[i].freq = D3DSSF_INSTANCEDATA | (2 + alea() % 7);
    else if (u < 4 && c->nflux > 1) {                           /* second INDEXEDDATA (même N ou non) */
        int h; do h = (int)(alea() % c->nflux); while (h == g);
        c->f[h].freq = D3DSSF_INDEXEDDATA | (alea() % 2 ? n : tirer_n());
    } else if (u < 6) c->f[alea() % c->nflux].freq = D3DSSF_INDEXEDDATA | D3DSSF_INSTANCEDATA | (alea() % 4);
    else if (u < 8) {                                           /* ancienne fréquence de sommets : ignorée, par sommet */
        uint32_t f = alea() % c->nflux;
        if ((int)f != g && (int)f != i) c->f[f].freq = 2 + alea() % 9;
    }
    c->base = alea() % 4 ? (int32_t)(alea() % 2001) : 0;
    c->minidx = alea() % 3 ? alea() % 65 : 0;
    c->nverts = 1 + alea() % 64;
    c->nidx = 1 + alea() % NI_MAX;
    for (uint32_t k = 0; k < c->nidx; k++) c->idx[k] = c->minidx + alea() % c->nverts;
    uint32_t nref = ref_instances(c);
    for (uint32_t f = 0; f < c->nflux; f++) {
        flux_cas *x = &c->f[f];
        uint32_t lim = x->stride ? 0 : 16;                       /* stride 0 : l'élément 0 seul (16 octets au plus) */
        if (x->freq & D3DSSF_INSTANCEDATA) {
            x->tvb = x->off + nref * x->stride + lim + alea() % 64;
            uint32_t t = alea() % 100;
            if (t < 12) x->tvb = x->off + lim + alea() % (nref * x->stride + 1);   /* tronqué */
            else if (t < 14) x->tvb = x->off + lim;                                 /* vide (stride 0 : un élément) */
            else if (t < 15) x->tvb = x->off ? alea() % x->off : 0;                 /* décalage au-delà du VB */
        } else x->tvb = x->off + (uint32_t)(c->base + (int32_t)(c->minidx + c->nverts)) * x->stride + lim + alea() % 64;
    }
}
static void imprimer(const cas *c)
{
    printf("      cas : base %d, minidx %u, nverts %u, %u index :", c->base, c->minidx, c->nverts, c->nidx);
    for (uint32_t k = 0; k < c->nidx; k++) printf(" %u", c->idx[k]);
    printf("\n");
    for (uint32_t f = 0; f < c->nflux; f++)
        printf("      flux D3D9 %u : freq 0x%08X, stride %u, off %u, VB %u o, %u attributs\n", c->f[f].s, c->f[f].freq,
               c->f[f].stride, c->f[f].off, c->f[f].tvb, c->f[f].nattr);
}

/* ---- cas fixes -------------------------------------------------------------------------------------------------------- */
/* forme SC2 (V1.3 : un flux INDEXEDDATA | N et un flux INSTANCEDATA | 1) : quad de 4 coins, 6 index */
static void forme_sc2(cas *c, uint32_t n, uint32_t sg, uint32_t si, int32_t base, uint32_t minidx)
{
    static const uint32_t quad[6] = { 0, 1, 2, 2, 1, 3 };
    memset(c, 0, sizeof *c);
    c->nflux = 2; c->base = base; c->minidx = minidx; c->nverts = 4; c->nidx = 6;
    for (int k = 0; k < 6; k++) c->idx[k] = minidx + quad[k];
    c->f[0] = (flux_cas){ sg, D3DSSF_INDEXEDDATA | n, 8, 32, 0, 1, { { 0, 8 } } };              /* normal0 : coin */
    c->f[0].tvb = c->f[0].off + (uint32_t)(base + (int32_t)minidx + 4) * 8;
    c->f[1] = (flux_cas){ si, D3DSSF_INSTANCEDATA | 1, 64, 128, 0, 3, { { 0, 12 }, { 12, 16 }, { 48, 16 } } };
    c->f[1].tvb = c->f[1].off + n * 64;
}
static void tests_fixes(void)
{
    char err[256]; cas c; vk_dessin v;
    /* 1. forme SC2 */
    static const uint32_t ns[8] = { 1, 2, 3, 15, 96, 255, 441, 516 };
    static const int32_t bases[3] = { 0, 7, 1000 };
    int ko = 0, nb = 0, temoins = 0;
    for (int a = 0; a < 8; a++) for (int sens = 0; sens < 2; sens++) for (int b = 0; b < 3; b++) for (int m = 0; m < 2; m++) {
        forme_sc2(&c, ns[a], sens ? 1 : 0, sens ? 0 : 1, bases[b], m ? 3 : 0);
        nb++;
        if (juger(&c, 0, err, sizeof err)) {
            if (!ko++) printf("      N %u, sens %d, base %d, minidx %u : %s\n", ns[a], sens, bases[b], c.minidx, err);
            continue;
        }
        vk_preparer(&c, &v, 0);
        if (v.instances != ns[a] || v.p.par_inst != 1u << c.f[1].s || v.p.indexes != 1u << c.f[0].s || v.p.somme_idx != ns[a])
            { ko++; continue; }
        /* témoin : le dessin d'avant (plan neutre) ne lit pas ce que lit D3D9 → le juge discrimine */
        vk_preparer(&c, &v, 1);
        uint64_t va = 0;
        int differe = v.instances == 1 && ns[a] > 1;
        for (uint32_t k = 0; k < 6 && !differe; k++)
            if (!vk_adresse(&c, &v, 1, 0, k, &c.f[1].attr[0], &va) || va != ref_adresse(&c, 1, 0, k, &c.f[1].attr[0])) differe = 1;
        temoins += differe;
    }
    CHECK(!ko, "forme SC2 (quad 4 coins, 6 index ; N 1 à 516 ; géométrie/instance sur les flux 0/1 et 1/0 ; base 0, 7, 1000 ; "
               "minidx 0, 3) : %d configurations conformes à la référence D3D9", nb - ko);
    CHECK(temoins == nb, "témoin : le dessin d'avant (instanceCount 1, flux d'instance lu par sommet) diffère de la référence "
                         "dans %d configurations sur %d", temoins, nb);
    /* 2. plan neutre = canon (fréquences dans {0, 1}) */
    uint32_t bidon[16]; for (int s = 0; s < 16; s++) bidon[s] = 0xC0000000u | (uint32_t)s;
    inst_plan p = inst_planifier(bidon, 0);
    CHECK(p.n == 1 && !p.par_inst && !p.indexes && !p.somme_idx && !p.repli && !p.vu, "masque nul : plan neutre, freq[] non lu");
    forme_sc2(&c, 1, 0, 1, 7, 3); c.f[0].freq = 1; c.f[1].freq = 0; c.f[1].tvb = c.f[1].off + 14 * 64;
    vk_preparer(&c, &v, 0);
    CHECK(!v.masque && !v.instancie && v.instances == 1 && v.f[0].debut == 10 * 8 && v.f[0].octets == 4 * 8
          && v.f[1].debut == 10 * 64 && v.f[1].octets == 4 * 64 && !juger(&c, 0, err, sizeof err),
          "fréquences 1 et 0 : plan neutre, plages de la formule d'avant (premier × stride, nsommets × stride)");
    forme_sc2(&c, 1, 0, 1, 0, 0); c.f[1].freq = 1;              /* INDEXEDDATA | 1 sans flux d'instance */
    c.f[1].tvb = c.f[1].off + 4 * 64;
    vk_preparer(&c, &v, 0);
    CHECK(v.masque == 1 && v.p.n == 1 && !v.p.par_inst && !v.instancie && v.instances == 1 && !juger(&c, 0, err, sizeof err),
          "INDEXEDDATA | 1 sans flux d'instance : n = 1, aucun flux par instance, dessin d'avant");
    uint32_t ancien[16] = { 0 }; ancien[2] = 5; ancien[4] = 0x3FFFFFFFu;    /* fréquences de sommets anciennes, sans drapeau */
    p = inst_planifier(ancien, (1u << 2) | (1u << 4));
    CHECK(p.n == 1 && !p.par_inst && !p.indexes && !p.repli, "fréquence ≥ 2 sans drapeau (ancienne fréquence de sommets) : ignorée");
    int ecart = 0;
    g_x = 0x5EED2022u;
    for (int t = 0; t < 10000; t++) {
        uint32_t pr = alea() ^ (alea() << 16), ns2 = alea() % 70000, st = alea() % 256, n = 1 + alea() % 600;
        uint64_t d, o;
        inst_plage(0, pr, ns2, n, st, &d, &o);
        if (d != (uint64_t)pr * st || o != (uint64_t)ns2 * st) ecart++;
    }
    CHECK(!ecart, "inst_plage par sommet = formule d'avant à l'octet sur 10 000 tirages (%d écarts)", ecart);
    /* 3. replis */
    uint32_t fq[16] = { 0 };
    fq[0] = D3DSSF_INDEXEDDATA | 10; fq[1] = D3DSSF_INSTANCEDATA | 2;
    p = inst_planifier(fq, 3);
    CHECK(p.repli == INST_REPLI_DIVISEUR && p.vu == 2 && p.n == 1 && !p.par_inst, "INSTANCEDATA | 2 : repli diviseur (vu 2), plan neutre");
    fq[1] = D3DSSF_INSTANCEDATA | 1; fq[2] = D3DSSF_INDEXEDDATA | 12;
    p = inst_planifier(fq, 7);
    CHECK(p.repli == INST_REPLI_N && p.vu == 12 && p.n == 1 && !p.par_inst, "deux N différents (10, 12) : repli, plan neutre");
    fq[2] = D3DSSF_INDEXEDDATA | 10;
    p = inst_planifier(fq, 7);
    CHECK(!p.repli && p.n == 10 && p.indexes == 5 && p.somme_idx == 20 && p.par_inst == 2, "deux INDEXEDDATA de même N : n = 10, Σ 20");
    fq[2] = 0; fq[1] = 0xC0000001u;
    p = inst_planifier(fq, 3);
    CHECK(p.repli == INST_REPLI_DRAPEAUX && p.vu == 0xC0000001u && p.n == 1 && !p.par_inst,
          "INSTANCEDATA et INDEXEDDATA sur un même flux : repli drapeaux, plan neutre");
    forme_sc2(&c, 37, 0, 1, 7, 3); c.f[1].freq = D3DSSF_INSTANCEDATA | 3;
    CHECK(!juger(&c, 0, err, sizeof err), "repli diviseur dans le juge : plages de la formule d'avant%s%s", *err ? " — " : "", err);
    /* 4. INSTANCEDATA sans INDEXEDDATA */
    forme_sc2(&c, 1, 0, 1, 7, 3); c.f[0].freq = 0; c.f[1].tvb = c.f[1].off + 64;
    vk_preparer(&c, &v, 0);
    CHECK(v.p.n == 1 && v.p.par_inst == 2 && v.instancie && v.instances == 1 && !juger(&c, 0, err, sizeof err),
          "INSTANCEDATA sans INDEXEDDATA : une instance, enregistrement 0 pour tous les sommets");
    /* 5. troncature */
    forme_sc2(&c, 96, 0, 1, 7, 3); c.f[1].tvb = c.f[1].off + 96 * 64 - 1;
    vk_preparer(&c, &v, 0);
    CHECK(v.instances == 95 && !juger(&c, 0, err, sizeof err),
          "VB d'instance de N × stride − 1 octets : instanceCount N − 1, aucune lecture hors copie%s%s", *err ? " — " : "", err);
    forme_sc2(&c, 96, 0, 1, 7, 3); c.f[1].tvb = c.f[1].off;
    vk_preparer(&c, &v, 0);
    CHECK(v.instancie && v.instances == 0 && !juger(&c, 0, err, sizeof err), "VB d'instance vide : instanceCount 0 (dessin supprimé)");
    forme_sc2(&c, 96, 0, 1, 7, 3); c.f[1].tvb = c.f[1].off - 4;
    vk_preparer(&c, &v, 0);
    CHECK(v.instances == 0, "décalage du flux d'instance au-delà du VB : instanceCount 0");
    /* 6. stride 0 sur le flux d'instance */
    forme_sc2(&c, 441, 1, 0, 1000, 0); c.f[1].stride = 0; c.f[1].nattr = 1; c.f[1].attr[0] = (attribut){ 0, 16 };
    c.f[1].tvb = c.f[1].off + 16;
    vk_preparer(&c, &v, 0);
    CHECK(v.instances == 441 && !juger(&c, 0, err, sizeof err), "stride 0 sur le flux d'instance : 441 instances, toutes à l'élément 0");
}

int main(void)
{
    const char *e = getenv("SC2_TEST_GRAINE");
    char err[256] = "";
    if (e && *e) {                                             /* rejeu d'un seul cas, en détail */
        uint64_t g = strtoull(e, NULL, 0); cas c;
        tirer(&c, g);
        imprimer(&c);
        int n = juger(&c, 1, err, sizeof err);
        CHECK(!n, "cas de graine 0x%016" PRIx64 " : %d écart(s)%s%s", g, n, n ? " — " : "", err);
        printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    tests_fixes();
    const uint64_t base = 0x2210202622000000ull;
    const uint32_t NCAS = 200000;
    uint32_t nko = 0, nrepli = 0, ninst = 0, ntronq = 0, nneutre = 0;
    g_lectures = 0;
    printf("      %u cas tirés, graine de base 0x%016" PRIx64 " (cas n°i : graine base + i)\n", NCAS, base);
    for (uint32_t i = 0; i < NCAS; i++) {
        cas c; tirer(&c, base + i);
        vk_dessin v; vk_preparer(&c, &v, 0);
        if (ref_repli(&c)) nrepli++;
        else if (!v.masque) nneutre++;
        else if (v.instancie) { ninst++; if (v.instances < ref_instances(&c)) ntronq++; }
        if (juger(&c, 0, err, sizeof err) && ++nko <= 8) {
            printf("      ÉCART cas n°%u (SC2_TEST_GRAINE=0x%016" PRIx64 ") : %s\n", i, base + i, err);
            imprimer(&c);
        }
    }
    CHECK(!nko, "%u cas tirés conformes à la référence D3D9 (%u écarts) : %u instanciés dont %u tronqués, %u replis, "
                "%u neutres ; %" PRIu64 " lectures d'attribut jugées, aucune hors copie", NCAS - nko, nko, ninst, ntronq, nrepli,
          nneutre, g_lectures);
    printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
