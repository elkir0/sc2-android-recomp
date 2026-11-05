/* sc2_spv_pack.c — pack SPIR-V externe sc2-spirv.pak (passe 10t), cf. sc2_spv_pack.h.
 *
 * Indépendant du reste du runtime : mmap lecture seule, pthread (ouverture
 * paresseuse et unique au premier appel si ouvrir() ne l'a pas fait), aucun
 * munmap — les mappings vivent le temps du processus (les ouvertures successives
 * du test se contentent d'empiler). Hôtes petit-boutistes (macOS arm64, Linux
 * x86-64, Android aarch64) ; champs relus par memcpy, aucun accès non aligné.
 */
#include "sc2_spv_pack.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* sc2_log est fourni par sc2_runtime.c dans le binaire ; la définition faible
 * sert aux cibles qui lient ce fichier seul (test_spv_pack). */
void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((weak)) void sc2_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout);
}

#define SPV_PACK_MAGIC   "SC2SPV01"   /* 8 octets, sans zéro final */
#define SPV_PACK_ENTREE  96           /* une entrée d'index */
#define SPV_PACK_MOT     0x07230203u  /* premier mot d'un blob SPIR-V */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static const uint8_t *g_base;     /* mmap du pack, ou NULL */
static uint64_t g_taille;         /* taille du fichier, vérifiée contre l'en-tête */
static uint32_t g_n;              /* entrées de l'index */
static char g_chemin[1024];       /* chemin du pack ouvert ("" : aucun) */
static char g_raison[192];        /* dernière raison d'absence (bilan) */
static uint8_t *g_journal;        /* bit par entrée : entrée invalide déjà signalée */

/* champs de l'en-tête et de l'index par memcpy (aucun accès non aligné) */
static uint32_t rd_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static uint64_t rd_u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, sizeof v); return v; }

/* Note l'absence du pack : raison courte pour le bilan, une ligne de journal. */
static void noter_absence(const char *fmt, ...)
{
    va_list ap;
    g_base = NULL; g_n = 0; g_chemin[0] = '\0';
    va_start(ap, fmt);
    vsnprintf(g_raison, sizeof g_raison, fmt, ap);
    va_end(ap);
    sc2_log("[SPV] pack SPIR-V externe absent : %s", g_raison);
}

/* Ouvre et valide le pack (en-tête intégral : magic, version, off_index, taille
 * annoncée = taille réelle de fstat, index entier dans le fichier, réservés à
 * zéro). g_mu tenue (ou appel unique du once). */
static int ouvrir_interne(const char *chemin)
{
    if (!chemin || !*chemin) { noter_absence("chemin vide"); return -1; }
    int fd = open(chemin, O_RDONLY);
    if (fd < 0) { noter_absence("%s : ouverture impossible", chemin); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 64) { close(fd); noter_absence("%s : fichier trop petit", chemin); return -1; }
    uint64_t taille = (uint64_t)st.st_size;
    const uint8_t *base = mmap(NULL, (size_t)taille, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);   /* le mapping vit sa vie : jamais défait, descripteur inutile */
    if (base == MAP_FAILED) { noter_absence("%s : mmap échoué", chemin); return -1; }

    if (memcmp(base, SPV_PACK_MAGIC, 8) != 0) { noter_absence("%s : magic invalide", chemin); return -1; }
    if (rd_u32(base + 8) != 1) { noter_absence("%s : version %u != 1", chemin, rd_u32(base + 8)); return -1; }
    uint32_t n = rd_u32(base + 12);
    uint64_t off_index = rd_u64(base + 16), taille_dite = rd_u64(base + 24);
    if (off_index != 64) { noter_absence("%s : off_index %llu != 64", chemin, (unsigned long long)off_index); return -1; }
    if (taille_dite != taille)
        { noter_absence("%s : taille annoncée %llu != taille réelle %llu", chemin,
                        (unsigned long long)taille_dite, (unsigned long long)taille); return -1; }
    if (64 + (uint64_t)n * SPV_PACK_ENTREE > taille)
        { noter_absence("%s : index de %u entrées hors fichier", chemin, n); return -1; }
    for (int i = 32; i < 64; i++) if (base[i]) { noter_absence("%s : octets réservés non nuls", chemin); return -1; }

    free(g_journal);   /* état de l'éventuel pack précédent (son mmap, lui, reste) */
    g_journal = calloc((n + 7) / 8, 1);   /* NULL si pénurie : une entrée invalide sera ressignalée à chaque recherche */
    snprintf(g_chemin, sizeof g_chemin, "%s", chemin);
    g_base = base; g_taille = taille; g_n = n;
    g_raison[0] = '\0';
    sc2_log("[SPV] pack SPIR-V externe ouvert : %s — %u entrées, %.1f Mo (mmap lecture seule)",
            g_chemin, g_n, g_taille / 1048576.0);
    return 0;
}

/* Chemin du pack par l'environnement : SC2_SPIRV_PACK s'il est défini et non
 * vide, sinon <dossier parent de SC2_INSTALL>/sc2-spirv.pak (ex. /sdcard/
 * SC2Partout/install → /sdcard/SC2Partout/sc2-spirv.pak). Rend 0 si rien. */
static int chemin_env(char *dst, size_t taille_dst)
{
    const char *e = getenv("SC2_SPIRV_PACK");
    if (e && *e) { snprintf(dst, taille_dst, "%s", e); return 1; }
    e = getenv("SC2_INSTALL");
    if (e && *e) {
        const char *s = strrchr(e, '/');
        if (!s) snprintf(dst, taille_dst, "sc2-spirv.pak");   /* pas de '/' : répertoire courant */
        else snprintf(dst, taille_dst, "%.*s/sc2-spirv.pak", (int)(s - e), e);
        return 1;
    }
    return 0;
}

/* Ouverture paresseuse (pthread_once) au premier appel si ouvrir() n'a pas été
 * appelé. Sans chemin dans l'environnement : pas de pack, une seule ligne. */
static void init_paresseuse(void)
{
    pthread_mutex_lock(&g_mu);
    char chemin[1024];
    if (chemin_env(chemin, sizeof chemin)) ouvrir_interne(chemin);
    else noter_absence("ni SC2_SPIRV_PACK ni SC2_INSTALL défini");
    pthread_mutex_unlock(&g_mu);
}

static void init_rien(void) { }   /* consomme le once : ouvrir() explicite il y a */

int sc2_spv_pack_ouvrir(const char *chemin)
{
    pthread_once(&g_once, init_rien);
    pthread_mutex_lock(&g_mu);
    int r = ouvrir_interne(chemin);
    pthread_mutex_unlock(&g_mu);
    return r;
}

int sc2_spv_pack_chercher(uint64_t hash, int ps, sc2_spv_pack_entree *e)
{
    pthread_once(&g_once, init_paresseuse);
    if (!g_base || !e || (ps != 0 && ps != 1)) return 0;
    /* dichotomie sur (hash, ps) : l'index est trié, sans doublon */
    uint64_t lo = 0, hi = g_n;
    while (lo < hi) {
        uint64_t m = lo + (hi - lo) / 2;
        const uint8_t *ent = g_base + 64 + m * SPV_PACK_ENTREE;
        uint64_t h = rd_u64(ent);
        if (h < hash || (h == hash && ent[8] < (uint8_t)ps)) lo = m + 1; else hi = m;
    }
    if (lo >= g_n) return 0;
    const uint8_t *ent = g_base + 64 + lo * SPV_PACK_ENTREE;
    if (rd_u64(ent) != hash || ent[8] != (uint8_t)ps) return 0;

    /* validation de l'entrée trouvée : bornes, alignement, premier mot SPIR-V */
    uint8_t nentrees = ent[9];
    uint16_t nconst = (uint16_t)(ent[10] | ent[11] << 8);   /* passe 10t : registres c lus (0 : bloc entier) */
    uint32_t nmots = rd_u32(ent + 12), nmots_ombre = rd_u32(ent + 16);
    uint64_t off_spv = rd_u64(ent + 24);
    const char *probleme = NULL;
    if (nentrees > 16) probleme = "nentrees > 16";
    else if (nconst > 256) probleme = "nconst > 256";
    else if (nmots == 0) probleme = "nmots nul";
    else if (off_spv % 16 != 0) probleme = "off_spv non aligné sur 16";
    else if (off_spv > g_taille || 4 * ((uint64_t)nmots + nmots_ombre) > g_taille - off_spv)   /* somme sur 64 bits : pas de débordement */
        probleme = "blob hors du fichier";
    else if (rd_u32(g_base + off_spv) != SPV_PACK_MOT) probleme = "premier mot != 0x07230203";
    else if (nmots_ombre && rd_u32(g_base + off_spv + (uint64_t)4 * nmots) != SPV_PACK_MOT)
        probleme = "variante « ombres » : premier mot != 0x07230203";
    if (probleme) {   /* entrée invalide = non trouvée, journalisée une seule fois */
        if (g_journal && !(g_journal[lo / 8] >> (lo % 8) & 1)) {
            g_journal[lo / 8] |= (uint8_t)(1u << (lo % 8));
            sc2_log("[SPV] pack %s : entrée %016llx/%d invalide — %s (dessins de ce shader sautés)",
                    g_chemin, (unsigned long long)hash, ps, probleme);
        }
        return 0;
    }

    e->hash = rd_u64(ent);
    e->ps = ent[8];
    e->nentrees = nentrees;
    e->nconst = nconst;
    memcpy(e->entrees, ent + 32, sizeof e->entrees);
    memcpy(e->samplers, ent + 80, sizeof e->samplers);
    e->nmots = nmots;
    e->spv = (const uint32_t *)(g_base + off_spv);   /* off_spv multiple de 16 : pointeur aligné */
    e->nmots_ombre = nmots_ombre;
    e->spv_ombre = nmots_ombre ? (const uint32_t *)(g_base + off_spv + (uint64_t)4 * nmots) : NULL;
    return 1;
}

uint32_t sc2_spv_pack_n(void) { pthread_once(&g_once, init_paresseuse); return g_n; }

const char *sc2_spv_pack_chemin(void)
{
    pthread_once(&g_once, init_paresseuse);   /* bilan exact même si aucun shader n'a manqué de la table */
    return g_chemin[0] ? g_chemin : NULL;
}

const char *sc2_spv_pack_raison(void)
{
    pthread_once(&g_once, init_paresseuse);
    return g_chemin[0] ? NULL : (g_raison[0] ? g_raison : "jamais ouvert");
}
