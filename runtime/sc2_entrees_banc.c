/* sc2_entrees_banc.c — entrées simulées du banc, horloge en SECONDES (passe 10i), pilotées par événements (V1.1, 22/10).
 *
 * Complète SC2_INPUT_SIM (sc2_input_sim.c, horloge en tours de pompe) pour les runs de régression à travers les
 * écrans et dialogues : une seule variable d'environnement, sans fichier, et des instants en secondes depuis le
 * démarrage du run (l'activation du module d'entrées, au premier import résolu, ~0,1 s après le lancement).
 *
 * FORMAT 1 (passe 10i, 20/10) — lu à l'identique (test_entrees_banc : copie de l'ancien analyseur, mêmes événements
 * et même suite d'appels sur tous les fichiers de scripts/entrees/) :
 *   SC2_BANC_ENTREES="<instant>:<action>[:<x>,<y>];…"       ou   SC2_BANC_ENTREES=@<fichier> (mêmes éléments,
 *                                                                séparés par « ; » ou par des fins de ligne, # = commentaire)
 *     <instant>  secondes (« 30 », « 12.5 ») ou tours de pompe préfixés de f (« f700 », comme SC2_INPUT_SIM)
 *     DOWN:x,y   WM_LBUTTONDOWN aux coordonnées CLIENT (x, y) — le curseur y est posé d'abord
 *     UP:x,y     WM_LBUTTONUP
 *     CLIC:x,y   DOWN puis UP au tour de pompe suivant
 *     MOVE:x,y   déplacement seul (WM_MOUSEMOVE)
 *     ESC        touche Échap (VK 0x1B) enfoncée puis relâchée au tour suivant
 *     KEY:vk     touche vk (décimal ou 0x…) enfoncée puis relâchée au tour suivant
 *     KEYDOWN:vk / KEYUP:vk   touche maintenue / relâchée (combinaisons : « 5:KEYDOWN:0x12;5.1:KEY:0x73;5.2:KEYUP:0x12 » = Alt+F4)
 *     QUIT       arrêt volontaire du run : sc2_stop(0) (garde-fou « 300:QUIT » ou fin de scénario « +120:QUIT »)
 *   ex. SC2_BANC_ENTREES="30:CLIC:640,360;f700:ESC;200:KEY:0x79"
 *   Déclencheur de VICTOIRE (20/10) : l'élément SANS instant « QUAND:VICTOIRE » coupe le scénario en deux — les éléments
 *   d'avant gardent leurs instants ABSOLUS, ceux d'après sont RELATIFS à la détection et se notent avec un « + »
 *   (« +6:CLIC:998,674 » ; sans « + » : rejetés, et un « + » avant le marqueur l'est aussi). Victoire = un fichier
 *   « …Victory.SC2Save » écrit PENDANT le run dans $SC2_ROOT/Users/sc2/Documents/StarCraft II/Saves/Unsaved/ (mtime ≥
 *   l'armement ; stat suit les liens), scruté ~1 fois par SECONDE. À la détection : les éléments absolus non encore
 *   joués sont ANNULÉS (journal + bilan), puis les relatifs partent. Un « QUAND:VICTOIRE » nu répété est sans effet.
 *
 * FORMAT 2 (V1.1) — sur-ensemble STRICT du format 1 (grammaire : inventaire v1-declencheurs §9) :
 *   en-têtes (n'importe où dans le fichier, globaux)
 *     REF:LxH            coordonnées écrites pour un client LxH, ramenées au client réel (sc2_d3d9_taille) au moment
 *                        de l'appliquer ; SANS REF: aucune transformation (pixels bruts, identité stricte du format 1)
 *     ECHELLE:<mode>     ancrage par défaut des coordonnées (défaut ui)
 *     GARDE:<s>          arrêt volontaire à t = s depuis l'activation, jamais annulé par un déclencheur
 *   marqueurs « QUAND:<déclencheur> [MAX:<s>] [SINON:SUITE|QUIT|SAUTE] [ECRITURE] » : ouvrent le segment suivant
 *     VICTOIRE           comme au format 1
 *     CARTE:<nom>        lecture MPQ de Maps\…\<nom>.SC2Map\DocumentHeader depuis l'armement (rappel de sc2_mpq.c, qui
 *                        ignore le drapeau « 1re lecture » : une 2e visite de la carte déclenche aussi) ; nom entier, sans casse
 *     FICHIER:<motif>    CreateFileW réussi dont le chemin Windows correspond au motif (glob * ?, sans casse, sans espace) ;
 *                        option ECRITURE : ouvertures en écriture seulement
 *     FINPARTIE          écran de fin de partie du moteur (Victory!/Defeat! avec Load / Restart / Score Screen) venu de
 *                        s'ouvrir (rappel de sc2_runtime.c, menu_jeu : L_00B80810(1) sur le cadre +0x3C4 ou
 *                        L_00B7DC50(1) appelé de L_00A28EA0 — le menu F10 ne montre ni l'un ni l'autre)
 *     PRET               bit Loading (0x10) du masque GlueUI vu posé puis retombé, les deux depuis l'armement
 *     CHARGEMENT         bit Loading vu baissé puis posé depuis l'armement (front montant)
 *     GLUE:<m>           (masque GlueUI & m) == m           FRAME:<n>   images présentées ≥ n
 *     STABLE:<s>         aucune lecture MPQ depuis s secondes
 *     MAX:<s>            délai depuis l'armement ; échu : SINON:SUITE (défaut) déclenche quand même, SINON:QUIT arrête
 *                        le run (code 3), SINON:SAUTE saute le segment (ses éléments ne sont jamais joués)
 *   Le segment 0 (avant le 1er QUAND) part à l'activation ; le segment k ≥ 1 est ARMÉ quand le k−1 se déclenche (ou est
 *   sauté). À son déclenchement : éléments restants du segment joué ANNULÉS, touches et boutons maintenus relâchés,
 *   puis ses « +s » (secondes) et « +f<n> » (tours de pompe) comptent depuis le déclenchement — depuis l'instant du
 *   rappel pour CARTE et FICHIER (horodatés au rappel, consommés au tour suivant). Un marqueur invalide ouvre un
 *   segment MORT (jamais déclenché) : ses éléments ne glissent pas dans le segment d'avant.
 *   actions nouvelles
 *     RCLIC:xy           bouton droit, relâché au tour suivant
 *     DRAG:x1,y1,x2,y2[,s][@mode]   bouton gauche enfoncé en (x1,y1), glissé à chaque tour jusqu'à (x2,y2) en s
 *                        secondes (défaut 0,5), relâché à l'arrivée
 *     WAIT               rien (jalon journalisé)
 *     CAPTURE:<nom>      le Present suivant écrit <SC2_FRAMES_DIR>/<nom>.ppm (sc2_banc_capture_prendre) ; nom [A-Za-z0-9._-]
 *     TEXTE:<chaîne>     [a-z0-9 ] (majuscules = minuscules) tapées en KEY, une tous les 0,3 s (tour après tour en f<n>)
 *     MOLETTE:<delta>    molette verticale au curseur (± 120 par cran)
 *   coordonnées « x,y[@mode] » ou « x%,y% » (fraction du client réel) ; modes (mesures du Fold, inventaire §6.2) :
 *     ui     boîte 4:3 en haut, centrée : hb = min(H, L·3/4), x' = L'/2 + (x − L/2)·hb'/hb, y' = y·hb'/hb (interface 2D)
 *     scene  champ horizontal constant : s = L'/L, x' = L'/2 + (x − L/2)·s, y' = H'/2 + (y − H/2)·s (scène 3D du bar)
 *     lin    étirement linéaire          brut   aucune transformation
 *   Plafonds : 1 024 événements (le 1 025e est rejeté), 64 segments ; le fichier est lu à sa taille réelle.
 *
 * Chaque événement appliqué est journalisé « [INPUT] banc t=…s tour … : … », puis les lignes [INPUT] habituelles de
 * sc2_input_sim.c suivent la livraison à la wndproc du jeu. Bilan à la sortie : « [BILAN] [INPUT] banc … » (lignes du
 * format 1 inchangées, puis une ligne par segment en format 2). Les lignes du banc ne citent jamais « .SC2Map »,
 * « → Maps\ » ni « Loading » (comptes de banc-bar.sh, phases Android, cinematique-fold.sh). Sans la variable : aucun effet.
 *
 * Inclus par sc2_input_sim.c (branchement 10i : #include + 2 appels), donc compilé partout où il l'est, APK compris ;
 * appelé sur le fil du jeu, à chaque tour de la pompe, HORS du verrou du module (sc2_input_* le prennent). Les rappels
 * sc2_banc_mpq_lu (sous le verrou de sc2_mpq.c), sc2_banc_fichier et sc2_banc_fin_partie viennent de n'importe quel
 * fil : ils ne font que déposer dans une boîte aux lettres (petit verrou propre), jamais d'entrée injectée ni de
 * code invité.
 */
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

/* V1.1 : taille du client (sc2_d3d9.c), masque des écrans GlueUI (sc2_runtime.c), images présentées (sc2_d3d9.c) —
 * références faibles : le test les bouchonne, une cible qui ne les aurait pas garde le comportement neutre. */
void     sc2_d3d9_taille(uint32_t *w, uint32_t *h) __attribute__((weak));
uint32_t sc2_glue_masque(void) __attribute__((weak));
uint32_t sc2_frames_presentees(void) __attribute__((weak));

#define BANC_MAX 1024
#define BANC_SEG_MAX 64
#define BANC_CHEMIN 4096
typedef struct { double t; uint32_t tour; int par_tour; int relatif; int action; int32_t x, y; uint32_t vk;
                 int seg;                          /* V1.1 : segment (0 = avant le 1er QUAND) ; relatif = seg > 0 */
                 int mode; double px, py;          /* ancrage des coordonnées ; x%, y% (M_POURCENT) */
                 int32_t x2, y2; double duree;     /* DRAG */
                 char nom[48];                     /* CAPTURE */
               } banc_evt;
enum { B_DOWN, B_UP, B_CLIC, B_MOVE, B_TOUCHE, B_TBAS, B_THAUT, B_QUIT,
       B_RCLIC, B_DRAG, B_WAIT, B_CAPTURE, B_MOLETTE };                       /* V1.1 : à la suite, format 1 inchangé */
enum { M_DEFAUT, M_UI, M_SCENE, M_LIN, M_BRUT, M_POURCENT };
static const char *const NOMS_MODE[] = { "", "ui", "scene", "lin", "brut", "%" };
enum { Q_DEPART, Q_VICTOIRE, Q_CARTE, Q_FICHIER, Q_FINPARTIE, Q_PRET, Q_CHARGEMENT, Q_GLUE, Q_FRAME, Q_STABLE,
       Q_INVALIDE };
static const char *const NOMS_Q[] = { "DEPART", "VICTOIRE", "CARTE", "FICHIER", "FINPARTIE", "PRET", "CHARGEMENT", "GLUE",
                                      "FRAME", "STABLE", "INVALIDE" };
enum { S_SUITE, S_QUIT, S_SAUTE };
static const char *const NOMS_SINON[] = { "SUITE", "QUIT", "SAUTE" };
enum { E_INACTIF, E_ARME, E_DECLENCHE, E_SAUTE };
#define GLUE_LOADING 0x10u                         /* écran 4 (ScreenLoading) de GlueUI, sc2_runtime.c */
typedef struct {
    int decl, sinon, ecriture;
    char arg[96];                                  /* CARTE : nom ; FICHIER : motif */
    uint32_t val;                                  /* GLUE : masque ; FRAME : n */
    double sec, max;                               /* STABLE : secondes ; MAX: (0 = sans) */
    int deb, fin;                                  /* éléments [deb, fin) */
    int etat, vu_haut, vu_bas, annules;
    double t_arme, t_decl; uint32_t tour_decl;
    char vu[160];                                  /* ce qui l'a déclenché (journal, bilan) */
} banc_seg;
static struct {
    int pret, n, i;
    banc_evt ev[BANC_MAX];
    banc_seg seg[BANC_SEG_MAX];
    int nseg, seg_joue, seg_arme, seg_plein;       /* segment dont les éléments sont joués ; segment armé (nseg : aucun) */
    struct timespec t0;
    uint32_t tour;
    int relacher_bouton; uint32_t relacher_vk;     /* CLIC : 1, RCLIC : 3 (bouton + 1) ; ESC / KEY : relâchés au tour suivant */
    int appliques, rejetes;
    int quand_victoire;                            /* un segment QUAND:VICTOIRE existe : dossier scruté, bilan du format 1 */
    char dossier_saves[BANC_CHEMIN];               /* vide : pas de scrutation (SC2_ROOT absent) */
    time_t depart_mur;                             /* mtime des victoires ≥ cet instant (armement du segment VICTOIRE) */
    double t_victoire;                             /* < 0 : pas (encore) de victoire ; sinon l'instant de la 1re détection */
    double prochaine_scrutation;
    char fichier_victoire[256];
    int annules;                                   /* éléments annulés aux déclenchements (format 1 : à la victoire) */
    int format2;                                   /* un élément du format 2 a été lu : journal des segments, bilan */
    uint32_t ref_w, ref_h; int mode_defaut; double garde;
    uint8_t touches[256]; int boutons;             /* maintenus (KEYDOWN, DOWN, DRAG) : relâchés à un déclenchement */
    struct { int actif; double t0, duree; int32_t x1, y1, x2, y2; } drag;
} B;

/* Boîte aux lettres des rappels (sc2_banc_mpq_lu, sc2_banc_fichier : n'importe quel fil, sous le verrou de sc2_mpq.c
 * pour le premier). « arme » est lu sans verrou (sortie immédiate quand aucun rappel n'est attendu) ; le reste sous mu.
 * Ordre des verrous : g_mu (sc2_mpq.c) → BX.mu seulement ; le tour du banc ne prend que BX.mu, sans rien appeler dessous. */
static struct {
    pthread_mutex_t mu;
    int arme;                                      /* atomique : CARTE, FICHIER, FINPARTIE ou STABLE attend un rappel */
    int decl, ecriture; char motif[96];
    int vu; double t_vu; char quoi[160];
    double t_derniere;                             /* STABLE : dernière lecture MPQ */
} BX = { .mu = PTHREAD_MUTEX_INITIALIZER };

static double banc_secondes(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - B.t0.tv_sec) + (double)(t.tv_nsec - B.t0.tv_nsec) / 1e9;
}
static uint32_t banc_present(void) { return sc2_frames_presentees ? sc2_frames_presentees() : 0; }

/* état neutre : segment 0 seul, rien d'armé (init et tests) */
static void banc_raz(void)
{
    memset(&B, 0, sizeof B);
    B.t_victoire = -1;
    B.nseg = 1;
    B.seg[0].decl = Q_DEPART;
    B.mode_defaut = M_UI;
    __atomic_store_n(&BX.arme, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&BX.mu);
    BX.vu = 0; BX.quoi[0] = 0; BX.motif[0] = 0;
    pthread_mutex_unlock(&BX.mu);
}

/* ordre d'application d'un segment : instants croissants (les éléments « f » gardent leur rang relatif) */
static void banc_trier(int deb, int fin)
{
    for (int a = deb + 1; a < fin; a++)
        for (int b = a; b > deb && !B.ev[b].par_tour && !B.ev[b - 1].par_tour && B.ev[b].t < B.ev[b - 1].t; b--) {
            banc_evt t = B.ev[b]; B.ev[b] = B.ev[b - 1]; B.ev[b - 1] = t;
        }
}

static int banc_mode_de(const char *s)
{
    for (int m = M_UI; m <= M_BRUT; m++) if (!strcasecmp(s, NOMS_MODE[m])) return m;
    return -1;
}

/* « x,y[@mode] » (comme le format 1 : sscanf %d,%d, la suite ignorée — sauf « @mode ») ou « x%,y% ». 1 si valide. */
static int banc_xy(const char *q, banc_evt *e)
{
    if (!q) return 0;
    if (strchr(q, '%')) {
        char *f; double px = strtod(q, &f);
        if (f == q || *f != '%' || f[1] != ',') return 0;
        const char *g = f + 2; double py = strtod(g, &f);
        if (f == g || *f != '%' || f[1] || px < 0 || px > 100 || py < 0 || py > 100) return 0;
        e->mode = M_POURCENT; e->px = px; e->py = py;
        return 1;
    }
    int n = 0;
    if (sscanf(q, "%d,%d%n", &e->x, &e->y, &n) != 2) return 0;
    const char *at = strchr(q + n, '@');
    if (at) {
        int m = banc_mode_de(at + 1);
        if (m < 0) return 0;
        e->mode = m;
    }
    return 1;
}

/* VK d'un caractère de TEXTE: (disposition US, minuscules) ; 0 : refusé */
static uint32_t banc_vk_texte(char c)
{
    if (c >= 'a' && c <= 'z') return 0x41u + (uint32_t)(c - 'a');
    if (c >= 'A' && c <= 'Z') return 0x41u + (uint32_t)(c - 'A');
    if (c >= '0' && c <= '9') return 0x30u + (uint32_t)(c - '0');
    if (c == ' ') return 0x20u;
    return 0;
}

/* nom de CAPTURE : [A-Za-z0-9._-], sans « / » ni « .. » en tête, 1 à 47 caractères */
static int banc_nom_valide(const char *s)
{
    size_t n = strlen(s);
    if (!n || n >= sizeof ((banc_evt *)0)->nom || *s == '.') return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-'))
            return 0;
    return 1;
}

/* « QUAND:<p> » : nouveau segment (format 2) ; « QUAND:VICTOIRE » nu derrière un segment VICTOIRE : sans effet (format 1). */
static void banc_marqueur(char *p)
{
    banc_seg *der = &B.seg[B.nseg - 1];
    if (!strcasecmp(p, "VICTOIRE") && B.nseg > 1 && der->decl == Q_VICTOIRE) return;
    if (B.nseg >= BANC_SEG_MAX) {
        B.rejetes++; B.seg_plein = 1;
        sc2_log("[INPUT] banc : plus de %d segments — marqueur « QUAND:%s » rejeté, éléments suivants rejetés", BANC_SEG_MAX, p);
        return;
    }
    banc_seg *s = &B.seg[B.nseg];
    memset(s, 0, sizeof *s);
    der->fin = B.n;
    s->deb = s->fin = B.n;
    char *ctx = NULL, *tok = strtok_r(p, " \t", &ctx), *arg = tok ? strchr(tok, ':') : NULL;
    if (arg) *arg++ = 0;
    int ok = tok != NULL, options = 0;
    if (!ok) ;
    else if (!strcasecmp(tok, "VICTOIRE") && !arg) s->decl = Q_VICTOIRE;
    else if (!strcasecmp(tok, "CARTE") && arg && *arg) s->decl = Q_CARTE;
    else if (!strcasecmp(tok, "FICHIER") && arg && *arg) s->decl = Q_FICHIER;
    else if (!strcasecmp(tok, "FINPARTIE") && !arg) s->decl = Q_FINPARTIE;
    else if (!strcasecmp(tok, "PRET") && !arg) s->decl = Q_PRET;
    else if (!strcasecmp(tok, "CHARGEMENT") && !arg) s->decl = Q_CHARGEMENT;
    else if (!strcasecmp(tok, "GLUE") && arg && *arg) { s->decl = Q_GLUE; s->val = (uint32_t)strtoul(arg, NULL, 0); ok = s->val != 0; }
    else if (!strcasecmp(tok, "FRAME") && arg && *arg) { s->decl = Q_FRAME; s->val = (uint32_t)strtoul(arg, NULL, 10); }
    else if (!strcasecmp(tok, "STABLE") && arg && *arg) { s->decl = Q_STABLE; s->sec = strtod(arg, NULL); ok = s->sec > 0; }
    else ok = 0;
    if (ok && (s->decl == Q_CARTE || s->decl == Q_FICHIER)) {
        if (strlen(arg) >= sizeof s->arg) ok = 0; else snprintf(s->arg, sizeof s->arg, "%s", arg);
    }
    for (char *o; ok && (o = strtok_r(NULL, " \t", &ctx)); options++) {
        char *v = strchr(o, ':'); if (v) *v++ = 0;
        if (!strcasecmp(o, "MAX") && v && *v) { s->max = strtod(v, NULL); ok = s->max > 0; }
        else if (!strcasecmp(o, "SINON") && v && !strcasecmp(v, "SUITE")) s->sinon = S_SUITE;
        else if (!strcasecmp(o, "SINON") && v && !strcasecmp(v, "QUIT")) s->sinon = S_QUIT;
        else if (!strcasecmp(o, "SINON") && v && !strcasecmp(v, "SAUTE")) s->sinon = S_SAUTE;
        else if (!strcasecmp(o, "ECRITURE") && !v && s->decl == Q_FICHIER) s->ecriture = 1;
        else ok = 0;
    }
    B.nseg++;
    if (ok && s->decl == Q_VICTOIRE) B.quand_victoire = 1;
    if (!ok || s->decl != Q_VICTOIRE || options) B.format2 = 1;
    if (!ok) {
        /* segment MORT (jamais déclenché, MAX ignoré) : ses « + » ne glissent pas dans le segment d'avant */
        s->decl = Q_INVALIDE; s->max = 0; B.rejetes++;
        sc2_log("[INPUT] banc : marqueur rejeté « QUAND:%s%s%s… » — segment %d jamais déclenché", tok ? tok : "", arg ? ":" : "",
                arg ? arg : "", B.nseg - 1);
    }
}

/* « REF: », « ECHELLE: », « GARDE: » ; 0 si l'élément n'est pas un en-tête */
static int banc_entete(const char *s, const char *p)
{
    if (!strcasecmp(s, "REF")) {
        unsigned w, h; char c;
        if (sscanf(p, "%ux%u%c", &w, &h, &c) == 2 && w >= 320 && h >= 200 && w <= 8192 && h <= 8192) { B.ref_w = w; B.ref_h = h; }
        else { B.rejetes++; sc2_log("[INPUT] banc : en-tête rejeté « REF:%s » (LxH attendu, 320×200 à 8192×8192)", p); }
    } else if (!strcasecmp(s, "ECHELLE")) {
        int m = banc_mode_de(p);
        if (m > 0) B.mode_defaut = m;
        else { B.rejetes++; sc2_log("[INPUT] banc : en-tête rejeté « ECHELLE:%s » (ui, scene, lin, brut)", p); }
    } else if (!strcasecmp(s, "GARDE")) {
        double g = strtod(p, NULL);
        if (g > 0) B.garde = g;
        else { B.rejetes++; sc2_log("[INPUT] banc : en-tête rejeté « GARDE:%s » (secondes > 0)", p); }
    } else return 0;
    B.format2 = 1;
    return 1;
}

static void banc_element(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    for (size_t n = strlen(s); n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'); ) s[--n] = 0;
    if (!*s || *s == '#') return;
    banc_evt e = { 0 };
    char *p = strchr(s, ':');
    if (!p || B.n >= BANC_MAX || B.seg_plein) { B.rejetes++; sc2_log("[INPUT] banc : élément rejeté « %s »", s); return; }
    *p++ = 0;
    if (!strcasecmp(s, "QUAND")) { banc_marqueur(p); return; }
    if (banc_entete(s, p)) return;
    int k = B.nseg - 1;                            /* segment courant */
    if (k > 0 && *s != '+') {
        B.rejetes++; sc2_log("[INPUT] banc : élément relatif sans « + » rejeté « %s:%s » (après QUAND:%s)", s, p, NOMS_Q[B.seg[k].decl]);
        return;
    }
    if (!k && *s == '+') {
        B.rejetes++; sc2_log("[INPUT] banc : « + » sans QUAND:VICTOIRE — élément rejeté « %s:%s »", s, p); return;
    }
    e.seg = k;
    if (k) {                                       /* « +6 » : secondes après le déclenchement ; « +f30 » : tours */
        e.relatif = 1;
        if (s[1] == 'f' || s[1] == 'F') { e.par_tour = 1; e.tour = (uint32_t)strtoul(s + 2, NULL, 10); B.format2 = 1; }
        else e.t = strtod(s + 1, NULL);
    }
    else if (*s == 'f' || *s == 'F') { e.par_tour = 1; e.tour = (uint32_t)strtoul(s + 1, NULL, 10); }
    else e.t = strtod(s, NULL);
    char *q = strchr(p, ':'); if (q) *q++ = 0;
    int xy = banc_xy(q, &e);
    if (!strcasecmp(p, "DOWN") && xy) e.action = B_DOWN;
    else if (!strcasecmp(p, "UP") && xy) e.action = B_UP;
    else if (!strcasecmp(p, "CLIC") && xy) e.action = B_CLIC;
    else if (!strcasecmp(p, "MOVE") && xy) e.action = B_MOVE;
    else if (!strcasecmp(p, "ESC")) { e.action = B_TOUCHE; e.vk = 0x1B; }
    else if (!strcasecmp(p, "KEY") && q) { e.action = B_TOUCHE; e.vk = (uint32_t)strtoul(q, NULL, 0) & 0xFF; }
    else if (!strcasecmp(p, "KEYDOWN") && q) { e.action = B_TBAS; e.vk = (uint32_t)strtoul(q, NULL, 0) & 0xFF; }
    else if (!strcasecmp(p, "KEYUP") && q) { e.action = B_THAUT; e.vk = (uint32_t)strtoul(q, NULL, 0) & 0xFF; }
    else if (!strcasecmp(p, "QUIT")) e.action = B_QUIT;
    /* V1.1 (format 2) */
    else if (!strcasecmp(p, "RCLIC") && xy) e.action = B_RCLIC;
    else if (!strcasecmp(p, "WAIT") && !q) e.action = B_WAIT;
    else if (!strcasecmp(p, "MOLETTE") && q && *q) { e.action = B_MOLETTE; e.mode = M_DEFAUT; e.x = (int32_t)strtol(q, NULL, 10); }
    else if (!strcasecmp(p, "CAPTURE") && q && banc_nom_valide(q)) { e.action = B_CAPTURE; snprintf(e.nom, sizeof e.nom, "%s", q); }
    else if (!strcasecmp(p, "DRAG") && q) {
        int n = 0; double d = 0.5; char *r;
        e.mode = M_DEFAUT;
        if (sscanf(q, "%d,%d,%d,%d%n", &e.x, &e.y, &e.x2, &e.y2, &n) != 4) goto rejet;
        r = q + n;
        if (*r == ',') { char *f; d = strtod(r + 1, &f); if (f == r + 1 || d < 0) goto rejet; r = f; }
        if (*r == '@') { int m = banc_mode_de(r + 1); if (m < 0) goto rejet; e.mode = m; }
        else if (*r) goto rejet;
        e.action = B_DRAG; e.duree = d;
    }
    else if (!strcasecmp(p, "TEXTE") && q && *q) {
        int nc = (int)strlen(q);
        for (const char *c = q; *c; c++) if (!banc_vk_texte(*c)) goto rejet;
        if (B.n + nc > BANC_MAX) {
            B.rejetes++; sc2_log("[INPUT] banc : TEXTE de %d caractère(s) au-delà du plafond de %d événements — rejeté", nc, BANC_MAX);
            return;
        }
        e.mode = M_DEFAUT;
        for (int c = 0; c < nc; c++) {             /* une KEY par caractère ; espacées de 0,3 s (en f<n> : tour après tour) */
            banc_evt kc = e;
            kc.action = B_TOUCHE; kc.vk = banc_vk_texte(q[c]);
            if (!kc.par_tour) kc.t = e.t + 0.3 * c;
            B.ev[B.n++] = kc;
        }
        B.format2 = 1;
        return;
    }
    else goto rejet;
    if (e.action > B_QUIT || e.mode != M_DEFAUT) B.format2 = 1;
    B.ev[B.n++] = e;
    return;
rejet:
    B.rejetes++; sc2_log("[INPUT] banc : action rejetée « %s%s%s »", p, q ? ":" : "", q ? q : "");
}

/* fin de l'analyse : bornes et tri de chaque segment, rien d'armé */
static void banc_finir(void)
{
    B.seg[B.nseg - 1].fin = B.n;
    for (int k = 0; k < B.nseg; k++) banc_trier(B.seg[k].deb, B.seg[k].fin);
    B.seg_arme = B.nseg;
}

/* Découpe du texte (« ; » ou fin de ligne ; « # » en tête : commentaire, plus loin : fin de l'élément), analyse, tri. */
static void banc_charger_texte(char *buf)
{
    for (char *s = buf, *fin; s && *s; s = fin) {
        fin = s + strcspn(s, ";\n");
        if (*fin) *fin++ = 0; else fin = NULL;
        char *c = strchr(s, '#'); if (c && c != s) *c = 0;      /* commentaire en fin d'élément */
        banc_element(s);
    }
    banc_finir();
}

/* Victoire dans <dossier> : un fichier « …Victory.SC2Save » dont le mtime est postérieur ou égal à l'armement du
 * segment (celles des runs précédents ne comptent pas ; stat suit les liens). Rend 1 si trouvée, nom gardé. */
static const char BANC_SUFFIXE[] = "Victory.SC2Save";
static int banc_scruter_dossier(const char *dossier)
{
    DIR *d = opendir(dossier);
    if (!d) return 0;                                    /* absent ou pas encore créé : rien à voir */
    struct dirent *f;
    int vu = 0;
    while (!vu && (f = readdir(d))) {
        size_t n = strlen(f->d_name), ns = sizeof BANC_SUFFIXE - 1;
        if (n < ns || strcmp(f->d_name + n - ns, BANC_SUFFIXE)) continue;
        char chemin[BANC_CHEMIN];
        if (snprintf(chemin, sizeof chemin, "%s/%s", dossier, f->d_name) >= (int)sizeof chemin) continue;
        struct stat st;
        if (!stat(chemin, &st) && st.st_mtime >= B.depart_mur) {
            snprintf(B.fichier_victoire, sizeof B.fichier_victoire, "%s", f->d_name);
            vu = 1;
        }
    }
    closedir(d);
    return vu;
}

/* ---- rappels (n'importe quel fil) ------------------------------------------------------------------------------ */
static char banc_min(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int banc_glob(const char *m, const char *s)       /* * et ?, sans casse */
{
    const char *etoile = NULL, *reprise = NULL;
    while (*s) {
        if (*m == '*') { etoile = m++; reprise = s; }
        else if (*m && (*m == '?' || banc_min(*m) == banc_min(*s))) { m++; s++; }
        else if (etoile) { m = etoile + 1; s = ++reprise; }
        else return 0;
    }
    while (*m == '*') m++;
    return !*m;
}
/* « Maps\…\<nom>.SC2Map\DocumentHeader » dont <nom> vaut carte (sans casse, en entier) ; *ici, *lg : le nom lu */
static int banc_carte_lue(const char *nom, const char *carte, const char **ici, size_t *lg)
{
    static const char EXT[] = ".SC2Map\\", DOC[] = "DocumentHeader";
    if (strncasecmp(nom, "Maps\\", 5)) return 0;
    for (const char *p = nom + 5; *p; p++) {
        if (strncasecmp(p, EXT, sizeof EXT - 1)) continue;
        if (strcasecmp(p + sizeof EXT - 1, DOC)) return 0;
        const char *d = p;
        while (d > nom && d[-1] != '\\') d--;
        *ici = d; *lg = (size_t)(p - d);
        return *lg == strlen(carte) && !strncasecmp(d, carte, *lg);
    }
    return 0;
}
/* sc2_mpq.c (sc2_mpq_lu, sous son verrou) : fichier interne touché par une lecture du moteur — à CHAQUE lecture,
 * indépendamment du drapeau « 1re lecture » du journal [MPQ]. */
void sc2_banc_mpq_lu(const char *nom)
{
    if (!__atomic_load_n(&BX.arme, __ATOMIC_ACQUIRE) || !nom) return;
    pthread_mutex_lock(&BX.mu);
    if (BX.arme && !BX.vu) {
        const char *d = NULL; size_t n = 0;
        if (BX.decl == Q_STABLE) BX.t_derniere = banc_secondes();
        else if (BX.decl == Q_CARTE && banc_carte_lue(nom, BX.motif, &d, &n)) {
            BX.vu = 1; BX.t_vu = banc_secondes();
            snprintf(BX.quoi, sizeof BX.quoi, "DocumentHeader de %.*s lu", (int)n, d);
        }
    }
    pthread_mutex_unlock(&BX.mu);
}
/* sc2_iat.c (h_CreateFileW) : ouverture réussie, chemin Windows, en écriture ou non */
void sc2_banc_fichier(const char *win, int ecriture)
{
    if (!__atomic_load_n(&BX.arme, __ATOMIC_ACQUIRE) || !win) return;
    pthread_mutex_lock(&BX.mu);
    if (BX.arme && !BX.vu && BX.decl == Q_FICHIER && (!BX.ecriture || ecriture) && banc_glob(BX.motif, win)) {
        BX.vu = 1; BX.t_vu = banc_secondes();
        snprintf(BX.quoi, sizeof BX.quoi, "CreateFileW en %s de « %s »", ecriture ? "écriture" : "lecture", win);
    }
    pthread_mutex_unlock(&BX.mu);
}
/* sc2_runtime.c (menu_jeu, fil du jeu) : l'écran de fin de partie du moteur s'ouvre (Victory!/Defeat!), quoi le dit */
void sc2_banc_fin_partie(const char *quoi)
{
    if (!__atomic_load_n(&BX.arme, __ATOMIC_ACQUIRE) || !quoi) return;
    pthread_mutex_lock(&BX.mu);
    if (BX.arme && !BX.vu && BX.decl == Q_FINPARTIE) {
        BX.vu = 1; BX.t_vu = banc_secondes();
        snprintf(BX.quoi, sizeof BX.quoi, "fin de partie : %s", quoi);
    }
    pthread_mutex_unlock(&BX.mu);
}

/* ---- CAPTURE:<nom> : demande posée par le tour, prise UNE fois par present_vk (sc2_d3d9.c) au Present suivant ---- */
static char g_capture_noms[2][48];
static int g_capture_idx;
static const char *g_capture_demande;
static void banc_capture_demander(const char *nom)
{
    g_capture_idx ^= 1;                                  /* double tampon : le nom rendu reste valide jusqu'à 2 demandes */
    snprintf(g_capture_noms[g_capture_idx], sizeof g_capture_noms[0], "%s", nom);
    __atomic_store_n(&g_capture_demande, g_capture_noms[g_capture_idx], __ATOMIC_RELEASE);
}
/* sc2_d3d9.h : nom de la capture demandée, rendu UNE fois puis oublié ; NULL sinon (appelé à chaque Present) */
const char *sc2_banc_capture_prendre(void)
{
    if (!__atomic_load_n(&g_capture_demande, __ATOMIC_RELAXED)) return NULL;
    return __atomic_exchange_n(&g_capture_demande, (const char *)NULL, __ATOMIC_ACQ_REL);
}

/* ---- échelle (REF:) --------------------------------------------------------------------------------------------- */
static int32_t banc_arrondi(double v) { return (int32_t)(v >= 0 ? v + 0.5 : v - 0.5); }
static void banc_taille(uint32_t *w, uint32_t *h)
{
    *w = *h = 0;
    if (sc2_d3d9_taille) sc2_d3d9_taille(w, h);
    if (!*w || !*h) { *w = 1280; *h = 720; }             /* défaut du runtime (SC2_ECRAN absent) */
}
/* (x, y) écrit pour REF (ou x%, y% du client) → pixels du client réel, selon le mode de l'élément ou ECHELLE: */
static void banc_vers_client(const banc_evt *e, int32_t x, int32_t y, int32_t *ox, int32_t *oy)
{
    uint32_t w, h;
    if (e->mode == M_POURCENT) { banc_taille(&w, &h); *ox = banc_arrondi(e->px * w / 100.0); *oy = banc_arrondi(e->py * h / 100.0); return; }
    int mode = e->mode ? e->mode : B.mode_defaut;
    if (!B.ref_w || mode == M_BRUT) { *ox = x; *oy = y; return; }
    banc_taille(&w, &h);
    double L = B.ref_w, H = B.ref_h, l = w, hh = h, X, Y;
    if (mode == M_LIN) { X = x * l / L; Y = y * hh / H; }
    else if (mode == M_SCENE) { double s = l / L; X = l / 2 + (x - L / 2) * s; Y = hh / 2 + (y - H / 2) * s; }
    else {                                                /* M_UI : boîte 4:3 en haut, centrée */
        double hb = H < L * 0.75 ? H : L * 0.75, hb2 = hh < l * 0.75 ? hh : l * 0.75, s = hb2 / hb;
        X = l / 2 + (x - L / 2) * s; Y = y * s;
    }
    *ox = banc_arrondi(X); *oy = banc_arrondi(Y);
}
static void banc_suffixe_ref(const banc_evt *e, char *b, size_t n)
{
    b[0] = 0;
    if (e->mode == M_POURCENT) snprintf(b, n, " ← (%.4g%%, %.4g%%)", e->px, e->py);
    else if (B.ref_w) snprintf(b, n, " ← (%d, %d)@%s", e->x, e->y, NOMS_MODE[e->mode ? e->mode : B.mode_defaut]);
}

/* ---- segments ---------------------------------------------------------------------------------------------------- */
static void banc_decrire(const banc_seg *s, char *b, size_t n)
{
    switch (s->decl) {
    case Q_CARTE: case Q_FICHIER: snprintf(b, n, "QUAND:%s:%s%s", NOMS_Q[s->decl], s->arg, s->ecriture ? " ECRITURE" : ""); break;
    case Q_GLUE: snprintf(b, n, "QUAND:GLUE:0x%X", s->val); break;
    case Q_FRAME: snprintf(b, n, "QUAND:FRAME:%u", s->val); break;
    case Q_STABLE: snprintf(b, n, "QUAND:STABLE:%g", s->sec); break;
    default: snprintf(b, n, "QUAND:%s", NOMS_Q[s->decl]); break;
    }
}

/* touches et boutons maintenus (KEYDOWN, DOWN, DRAG en cours) relâchés : un segment repart d'un clavier au repos */
static void banc_relacher_tout(double t)
{
    for (int vk = 0; vk < 256; vk++)
        if (B.touches[vk]) {
            B.touches[vk] = 0; sc2_input_touche((uint32_t)vk, 0);
            sc2_log("[INPUT] banc t=%.2fs tour %u : VK 0x%02X maintenue, relâchée au déclenchement", t, B.tour, vk);
        }
    for (int b = 0; b < 3; b++)
        if (B.boutons & (1 << b)) {
            sc2_input_bouton(b, 0);
            sc2_log("[INPUT] banc t=%.2fs tour %u : bouton %d maintenu, relâché au déclenchement", t, B.tour, b);
        }
    B.boutons = 0; B.drag.actif = 0;
}

static void banc_armer(int k, double t)
{
    B.seg_arme = k;
    if (k >= B.nseg) return;
    banc_seg *s = &B.seg[k];
    s->etat = E_ARME; s->t_arme = t; s->vu_haut = s->vu_bas = 0;
    pthread_mutex_lock(&BX.mu);
    BX.decl = s->decl; BX.ecriture = s->ecriture; BX.vu = 0; BX.quoi[0] = 0; BX.t_derniere = t;
    snprintf(BX.motif, sizeof BX.motif, "%s", s->arg);
    __atomic_store_n(&BX.arme, s->decl == Q_CARTE || s->decl == Q_FICHIER || s->decl == Q_FINPARTIE
                               || s->decl == Q_STABLE, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&BX.mu);
    if (s->decl == Q_VICTOIRE && t > 0) {                 /* VICTOIRE armée en cours de run : seules les victoires d'après */
        B.depart_mur = time(NULL);
        B.prochaine_scrutation = t + 1.0;
    }
    if (B.format2) {
        char d[160]; banc_decrire(s, d, sizeof d);
        if (s->max > 0) sc2_log("[INPUT] banc t=%.2fs tour %u : segment %d armé (%s MAX:%g SINON:%s)", t, B.tour, k, d, s->max, NOMS_SINON[s->sinon]);
        else sc2_log("[INPUT] banc t=%.2fs tour %u : segment %d armé (%s)", t, B.tour, k, d);
    }
}

/* Le segment k se déclenche (td : instant de l'événement, t : du tour) : restes annulés, relâchements, ses relatifs. */
static void banc_declencher(int k, double td, double t, const char *quoi, int au_max)
{
    banc_seg *s = &B.seg[k], *j = &B.seg[B.seg_joue];
    int annul = B.i < j->fin ? j->fin - B.i : 0;
    __atomic_store_n(&BX.arme, 0, __ATOMIC_RELEASE);
    B.annules += annul; s->annules = annul;
    B.i = s->deb; B.seg_joue = k;
    s->etat = E_DECLENCHE; s->t_decl = td; s->tour_decl = B.tour;
    snprintf(s->vu, sizeof s->vu, "%s", quoi);
    if (s->decl == Q_VICTOIRE && !au_max) {               /* lignes du format 1 (campagne-rapport.py) */
        if (B.t_victoire < 0) B.t_victoire = td;
        sc2_log("[INPUT] banc t=%.2fs tour %u : VICTOIRE détectée (%s)", t, B.tour, B.fichier_victoire);
        sc2_log("[INPUT] banc t=%.2fs : %d élément(s) absolu(s) restant(s) annulé(s) — la suite est relative à la victoire",
                t, annul);
    } else {
        char d[160]; banc_decrire(s, d, sizeof d);
        sc2_log("[INPUT] banc t=%.2fs tour %u : %s déclenché%s (%s) à t=%.2fs — %d élément(s) restant(s) annulé(s), Present n°%u",
                t, B.tour, d, au_max ? " au délai MAX" : "", quoi, td, annul, banc_present());
    }
    banc_relacher_tout(t);
    banc_armer(k + 1, t);
}

/* Le segment armé : son déclencheur, puis son délai MAX. Rend 1 si le run s'arrête (SINON:QUIT). */
static int banc_surveiller(double t)
{
    if (B.seg_arme >= B.nseg) return 0;
    int k = B.seg_arme;
    banc_seg *s = &B.seg[k];
    double td = -1; char quoi[160] = "";
    switch (s->decl) {
    case Q_VICTOIRE:
        if (B.dossier_saves[0] && t >= B.prochaine_scrutation) {
            B.prochaine_scrutation = t + 1.0;
            if (banc_scruter_dossier(B.dossier_saves)) { td = t; snprintf(quoi, sizeof quoi, "%s", B.fichier_victoire); }
        }
        break;
    case Q_CARTE: case Q_FICHIER: case Q_FINPARTIE:
        pthread_mutex_lock(&BX.mu);
        if (BX.vu) { td = BX.t_vu; snprintf(quoi, sizeof quoi, "%s", BX.quoi); }
        pthread_mutex_unlock(&BX.mu);
        break;
    case Q_STABLE:
        pthread_mutex_lock(&BX.mu);
        if (t - BX.t_derniere >= s->sec) { td = t; snprintf(quoi, sizeof quoi, "aucune lecture MPQ depuis t=%.2fs", BX.t_derniere); }
        pthread_mutex_unlock(&BX.mu);
        break;
    case Q_PRET: case Q_CHARGEMENT: case Q_GLUE: {
        uint32_t m = sc2_glue_masque ? sc2_glue_masque() : 0;
        snprintf(quoi, sizeof quoi, "masque GlueUI 0x%02X", m);
        if (s->decl == Q_GLUE) { if ((m & s->val) == s->val) td = t; }
        else if (m & GLUE_LOADING) { if (s->decl == Q_CHARGEMENT && s->vu_bas) td = t; s->vu_haut = 1; }
        else { if (s->decl == Q_PRET && s->vu_haut) td = t; s->vu_bas = 1; }
        break; }
    case Q_FRAME: {
        uint32_t n = banc_present();
        if (n >= s->val) { td = t; snprintf(quoi, sizeof quoi, "%u image(s) présentée(s)", n); }
        break; }
    default: break;                                       /* INVALIDE : jamais */
    }
    if (td >= 0) { banc_declencher(k, td, t, quoi, 0); return 0; }
    if (s->max <= 0 || t - s->t_arme < s->max) return 0;
    char d[160]; banc_decrire(s, d, sizeof d);
    if (s->sinon == S_QUIT) {
        sc2_log("[INPUT] banc t=%.2fs tour %u : %s non déclenché en MAX:%gs — SINON:QUIT", t, B.tour, d, s->max);
        sc2_stop(3, "SC2_BANC_ENTREES : %s non déclenché en MAX:%gs (SINON:QUIT) à t=%.1fs", d, s->max, t);
        return 1;                                         /* sc2_stop ne rend pas la main (bouchon du test excepté) */
    }
    if (s->sinon == S_SAUTE) {
        s->etat = E_SAUTE;
        sc2_log("[INPUT] banc t=%.2fs tour %u : %s non déclenché en MAX:%gs — segment %d sauté (%d élément(s))", t, B.tour, d,
                s->max, k, s->fin - s->deb);
        banc_armer(k + 1, t);
        return 0;
    }
    snprintf(quoi, sizeof quoi, "MAX:%gs échu, SINON:SUITE", s->max);
    banc_declencher(k, t, t, quoi, 1);
    return 0;
}

static void banc_bilan(void)
{
    printf("[BILAN] [INPUT] banc (SC2_BANC_ENTREES) : %d/%d événement(s) appliqué(s), %d rejeté(s), %u tours de pompe, %.1f s\n",
           B.appliques, B.n, B.rejetes, B.tour, banc_secondes());
    if (B.quand_victoire) {
        char v[320];
        if (B.t_victoire >= 0) snprintf(v, sizeof v, "t=%.2fs (%s)", B.t_victoire, B.fichier_victoire);
        else snprintf(v, sizeof v, "pas de victoire");
        printf("[BILAN] [INPUT] banc victoire (QUAND:VICTOIRE) : %s ; %d élément(s) absolu(s) annulé(s)\n", v, B.annules);
    }
    if (!B.format2) return;
    for (int k = 1; k < B.nseg; k++) {
        banc_seg *s = &B.seg[k]; char d[160], e[240];
        banc_decrire(s, d, sizeof d);
        if (s->etat == E_DECLENCHE)
            snprintf(e, sizeof e, "déclenché à t=%.2fs, tour %u (%s), %d annulé(s)", s->t_decl, s->tour_decl, s->vu, s->annules);
        else snprintf(e, sizeof e, "%s", s->etat == E_SAUTE ? "sauté au délai MAX" : s->etat == E_ARME ? "armé, jamais déclenché" : "jamais armé");
        printf("[BILAN] [INPUT] banc segment %d %s : %s ; %d élément(s)\n", k, d, e, s->fin - s->deb);
    }
}

/* SC2_BANC_ENTREES : le texte, ou @fichier lu EN ENTIER (V1.1 : plus de plafond silencieux de 16 Kio). Jamais NULL. */
static char *banc_lire(const char *v)
{
    if (*v != '@') { char *c = strdup(v); return c ? c : (char *)""; }
    size_t n = 0, cap = 16384;
    char *buf = malloc(cap);
    if (!buf) return (char *)"";
    FILE *f = fopen(v + 1, "r");
    if (f) {
        for (size_t r; (r = fread(buf + n, 1, cap - 1 - n, f)) > 0; ) {
            n += r;
            if (n + 1 == cap) { char *b2 = realloc(buf, cap * 2); if (!b2) break; buf = b2; cap *= 2; }
        }
        fclose(f);
    } else sc2_log("[INPUT] banc : SC2_BANC_ENTREES=%s illisible", v);
    buf[n] = 0;
    return buf;
}

/* Au premier import résolu (sc2_input_find) : lecture de la variable, activation du module d'entrées. */
static void sc2_banc_entrees_init(void)
{
    const char *v = getenv("SC2_BANC_ENTREES");
    if (!v || !*v || B.pret) return;
    banc_raz();
    B.pret = 1;
    clock_gettime(CLOCK_MONOTONIC, &B.t0);
    B.depart_mur = time(NULL);                           /* heure murale : comparée aux mtime des sauvegardes */
    B.prochaine_scrutation = 1.0;                        /* 1re scrutation à t ≈ 1 s, puis une par seconde */
    banc_charger_texte(banc_lire(v));                    /* texte gardé : une lecture par processus */
    if (B.quand_victoire) {
        const char *racine = getenv("SC2_ROOT");
        if (!racine || !*racine || snprintf(B.dossier_saves, sizeof B.dossier_saves,
                "%s/Users/sc2/Documents/StarCraft II/Saves/Unsaved", racine) >= (int)sizeof B.dossier_saves) {
            B.dossier_saves[0] = 0;
            sc2_log("[INPUT] banc : SC2_ROOT absent — victoire non scrutée, les éléments relatifs ne partiront jamais");
        } else
            sc2_log("[INPUT] banc : victoire scrutée dans %s (…Victory.SC2Save écrit pendant le run, ~1 fois/s)",
                    B.dossier_saves);
    }
    sc2_input_activer();
    atexit(banc_bilan);
    sc2_log("[INPUT] banc : SC2_BANC_ENTREES = %d événement(s), %d rejeté(s) ; horloge = secondes depuis l'activation (f<n> : tours de pompe)",
            B.n, B.rejetes);
    /* V1.1 : une ligne qui dit le format (un vieux binaire lit un fichier du format 2 en rejetant ses éléments nouveaux) */
    int cartes = 0, ancres = 0;
    for (int k = 1; k < B.nseg; k++) cartes += B.seg[k].decl == Q_CARTE;
    for (int i = 0; i < B.n; i++) ancres += B.ev[i].mode >= M_UI && B.ev[i].mode <= M_LIN;
    const char *mt = getenv("SC2_MPQ_TRACE");
    char ref[96], garde[48] = "";
    if (B.ref_w) snprintf(ref, sizeof ref, "REF %ux%u (ancrage par défaut %s)", B.ref_w, B.ref_h, NOMS_MODE[B.mode_defaut]);
    else snprintf(ref, sizeof ref, "sans REF (pixels bruts)");
    if (B.garde > 0) snprintf(garde, sizeof garde, ", GARDE à t=%gs", B.garde);
    sc2_log("[INPUT] banc : analyseur format 2 — scénario au format %d, %d segment(s) après le départ, %s%s%s%s",
            B.format2 ? 2 : 1, B.nseg - 1, ref, garde,
            cartes && mt && !strcmp(mt, "0") ? " ; ATTENTION : SC2_MPQ_TRACE=0 (aucun index MPQ), QUAND:CARTE ne partira jamais" : "",
            ancres && !B.ref_w ? " ; ATTENTION : « @mode » sans REF: (coordonnées brutes)" : "");
    banc_armer(1, 0.0);                                   /* le segment 0 part à l'activation : le 1 est armé */
}

/* DRAG en cours : curseur interpolé à chaque tour, bouton relâché à l'arrivée */
static void banc_drag_tour(double t)
{
    double f = B.drag.duree > 0 ? (t - B.drag.t0) / B.drag.duree : 1.0;
    if (f > 1) f = 1;
    int32_t x = banc_arrondi(B.drag.x1 + (B.drag.x2 - B.drag.x1) * f), y = banc_arrondi(B.drag.y1 + (B.drag.y2 - B.drag.y1) * f);
    sc2_input_souris(x, y);
    if (f < 1) return;
    sc2_input_bouton(0, 0); B.boutons &= ~1; B.drag.actif = 0;
    sc2_log("[INPUT] banc t=%.2fs tour %u : DRAG arrivé en (%d, %d), bouton gauche relâché", t, B.tour, x, y);
}

/* À chaque tour de la pompe du jeu, hors verrou : relâchements en attente, glissé en cours, garde, déclencheur du
 * segment armé (scrutation ~1/s pour VICTOIRE), puis événements échus du segment joué. */
static void sc2_banc_entrees_tour(void)
{
    if (!B.pret) return;
    B.tour++;
    double t = banc_secondes();
    if (B.relacher_bouton == 1) { sc2_input_bouton(0, 0); B.relacher_bouton = 0; sc2_log("[INPUT] banc t=%.2fs tour %u : bouton gauche relâché (fin de CLIC)", t, B.tour); }
    else if (B.relacher_bouton) { sc2_input_bouton(B.relacher_bouton - 1, 0); B.relacher_bouton = 0; sc2_log("[INPUT] banc t=%.2fs tour %u : bouton droit relâché (fin de RCLIC)", t, B.tour); }
    if (B.relacher_vk) { sc2_input_touche(B.relacher_vk, 0); sc2_log("[INPUT] banc t=%.2fs tour %u : VK 0x%02X relâchée", t, B.tour, B.relacher_vk); B.relacher_vk = 0; }
    if (B.drag.actif) banc_drag_tour(t);
    if (B.garde > 0 && t >= B.garde) {
        sc2_log("[INPUT] banc t=%.2fs tour %u : GARDE:%g atteinte — arrêt du run", t, B.tour, B.garde);
        sc2_stop(0, "SC2_BANC_ENTREES : GARDE:%g atteinte à t=%.1fs (segment armé : %d)", B.garde, t, B.seg_arme);
        return;                                        /* sc2_stop ne rend pas la main (bouchon du test excepté) */
    }
    if (banc_surveiller(t)) return;
    while (B.i < B.n) {
        banc_evt *e = &B.ev[B.i];
        banc_seg *s = &B.seg[B.seg_joue];
        if (B.i >= s->fin) break;                         /* fin du segment joué : on attend le déclencheur suivant */
        if (e->seg) { if (e->par_tour ? B.tour - s->tour_decl < e->tour : t - s->t_decl < e->t) break; }
        else if (e->par_tour ? B.tour < e->tour : t < e->t) break;
        B.i++; B.appliques++;
        int32_t x, y; char ref[96];
        switch (e->action) {
        case B_DOWN: case B_UP: case B_CLIC: case B_MOVE: case B_RCLIC:
            banc_vers_client(e, e->x, e->y, &x, &y);
            sc2_input_souris(x, y);
            if (e->action == B_RCLIC) { sc2_input_bouton(2, 1); B.relacher_bouton = 3; }
            else if (e->action != B_MOVE) sc2_input_bouton(0, e->action != B_UP);
            if (e->action == B_CLIC) B.relacher_bouton = 1;
            if (e->action == B_DOWN) B.boutons |= 1; else if (e->action == B_UP) B.boutons &= ~1;
            banc_suffixe_ref(e, ref, sizeof ref);
            sc2_log("[INPUT] banc t=%.2fs tour %u : %s (%d, %d)%s", t, B.tour,
                    e->action == B_DOWN ? "DOWN" : e->action == B_UP ? "UP" : e->action == B_CLIC ? "CLIC" : e->action == B_RCLIC ? "RCLIC" : "MOVE",
                    x, y, ref);
            break;
        case B_TOUCHE:
            sc2_input_touche(e->vk, 1); B.relacher_vk = e->vk;
            sc2_log("[INPUT] banc t=%.2fs tour %u : touche VK 0x%02X enfoncée", t, B.tour, e->vk);
            return;                                    /* une touche à la fois : relâchée au tour suivant */
        case B_TBAS: case B_THAUT:
            sc2_input_touche(e->vk, e->action == B_TBAS);
            B.touches[e->vk & 0xFF] = e->action == B_TBAS;
            sc2_log("[INPUT] banc t=%.2fs tour %u : touche VK 0x%02X %s", t, B.tour, e->vk, e->action == B_TBAS ? "maintenue" : "relâchée");
            return;                                    /* un changement d'état de touche par tour */
        case B_QUIT:
            sc2_log("[INPUT] banc t=%.2fs tour %u : QUIT — arrêt volontaire du run", t, B.tour);
            sc2_stop(0, "SC2_BANC_ENTREES : QUIT à t=%.1fs", t);
            return;                                    /* sc2_stop ne rend pas la main (bouchon du test excepté) */
        case B_DRAG: {
            int32_t x2, y2;
            banc_vers_client(e, e->x, e->y, &x, &y);
            banc_vers_client(e, e->x2, e->y2, &x2, &y2);
            sc2_input_souris(x, y); sc2_input_bouton(0, 1); B.boutons |= 1;
            B.drag.actif = 1; B.drag.t0 = t; B.drag.duree = e->duree;
            B.drag.x1 = x; B.drag.y1 = y; B.drag.x2 = x2; B.drag.y2 = y2;
            sc2_log("[INPUT] banc t=%.2fs tour %u : DRAG (%d, %d) vers (%d, %d) en %.2f s", t, B.tour, x, y, x2, y2, e->duree);
            return; }                                  /* l'enfoncement est livré avant le premier déplacement */
        case B_WAIT:
            sc2_log("[INPUT] banc t=%.2fs tour %u : WAIT (jalon), Present n°%u", t, B.tour, banc_present());
            break;
        case B_CAPTURE:
            banc_capture_demander(e->nom);
            sc2_log("[INPUT] banc t=%.2fs tour %u : CAPTURE %s demandée (Present suivant le n°%u)", t, B.tour, e->nom, banc_present());
            return;                                    /* une capture par tour (un Present par tour) */
        case B_MOLETTE:
            sc2_input_molette(e->x, 0);
            sc2_log("[INPUT] banc t=%.2fs tour %u : MOLETTE %+d", t, B.tour, e->x);
            break;
        }
        if (B.relacher_bouton) return;                 /* le relâchement du CLIC passe avant l'événement suivant */
    }
}
