/* mac/sc2_entrees_mac.m — entrées Cocoa : NSEvent → API d'entrées du runtime (V3, lot M8, 24/10/2026 ;
 * conception A §2.4 « Entrées », fiche M8 de v3-critique §3.4 ; appuyé sur M2 : mac/sc2_clavier_mac.{h,c} pour le
 * clavier, mac/sc2_souris_mac.{h,c} pour la souris et la molette).
 *
 * Fil principal uniquement : ces fonctions fortes remplacent les talons faibles de sc2_mac_hote.c (SC2M_TALON).
 * Appelants : le routeur -[SC2Application sendEvent:] et les méthodes SC2Vue de M7 (clavier et souris), livrer() du
 * rejeu en VOIE directe (M7) — jamais le fil du jeu. Aucune prise de verrou ici : la livraison fait tout sous g_mu
 * (sc2a_deplacer / sc2a_bouton / sc2a_molette dans sc2_android_hooks.c) ou sous I.mu (sc2_input_touche_plateforme /
 * sc2_input_caractere, récursif).
 *
 * Livraison :
 *   souris  : sc2a_deplacer (fusionné, livré au Present) pour les déplacements ; sc2a_bouton pour les appuis et
 *             relâchements (il pose d'abord la position exacte par sc2_input_souris) ;
 *   molette : sc2a_molette (crans × 120, par axe) ;
 *   clavier : DIRECTEMENT sc2_input_touche_plateforme / sc2_input_caractere — pas sc2a_cle (sc2_input_touche nu) :
 *             le message doit être MARQUÉ fourni=1, sinon i_TranslateMessage du runtime re-synthétise un WM_CHAR
 *             US en plus du nôtre (WM_CHAR en double, critère « victoire »).
 *
 * Conversion des coordonnées (A §2.4, inv. E §6.2) :
 *   1. vue isFlipped = YES (M7) ;
 *   2. p = [vue convertPoint:e.locationInWindow fromView:nil] ;
 *   3. fx, fy = sc2s_points_vers_pixels(p, échelle du calque) ;
 *   4. fw, fh = sc2m_taille_pixels ;
 *   5. g = sc2_geo_swapchain(jw, jh, dw, dh, fw, fh) quand sc2v_geometrie rend le blit du dernier Present (même
 *      modèle que vue_courante de M7 : sw == jw && sh == jh y est la règle d'Android, ici le blit presentera
 *      toujours le back buffer entier, la géométrie rendue EST celle du présentateur) ; sans géométrie connue
 *      (aucun Present encore), l'événement est compté et abandonné — le rejeu attend geo_ok pour ses « j » ;
 *   6. sc2_geo_vers_jeu(&g, fx, fy, &x, &y).
 *
 * Hypothèses (fiche : « Exige le mainteneur » reste pour le matériel) :
 *   - car_sans_modif = 1er caractère de charactersIgnoringModifiers (essai AZERTY du 24/10 : un keyDown synthétique
 *     portant le kVK d'une touche donne dans charactersByApplyingModifiers:0 le caractère de la DISPOSITION AZERTY
 *     courante — « a » pour le kVK 0x00 — mais charactersIgnoringModifiers suit l'événement, donc le matériel) ;
 *     repli charactersByApplyingModifiers:0, puis 0 ;
 *   - molette : inverse = isDirectionInvertedFromDevice (le rejeu synthétique vaut 0 → MOLETTE 3 donne +360 ;
 *     le trackpad suit le réglage « défilement naturel » de la machine). */
#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc2_clavier_mac.h"
#include "sc2_souris_mac.h"
#include "sc2_mac.h"
#include "../android/jni/sc2_entrees_bt.h"
#include "../android/jni/sc2_geo.h"
#include "../android/jni/sc2_vk_presente.h"

/* runtime (sc2_host.h et sc2_input.h tirent cpu.h : pas d'inclusion en Objective-C) */
void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sc2_input_touche_plateforme(uint32_t vk, int bas);
void sc2_input_caractere(uint32_t cp);

/* ---- état (fil principal) ----------------------------------------------------------------------------------------- */

static sc2m_clavier_etat   g_clavier;         /* VK mémorisés par kVK, compteurs */
static sc2m_clavier_options g_options = SC2M_CLAVIER_OPTIONS_DEFAUT;
static sc2s_boutons        g_boutons;         /* bouton joué à l'appui (Ctrl+clic mémorisé) */
static sc2s_cumul          g_molette;         /* reste fractionnaire par axe */
static double              g_points_par_cran = 12.0;
static int                 g_options_lues;    /* une lecture par run : sc2m_entrees_resync, jamais keyDown */
static uint64_t            g_non_convertibles, g_kvk_inconnus, g_molettes;

/* ---- conversion --------------------------------------------------------------------------------------------------- */

/* Géométrie du dernier Present (modèle de vue_courante, M7). Rend 1 avec *g rempli. */
static int geo_courante(sc2_geo *g)
{
    uint32_t fw = 0, fh = 0;
    double echelle = 0;
    sc2m_taille_pixels(&fw, &fh, &echelle);
    uint32_t sw, sh, dw, dh;
    memset(g, 0, sizeof *g);
    if (sc2v_geometrie(&sw, &sh, &dw, &dh)) return 0;
    *g = sc2_geo_swapchain(sw, sh, dw, dh, fw, fh);
    return g->ok;
}

/* Point de la fenêtre → pixel client du jeu. Rend 1 si converti. */
static int vers_client(NSEvent *e, NSView *vue, int32_t *x, int32_t *y)
{
    if (!vue) return 0;
    uint32_t fw, fh;
    double echelle;
    sc2m_taille_pixels(&fw, &fh, &echelle);
    NSPoint p = [vue convertPoint:e.locationInWindow fromView:nil];   /* vue retournée : origine en haut à gauche */
    double fx, fy;
    sc2s_points_vers_pixels(p.x, p.y, echelle, &fx, &fy);
    sc2_geo g;
    if (!geo_courante(&g)) return 0;
    sc2_geo_vers_jeu(&g, fx, fy, x, y);
    return 1;
}

/* ---- molette ------------------------------------------------------------------------------------------------------ */

static void molette(NSEvent *e, NSView *vue)
{
    int32_t x = 0, y = 0;
    int pose = vers_client(e, vue, &x, &y);   /* hors image : borné au bord par sc2_geo_vers_jeu, la position part quand même */
    int inverse = e.isDirectionInvertedFromDevice;
    int precis = e.hasPreciseScrollingDeltas;
    int dv = sc2s_crans(&g_molette, e.scrollingDeltaY, 0, precis, g_points_par_cran, inverse);
    int dh = sc2s_crans(&g_molette, e.scrollingDeltaX, 1, precis, g_points_par_cran, inverse);
    if (dv) { sc2a_molette(dv, 0, x, y); g_molettes++; }
    if (dh) { sc2a_molette(dh, 1, x, y); g_molettes++; }
    (void)pose;
}

/* ---- clavier ------------------------------------------------------------------------------------------------------ */

/* 1er caractère UTF-32 de s, 0 si vide ; paires de substitution décodées (sc2m_caracteres reçoit l'UTF-16). */
static uint32_t premier_caractere(NSString *s)
{
    if (!s || !s.length) return 0;
    uint16_t u[s.length];
    [s getCharacters:u range:NSMakeRange(0, s.length)];
    uint32_t r[4];
    int n = sc2m_caracteres(0, 0, NULL, u, s.length, NULL, 0, r, 4);
    return n > 0 ? r[0] : u[0];
}

static void key_down(NSEvent *e)
{
    /* type du clavier de l'ÉVÉNEMENT (suit le clavier réellement tapé) ; sans CGEvent (synthétique sans), celui
     * du dernier événement du processus (LMGetKbdType) ; 0 (inconnu) : traité comme ANSI par sc2m_iso_de_type */
    int64_t type = e.CGEvent ? CGEventGetIntegerValueField(e.CGEvent, kCGKeyboardEventKeyboardType) : -1;
    int iso = type >= 0 ? sc2m_iso_de_clavier((int)type) : sc2m_iso();
    NSString *cim = e.charactersIgnoringModifiers;
    uint32_t car_sans_modif = cim.length ? premier_caractere(cim) : 0;
    if (!car_sans_modif)
        car_sans_modif = premier_caractere([e charactersByApplyingModifiers:0]);
    uint16_t kvk = (uint16_t)e.keyCode;
    uint32_t vk = sc2m_vk_de_kvk(kvk, car_sans_modif, iso);
    if (!vk) {
        g_kvk_inconnus++;
        if (g_kvk_inconnus <= 10)
            sc2_log("[MAC] entrées : kVK 0x%02X non traduit (caractère U+%04X) — touche ignorée", kvk, car_sans_modif);
        return;
    }
    vk = sc2m_clavier_appui(&g_clavier, kvk, vk);
    if (!vk) return;
    sc2_input_touche_plateforme(vk, 1);
    /* caractères : characters (car) et charactersIgnoringModifiers (car_sans_option, pour Alt) en UTF-16 */
    NSUInteger nc = e.characters.length, nn = cim.length;
    uint16_t car[nc ? nc : 1], sans[nn ? nn : 1];
    if (nc) [e.characters getCharacters:car range:NSMakeRange(0, nc)];
    if (nn) [cim getCharacters:sans range:NSMakeRange(0, nn)];
    uint32_t sortie[16];
    int n = sc2m_caracteres(vk, e.modifierFlags, &g_options, nc ? car : NULL, nc, nn ? sans : NULL, nn,
                            sortie, (int)(sizeof sortie / sizeof sortie[0]));
    for (int i = 0; i < n; i++) sc2_input_caractere(sortie[i]);
}

static void key_up(NSEvent *e)
{
    uint32_t vk = sc2m_clavier_relache(&g_clavier, (uint16_t)e.keyCode);   /* le VK MÉMORISÉ à l'appui */
    if (vk) sc2_input_touche_plateforme(vk, 0);
}

static void flags_changed(NSEvent *e)
{
    int bas = 0;
    uint32_t vk = sc2m_vk_modificateur((uint16_t)e.keyCode, e.modifierFlags, &g_options, &bas);
    if (!vk) return;
    if (vk == 0x14) {                       /* Verr. maj : un seul événement par bascule, jamais de keyUp */
        sc2_input_touche_plateforme(0x14, 1);
        sc2_input_touche_plateforme(0x14, 0);
        return;
    }
    sc2_input_touche_plateforme(vk, bas);
}

/* ---- l'entrée unique ---------------------------------------------------------------------------------------------- */

void sc2m_entree(void *nsevent, void *vue)
{
    NSEvent *e = (__bridge NSEvent *)nsevent;
    NSView *v = (__bridge NSView *)vue;
    int32_t x = 0, y = 0;
    switch (e.type) {
    case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
        if (!vers_client(e, v, &x, &y)) { g_non_convertibles++; break; }
        sc2a_deplacer(x, y);
        break;
    case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown: case NSEventTypeOtherMouseDown:
        if (!vers_client(e, v, &x, &y)) { g_non_convertibles++; break; }
        {
            int b = sc2s_bouton_appui(&g_boutons, (int)e.buttonNumber,
                                      (e.modifierFlags & SC2M_DRAP_CTRL) != 0);
            if (b >= 0) sc2a_bouton(b, 1, x, y);
        }
        break;
    case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseUp:
        if (!vers_client(e, v, &x, &y)) { g_non_convertibles++; break; }
        {
            int b = sc2s_bouton_relache(&g_boutons, (int)e.buttonNumber);
            if (b >= 0) sc2a_bouton(b, 0, x, y);
        }
        break;
    case NSEventTypeScrollWheel:
        molette(e, v);
        break;
    case NSEventTypeKeyDown:   key_down(e); break;
    case NSEventTypeKeyUp:     key_up(e); break;
    case NSEventTypeFlagsChanged: flags_changed(e); break;
    default: break;                          /* gestes, tablette : rien (le jeu ne les connaît pas) */
    }
}

/* ---- focus -------------------------------------------------------------------------------------------------------- */

void sc2m_entrees_reinitialiser(void)        /* perte du focus : le runtime relâche tout par sc2_input_tout_relacher */
{
    sc2m_clavier_reinitialiser(&g_clavier);
    sc2s_boutons_reinitialiser(&g_boutons);
    g_molette.v = g_molette.h = 0;
}

void sc2m_entrees_resync(void)               /* retour du focus : Verr. maj d'après l'état du système */
{
    if (!g_options_lues) {                   /* réglages lus une fois, au premier retour de focus (fenêtre prête) */
        g_options_lues = 1;
        g_options = (sc2m_clavier_options)SC2M_CLAVIER_OPTIONS_DEFAUT;
        int inconnues = sc2m_clavier_options_lire(&g_options, getenv("SC2_MAC_CMD"), getenv("SC2_MAC_CTRL"),
                                                  getenv("SC2_MAC_OPTION"));
        for (int i = inconnues; i > 0; i--)
            sc2_log("[MAC] entrées : valeur non reconnue dans SC2_MAC_CMD/_CTRL/_OPTION — défaut gardé");
        const char *pts = getenv("SC2_MAC_MOLETTE_PTS");
        if (pts && *pts) {
            double p = atof(pts);
            if (p > 0) g_points_par_cran = p;
        }
    }
    NSEventModifierFlags f = [NSEvent modifierFlags];
    if (f & NSEventModifierFlagCapsLock) {
        sc2_input_touche_plateforme(0x14, 1);
        sc2_input_touche_plateforme(0x14, 0);
    }
}

void sc2m_entrees_bilan(char *o, size_t cap)
{
    if (!cap) return;
    snprintf(o, cap,
             "entrées : %llu appui(s) de touche, %llu répétition(s), %llu relâché(s) orphelin(s), "
             "%llu clic(s) dont %llu Ctrl+clic, %llu molette(s) livrée(s), %llu non converti(s), "
             "%llu kVK non traduit(s), touches tenues %d",
             (unsigned long long)g_clavier.appuis, (unsigned long long)g_clavier.repetitions,
             (unsigned long long)g_clavier.relaches_orphelins,
             (unsigned long long)g_boutons.appuis, (unsigned long long)g_boutons.ctrl_clics,
             (unsigned long long)g_molettes, (unsigned long long)g_non_convertibles,
             (unsigned long long)g_kvk_inconnus, sc2m_clavier_tenues(&g_clavier));
}
