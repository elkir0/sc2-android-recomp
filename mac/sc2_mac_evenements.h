/* sc2_mac_evenements.h — rejeu SC2_MAC_EVENEMENTS de l'app Mac (V3, lot M4, 22/10) : le CŒUR en C pur.
 *
 * Ce module analyse le scénario, l'ordonnance sur l'horloge de l'hôte et le compteur de Present (sc2a_presents), et
 * convertit les coordonnées en points de vue. Il n'appelle ni AppKit, ni le runtime, ni getenv : la synthèse des NSEvent
 * et leur routage (voie directe ou file d'AppKit), la lecture des variables et le journal sont dans
 * sc2_mac_evenements_ns.m (M7). Une primitive = UN NSEvent ou UNE action AppKit. Testé sur l'hôte :
 * mac/tests/test_mac_evenements.c (tests-hote/v3/evenements.cmake). Rien ici ne tient d'état global : tout est dans
 * sc2en_script et sc2en_rejeu, à n'utiliser que depuis un fil à la fois (le fil principal de l'hôte).
 *
 * ---- SPÉCIFICATION -------------------------------------------------------------------------------------------------
 * « @chemin » : le fichier (UTF-8, 1 Mio au plus, BOM et CR de fin de ligne tolérés). Sinon : le texte lui-même, dont
 * les éléments sont séparés par « ; » ou des fins de ligne (hors guillemets). « @p » suivi d'un blanc ou d'un « + »
 * n'est jamais un nom de fichier : « @p 300 QUITTER » est un scénario d'une ligne.
 *
 * ---- LEXIQUE ---------------------------------------------------------------------------------------------------------
 * Une commande par ligne ; mots séparés par des blancs ; « # » en début de mot (hors guillemets) : commentaire jusqu'à la
 * fin de la ligne ; lignes vides ignorées ; mots-clés sans casse. Entiers décimaux ou 0x… ; réels « 12.5 » (point
 * décimal, indépendant de la locale). Chaînes entre guillemets droits, échappements \" \\ \n \r \t \e (Échap)
 * \uXXXX \UXXXXXXXX (UTF-8 en sortie, 255 octets au plus). Les erreurs sont nommées « source:ligne:colonne: message »
 * (colonne en caractères, 1 = le premier ; pour un texte, la « ligne » est le n° d'élément) ; une erreur, et rien n'est
 * rejoué (l'hôte journalise chaque ligne d'erreur puis sort, code 5 recommandé).
 *
 * ---- CONDITIONS DE DÉPART (en tête de ligne, facultatives, une au plus) -------------------------------------------
 * L'exécution suit TOUJOURS l'ordre du fichier : une ligne n'est examinée qu'après l'exécution de la précédente.
 *   @p N        au Present n° N : sc2a_presents() ≥ N (tout de suite s'il est déjà passé) ;
 *   @p +N       N Present après l'exécution de la commande précédente ;
 *   +MS         MS millisecondes après l'exécution de la précédente ; « +MSms » et « +Ss » (secondes, « +1.5s ») aussi ;
 *   (rien)      tout de suite après la précédente (dans le même tour de l'hôte).
 * La « précédente » de la première ligne est le démarrage du rejeu (sc2en_demarrer). Une ligne qui donne plusieurs
 * primitives (CLIC, TOUCHE, TEXTE, MODS) porte sa condition sur la première ; les suivantes partent tout de suite.
 *
 * ---- COORDONNÉES -----------------------------------------------------------------------------------------------------
 *   j X Y [@ancre]  pixels CLIENT du jeu (entiers), ramenés par REF au back buffer présenté, puis sc2_geo_vers_ecran
 *                   (android/jni/sc2_geo.h, géométrie du dernier Present) → pixel de vue P → points P / échelle. Une
 *                   primitive en « j » attend la première géométrie (sc2en_instant.geo_ok) ;
 *   v X Y           points de vue, origine en haut à gauche ;
 *   f FX FY         fractions de la vue dans [0, 1] : pixel floor(FX × largeur px), borné au dernier (bords : f 0 0.5,
 *                   f 1 0.5).
 * Le chemin inverse, celui des entrées (M2, M8), doit faire pixels = points × échelle (sans demi-pixel ajouté : le centre
 * est ajouté par sc2_geo_vers_jeu), puis sc2_geo_vers_jeu : sc2en_vue_vers_jeu en est le témoin. L'aller-retour est
 * exact dès que l'image est agrandie (pixels de vue par pixel client ≥ 1), à un pixel près sinon.
 *
 * ---- DIRECTIVES (sans condition ; elles valent pour les lignes qui SUIVENT) ------------------------------------------
 *   REF LxH [ui|scene|lin]  les « j » suivants sont écrits pour un client LxH (320×200 à 8192×8192) et ramenés au
 *                   back buffer présenté — mêmes modèles que REF:/ECHELLE: de SC2_BANC_ENTREES (V1.1, inventaire
 *                   v1-declencheurs §6.2) : ui (défaut) = boîte 4:3 en haut, centrée (interface 2D) ; scene = champ
 *                   horizontal constant (scène 3D) ; lin = étirement linéaire. « @ui », « @scene », « @lin », « @brut »
 *                   après une coordonnée j en changent l'ancre (« @brut » : pas de mise à l'échelle). « REF - » : retour
 *                   aux pixels bruts. Sans REF : aucune transformation ;
 *   VOIE directe|file       directe (défaut) : l'événement est remis à l'entrée de la couche Cocoa (sc2m_entree) ; file :
 *                   [NSApp postEvent:atStart:NO] → sendEvent: → fenêtre → vue (quirks d'AppKit inclus ; exige le focus,
 *                   donc ISOLE 0 et la séance du mainteneur) ;
 *   ISOLE 0|1               rejeu isolé, 1 par défaut : l'app n'est jamais activée, les NSEvent matériels et les vraies
 *                   notifications de focus sont ignorés et comptés, fenêtre en orderFrontRegardless dans un coin
 *                   (greffe 3 de v3-critique §3.1). Une fois par scénario. La variable SC2_MAC_ISOLE, si elle vaut
 *                   0 ou 1, l'emporte sur le fichier (sc2en_isole).
 *
 * ---- COMMANDES ------------------------------------------------------------------------------------------------------
 *   MOVE <coord> [mods=…]                mouseMoved
 *   DOWN g|d|m <coord> [mods=…]          *MouseDown ; g, d, m = buttonNumber 0, 1, 2 (gauche, droit, milieu)
 *   UP   g|d|m <coord> [mods=…]          *MouseUp
 *   DRAG g|d|m <coord> [mods=…]          *MouseDragged
 *   CLIC g|d|m <coord> [mods=…]          DOWN puis UP au même point, au même instant
 *   MOLETTE DY [DX] [precis]             scrollWheel au curseur : DY, DX passés tels quels (crans, ou points avec
 *                                        precis = hasPreciseScrollingDeltas)
 *   TOUCHE kvk ["car"] [mods=…] [rep]    keyDown puis keyUp : keyCode kvk (0 à 0x7F), characters = car (vide sans
 *                                        lui), isARepeat avec rep (sur le keyDown)
 *   TOUCHE_BAS kvk ["car"] [mods=…] [rep]   /   TOUCHE_HAUT kvk ["car"] [mods=…]
 *   MODS liste | MODS -                  flagsChanged : modificateurs tenus APRÈS. Un événement par touche qui change
 *                                        (relâchées d'abord), keyCode de la touche, drapeaux d'après ; sans changement :
 *                                        un jalon (la condition et l'instant de la ligne sont gardés)
 *   TEXTE "chaîne" [pas=MS]              une TOUCHE par caractère : kVK de la disposition US (ANSI), characters = le
 *                                        caractère, shift pour les majuscules et les symboles hauts (sans flagsChanged) ;
 *                                        un caractère par Present (@p +1), ou un toutes les MS ms avec pas= ; ASCII
 *                                        imprimable, \n (Retour), \t (Tab) — pour le reste, TOUCHE kvk "car"
 *   FOCUS 0|1                            perte / retour du focus (voie directe : chemin des notifications)
 *   OCCULTE 0|1                          fenêtre occultée / visible (chemin de NSWindowDidChangeOcclusionState)
 *   MINIATURISER   RESTAURER             miniaturize: / deminiaturize:
 *   PLEIN_ECRAN [0|1]                    toggleFullScreen: (sans argument : bascule)
 *   TAILLE W H   (ou TAILLE WxH)         setContentSize:, en points (100 à 16384)
 *   FERMER                               le bouton de fermeture (windowShouldClose:)
 *   QUITTER                              [NSApp terminate:nil] : le chemin de Cmd+Q
 *   SIGNAL TERM|INT                      kill(getpid(), SIGTERM | SIGINT) (sources dispatch du mode fenêtre)
 *   ATTENDRE_LIGNE "motif" [délai_ms]    attend une ligne COMPLÈTE du journal contenant le motif (délai par défaut
 *                                        60 000 ms) ; le journal est lu par un curseur qui n'avance que vers l'avant :
 *                                        une ligne ne sert qu'une fois. Les lignes du rejeu lui-même (« [MACEV] » en
 *                                        tête de ligne) ne comptent pas : la trace de l'ATTENDRE_LIGNE, qui recopie le
 *                                        motif, ne le satisfait pas. Délai dépassé : SC2EN_DELAI, l'hôte journalise
 *                                        « [MACEV] délai dépassé » puis sort (code 5 au journal)
 *   JOURNAL "texte"                      une ligne témoin
 * Modificateurs « mods=a,b,… » : cmd ctrl alt (= opt) shift caps fn ; préfixe r pour la touche de droite (rcmd, rctrl,
 * ralt, ropt, rshift), l… accepté pour la gauche. Ils S'AJOUTENT aux modificateurs tenus par MODS.
 *
 * ---- JOURNAL (fait par l'hôte) --------------------------------------------------------------------------------------
 * « [MACEV] » + sc2en_resumer au démarrage ; « [MACEV] » + sc2en_decrire pour chaque primitive exécutée, par exemple
 *   [MACEV] l.12 CLIC → DOWN g j 767 301 → v 383.50 150.50 (px 767 301, client 767 301) — Present n°412, t=12.345 s
 * Aucune de ces lignes ne cite « .SC2Map », « → Maps\ » ni « Loading » (comptes des scripts de banc). « [MACEV] » est
 * en TÊTE de ligne (préfixe réservé au rejeu ; sc2_log n'en ajoute aucun) : ATTENDRE_LIGNE saute ces lignes.
 *
 * ---- UTILISATION (M7) -----------------------------------------------------------------------------------------------
 *   sc2en_script s; char err[4096];
 *   if (sc2en_charger(getenv(SC2_MAC_EVENEMENTS), <son nom>, &s, err, sizeof err)) { journal(err); sortie 5; }
 *   isole = sc2en_isole(&s, getenv(SC2_MAC_ISOLE), &pourquoi);
 *   sc2en_suivi j; sc2en_suivi_ouvrir(&j, chemin_du_journal ou "-", taille_au_lancement);
 *   sc2en_rejeu r; sc2en_demarrer(&r, &s, maintenant(), sc2en_suivi_rappel, &j);
 *   à chaque Present (dispatch_async vers le fil principal) et à l'échéance rendue (dispatch_after) :
 *     for (;;) { k = sc2en_avancer(&r, maintenant(), &e, &ech);
 *                if (k == SC2EN_EXECUTER) { sc2en_vers_vue(&e->pos, &vue, &pt) ; NSEvent ou action ; journal ; continue; }
 *                if (k == SC2EN_DELAI) { journal « délai dépassé » ; sortie 5 ; } break; }
 * maintenant() : t_ms d'une horloge monotone, presents = sc2a_presents(), geo_ok = géométrie de présentation connue. */
#ifndef SC2_MAC_EVENEMENTS_H
#define SC2_MAC_EVENEMENTS_H
#include <stddef.h>
#include <stdint.h>
#include "../android/jni/sc2_geo.h"

#define SC2EN_TEXTE_MAX        256        /* characters, motif, texte : octets UTF-8, NUL compris */
#define SC2EN_DELAI_DEFAUT_MS  60000u     /* ATTENDRE_LIGNE sans délai */
#define SC2EN_SCRUTATION_MS    50.0       /* ATTENDRE_LIGNE : échéance de relecture du journal */
#define SC2EN_FICHIER_MAX      (1u << 20) /* taille maximale d'un scénario */
#define SC2EN_EVT_MAX          100000     /* primitives au plus */

/* Modificateurs : valeurs EXACTES des bits de côté d'IOLLEvent.h (NX_DEVICE…KEYMASK) et de NSEventModifierFlags
 * (bits 16-23). sc2en_evt.drapeaux = sc2en_drapeaux(tenus | mods=) se passe tel quel à modifierFlags. */
#define SC2EN_MOD_LCTRL   0x00000001u     /* NX_DEVICELCTLKEYMASK */
#define SC2EN_MOD_LSHIFT  0x00000002u     /* NX_DEVICELSHIFTKEYMASK */
#define SC2EN_MOD_RSHIFT  0x00000004u     /* NX_DEVICERSHIFTKEYMASK */
#define SC2EN_MOD_LCMD    0x00000008u     /* NX_DEVICELCMDKEYMASK */
#define SC2EN_MOD_RCMD    0x00000010u     /* NX_DEVICERCMDKEYMASK */
#define SC2EN_MOD_LALT    0x00000020u     /* NX_DEVICELALTKEYMASK */
#define SC2EN_MOD_RALT    0x00000040u     /* NX_DEVICERALTKEYMASK */
#define SC2EN_MOD_RCTRL   0x00002000u     /* NX_DEVICERCTLKEYMASK */
#define SC2EN_MOD_CAPS    (1u << 16)      /* NSEventModifierFlagCapsLock */
#define SC2EN_MOD_SHIFT   (1u << 17)      /* NSEventModifierFlagShift */
#define SC2EN_MOD_CTRL    (1u << 18)      /* NSEventModifierFlagControl */
#define SC2EN_MOD_ALT     (1u << 19)      /* NSEventModifierFlagOption */
#define SC2EN_MOD_CMD     (1u << 20)      /* NSEventModifierFlagCommand */
#define SC2EN_MOD_FN      (1u << 23)      /* NSEventModifierFlagFunction */

typedef enum {
    SC2EN_DEPLACER = 1,    /* mouseMoved           pos, drapeaux */
    SC2EN_BAS,             /* *MouseDown           pos, bouton, drapeaux (clickCount 1) */
    SC2EN_HAUT,            /* *MouseUp             pos, bouton, drapeaux */
    SC2EN_GLISSER,         /* *MouseDragged        pos, bouton, drapeaux */
    SC2EN_MOLETTE,         /* scrollWheel          dy, dx, precis, drapeaux (au curseur : pas de pos) */
    SC2EN_TOUCHE_BAS,      /* keyDown              kvk, texte (characters), drapeaux, repetition */
    SC2EN_TOUCHE_HAUT,     /* keyUp                kvk, texte, drapeaux */
    SC2EN_MODIFICATEURS,   /* flagsChanged         kvk (la touche qui change), drapeaux (APRÈS l'événement) */
    SC2EN_FOCUS,           /* valeur 0|1 */
    SC2EN_OCCULTE,         /* valeur 0|1 */
    SC2EN_MINIATURISER,
    SC2EN_RESTAURER,
    SC2EN_PLEIN_ECRAN,     /* valeur -1 (bascule), 0, 1 */
    SC2EN_TAILLE,          /* l, h (points) */
    SC2EN_FERMER,
    SC2EN_QUITTER,
    SC2EN_SIGNAL,          /* valeur = n° de signal (SIGTERM, SIGINT) */
    SC2EN_ATTENDRE_LIGNE,  /* texte = motif, delai_ms : géré par sc2en_avancer (l'hôte ne fait que journaliser) */
    SC2EN_JOURNAL,         /* texte */
    SC2EN_JALON            /* rien à faire (MODS sans changement) : garde la condition et l'instant de sa ligne */
} sc2en_type;

typedef enum {
    SC2EN_TOUT_DE_SUITE = 0,  /* aucune condition */
    SC2EN_AU_PRESENT,         /* @p N : presents ≥ v */
    SC2EN_PRESENTS_APRES,     /* @p +N : presents − presents de la précédente ≥ v */
    SC2EN_MS_APRES            /* +MS : t − t de la précédente ≥ v (ms) */
} sc2en_quand_type;
typedef struct { sc2en_quand_type type; double v; } sc2en_quand;

enum { SC2EN_ANCRE_UI = 1, SC2EN_ANCRE_SCENE, SC2EN_ANCRE_LIN, SC2EN_ANCRE_BRUT };
enum { SC2EN_VOIE_DIRECTE = 0, SC2EN_VOIE_FILE = 1 };

typedef struct {
    char     repere;          /* 'j' client, 'v' points de vue, 'f' fractions de la vue ; 0 : pas de coordonnée */
    double   x, y;
    uint32_t ref_w, ref_h;    /* REF en vigueur à cette ligne (0 : pixels bruts) */
    int      ancre;           /* résolue : SC2EN_ANCRE_UI, _SCENE, _LIN ou _BRUT (toujours BRUT sans REF) */
} sc2en_coord;

typedef struct {
    int         ligne, colonne;   /* source : ligne (n° d'élément pour un texte), colonne de la commande */
    int         sous;             /* rang dans l'expansion de la ligne (0 = la première primitive) */
    const char *commande;         /* commande source (« CLIC », « TEXTE »…) : chaîne statique */
    sc2en_quand quand;
    sc2en_type  type;
    int         voie;             /* SC2EN_VOIE_DIRECTE ou SC2EN_VOIE_FILE (VOIE en vigueur à cette ligne) */
    sc2en_coord pos;
    int         bouton;           /* buttonNumber NSEvent : 0 gauche, 1 droit, 2 milieu */
    uint32_t    drapeaux;         /* modifierFlags (bits de côté + bits NSEventModifierFlag) */
    uint16_t    kvk;              /* keyCode (kVK_…) */
    int         repetition;       /* isARepeat */
    double      dy, dx;           /* MOLETTE */
    int         precis;           /* MOLETTE : hasPreciseScrollingDeltas */
    double      l, h;             /* TAILLE (points) */
    int         valeur;           /* FOCUS, OCCULTE, PLEIN_ECRAN, SIGNAL */
    uint32_t    delai_ms;         /* ATTENDRE_LIGNE */
    char        texte[SC2EN_TEXTE_MAX];  /* characters (TOUCHE_*), motif (ATTENDRE_LIGNE), texte (JOURNAL) */
} sc2en_evt;

typedef struct {
    sc2en_evt *evt;               /* primitives, dans l'ordre d'exécution */
    int        n, cap;
    int        lignes;            /* lignes (éléments) lues */
    int        isole;             /* ISOLE du scénario : -1 absent, 0, 1 */
    int        n_file;            /* primitives en VOIE file */
    int        n_j;               /* primitives en coordonnées j */
    uint32_t   ref_w, ref_h;      /* dernière REF lue (résumé) */
    char       source[256];
} sc2en_script;

/* ---- analyse ---------------------------------------------------------------------------------------------------- */
/* Analyse texte[0..n) ; separateur_pv : « ; » sépare aussi les éléments. Rend le nombre d'erreurs (0 = scénario valide) ;
 * err reçoit une ligne « source:ligne:colonne: message\n » par erreur (tronquée à errcap). s est (ré)initialisé, puis
 * libéré par sc2en_liberer, même en cas d'erreur. */
int  sc2en_analyser(const char *texte, size_t n, int separateur_pv, const char *source, sc2en_script *s,
                    char *err, size_t errcap);
/* spec « @chemin » ou texte (cf. SPÉCIFICATION) ; nom_texte : source nommée dans les erreurs d'un texte (le nom de la
 * variable). Rend le nombre d'erreurs ; un fichier illisible compte pour une. */
int  sc2en_charger(const char *spec, const char *nom_texte, sc2en_script *s, char *err, size_t errcap);
void sc2en_liberer(sc2en_script *s);
/* Isolation effective : env (la valeur de SC2_MAC_ISOLE, ou NULL) « 0 » ou « 1 » l'emporte ; sinon ISOLE du scénario ;
 * sinon 1. *pourquoi (facultatif) : « variable », « scénario » ou « défaut » (une valeur invalide de la variable est
 * ignorée : « défaut » ou « scénario »). */
int  sc2en_isole(const sc2en_script *s, const char *env, const char **pourquoi);
/* « scénario <source> : L lignes, N événements (j : a, VOIE file : b), REF LxH, ISOLE i (pourquoi) » ; rend la longueur. */
int  sc2en_resumer(const sc2en_script *s, int isole, const char *pourquoi, char *o, size_t cap);

/* ---- modificateurs et clavier ------------------------------------------------------------------------------------ */
uint32_t sc2en_drapeaux(uint32_t bits_de_cote);   /* ajoute les bits NSEventModifierFlag des côtés tenus */
/* Caractère → touche de la disposition US (ANSI) : 1 trouvé (kvk, *shift = 1 s'il faut Maj), 0 sinon. */
int  sc2en_qwerty(uint32_t cp, uint16_t *kvk, int *shift);

/* ---- coordonnées ------------------------------------------------------------------------------------------------- */
typedef struct {
    sc2_geo  geo;                 /* géométrie du dernier Present : pixels de vue ↔ pixels client (geo.ok) */
    double   echelle;             /* backingScaleFactor : pixels par point */
    uint32_t fw, fh;              /* taille de la vue en pixels (bounds × échelle) */
} sc2en_vue;
typedef struct {
    double  vx, vy;               /* points de vue, origine en haut à gauche */
    int32_t px, py;               /* pixel de vue */
    int32_t cx, cy;               /* pixel client visé (j : après REF ; v, f : par sc2_geo_vers_jeu, -1 sans géométrie) */
    int     dedans;               /* 1 : dans l'image du jeu ; 0 : bande noire ou hors vue */
} sc2en_point;
/* REF : (x, y) écrit pour un client LxH → client réel w×h selon l'ancre (arrondi au plus proche). */
void sc2en_ref_vers_client(uint32_t L, uint32_t H, int ancre, int32_t x, int32_t y, uint32_t w, uint32_t h,
                           int32_t *ox, int32_t *oy);
/* Coordonnée → points de vue. Rend pt->dedans (1 ou 0), ou -1 si la conversion est impossible (j sans géométrie, f
 * sans taille de vue, échelle ≤ 0, pas de coordonnée). */
int  sc2en_vers_vue(const sc2en_coord *c, const sc2en_vue *v, sc2en_point *pt);
/* Témoin du chemin des entrées : points de vue → pixels (× échelle) → sc2_geo_vers_jeu. Rend 1 dans l'image, 0 sur une
 * bande (point ramené au bord), -1 sans géométrie. */
int  sc2en_vue_vers_jeu(const sc2en_vue *v, double vx, double vy, int32_t *x, int32_t *y);

/* ---- ordonnancement ---------------------------------------------------------------------------------------------- */
typedef struct {
    double   t_ms;                /* horloge monotone de l'hôte, en ms */
    unsigned presents;            /* sc2a_presents() */
    int      geo_ok;              /* géométrie de présentation connue (les primitives en j l'attendent) */
} sc2en_instant;

enum { SC2EN_RIEN = 0, SC2EN_EXECUTER = 1, SC2EN_FINI = 2, SC2EN_DELAI = 3 };

typedef struct {
    const sc2en_script *s;
    int      i;                   /* prochaine primitive */
    double   t_prec;              /* exécution de la précédente (ou démarrage) */
    unsigned p_prec;
    int      attente;             /* ATTENDRE_LIGNE en cours (primitive i) */
    double   attente_debut;
    int      final;               /* 0, SC2EN_FINI ou SC2EN_DELAI */
    int    (*ligne_vue)(void *ctx, const char *motif);  /* > 0 : une ligne contenant motif est apparue */
    void    *ctx;
} sc2en_rejeu;

/* t0 sert de « précédente » à la 1re ligne. ligne_vue : NULL = jamais (ATTENDRE_LIGNE finit en délai). */
void sc2en_demarrer(sc2en_rejeu *r, const sc2en_script *s, sc2en_instant t0,
                    int (*ligne_vue)(void *ctx, const char *motif), void *ctx);
/* Un pas : SC2EN_EXECUTER → *e à exécuter MAINTENANT (rappeler aussitôt : plusieurs primitives peuvent être dues dans le
 * même tour) ; SC2EN_RIEN → rien de dû, *echeance_ms = prochaine échéance d'horloge, ou -1 (revenir au prochain
 * Present) ; SC2EN_FINI → tout est joué ; SC2EN_DELAI → ATTENDRE_LIGNE *e échue, rejeu arrêté. FINI et DELAI restent. */
int  sc2en_avancer(sc2en_rejeu *r, sc2en_instant t, const sc2en_evt **e, double *echeance_ms);

/* Horodatage-marqueur des NSEvent synthétiques (greffe 3 ; B §2.7) : SC2EN_MARQUEUR + rang de la primitive, en
 * secondes. Un horodatage matériel (secondes depuis le démarrage de la machine) n'en approche jamais : le routeur de
 * sendEvent: (VOIE file) distingue ainsi le rejeu du matériel, à ignorer et compter en mode isolé. HYPOTHÈSE H8 (M7) :
 * AppKit conserve l'horodatage donné à la création. */
#define SC2EN_MARQUEUR 1e9
double sc2en_horodatage(int rang);
int    sc2en_synthetique(double horodatage);

/* Ligne de journal d'une primitive exécutée (sans le préfixe « [MACEV] ») ; pt : sa conversion, ou NULL. */
int  sc2en_decrire(const sc2en_evt *e, const sc2en_point *pt, unsigned presents, double t_s, char *o, size_t cap);

/* ---- suivi du journal (ATTENDRE_LIGNE) --------------------------------------------------------------------------- */
typedef struct {
    int       fd, possede;        /* possede : fd ouvert ou rouvert ici (fermé par sc2en_suivi_fermer) */
    long long pos;                /* premier octet non examiné (début d'une ligne, sauf milieu) */
    int       milieu, saute;      /* milieu : pos est dans une ligne géante (> 32 Kio) entamée ; saute : son reste ne
                                     compte pas (ligne du rejeu, ou motif déjà vu dans cette ligne) */
    int       erreur;             /* errno de la dernière lecture en échec, 0 sinon */
} sc2en_suivi;
/* chemin NULL ou « - » : la sortie standard, lue par pread (son décalage d'écriture n'est pas touché ; fichier ordinaire
 * seulement ; rouverte en lecture si elle est en écriture seule, voir sc2en_suivi_fd). depart < 0 : la fin actuelle du
 * fichier. Rend 0, ou -1 (errno). */
int  sc2en_suivi_ouvrir(sc2en_suivi *j, const char *chemin, long long depart);
/* fd prêté, jamais fermé. En écriture seule (« > fichier », « >> fichier », où pread rendrait EBADF), le même fichier est
 * rouvert en lecture (F_GETPATH sous macOS, /proc/self/fd sous Linux ; même st_dev et st_ino), double possédé ; si la
 * réouverture échoue : -1, EBADF, dès ici. */
int  sc2en_suivi_fd(sc2en_suivi *j, int fd, long long depart);
/* 1 : une ligne COMPLÈTE contenant motif a été lue, curseur juste après elle ; 0 : pas encore ; -1 : erreur. Les lignes
 * qui commencent par « [MACEV] » (le rejeu lui-même) sont passées sans compter. */
int  sc2en_suivi_chercher(sc2en_suivi *j, const char *motif);
int  sc2en_suivi_rappel(void *suivi, const char *motif);           /* adaptateur pour sc2en_demarrer */
void sc2en_suivi_fermer(sc2en_suivi *j);

#endif
