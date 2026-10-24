/* mac/sc2_mac_fenetre.m — hôte Cocoa de l'app macOS (V3, lot M7, 24/10/2026 ; v3-critique.md §3.4 fiche M7, d'après
 * v3-conception-A.md §1.2-1.3 et les greffes 2 et 3 de B).
 *
 * Mode fenêtre (SC2_FENETRE=1, posée par LSEnvironment) : main() → sc2m_fenetre_lancer, sur le fil principal, AVANT tout
 * autre fil : journal, pile alternée du fil principal, verrou d'instance, configuration (tous les setenv, M3), scénario du
 * rejeu (son isolement décide de la fenêtre), SC2_ECRAN=auto résolu, SIGTERM/SIGINT par sources dispatch, puis
 * NSApplication. À applicationDidFinishLaunching : fenêtre, vue SC2Vue dont la couche est une CAMetalLayer,
 * sc2v_fenetre(couche) AVANT le fil du jeu (aucune contention sur le verrou du présentateur), rejeu, fil « sc2-jeu »
 * (sc2m_jeu_lancer, M0). Le fil du jeu crée surface (VK_EXT_metal_surface seulement) et swapchain à son 1er Present.
 *
 * Règles (A §1.2, critique §3.2) — le fil principal :
 *   - ne prend le verrou du présentateur (sc2v_fenetre) qu'une fois avant le fil du jeu, puis seulement pour RÉARMER une
 *     swapchain basculée (présentateur inactif ; attente ≤ 2 s, journalisée « [MAC] réarmement : fil principal bloqué ») ;
 *   - n'attend jamais le fil du jeu, n'appelle jamais sc2a_sortir (sauf le secours, sur une file globale) ;
 *   - le fil du jeu ne fait jamais de dispatch_sync vers lui, ni d'appel AppKit synchrone.
 * Cycle de vie :
 *   - focus : perte → touches relâchées, puis jeu désactivé (sc2_input_activation(0)) sauf SC2_MAC_DESACTIVER=0 ; retour →
 *     Verr. maj resynchronisé (M8), sc2_input_activation(1), réarmement éventuel ;
 *   - occultation (greffe 2) : sc2v_visible(0), la présentation est sautée et le jeu CONTINUE ;
 *   - pause (sc2a_suspendre) : réduction, Cmd+H (masquée), veille de l'écran ou du système ; SC2_PAUSE=0 : jamais ;
 *   - sortie : Cmd+Q, menu Quitter et Apple event « quit » (applicationShouldTerminate → NSTerminateLater : la
 *     fermeture de session ou l'extinction n'est jamais annulée, le processus finit par exit()), bouton de fermeture
 *     (windowShouldClose: rend NO), SIGTERM, SIGINT → sc2m_quitter : sc2a_demander_sortie, le fil du jeu sort à son Present
 *     suivant (sc2a_sortir → sc2_stop(0) → exit) ; secours 3 s (sc2a_sortir depuis une file globale si aucun Present n'a
 *     suivi) puis 8 s (_exit(3)). Le code est noté par le crochet de sc2_stop (M10) ; avant M10, seuls les codes que l'hôte
 *     connaît sûrement (fin du runtime sans exit, délai du rejeu, sortie avant le jeu) portent « [MAC] code de sortie N ».
 * Rejeu isolé (greffe 3, SC2_MAC_ISOLE, défaut 1 dès que SC2_MAC_EVENEMENTS est posée) : l'app n'est jamais activée
 * (politique Accessory, fenêtre en orderFrontRegardless dans le coin bas-gauche), les NSEvent matériels sont ignorés et
 * comptés (sendEvent:), comme les vraies notifications de focus, d'occultation et de veille ; les commandes du rejeu
 * (FOCUS, OCCULTE) passent par les mêmes chemins. Réduction, plein écran et taille ne peuvent venir que du rejeu (le
 * matériel est ignoré) : leurs notifications sont traitées.
 */
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>               /* kVK_Return, kVK_ANSI_KeypadEnter (en-tête seulement) */
#import <QuartzCore/CAMetalLayer.h>
#include <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sc2_config_mac.h"
#include "sc2_mac.h"
#include "sc2_mac_evenements.h"         /* sc2en_synthetique : horodatage-marqueur du rejeu */
#include "sc2_mac_fenetre.h"
#include "sc2_vk_presente.h"
#include "../android/jni/sc2_geo.h"

/* runtime (sc2_host.h et sc2_input.h tirent cpu.h : pas d'inclusion en Objective-C) */
void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sc2_input_activation(int actif);
void sc2_input_tout_relacher(void);
int  sc2m_sortie_en_cours(void);                     /* M10, mac/sc2_mac_sortie.m : sc2_stop a remis la sortie au crochet */

/* ---- état (fil principal, sauf les atomiques) -------------------------------------------------------------------- */

@class SC2Vue;
static NSWindow *g_fen;
static SC2Vue *g_vue;
static CAMetalLayer *g_couche;
static id g_delegue;
static sc2m_dossiers g_dossiers;
static sc2m_config g_cfg;
static int g_isole;                                  /* décidé avant NSApplication, jamais changé ensuite */
static atomic_uint g_fw, g_fh;                       /* taille du calque en pixels (bounds × contentsScale) */
static _Atomic double g_echelle;
static int g_reduite, g_cachee, g_veille_ecran, g_veille_sys, g_suspendu, g_pause_permise = 1, g_desactiver = 1;
static atomic_int g_quitter;                          /* 1 : sortie demandée */
static atomic_ullong g_ign_materiels, g_ign_notifs;
static id<NSObject> g_activite;
static dispatch_source_t g_signaux[2], g_veille_fil, g_faute;
/* keyCode dont l'hôte a pris le keyDown (Cmd+Q, Ctrl+Cmd+F, Option+Entrée) : son keyUp n'ira pas au jeu (revue MIN-4).
 * Remis à zéro par un nouveau keyDown de la même touche remis au jeu, et à la perte du focus (keyUp jamais venu). */
static uint8_t g_avale[256];

double sc2mf_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
int sc2mf_isole(void) { return g_isole; }
void sc2mf_ignores(uint64_t *m, uint64_t *n) { *m = atomic_load(&g_ign_materiels); *n = atomic_load(&g_ign_notifs); }

static void ignorer_notification(const char *quoi)
{
    if (!atomic_fetch_add(&g_ign_notifs, 1) || !strncmp(quoi, "veille", 6))
        sc2_log("[MAC] isolé : notification réelle ignorée (%s) — les suivantes sont comptées", quoi);
}

void sc2m_taille_pixels(uint32_t *fw, uint32_t *fh, double *echelle)
{
    if (fw) *fw = atomic_load(&g_fw);
    if (fh) *fh = atomic_load(&g_fh);
    if (echelle) *echelle = atomic_load(&g_echelle);
}

/* « LxH » (bornes du jeu) ; 0 = lu */
static int lire_taille(const char *e, double *w, double *h)
{
    double a, b;
    if (!e || sscanf(e, "%lfx%lf", &a, &b) != 2 || a < 100 || b < 100 || a > 16384 || b > 16384) return -1;
    *w = a; *h = b;
    return 0;
}

/* ---- réarmement d'une swapchain basculée (A §2.3) ------------------------------------------------------------------ */

static int g_plafond_vu;                             /* revue MIN-1 : réarmement refusé une fois → plus d'essai */
static void rearmer_si_bascule(const char *pourquoi)
{
    if (g_plafond_vu || !g_couche || !sc2m_jeu_lance() || !sc2a_presents()) return;
    char e[192];
    sc2v_etat_court(e, sizeof e);
    if (!strstr(e, "basculée")) return;               /* « swapchain basculée : … » : présentateur inactif */
    uint32_t fw = atomic_load(&g_fw), fh = atomic_load(&g_fh);
    double t0 = sc2mf_ms();
    sc2v_fenetre((__bridge void *)g_couche, fw, fh);  /* verrou libre : le jeu ne présente plus ; fence ≤ 2 s */
    double dt = sc2mf_ms() - t0;
    sc2v_etat_court(e, sizeof e);
    g_plafond_vu = !!strstr(e, "basculée");          /* toujours basculée : plafond (REARMEMENTS_MAX) atteint */
    sc2_log("[MAC] réarmement : fil principal bloqué %.0f ms (%s) — %s", dt, pourquoi,
            g_plafond_vu ? "REFUSÉ par le présentateur (plafond de réarmements atteint) : plus aucun essai, image figée"
                         : "swapchain réarmée sur la couche, recréée au Present suivant");
}

/* ---- taille du calque ------------------------------------------------------------------------------------------- */

static void taille_maj(const char *pourquoi)
{
    if (!g_fen || !g_couche) return;
    double s = g_fen.backingScaleFactor > 0 ? g_fen.backingScaleFactor : 1.0;
    if (g_couche.contentsScale != s) g_couche.contentsScale = s;
    NSSize b = ((NSView *)g_vue).bounds.size;
    uint32_t w = (uint32_t)lround(b.width * s), h = (uint32_t)lround(b.height * s);
    uint32_t ow = atomic_load(&g_fw), oh = atomic_load(&g_fh);
    if (w == ow && h == oh && s == atomic_load(&g_echelle)) return;
    atomic_store(&g_fw, w); atomic_store(&g_fh, h); atomic_store(&g_echelle, s);
    if (!ow) return;                                  /* 1re mesure : journalisée par la création de la fenêtre */
    static unsigned n;
    if (n++ < 64)
        sc2_log("[MAC] taille (%s) : vue %.0fx%.0f points, %ux%u px (échelle %g) — sc2v_taille_changee", pourquoi,
                b.width, b.height, w, h, s);
    sc2v_taille_changee();                            /* atomique, sans verrou : vérifiée au Present suivant */
    rearmer_si_bascule(pourquoi);
}

/* ---- pause (réduction, Cmd+H, veille) --------------------------------------------------------------------------- */

static void activite(int oui)
{
    if (oui && !g_activite)
        g_activite = [[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                              reason:@"SC2 Partout : partie en cours"];
    else if (!oui && g_activite) { [[NSProcessInfo processInfo] endActivity:g_activite]; g_activite = nil; }
}

static void suspension_maj(const char *pourquoi)
{
    int s = g_pause_permise && (g_reduite || g_cachee || g_veille_ecran || g_veille_sys);
    if (s == g_suspendu) return;
    g_suspendu = s;
    sc2_log("[MAC] %s : jeu %s (réduite %d, masquée %d, veille écran %d, veille système %d)", pourquoi,
            s ? "SUSPENDU au Present suivant, son en pause" : "REPRIS", g_reduite, g_cachee, g_veille_ecran, g_veille_sys);
    sc2a_suspendre(s);                                /* son en pause sous g_susp_mu ; le jeu s'arrête à son Present */
    activite(!s || sc2mr_actif());                    /* en pause, App Nap permis — sauf pendant un rejeu (minuteurs exacts) */
}

/* ---- focus et occultation --------------------------------------------------------------------------------------- */

void sc2m_focus(int actif)
{
    static int etat = -1;
    actif = !!actif;
    if (etat == actif) return;
    int avant = etat;
    etat = actif;
    if (!actif) {
        memset(g_avale, 0, sizeof g_avale);           /* un keyUp avalé qui ne viendra plus */
        sc2_input_tout_relacher();                   /* KEYUP de chaque touche tenue, puis boutons */
        sc2m_entrees_reinitialiser();                /* M8 : VK mémorisés, cumuls de molette, Ctrl+clic */
        if (g_desactiver) sc2_input_activation(0);   /* WM_ACTIVATEAPP(0) + WM_ACTIVATE(WA_INACTIVE), auto-activation suspendue */
        else sc2_log("[MAC] focus perdu : touches relâchées, jeu laissé actif (SC2_MAC_DESACTIVER=0)");
    } else {
        sc2m_entrees_resync();                       /* M8 : Verr. maj d'après modifierFlags */
        sc2_input_activation(1);
        if (avant == 0) rearmer_si_bascule("retour du focus");
    }
}

void sc2mf_focus(int actif, int simule)
{
    if (g_isole && !simule) { ignorer_notification(actif ? "fenêtre clé" : "perte du focus"); return; }
    sc2m_focus(actif);
}

void sc2mf_occultation(int occultee, int simule)
{
    if (g_isole && !simule) { ignorer_notification(occultee ? "fenêtre occultée" : "fenêtre visible"); return; }
    static int etat;
    occultee = !!occultee;
    if (etat == occultee) return;
    etat = occultee;
    sc2_log("[MAC] fenêtre %s%s", occultee ? "occultée : présentation suspendue, le jeu continue"
                                           : "de nouveau visible : présentation reprise", simule ? " (rejeu)" : "");
    sc2v_visible(!occultee);                          /* atomique : le présentateur saute la présentation (M5) */
}

/* ---- sortie ------------------------------------------------------------------------------------------------------- */

void sc2m_quitter(const char *pourquoi)
{
    if (atomic_exchange(&g_quitter, 1)) {
        sc2_log("[MAC] sortie déjà demandée (%s : ignoré)", pourquoi);
        return;
    }
    sc2_log("[MAC] sortie demandée (%s)", pourquoi);
    sc2mr_arreter("sortie demandée");
    if (!sc2m_jeu_lance() || sc2m_jeu_fini()) {      /* rien à arrêter : sortie tout de suite, sur ce fil */
        if (sc2m_sortie_code() < 0) sc2m_sortie_noter(0);
        exit(sc2m_sortie_code());
    }
    unsigned p0 = sc2a_presents();
    sc2a_demander_sortie();                           /* réveille aussi une pause ; le jeu sort à son Present suivant */
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), q, ^{
        if (sc2a_presents() != p0) return;           /* un Present a suivi : le fil du jeu est déjà dans sa sortie */
        if (sc2m_sortie_en_cours()) return;          /* M10 (MIN-7) : sc2_stop a fini, exit() en route — jamais de swapchain
                                                      * libérée pendant les finaliseurs de MoltenVK */
        sc2_log("[MAC] sortie de secours : aucun Present 3 s après la demande — bilan et exit depuis un fil de secours");
        sc2a_sortir("secours (aucun Present en 3 s)");
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), q, ^{
        static const char m[] = "[MAC] sortie de secours : processus toujours vivant 8 s après la demande — _exit(3)\n";
        (void)!write(1, m, sizeof m - 1);
        _exit(3);
    });
}

void sc2mf_sortie_code(int code, const char *pourquoi)
{
    sc2m_sortie_noter(code);                          /* le dernier gestionnaire atexit sortira avec ce code */
    sc2m_quitter(pourquoi);
}

/* J : sc2_poc_main a rendu sans exit() (image introuvable : 2). Le fil principal finit, sans être attendu. */
void sc2m_jeu_rendu(int rc)
{
    int code = rc ? rc : 4;
    dispatch_async(dispatch_get_main_queue(), ^{
        sc2_log("[MAC] fin : le runtime a rendu %d sans exit() (image introuvable ?) — sortie, code %d", rc, code);
        if (!g_isole) {
            NSAlert *a = [NSAlert new];
            a.messageText = @"SC2 Partout n'a pas pu démarrer le jeu";
            a.informativeText = [NSString stringWithFormat:@"Le runtime a rendu %d (image introuvable ?). "
                                 @"Journal : ~/Library/Logs/SC2Partout/sc2-partout.log", rc];
            [a runModal];
        }
        sc2m_sortie_noter(code);
        exit(code);
    });
}

static void exception_journal(NSException *e)
{
    sc2_log("[MAC] exception Objective-C non rattrapée : %s — %s", e.name.UTF8String, e.reason.UTF8String);
}

/* ---- vue ----------------------------------------------------------------------------------------------------------- */

@interface SC2Vue : NSView
@end
@implementation SC2Vue
- (instancetype)initWithFrame:(NSRect)r
{
    if ((self = [super initWithFrame:r])) {
        self.wantsLayer = YES;                        /* couche de soutien = makeBackingLayer : la CAMetalLayer */
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    }
    return self;
}
- (CALayer *)makeBackingLayer
{
    CAMetalLayer *l = [CAMetalLayer layer];
    l.opaque = YES;
    l.needsDisplayOnBoundsChange = NO;
    return l;
}
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer { }                               /* MoltenVK présente depuis le fil du jeu */
- (BOOL)isFlipped { return YES; }                     /* points de vue : origine en haut à gauche (rejeu, M8) */
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; taille_maj("redimensionnement de la vue"); }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; taille_maj("échelle de l'écran"); }
/* Entrées : M8 (sc2m_entree). Le clavier arrive par -[SC2Application sendEvent:], ces méthodes ne sont qu'un repli. */
#define SC2_VERS_M8(sel) - (void)sel:(NSEvent *)e { sc2m_entree((__bridge void *)e, (__bridge void *)self); }
SC2_VERS_M8(mouseDown) SC2_VERS_M8(mouseUp) SC2_VERS_M8(mouseDragged) SC2_VERS_M8(mouseMoved)
SC2_VERS_M8(rightMouseDown) SC2_VERS_M8(rightMouseUp) SC2_VERS_M8(rightMouseDragged)
SC2_VERS_M8(otherMouseDown) SC2_VERS_M8(otherMouseUp) SC2_VERS_M8(otherMouseDragged)
SC2_VERS_M8(scrollWheel) SC2_VERS_M8(keyDown) SC2_VERS_M8(keyUp) SC2_VERS_M8(flagsChanged)
#undef SC2_VERS_M8
@end

/* ---- application : routeur des NSEvent --------------------------------------------------------------------------- */

static int type_entree(NSEventType t)                 /* 2 : clavier ; 1 : souris, molette, gestes, tablette */
{
    switch (t) {
    case NSEventTypeKeyDown: case NSEventTypeKeyUp: case NSEventTypeFlagsChanged: return 2;
    case NSEventTypeLeftMouseDown: case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseDown: case NSEventTypeOtherMouseUp:
    case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged: case NSEventTypeScrollWheel: case NSEventTypeMagnify: case NSEventTypeSwipe:
    case NSEventTypeRotate: case NSEventTypeBeginGesture: case NSEventTypeEndGesture: case NSEventTypeGesture:
    case NSEventTypeSmartMagnify: case NSEventTypePressure: case NSEventTypeTabletPoint:
    case NSEventTypeTabletProximity: return 1;
    default: return 0;
    }
}

@interface SC2Application : NSApplication
@end
@implementation SC2Application
- (void)sendEvent:(NSEvent *)e
{
    int k = type_entree(e.type);
    if (k && g_isole && !sc2en_synthetique(e.timestamp)) {   /* greffe 3 : le matériel ne touche jamais au jeu */
        if (!atomic_fetch_add(&g_ign_materiels, 1))
            sc2_log("[MAC] isolé : NSEvent matériel ignoré (type %d) — les suivants sont comptés", (int)e.type);
        return;
    }
    if (k == 2 && g_fen && e.window == g_fen) {
        NSEventModifierFlags f = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
        if (e.type == NSEventTypeKeyDown) {
            NSString *c = e.charactersIgnoringModifiers.lowercaseString;
            int cmd = !!(f & NSEventModifierFlagCommand), ctrl = !!(f & NSEventModifierFlagControl);
            int opt = !!(f & NSEventModifierFlagOption);
            uint8_t *av = &g_avale[e.keyCode & 0xFF];
            if (cmd && !opt && [c isEqualToString:@"q"]) { *av = 1; [super sendEvent:e]; return; }   /* menu : Quitter */
            if (cmd && ctrl && [c isEqualToString:@"f"]) { *av = 1; [super sendEvent:e]; return; }   /* menu : plein écran */
            if (opt && !cmd && !ctrl && (e.keyCode == kVK_Return || e.keyCode == kVK_ANSI_KeypadEnter)) {
                *av = 1;
                if (!e.isARepeat) [g_fen toggleFullScreen:nil];                             /* Option+Entrée */
                return;
            }
            *av = 0;                                  /* touche remise au jeu : son keyUp aussi */
            /* Cmd+H et Cmd+M partent au jeu (Ctrl+H, Ctrl+M en M8) : DÉCISION de conception (A §2.4 ; Cmd → Ctrl, aucun
             * Ctrl+lettre du jeu volé) — masquer / réduire restent au Dock et au sélecteur d'apps */
        } else if (e.type == NSEventTypeKeyUp && g_avale[e.keyCode & 0xFF]) {
            g_avale[e.keyCode & 0xFF] = 0;            /* keyDown pris par l'hôte : ce keyUp n'est pas pour le jeu */
            return;
        }
        sc2m_entree((__bridge void *)e, (__bridge void *)g_vue);   /* keyUp sous Cmd compris (AppKit l'avalerait) */
        return;
    }
    [super sendEvent:e];
}
@end

/* ---- délégué de l'application et de la fenêtre -------------------------------------------------------------------- */

static void fenetre_prete(void);

@interface SC2Delegue : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation SC2Delegue
- (void)applicationDidFinishLaunching:(NSNotification *)n { (void)n; fenetre_prete(); }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)a
{
    (void)a;
    if (!sc2m_jeu_lance() || sc2m_jeu_fini()) return NSTerminateNow;
    sc2m_quitter("Cmd+Q");                            /* le fil du jeu sortira à son Present ; jamais d'attente ici */
    /* revue adverse MAJ-2 : NSTerminateCancel ANNULERAIT la fermeture de session, l'extinction et le redémarrage d'une
     * mise à jour (loginwindow envoie « quit » à chaque app). NSTerminateLater : AppKit attend une réponse que l'on ne
     * donne jamais — la sortie du jeu (puis exit()) termine le processus ; les sources dispatch de la file principale
     * (rejeu, signaux, secours) continuent dans ce mode. Jamais replyToApplicationShouldTerminate: depuis le fil du jeu. */
    return NSTerminateLater;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return NO; }
- (BOOL)windowShouldClose:(NSWindow *)w { (void)w; sc2m_quitter("fermeture"); return NO; }
- (void)windowDidBecomeKey:(NSNotification *)n { (void)n; sc2mf_focus(1, 0); }
- (void)windowDidResignKey:(NSNotification *)n { (void)n; sc2mf_focus(0, 0); }
- (void)applicationDidResignActive:(NSNotification *)n { (void)n; sc2mf_focus(0, 0); }
- (void)windowDidChangeOcclusionState:(NSNotification *)n
{
    (void)n;
    sc2mf_occultation(!(g_fen.occlusionState & NSWindowOcclusionStateVisible), 0);
}
- (void)windowWillMiniaturize:(NSNotification *)n { (void)n; g_reduite = 1; suspension_maj("fenêtre réduite"); }
- (void)windowDidMiniaturize:(NSNotification *)n { (void)n; g_reduite = 1; suspension_maj("fenêtre réduite"); }
- (void)windowDidDeminiaturize:(NSNotification *)n { (void)n; g_reduite = 0; suspension_maj("fenêtre restaurée"); }
- (void)applicationDidHide:(NSNotification *)n { (void)n; g_cachee = 1; suspension_maj("app masquée"); }
- (void)applicationDidUnhide:(NSNotification *)n { (void)n; g_cachee = 0; suspension_maj("app de nouveau affichée"); }
- (void)veille:(NSNotification *)n
{
    NSString *k = n.name;
    int dort = [k isEqualToString:NSWorkspaceScreensDidSleepNotification] || [k isEqualToString:NSWorkspaceWillSleepNotification];
    int ecran = [k isEqualToString:NSWorkspaceScreensDidSleepNotification] || [k isEqualToString:NSWorkspaceScreensDidWakeNotification];
    if (g_isole) { ignorer_notification(ecran ? "veille de l'écran" : "veille du système"); return; }
    if (ecran) g_veille_ecran = dort; else g_veille_sys = dort;
    suspension_maj(ecran ? (dort ? "écran en veille" : "écran réveillé") : (dort ? "système en veille" : "système réveillé"));
}
- (void)windowDidResize:(NSNotification *)n { (void)n; taille_maj("fenêtre redimensionnée"); }
- (void)windowDidEndLiveResize:(NSNotification *)n { (void)n; taille_maj("fin du redimensionnement"); }
- (void)windowDidChangeBackingProperties:(NSNotification *)n { (void)n; taille_maj("propriétés de l'écran"); }
- (void)windowDidChangeScreen:(NSNotification *)n { (void)n; taille_maj("autre écran"); }
- (void)windowDidEnterFullScreen:(NSNotification *)n { (void)n; sc2_log("[MAC] plein écran 1"); taille_maj("plein écran"); }
- (void)windowDidExitFullScreen:(NSNotification *)n { (void)n; sc2_log("[MAC] plein écran 0"); taille_maj("fenêtré"); }
/* constaté le 24/10 (session verrouillée) : toggleFullScreen: échoue pour toute app, active ou non — journalisé */
- (void)windowDidFailToEnterFullScreen:(NSWindow *)w { (void)w; sc2_log("[MAC] plein écran REFUSÉ par le système (session verrouillée ou Spaces indisponibles ?)"); }
- (void)windowDidFailToExitFullScreen:(NSWindow *)w { (void)w; sc2_log("[MAC] sortie du plein écran REFUSÉE par le système"); }
@end

/* ---- veille du fil principal (journal seulement) ------------------------------------------------------------------ */

static _Atomic double g_battement;
static void veille_demarrer(void)
{
    atomic_store(&g_battement, sc2mf_ms());
    g_veille_fil = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                          dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(g_veille_fil, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC,
                              100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_veille_fil, ^{
        static int dit_bloque, dit_present; static unsigned p_vu; static double t_p;
        double t = sc2mf_ms(), b = atomic_load(&g_battement);
        if (t - b > 3000 && !dit_bloque) {
            dit_bloque = 1;
            sc2_log("[MAC] fil principal sans réponse depuis %.1f s", (t - b) / 1e3);
        }
        dispatch_async(dispatch_get_main_queue(), ^{ atomic_store(&g_battement, sc2mf_ms()); });
        if (dit_bloque && t - b < 1500) {
            dit_bloque = 0;
            sc2_log("[MAC] fil principal de nouveau réactif");
        }
        unsigned p = sc2a_presents();
        if (p != p_vu || !t_p) { p_vu = p; t_p = t; dit_present = 0; }
        else if (sc2m_jeu_lance() && !sc2m_jeu_fini() && !g_suspendu && !atomic_load(&g_quitter) && p
                 && t - t_p > 10000 && !dit_present) {
            dit_present = 1;
            sc2_log("[MAC] aucun Present depuis %.0f s hors pause (dernier n°%u) : chargement long ou jeu bloqué", (t - t_p) / 1e3, p);
        }
    });
    dispatch_resume(g_veille_fil);
}

/* ---- SC2_MAC_TEST_FAUTE=principal:N | jeu:N (diagnostic du chemin de plantage, M10) ------------------------------- */

static void faute_armer(void)
{
    const char *e = getenv("SC2_MAC_TEST_FAUTE");
    if (!e || !*e) return;
    char fil[16] = ""; unsigned n = 0;
    if (sscanf(e, "%15[a-z]:%u", fil, &n) != 2 || (strcmp(fil, "principal") && strcmp(fil, "jeu"))) {
        sc2_log("[MAC] SC2_MAC_TEST_FAUTE=%s ignorée (principal:N ou jeu:N)", e);
        return;
    }
    int jeu = !strcmp(fil, "jeu");
    sc2_log("[MAC] SC2_MAC_TEST_FAUTE : faute provoquée sur le fil %s au Present n°%u", jeu ? "du jeu" : "principal", n);
    g_faute = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(g_faute, DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_faute, ^{
        if (sc2a_presents() < n) return;
        dispatch_source_cancel(g_faute);
        if (!jeu) {
            sc2_log("[MAC] SC2_MAC_TEST_FAUTE : SIGSEGV sur le fil principal (Present n°%u)", sc2a_presents());
            volatile int *p = (volatile int *)(uintptr_t)16;
            *p = 1;
            return;
        }
        thread_act_array_t l; mach_msg_type_number_t nl = 0; pthread_t cible = NULL;
        if (task_threads(mach_task_self(), &l, &nl) == KERN_SUCCESS) {
            for (mach_msg_type_number_t i = 0; i < nl; i++) {
                pthread_t p = pthread_from_mach_thread_np(l[i]);
                char nom[64] = "";
                if (p && !pthread_getname_np(p, nom, sizeof nom) && !strcmp(nom, "sc2-jeu")) cible = p;
                mach_port_deallocate(mach_task_self(), l[i]);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)l, nl * sizeof *l);
        }
        sc2_log("[MAC] SC2_MAC_TEST_FAUTE : SIGSEGV envoyé au fil « sc2-jeu » (Present n°%u)%s", sc2a_presents(),
                cible ? "" : " — fil introuvable, rien envoyé");
        if (cible) pthread_kill(cible, SIGSEGV);
    });
    dispatch_resume(g_faute);
}

/* ---- fenêtre prête (applicationDidFinishLaunching) ---------------------------------------------------------------- */

static void fenetre_prete(void)
{
    /* 1. installation à choisir (premier lancement sans SC2_INSTALL) : M9, puis configuration terminée */
    if (g_cfg.install_a_choisir) {
        char inst[1024];
        int r = 1;
        if (!sc2m_lanceur_install_choisir(inst, sizeof inst)) {
            setenv("SC2_INSTALL", inst, 1);
            r = sc2m_configurer(&g_dossiers, &g_cfg);
        }
        if (r) {
            sc2_log("[MAC] installation de StarCraft II introuvable ou non conforme : %s — sortie, code 2", g_cfg.motif);
            sc2m_sortie_noter(2);
            exit(2);
        }
    }

    /* 2. fenêtre, vue, CAMetalLayer */
    double tw = 1280, th = 720;
    const char *e = getenv("SC2_MAC_TAILLE");
    if (e && lire_taille(e, &tw, &th)) sc2_log("[MAC] SC2_MAC_TAILLE=%s ignorée (LxH en points)", e);
    NSRect r = NSMakeRect(0, 0, tw, th);
    NSWindowStyleMask st = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                         | NSWindowStyleMaskResizable;
    g_fen = [[NSWindow alloc] initWithContentRect:r styleMask:st backing:NSBackingStoreBuffered defer:NO];
    g_fen.releasedWhenClosed = NO;                    /* C-4 : la fenêtre ne se ferme jamais sous le présentateur */
    g_fen.title = @"SC2 Partout";
    g_fen.delegate = g_delegue;
    g_fen.acceptsMouseMovedEvents = YES;
    g_fen.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    g_fen.contentMinSize = NSMakeSize(320, 200);
    g_vue = [[SC2Vue alloc] initWithFrame:r];
    g_fen.contentView = g_vue;
    [g_fen makeFirstResponder:g_vue];
    if (![g_vue.layer isKindOfClass:[CAMetalLayer class]]) {
        sc2_log("[MAC] la vue n'a pas de CAMetalLayer (%s) — sortie, code 4", object_getClassName(g_vue.layer));
        sc2m_sortie_noter(4);
        exit(4);
    }
    g_couche = (CAMetalLayer *)g_vue.layer;
    NSScreen *ecr = g_fen.screen ? g_fen.screen : [NSScreen mainScreen];
    if (g_isole) {                                    /* greffe 3 : coin bas-gauche, jamais clé, jamais activée */
        NSRect v = ecr.visibleFrame;
        [g_fen setFrameOrigin:NSMakePoint(NSMinX(v), NSMinY(v))];
        [g_fen orderFrontRegardless];
    } else {
        [g_fen center];
        [g_fen makeKeyAndOrderFront:nil];
        [NSApp activate];
    }
    taille_maj("création");
    uint32_t fw = atomic_load(&g_fw), fh = atomic_load(&g_fh);
    const char *ecran = getenv("SC2_ECRAN");
    sc2_log("[MAC] fenêtre %.0fx%.0f points, %ux%u px (échelle %g) ; écran « %s » %.0fx%.0f points ; SC2_ECRAN=%s ; %s",
            g_vue.bounds.size.width, g_vue.bounds.size.height, fw, fh, atomic_load(&g_echelle),
            ecr.localizedName.UTF8String ? ecr.localizedName.UTF8String : "?", ecr.frame.size.width,
            ecr.frame.size.height, ecran ? ecran : "(absente)",
            g_isole ? "rejeu isolé (jamais activée, matériel ignoré)" : "fenêtre clé");
    unsigned jw, jh;
    if (ecran && sscanf(ecran, "%ux%u", &jw, &jh) == 2) sc2a_taille_ecran(jw, jh);   /* comptes « sans fenêtre » */

    /* 3. la couche au présentateur AVANT le fil du jeu : le seul sc2v_fenetre hors réarmement */
    e = getenv("SC2_SWAPCHAIN");
    if (e && *e == '1') sc2v_fenetre((__bridge void *)g_couche, fw, fh);
    else sc2_log("[MAC] SC2_SWAPCHAIN=%s : aucune présentation dans la fenêtre (copie CPU absente sur Mac)", e ? e : "(absente)");

    /* 4. rejeu, puis le fil du jeu */
    e = getenv("SC2_MAC_EVENEMENTS");
    if (e && *e && sc2m_evenements_demarrer(e, (__bridge void *)g_fen, (__bridge void *)g_vue)) {
        sc2_log("[MAC] rejeu SC2_MAC_EVENEMENTS non démarré — sortie, code 5");
        sc2m_sortie_noter(5);
        exit(5);
    }
    if (sc2m_jeu_lancer(g_cfg.exe)) {
        sc2_log("[MAC] fil du jeu non créé — sortie, code 4");
        sc2m_sortie_noter(4);
        exit(4);
    }
    activite(1);                                      /* contre App Nap (fenêtre occultée) */
    veille_demarrer();
    faute_armer();
    e = getenv("SC2_MAC_PLEIN_ECRAN");
    if (e && *e == '1') [g_fen toggleFullScreen:nil];
}

/* ---- lancement ---------------------------------------------------------------------------------------------------- */

static int sortie_tot(int code)                       /* avant NSApplication : main() rend ce code */
{
    sc2m_sortie_noter(code);
    return code;
}

static void signal_source(int i, int sig, const char *nom)
{
    signal(sig, SIG_IGN);                             /* la source dispatch reçoit le signal à la place */
    g_signaux[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(g_signaux[i], ^{ sc2m_quitter(nom); });
    dispatch_resume(g_signaux[i]);
}

int sc2m_fenetre_lancer(int argc, char **argv)
{
    (void)argc; (void)argv;                           /* jamais l'argv de LaunchServices : le jeu reçoit {exe} */
    setvbuf(stdout, NULL, _IOLBF, 0);                 /* journal par ligne : ATTENDRE_LIGNE du rejeu le relit */

    /* 1. dossiers et journal */
    NSBundle *pq = [NSBundle mainBundle];
    const char *paquet = [pq.bundlePath hasSuffix:@".app"] ? pq.bundlePath.UTF8String : NULL;
    if (sc2m_dossiers_defaut(&g_dossiers, paquet)) {
        printf("[MAC] mode fenêtre : ni HOME ni SC2_MAC_SUPPORT utilisables — sortie, code 2\n");
        return sortie_tot(2);
    }
    char j[1100];
    sc2m_journal_ouvrir(sc2m_journal_chemin(&g_dossiers, j, sizeof j));
    const char *ls = getenv("__CFBundleIdentifier"), *id = pq.bundleIdentifier.UTF8String;
    int par_ls = ls && id && !strcmp(ls, id);         /* LaunchServices pose l'identifiant du paquet lancé */
    sc2_log("[MAC] mode fenêtre (source : %s) — %s, pid %d, paquet %s", par_ls ? "LaunchServices" : "environnement",
            [[pq objectForInfoDictionaryKey:@"SC2PartoutRevision"] UTF8String] ?: "révision inconnue", (int)getpid(),
            paquet ? paquet : "(aucun : binaire nu)");

    /* 2. fil principal : pile alternée et pile fantôme vide (une faute d'AppKit n'imprime pas la pile du jeu) */
    sc2m_fil_principal_preparer();

    /* 3. une seule instance, puis tous les setenv (AVANT tout autre fil) */
    int v = sc2m_verrou_instance(g_dossiers.support, SC2M_VERROU_ATTENTE_S);
    if (v == 1) { sc2_log("[MAC] une autre instance de l'app tourne — sortie, code 6"); return sortie_tot(6); }
    int c = sc2m_configurer(&g_dossiers, &g_cfg);
    if (c < 0) { sc2_log("[MAC] configuration impossible : %s — sortie, code 2", g_cfg.motif); return sortie_tot(2); }
    const char *e = getenv("SC2_PAUSE");
    g_pause_permise = !(e && *e == '0');
    e = getenv("SC2_MAC_DESACTIVER");
    g_desactiver = !(e && *e == '0');

    /* 4. scénario du rejeu : son isolement décide de la fenêtre et de la politique d'activation */
    const char *pourquoi = "défaut";
    int pr = sc2mr_preparer(getenv("SC2_MAC_EVENEMENTS"), &g_isole, &pourquoi);
    if (pr < 0) { sc2_log("[MAC] scénario SC2_MAC_EVENEMENTS invalide — sortie, code 5"); return sortie_tot(5); }

    /* 5. SC2_ECRAN=auto : le mode du jeu au rapport de la fenêtre (ou de l'écran en plein écran) */
    e = getenv("SC2_ECRAN");
    if (e && !strcmp(e, "auto")) {
        double tw = 1280, th = 720;
        const char *pe = getenv("SC2_MAC_PLEIN_ECRAN");
        if (pe && *pe == '1') { CGRect b = CGDisplayBounds(CGMainDisplayID()); tw = b.size.width; th = b.size.height; }
        else lire_taille(getenv("SC2_MAC_TAILLE"), &tw, &th);
        uint32_t w, h;
        sc2_geo_mode_auto((uint32_t)tw, (uint32_t)th, &w, &h);
        char b[32];
        snprintf(b, sizeof b, "%ux%u", w, h);
        setenv("SC2_ECRAN", b, 1);
        sc2_log("[MAC] SC2_ECRAN=auto → %s (rapport de %.0fx%.0f %s)", b, tw, th, pe && *pe == '1' ? "de l'écran" : "de la fenêtre");
    }

    /* 6. sortie : crochet de sc2_stop (M10 ; sans effet avant), signaux, exceptions */
    sc2_sortie_crochet = sc2m_sortie_crochet;
    signal_source(0, SIGTERM, "SIGTERM");
    signal_source(1, SIGINT, "SIGINT");
    NSSetUncaughtExceptionHandler(&exception_journal);

    /* 7. NSApplication */
    [SC2Application sharedApplication];
    [NSApp setActivationPolicy:g_isole ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular];
    g_delegue = [SC2Delegue new];
    NSApp.delegate = g_delegue;
    NSNotificationCenter *nc = [[NSWorkspace sharedWorkspace] notificationCenter];
    for (NSString *k in @[ NSWorkspaceScreensDidSleepNotification, NSWorkspaceScreensDidWakeNotification,
                           NSWorkspaceWillSleepNotification, NSWorkspaceDidWakeNotification ])
        [nc addObserver:g_delegue selector:@selector(veille:) name:k object:nil];
    NSMenu *barre = [NSMenu new];
    NSMenuItem *app = [NSMenuItem new];
    [barre addItem:app];
    NSMenu *m = [[NSMenu alloc] initWithTitle:@"SC2 Partout"];
    NSMenuItem *pe = [m addItemWithTitle:@"Plein écran" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    pe.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Quitter SC2 Partout" action:@selector(terminate:) keyEquivalent:@"q"];
    app.submenu = m;
    NSApp.mainMenu = barre;
    sc2m_lanceur_menu((__bridge void *)barre);        /* M9 : Partie, Missions, Réglages (talon vide) */
    [NSApp run];                                      /* ne revient pas : la sortie passe par exit() */
    return 0;
}
