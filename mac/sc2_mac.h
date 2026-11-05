/* mac/sc2_mac.h — interfaces internes de l'hôte macOS (V3, 22/10). Fixées par le lot M0 (v3-critique.md §3.4, d'après
 * v3-conception-A.md §2.7 et les greffes de B) ; implémentées par les lots indiqués.
 *
 * Talons : DÉFINITIONS faibles (__attribute__((weak)) ; ld64 les admet et une définition forte d'un autre objet du même
 * lien l'emporte), toutes dans mac/sc2_mac_hote.c. Jamais de RÉFÉRENCE faible non définie : Mach-O la refuse.
 * Un lot ajoute la définition FORTE dans SON fichier (les .c et .m de mac/ sont pris par le GLOB de mac/mac.cmake) et ne
 * touche ni ce fichier ni sc2_mac_hote.c ; une signature à changer passe par [SUP].
 * Fils : P = principal (AppKit), J = jeu (« sc2-jeu » en mode fenêtre ; le fil principal au banc), T = tout fil.
 * Rien ici n'exécute de code invité. En-tête C, utilisable depuis .c et .m : les objets AppKit passent en void *.
 */
#ifndef SC2_MAC_H
#define SC2_MAC_H
#include <dlfcn.h>                  /* Dl_info : même prototype que le présentateur (A-12) */
#include <stddef.h>
#include <stdint.h>
#include "sc2_android.h"            /* sc2_poc_main, sc2a_* (android/jni, couche plate-forme compilée en place) */
#include "sc2_audio_android.h"      /* sc2aa_* (M6 ; talons faibles ici tant que SC2_MAC_SON_COREAUDIO est OFF) */

/* ---- M0 : mode, banc, fil du jeu, bilan (sc2_mac_main.m, sc2_mac_hote.c) -------------------------------------------- */
int  sc2m_mode_fenetre(void);           /* T ; SC2_FENETRE=1 → 1 (app), absente ou autre → 0 (banc) ; lue une fois, dans
                                         * main(), et gardée : un setenv ultérieur n'y change rien */
int  sc2m_banc(int argc, char **argv);  /* P ; mode banc : surface SANS ÉCRAN si SC2_SWAPCHAIN=1 (sc2v_fenetre(NULL, w, h),
                                         * taille SC2_ECRAN, défaut 1280x720), son si SC2_MAC_SON=1|muet, puis
                                         * return sc2_poc_main(argc, argv) ; rien d'AppKit. Avec l'une de ces options
                                         * ou SC2_TRACE_PRESENT=1 : gestionnaire atexit de BILAN SEUL (sans libération) */
void sc2m_env_capturer(void);           /* P, 1re instruction de main() en mode fenêtre : copie profonde d'environ */
char *const *sc2m_env_origine(void);    /* T ; l'environnement capturé AVANT toute configuration (relance, M9) ; NULL
                                         * si rien n'a été capturé */
void sc2m_fil_principal_preparer(void); /* P, avant sc2m_jeu_lancer (M7) ; sigaltstack de ce fil (sc2_thread_fault_stack)
                                         * et g_sc2_ss = pile fantôme VIDE : une faute d'AppKit n'imprime pas la pile du jeu */
int  sc2m_jeu_lancer(const char *exe);  /* P, une fois ; fil « sc2-jeu » de 16 Mio, QoS USER_INTERACTIVE, nommé dans le fil :
                                         * sc2a_entrees_activer() (AVANT sc2_iat_install), son (sauf SC2_MAC_SON=0), puis
                                         * sc2_poc_main(2, {"recomp-sc2", exe}) — jamais l'argv de LaunchServices.
                                         * 0 = fil créé ; -1 = déjà demandé, exe NULL ou pthread_create refusé */
int  sc2m_jeu_lance(void);              /* T ; 1 dès que le fil du jeu est créé */
int  sc2m_jeu_fini(void);               /* T ; 1 quand sc2_poc_main a rendu sans exit() (échec de chargement) */
void sc2m_jeu_rendu(int rc);            /* J ; sc2_poc_main a rendu rc (2 : image introuvable). M7 : alerte et fin par
                                         * dispatch_async sur le fil principal, sans jamais l'attendre. Talon : journal,
                                         * puis exit(rc ? rc : 4) */
void sc2m_bilan_hote(void);             /* J ou atexit ; idempotent ; jamais d'appel AppKit : « [MAC] sortie … »,
                                         * sc2v_bilan, sc2m_entrees_bilan, [BILAN] [PRESENT], sc2aa_arreter() */
unsigned sc2m_frames_presentees(void);  /* T ; frames présentées par la swapchain (sc2a_frame(n, NULL, …) reçus) */

/* Fournis par sc2_mac_hote.c à la couche (déclarés dans sc2_android.h, fil J) : sc2a_frame_voulue (toujours 0 : pas de
 * copie CPU sur Mac), sc2a_frame (rgba NULL : présentée ; compteur, ligne [FPS] ; au 1er Present RÉELLEMENT présenté,
 * gestionnaire atexit de sortie : bilan, puis swapchain libérée avant les finaliseurs de MoltenVK — sans attendre le
 * verrou du présentateur si aucun code n'est noté, rien sur une faute notée), sc2a_avant_sortie (bilan + swapchain
 * libérée, sans AppKit), sc2a_geo_invalider (bascule : journal seulement). */

/* ---- M10 : sortie (sc2_mac_sortie.m) — greffe 1 de v3-critique.md §3.1 ----------------------------------------------- */
extern void (*sc2_sortie_crochet)(int code);
                                        /* appelé par sc2_stop (M10, sc2_runtime.c) sur le fil appelant, après bilan et
                                         * fflush, juste avant exit(code) ; NULL = exit(code) comme avant. Définition
                                         * FAIBLE (NULL) dans sc2_mac_hote.c tant que sc2_runtime.c ne la porte pas ; la
                                         * sienne (M10) l'emporte. Posé (= sc2m_sortie_crochet) par l'hôte en mode fenêtre
                                         * seulement : le banc le laisse à NULL */
void sc2m_sortie_crochet(int code);     /* fil de sc2_stop. M10 : sc2m_sortie_noter(code) ; code ≥ 128 ou fil principal →
                                         * « [MAC] code de sortie N » (write) puis _exit(code) ; sinon swapchain libérée
                                         * sur ce fil (sc2v_fenetre(NULL, 0, 0)), dispatch_async(principal) { exit(code) },
                                         * et ce fil reste parqué (pause()). Talon : vide (sc2_stop fait exit(code)) */
void sc2m_sortie_noter(int code);       /* T, sûre dans un gestionnaire de signal (un store atomique) */
int  sc2m_sortie_code(void);            /* T ; dernier code noté, -1 si aucun */
void sc2m_sortie_sans_finaliseurs(void);/* atexit, 1er enregistré par main() en mode fenêtre (donc exécuté en DERNIER) : code
                                         * noté → « [MAC] code de sortie N », fflush, _exit(N) ; aucun code noté → rend la
                                         * main (exit finit avec son propre code) */

/* ---- M5 : présentateur, branche __APPLE__ (android/jni/sc2_vk_presente.c) ----------------------------------------- */
int  sc2v_dladdr(const void *a, Dl_info *i);   /* J ; le backend l'appelle à la place de dladdr (-Ddladdr=sc2v_dladdr,
                                                * mac/mac.cmake). M5 : m_gipa remplacé par r_gipa avant le vrai dladdr
                                                * (journal [VK] bibliothèque juste). Talon : dladdr(a, i) */
void sc2v_visible(int visible);                /* P ; 0 : fenêtre occultée — la présentation est sautée (sémaphore
                                                * « rendu » consommé, ni échec ni bascule) ; 1 : reprise. Talon : vide */
uint64_t sc2v_presentees(void);                /* J ; frames réellement présentées par la swapchain. M5, sans talon */
int  sc2v_liberer_essai(void);                 /* atexit ; sc2v_fenetre(NULL, 0, 0) SANS attendre le verrou du
                                                * présentateur : 0 = libérées ; -1 = verrou tenu (faute pendant la
                                                * présentation : le fil fautif exécute exit), rien touché. M5, sans talon */

/* ---- M7 : fenêtre, cycle de vie, rejeu AppKit (sc2_mac_fenetre.m, sc2_mac_evenements_ns.m) ------------------------- */
int  sc2m_fenetre_lancer(int argc, char **argv);   /* P ; NSApplication, fenêtre, puis [NSApp run] : ne revient pas.
                                                    * Talon : « [MAC] mode fenêtre absent de ce binaire … », rend 3 */
void sc2m_taille_pixels(uint32_t *fw, uint32_t *fh, double *echelle);   /* T ; dernière taille du calque en pixels
                                                    * (bounds × contentsScale) et l'échelle ; atomique */
void sc2m_focus(int actif);                     /* P ; perte : sc2_input_tout_relacher, sc2m_entrees_reinitialiser, puis
                                                 * sc2_input_activation(0) (sauf SC2_MAC_DESACTIVER=0) ; retour : Verr. maj
                                                 * resynchronisé (sc2m_entrees_resync), puis sc2_input_activation(1) */
void sc2m_quitter(const char *pourquoi);        /* P ; Cmd+Q, fermeture (windowShouldClose: rend NO), SIGTERM/SIGINT,
                                                 * menu : « [MAC] sortie demandée (pourquoi) », sc2a_demander_sortie(),
                                                 * secours 3 s (sc2a_sortir("secours"), file globale) puis 8 s (_exit(3)) */
int  sc2m_evenements_demarrer(const char *spec, void *fenetre, void *vue);   /* P, fenêtre prête ; spec =
                                                 * SC2_MAC_EVENEMENTS (« @fichier » ou texte) ; 0 = rejeu armé */
void sc2m_ca_vider(void);                       /* J ; [CATransaction flush] si M7 en a besoin. Talon : vide */

/* ---- M8 : entrées Cocoa (sc2_entrees_mac.m) -------------------------------------------------------------------------- */
void sc2m_entree(void *nsevent, void *vue);     /* P ; tout NSEvent souris, molette, clavier ou flagsChanged de la vue du
                                                 * jeu ou du rejeu → sc2a_* / sc2_input_*. Talon : vide */
void sc2m_entrees_reinitialiser(void);          /* P ; perte du focus : VK mémorisés, cumuls de molette, Ctrl+clic. Talon */
void sc2m_entrees_resync(void);                 /* P ; retour du focus : Verr. maj d'après modifierFlags. Talon */
void sc2m_entrees_bilan(char *o, size_t cap);   /* T ; écrit « entrées : … » SANS la balise (sc2m_bilan_hote préfixe
                                                 * « [MAC] ») ; talon : chaîne vide, rien n'est imprimé */

/* ---- M9 : lanceur et paquet (sc2_lanceur_mac.m, sc2_catalogue_mac.c) ----------------------------------------------- */
void sc2m_lanceur_menu(void *nsmenu_principal); /* P ; ajoute Partie / Missions / Réglages (relance par NSWorkspace
                                                 * openApplicationAtURL:, environnement sc2m_env_origine()). Talon : vide */
int  sc2m_lanceur_install_choisir(char *install, size_t cap);   /* P ; premier lancement sans SC2_INSTALL : NSOpenPanel
                                                 * (SC2_MAC_INSTALL_PROPOSEE en test) ; 0 = dossier écrit dans install.
                                                 * Talon : journal, rend -1 */
extern const char *const sc2m_cartes[];         /* M9 seul (aucun talon) : les 33 cartes de Catalogue.CARTES_ARCHIVE */
extern const int sc2m_n_cartes;

#endif
