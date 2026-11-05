/* test_spv_pack.c — pack SPIR-V externe sc2-spirv.pak (passe 10t), cf. runtime/sc2_spv_pack.{c,h}.
 *
 * Un écrivain embarqué fabrique des packs temporaires conformes au format imposé
 * (3 entrées : un même hash en VS et en PS, une variante « ombres », hash triés),
 * puis des packs corrompus (magic faux, taille annoncée fausse par troncature,
 * entrée hors bornes). Les ouvertures paresseuses par SC2_SPIRV_PACK et par
 * SC2_INSTALL (chemin par défaut) sont vérifiées dans des fils forkés :
 * pthread_once n'est consommé qu'une fois par processus.
 */
#include "sc2_spv_pack.h"

#include <sys/wait.h>
#include <unistd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, msg) do { if (cond) { g_pass++; printf("PASS  %s\n", msg); } \
                              else { g_fail++; printf("FAIL  %s\n", msg); } } while (0)

/* ---- écrivain du pack (format imposé, petit-boutiste) -------------------------------------- */

static void w_u32(FILE *f, uint32_t v)
{ uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, f); }
static void w_u64(FILE *f, uint64_t v) { w_u32(f, (uint32_t)v); w_u32(f, (uint32_t)(v >> 32)); }
static void b_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void b_u64(uint8_t *p, uint64_t v) { b_u32(p, (uint32_t)v); b_u32(p + 4, (uint32_t)(v >> 32)); }

typedef struct { uint64_t hash; uint8_t ps, nentrees; uint32_t nmots, nmots_ombre; uint16_t nconst; } entree_test;

/* Mot k d'un blob : magie SPIR-V si k = 0, sinon une valeur dérivable du shader. */
static uint32_t mot(uint64_t hash, uint8_t ps, int ombre, uint32_t k)
{
    return k == 0 ? 0x07230203u : (uint32_t)hash ^ 0xAB000000u ^ ((uint32_t)ps << 28)
                   ^ (ombre ? 0x000F0000u : 0u) ^ (k * 0x00101010u);
}

/* Écrit un pack de n entrées (≤ 8, déjà triées par hash puis ps) ; rend 0 si OK
 * et la taille du fichier dans *taille. entrees[k] = {k, 0x40+k, 0x80+k},
 * samplers[k] propre à chaque entrée (n°entrée × 0x10 + k). */
static int ecrire_pack(const char *chemin, const entree_test *t, int n, uint64_t *taille)
{
    if (n < 1 || n > 8) return -1;
    uint64_t off[8], o = (64 + 96ull * n + 15) & ~15ull;   /* données alignées sur 16 après l'index */
    for (int i = 0; i < n; i++) {
        off[i] = o;
        o = (o + 4ull * (t[i].nmots + t[i].nmots_ombre) + 15) & ~15ull;
    }
    FILE *f = fopen(chemin, "wb");
    if (!f) return -1;
    fwrite("SC2SPV01", 1, 8, f);
    w_u32(f, 1); w_u32(f, (uint32_t)n); w_u64(f, 64); w_u64(f, o);
    static const uint8_t zeros[32]; fwrite(zeros, 1, 32, f);
    for (int i = 0; i < n; i++) {
        uint8_t e[96]; memset(e, 0, sizeof e);
        b_u64(e, t[i].hash);
        e[8] = t[i].ps; e[9] = t[i].nentrees;
        e[10] = (uint8_t)t[i].nconst; e[11] = (uint8_t)(t[i].nconst >> 8);   /* passe 10t */
        b_u32(e + 12, t[i].nmots); b_u32(e + 16, t[i].nmots_ombre);
        b_u64(e + 24, off[i]);
        for (int k = 0; k < t[i].nentrees && k < 16; k++)
            e[32 + 3 * k] = (uint8_t)k, e[33 + 3 * k] = (uint8_t)(0x40 + k), e[34 + 3 * k] = (uint8_t)(0x80 + k);
        for (int k = 0; k < 16; k++) e[80 + k] = (uint8_t)(i * 0x10 + k);
        fwrite(e, 1, 96, f);
    }
    for (int i = 0; i < n; i++) {
        for (uint32_t k = 0; k < t[i].nmots; k++) w_u32(f, mot(t[i].hash, t[i].ps, 0, k));
        for (uint32_t k = 0; k < t[i].nmots_ombre; k++) w_u32(f, mot(t[i].hash, t[i].ps, 1, k));
        for (uint64_t p = off[i] + 4ull * (t[i].nmots + t[i].nmots_ombre); p < (i + 1 < n ? off[i + 1] : o); p++)
            fputc(0, f);   /* bourrage jusqu'au prochain blob aligné */
    }
    fclose(f);
    if (taille) *taille = o;
    return 0;
}

/* Remplace n octets du fichier à l'offset donné. */
static int patcher(const char *chemin, uint64_t off, const void *b, uint32_t n)
{
    FILE *f = fopen(chemin, "r+b");
    if (!f || fseek(f, (long)off, SEEK_SET) != 0 || fwrite(b, 1, n, f) != n) { if (f) fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* ---- fils de l'ouverture paresseuse (fork : pthread_once vierge dans chacun) ---------------- */

/* SC2_SPIRV_PACK explicite. */
static int fils_env_pack(const char *pack_env)
{
    setenv("SC2_SPIRV_PACK", pack_env, 1);
    unsetenv("SC2_INSTALL");
    sc2_spv_pack_entree e;
    CHECK(sc2_spv_pack_chercher(0x1111111111111111ull, 0, &e) == 1 && e.nmots == 8, "SC2_SPIRV_PACK : shader trouvé à la première recherche");
    CHECK(sc2_spv_pack_chemin() != NULL && !strcmp(sc2_spv_pack_chemin(), pack_env), "SC2_SPIRV_PACK : chemin du pack ouvert");
    CHECK(sc2_spv_pack_n() == 3, "SC2_SPIRV_PACK : 3 entrées");
    return g_fail ? 1 : 0;
}

/* Chemin par défaut : <dossier parent de SC2_INSTALL>/sc2-spirv.pak. */
static int fils_install(const char *pack_defaut, const char *dossier)
{
    char install[512];
    snprintf(install, sizeof install, "%s/install", dossier);
    unsetenv("SC2_SPIRV_PACK");
    setenv("SC2_INSTALL", install, 1);
    sc2_spv_pack_entree e;
    CHECK(sc2_spv_pack_chercher(0xDDDDDDDDDDDDDDDDull, 1, &e) == 1 && e.nmots == 4, "SC2_INSTALL : shader trouvé via le chemin par défaut");
    CHECK(sc2_spv_pack_chemin() != NULL && !strcmp(sc2_spv_pack_chemin(), pack_defaut), "SC2_INSTALL : <parent de SC2_INSTALL>/sc2-spirv.pak");
    return g_fail ? 1 : 0;
}

/* Aucun chemin : pas de pack, une seule ligne de journal. */
static int fils_sans_env(void)
{
    unsetenv("SC2_SPIRV_PACK");
    unsetenv("SC2_INSTALL");
    sc2_spv_pack_entree e;
    CHECK(sc2_spv_pack_chercher(0x1111111111111111ull, 1, &e) == 0, "sans environnement : rien trouvé");
    CHECK(sc2_spv_pack_chemin() == NULL && sc2_spv_pack_n() == 0, "sans environnement : pack absent");
    CHECK(sc2_spv_pack_raison() != NULL && strstr(sc2_spv_pack_raison(), "SC2_SPIRV_PACK") != NULL, "sans environnement : raison notée pour le bilan");
    return g_fail ? 1 : 0;
}

int main(void)
{
    char dossier[] = "/tmp/sc2_spv_pack_test.XXXXXX";
    if (!mkdtemp(dossier)) { perror("mkdtemp"); return 2; }
    char pack[512], pack_env[512], pack_defaut[512], pack_magic[512], pack_tronque[512], pack_bornes[512], pack_nconst[512], instable[512];
    snprintf(pack, sizeof pack, "%s/spv-main.pak", dossier);
    snprintf(pack_env, sizeof pack_env, "%s/spv-env.pak", dossier);
    snprintf(pack_defaut, sizeof pack_defaut, "%s/sc2-spirv.pak", dossier);   /* ce que SC2_INSTALL doit deviner */
    snprintf(pack_magic, sizeof pack_magic, "%s/spv-magic.pak", dossier);
    snprintf(pack_tronque, sizeof pack_tronque, "%s/spv-tronque.pak", dossier);
    snprintf(pack_bornes, sizeof pack_bornes, "%s/spv-bornes.pak", dossier);
    snprintf(pack_nconst, sizeof pack_nconst, "%s/spv-nconst.pak", dossier);
    snprintf(instable, sizeof instable, "%s/inexistant.pak", dossier);

    /* 3 entrées : même hash 0x1111… en VS et en PS (types différents), le PS porte
     * une variante « ombres » ; 0xDDDD… en PS seul. Tri par (hash, ps). */
    const entree_test t[3] = {
        { 0x1111111111111111ull, 0, 3, 8, 0, 94 },
        { 0x1111111111111111ull, 1, 3, 6, 5, 0 },
        { 0xDDDDDDDDDDDDDDDDull, 1, 2, 4, 0, 256 },
    };
    uint64_t taille = 0;
    CHECK(ecrire_pack(pack, t, 3, &taille) == 0, "pack de test écrit (3 entrées, variante « ombres »)");
    CHECK(ecrire_pack(pack_env, t, 3, NULL) == 0 && ecrire_pack(pack_defaut, t, 3, NULL) == 0, "copies du pack pour les scénarios d'environnement");
    CHECK(ecrire_pack(pack_tronque, t, 3, NULL) == 0 && truncate(pack_tronque, (off_t)taille - 100) == 0, "pack tronqué écrit (taille annoncée fausse)");

    /* entrée 0xDDDD… (index 2) pointée hors du fichier : 4096 de plus que la fin, alignée sur 16 */
    uint8_t hors[8]; b_u64(hors, (taille + 4096) & ~15ull);
    CHECK(ecrire_pack(pack_bornes, t, 3, NULL) == 0 && patcher(pack_bornes, 64 + 96 * 2 + 24, hors, 8) == 0, "pack à entrée hors bornes écrit");
    CHECK(ecrire_pack(pack_magic, t, 3, NULL) == 0 && patcher(pack_magic, 0, "SC2SPV0X", 8) == 0, "pack à magic faux écrit");
    /* passe 10t : nconst = 257 (> 256) sur l'entrée 0 (VS 0x1111…) */
    const uint8_t n257[2] = { 0x01, 0x01 };
    CHECK(ecrire_pack(pack_nconst, t, 3, NULL) == 0 && patcher(pack_nconst, 64 + 10, n257, 2) == 0, "pack à nconst 257 écrit");

    /* ---- ouverture paresseuse : fils forkés AVANT tout appel dans le père ------------------ */
    fflush(stdout);
    for (int fils = 0; fils < 3; fils++) {
        pid_t p = fork();
        if (p == 0) {
            int r = fils == 0 ? fils_env_pack(pack_env) : fils == 1 ? fils_install(pack_defaut, dossier) : fils_sans_env();
            printf("\nRÉSULTAT (fils %d) : %d PASS, %d FAIL\n", fils + 1, g_pass, g_fail);
            exit(r);
        }
        int st = 0;
        waitpid(p, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "ouverture paresseuse du fils sans erreur");
    }

    /* ---- pack explicite : recherche, champs, pointeurs dans le mmap ------------------------ */
    CHECK(sc2_spv_pack_ouvrir(pack) == 0, "ouverture du pack explicite");
    CHECK(sc2_spv_pack_n() == 3, "3 entrées déclarées");
    CHECK(sc2_spv_pack_chemin() != NULL && !strcmp(sc2_spv_pack_chemin(), pack), "chemin du pack ouvert");
    CHECK(sc2_spv_pack_raison() == NULL, "aucune raison quand le pack est ouvert");

    sc2_spv_pack_entree e;
    CHECK(sc2_spv_pack_chercher(0x1111111111111111ull, 0, &e) == 1, "VS 0x1111… trouvé (dichotomie)");
    CHECK(e.hash == 0x1111111111111111ull && e.ps == 0 && e.nentrees == 3, "champs de tête du VS");
    CHECK(e.entrees[0][0] == 0 && e.entrees[0][1] == 0x40 && e.entrees[2][2] == 0x82, "entrées de déclaration recopiées (reg, usage, index)");
    CHECK(e.samplers[0] == 0x00 && e.samplers[15] == 0x0F, "samplers recopiés (entrée 0)");
    CHECK(e.nmots == 8 && e.spv[0] == 0x07230203u && e.spv[7] == mot(0x1111111111111111ull, 0, 0, 7), "mots SPIR-V du VS (premier mot magique)");
    CHECK(e.nconst == 94, "nconst du VS lu à l'offset 10 (94)");
    CHECK(e.nmots_ombre == 0 && e.spv_ombre == NULL, "VS sans variante « ombres »");

    CHECK(sc2_spv_pack_chercher(0x1111111111111111ull, 1, &e) == 1, "même hash 0x1111… trouvé en PS");
    CHECK(e.ps == 1 && e.nmots == 6 && e.nmots_ombre == 5, "champs du PS (avec variante « ombres »)");
    CHECK(e.nconst == 0, "nconst 0 (bloc entier) lu");
    CHECK(e.spv_ombre != NULL && e.spv_ombre == e.spv + 6, "variante « ombres » juste après le blob normal");
    CHECK(e.spv_ombre[0] == 0x07230203u && e.spv_ombre[4] == mot(0x1111111111111111ull, 1, 1, 4), "mots de la variante « ombres »");
    CHECK(e.samplers[0] == 0x10 && e.samplers[15] == 0x1F, "samplers propres à la 2e entrée");

    CHECK(sc2_spv_pack_chercher(0xDDDDDDDDDDDDDDDDull, 1, &e) == 1 && e.nmots == 4, "PS 0xDDDD… trouvé");
    CHECK(e.nconst == 256, "nconst 256 (maximum) lu");
    CHECK(sc2_spv_pack_chercher(0xDDDDDDDDDDDDDDDDull, 0, &e) == 0, "0xDDDD… n'existe qu'en PS (VS non trouvé)");
    CHECK(sc2_spv_pack_chercher(0x9999999999999999ull, 1, &e) == 0, "hash absent non trouvé");
    CHECK(sc2_spv_pack_chercher(0x1111111111111110ull, 1, &e) == 0 && sc2_spv_pack_chercher(0x1111111111111112ull, 1, &e) == 0,
          "hash juste inférieur / supérieur non trouvés");

    /* ---- packs corrompus ------------------------------------------------------------------- */
    CHECK(sc2_spv_pack_ouvrir(pack_magic) == -1, "magic faux refusé à l'ouverture");
    CHECK(sc2_spv_pack_chemin() == NULL && sc2_spv_pack_raison() != NULL, "pack invalide : absent, raison notée");
    CHECK(sc2_spv_pack_ouvrir(pack_tronque) == -1, "taille annoncée != taille réelle (fichier tronqué) refusée");
    CHECK(sc2_spv_pack_ouvrir(instable) == -1, "fichier introuvable refusé");

    CHECK(sc2_spv_pack_ouvrir(pack_bornes) == 0, "pack à entrée hors bornes ouvrable (en-tête sain)");
    CHECK(sc2_spv_pack_chercher(0xDDDDDDDDDDDDDDDDull, 1, &e) == 0, "entrée hors bornes : non trouvée, journalisée une fois");
    CHECK(sc2_spv_pack_chercher(0x1111111111111111ull, 0, &e) == 1 && e.nmots == 8, "les autres entrées restent servies");

    CHECK(sc2_spv_pack_ouvrir(pack_nconst) == 0, "pack à nconst 257 ouvrable (en-tête sain)");
    CHECK(sc2_spv_pack_chercher(0x1111111111111111ull, 0, &e) == 0, "nconst 257 : entrée invalide, non trouvée");
    CHECK(sc2_spv_pack_chercher(0xDDDDDDDDDDDDDDDDull, 1, &e) == 1 && e.nconst == 256, "nconst 257 : les autres entrées restent servies");

    const char *fichiers[] = { pack, pack_env, pack_defaut, pack_magic, pack_tronque, pack_bornes, pack_nconst };
    for (int i = 0; i < 7; i++) unlink(fichiers[i]);
    rmdir(dossier);
    printf("\nRÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
