/* mac/sc2_mac_evenements_ns.m — rejeu SC2_MAC_EVENEMENTS de l'app Mac, partie AppKit (V3, lot M7, 24/10/2026 ;
 * v3-critique.md §3.4 fiche M7 ; grammaire, ordonnancement et conversions : le cœur en C pur de M4,
 * mac/sc2_mac_evenements.h).
 *
 * Fil principal seulement. Le scénario est analysé AVANT NSApplication (sc2mr_preparer : son isolement décide de la
 * fenêtre), puis armé à la fenêtre prête (sc2m_evenements_demarrer) : une source dispatch de 4 ms sur la file principale
 * fait avancer l'ordonnanceur (horloge monotone, sc2a_presents(), géométrie du dernier Present) — le fil du jeu n'est
 * jamais sollicité (aucun rappel au Present : le fil principal lit des compteurs atomiques). Une primitive = UN NSEvent
 * ou UNE action AppKit :
 *   - souris, molette, clavier, flagsChanged : NSEvent synthétique, horodaté sc2en_horodatage(rang) (≥ 1e9 s, jamais un
 *     horodatage matériel : le routeur de sendEvent: les reconnaît en mode isolé). VOIE directe (défaut) : remis à
 *     l'entrée de la couche Cocoa (sc2m_entree, M8) ; VOIE file : [NSApp postEvent:atStart:NO] → sendEvent: → fenêtre →
 *     vue (exige le focus : ISOLE 0, séance du mainteneur). Molette : CGEventCreateScrollWheelEvent2 + eventWithCGEvent:
 *     (aucune permission TCC : l'événement n'est jamais posté au système) ;
 *   - FOCUS, OCCULTE : les chemins des notifications (sc2mf_focus, sc2mf_occultation), même en mode isolé ; FOCUS en
 *     VOIE file hors isolement : vraie fenêtre clé (fenêtre auxiliaire, puis la nôtre) ;
 *   - MINIATURISER, RESTAURER, PLEIN_ECRAN, TAILLE, FERMER, QUITTER : vrais appels AppKit (miniaturize:, deminiaturize:,
 *     toggleFullScreen:, setContentSize:, performClose:, terminate:) ; SIGNAL : kill(getpid()) vers les sources dispatch.
 * Journal : « [MACEV] » + sc2en_decrire en TÊTE de ligne pour chaque primitive (ATTENDRE_LIGNE saute ces lignes) ; délai
 * dépassé : « [MACEV] délai dépassé », puis sortie demandée avec le code 5 noté.
 */
#import <AppKit/AppKit.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sc2_mac.h"
#include "sc2_mac_evenements.h"
#include "sc2_mac_fenetre.h"
#include "sc2_vk_presente.h"

void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static sc2en_script g_s;
static int g_pret, g_suivi_ok, g_rang, g_executees;
static sc2en_rejeu g_r;
static sc2en_suivi g_j;
static atomic_int g_actif, g_bilan;
static dispatch_source_t g_minuteur;
static NSWindow *g_fen;
static NSView *g_vue;
static NSWindow *g_aux;                               /* VOIE file : fenêtre auxiliaire rendue clé par FOCUS 0 */
static double g_t0, g_mx = -1, g_my = -1;             /* dernier point de la souris rejouée (points de vue) */

int sc2mr_actif(void) { return atomic_load(&g_actif); }

int sc2mr_preparer(const char *spec, int *isole, const char **pourquoi)
{
    if (!spec || !*spec) {
        const char *e = getenv("SC2_MAC_ISOLE");
        *isole = e && !strcmp(e, "1");
        *pourquoi = e && (!strcmp(e, "0") || !strcmp(e, "1")) ? "variable" : "défaut";
        return 0;
    }
    if (g_pret) { *isole = sc2en_isole(&g_s, getenv("SC2_MAC_ISOLE"), pourquoi); return 1; }
    size_t cap = 1 << 16;
    char *err = malloc(cap);
    if (!err) return -1;
    int n = sc2en_charger(spec, "SC2_MAC_EVENEMENTS", &g_s, err, cap);
    if (n) {
        for (char *l = err, *f; *l; l = f) {          /* une ligne « source:ligne:colonne: message » par erreur */
            f = strchr(l, '\n');
            if (f) *f++ = 0; else f = l + strlen(l);
            if (*l) sc2_log("[MACEV] erreur : %s", l);
        }
        sc2_log("[MACEV] %d erreur(s) : rien n'est rejoué", n);
        free(err);
        sc2en_liberer(&g_s);
        return -1;
    }
    free(err);
    *isole = sc2en_isole(&g_s, getenv("SC2_MAC_ISOLE"), pourquoi);
    char r[600];
    sc2en_resumer(&g_s, *isole, *pourquoi, r, sizeof r);
    sc2_log("[MACEV] %s", r);
    g_pret = 1;
    return 1;
}

/* ---- instant et vue courante ------------------------------------------------------------------------------------- */

static sc2en_instant instant(void)
{
    uint32_t a, b, c, d;
    sc2en_instant t = { sc2mf_ms(), sc2a_presents(), !sc2v_geometrie(&a, &b, &c, &d) };
    return t;
}

static void vue_courante(sc2en_vue *v)
{
    memset(v, 0, sizeof *v);
    sc2m_taille_pixels(&v->fw, &v->fh, &v->echelle);
    uint32_t sw, sh, dw, dh;
    if (!sc2v_geometrie(&sw, &sh, &dw, &dh)) v->geo = sc2_geo_swapchain(sw, sh, dw, dh, v->fw, v->fh);
}

/* ---- synthèse ---------------------------------------------------------------------------------------------------- */

static void livrer(const sc2en_evt *e, NSEvent *ev)
{
    if (!ev) { sc2_log("[MACEV] l.%d : NSEvent non créé — primitive ignorée", e->ligne); return; }
    if (e->voie == SC2EN_VOIE_FILE) [NSApp postEvent:ev atStart:NO];
    else sc2m_entree((__bridge void *)ev, (__bridge void *)g_vue);
}

static void souris(const sc2en_evt *e, const sc2en_point *pt, int rang)
{
    static const NSEventType bas[3] = { NSEventTypeLeftMouseDown, NSEventTypeRightMouseDown, NSEventTypeOtherMouseDown };
    static const NSEventType haut[3] = { NSEventTypeLeftMouseUp, NSEventTypeRightMouseUp, NSEventTypeOtherMouseUp };
    static const NSEventType glisse[3] = { NSEventTypeLeftMouseDragged, NSEventTypeRightMouseDragged,
                                           NSEventTypeOtherMouseDragged };
    int b = e->bouton >= 0 && e->bouton <= 2 ? e->bouton : 0;
    NSEventType t = e->type == SC2EN_BAS ? bas[b] : e->type == SC2EN_HAUT ? haut[b]
                  : e->type == SC2EN_GLISSER ? glisse[b] : NSEventTypeMouseMoved;
    g_mx = pt->vx; g_my = pt->vy;
    NSPoint p = [g_vue convertPoint:NSMakePoint(pt->vx, pt->vy) toView:nil];   /* vue retournée → fenêtre */
    int clic = e->type == SC2EN_BAS || e->type == SC2EN_HAUT;
    NSEvent *ev = [NSEvent mouseEventWithType:t location:p modifierFlags:e->drapeaux timestamp:sc2en_horodatage(rang)
                                 windowNumber:g_fen.windowNumber context:nil eventNumber:rang clickCount:clic ? 1 : 0
                                     pressure:e->type == SC2EN_BAS ? 1.0f : 0.0f];
    if (ev && b && e->type != SC2EN_DEPLACER) {
        /* constaté (macOS 27, M4, test_rejeu_ns) : un Right/OtherMouse* créé ainsi porte buttonNumber 0 ; la copie de son
         * CGEvent avec le bouton 1 ou 2 garde fenêtre, point et horodatage (essai du 24/10, rapport M7) */
        CGEventRef c = ev.CGEvent ? CGEventCreateCopy(ev.CGEvent) : NULL;
        if (c) {
            CGEventSetIntegerValueField(c, kCGMouseEventButtonNumber, b);
            NSEvent *m = [NSEvent eventWithCGEvent:c];
            CFRelease(c);
            if (m) ev = m;
        }
    }
    livrer(e, ev);
}

static void molette(const sc2en_evt *e, int rang)
{
    CGEventRef c = CGEventCreateScrollWheelEvent2(NULL, e->precis ? kCGScrollEventUnitPixel : kCGScrollEventUnitLine, 2,
                                                  (int32_t)lround(e->dy), (int32_t)lround(e->dx), 0);
    if (!c) { livrer(e, nil); return; }
    NSRect vb = g_vue.bounds;                         /* au curseur : dernier point rejoué, sinon le centre */
    NSPoint v = g_mx >= 0 ? NSMakePoint(g_mx, g_my) : NSMakePoint(NSMidX(vb), NSMidY(vb));
    NSPoint s = [g_fen convertPointToScreen:[g_vue convertPoint:v toView:nil]];
    CGFloat haut = NSMaxY([NSScreen screens].firstObject.frame);   /* CG : origine en haut à gauche de l'écran principal */
    CGEventSetLocation(c, CGPointMake(s.x, haut - s.y));
    CGEventSetFlags(c, (CGEventFlags)e->drapeaux);
    CGEventSetIntegerValueField(c, kCGScrollWheelEventIsContinuous, e->precis ? 1 : 0);
    CGEventSetTimestamp(c, (CGEventTimestamp)(sc2en_horodatage(rang) * 1e9));
    NSEvent *ev = [NSEvent eventWithCGEvent:c];
    CFRelease(c);
    livrer(e, ev);
}

static void clavier(const sc2en_evt *e, int rang)
{
    NSEventType t = e->type == SC2EN_TOUCHE_BAS ? NSEventTypeKeyDown : e->type == SC2EN_TOUCHE_HAUT ? NSEventTypeKeyUp
                  : NSEventTypeFlagsChanged;
    NSString *s = t == NSEventTypeFlagsChanged ? @"" : ([NSString stringWithUTF8String:e->texte] ?: @"");
    livrer(e, [NSEvent keyEventWithType:t location:NSZeroPoint modifierFlags:e->drapeaux
                              timestamp:sc2en_horodatage(rang) windowNumber:g_fen.windowNumber context:nil
                             characters:s charactersIgnoringModifiers:s isARepeat:e->repetition ? YES : NO
                                keyCode:e->kvk]);
}

static void focus_file(int actif)                     /* VOIE file, hors isolement : vrai changement de fenêtre clé */
{
    if (!actif) {
        if (!g_aux) {
            g_aux = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 160, 90) styleMask:NSWindowStyleMaskTitled
                                                  backing:NSBackingStoreBuffered defer:NO];
            g_aux.releasedWhenClosed = NO;
            g_aux.title = @"SC2 Partout — rejeu (focus)";
        }
        [g_aux makeKeyAndOrderFront:nil];
    } else {
        [g_aux orderOut:nil];
        [g_fen makeKeyAndOrderFront:nil];
    }
}

static void executer(const sc2en_evt *e)
{
    sc2en_vue v;
    vue_courante(&v);
    sc2en_point pt;
    int conv = e->pos.repere ? sc2en_vers_vue(&e->pos, &v, &pt) : -2;
    char d[900];
    sc2en_decrire(e, conv >= 0 ? &pt : NULL, sc2a_presents(), (sc2mf_ms() - g_t0) / 1e3, d, sizeof d);
    sc2_log("[MACEV] %s", d);
    int rang = ++g_rang;
    g_executees++;
    switch (e->type) {
    case SC2EN_DEPLACER: case SC2EN_BAS: case SC2EN_HAUT: case SC2EN_GLISSER:
        if (conv < 0) sc2_log("[MACEV] l.%d : coordonnée non convertible (vue ou géométrie inconnue) — ignorée", e->ligne);
        else souris(e, &pt, rang);
        break;
    case SC2EN_MOLETTE: molette(e, rang); break;
    case SC2EN_TOUCHE_BAS: case SC2EN_TOUCHE_HAUT: case SC2EN_MODIFICATEURS: clavier(e, rang); break;
    case SC2EN_FOCUS:
        if (e->voie == SC2EN_VOIE_FILE && !sc2mf_isole()) focus_file(e->valeur);
        else sc2mf_focus(e->valeur, 1);
        break;
    case SC2EN_OCCULTE: sc2mf_occultation(e->valeur, 1); break;
    case SC2EN_MINIATURISER: [g_fen miniaturize:nil]; break;
    case SC2EN_RESTAURER: [g_fen deminiaturize:nil]; break;
    case SC2EN_PLEIN_ECRAN: {
        int en = (g_fen.styleMask & NSWindowStyleMaskFullScreen) != 0;
        if (e->valeur < 0 || e->valeur != en) [g_fen toggleFullScreen:nil];
        else sc2_log("[MACEV] l.%d : plein écran déjà %d — rien à faire", e->ligne, en);
        break;
    }
    case SC2EN_TAILLE: [g_fen setContentSize:NSMakeSize(e->l, e->h)]; break;
    case SC2EN_FERMER: [g_fen performClose:nil]; break;
    case SC2EN_QUITTER: [NSApp terminate:nil]; break;  /* le chemin de Cmd+Q (applicationShouldTerminate:) */
    case SC2EN_SIGNAL: kill(getpid(), e->valeur); break;
    case SC2EN_ATTENDRE_LIGNE: case SC2EN_JOURNAL: case SC2EN_JALON: break;
    }
}

/* ---- ordonnancement --------------------------------------------------------------------------------------------- */

void sc2mr_arreter(const char *pourquoi)
{
    if (g_minuteur) { dispatch_source_cancel(g_minuteur); g_minuteur = nil; }
    atomic_store(&g_actif, 0);
    if (!g_pret || atomic_exchange(&g_bilan, 1)) return;
    uint64_t m, n;
    sc2mf_ignores(&m, &n);
    sc2_log("[MACEV] bilan (%s) : %d primitive(s) jouée(s) sur %d ; isolé %d : %llu NSEvent matériel(s) et %llu "
            "notification(s) réelle(s) ignorés", pourquoi, g_executees, g_s.n, sc2mf_isole(), (unsigned long long)m,
            (unsigned long long)n);
}

static void pas(void)
{
    for (int k = 0; k < 256 && atomic_load(&g_actif); k++) {   /* plusieurs primitives peuvent être dues au même tour */
        const sc2en_evt *e = NULL;
        double ech = -1;
        int r = sc2en_avancer(&g_r, instant(), &e, &ech);
        if (r == SC2EN_EXECUTER) { executer(e); continue; }
        if (r == SC2EN_DELAI) {
            sc2_log("[MACEV] délai dépassé : l.%d ATTENDRE_LIGNE « %s » (%u ms) — sortie, code 5", e ? e->ligne : 0,
                    e ? e->texte : "?", e ? e->delai_ms : 0);
            sc2mr_arreter("délai dépassé");
            sc2mf_sortie_code(5, "délai dépassé du rejeu");
        } else if (r == SC2EN_FINI) {
            sc2_log("[MACEV] scénario joué jusqu'au bout (Present n°%u, t=%.3f s)", sc2a_presents(), (sc2mf_ms() - g_t0) / 1e3);
            sc2mr_arreter("scénario terminé");
        }
        return;
    }
}

int sc2m_evenements_demarrer(const char *spec, void *fenetre, void *vue)
{
    if (!g_pret) {
        int iso; const char *p;
        if (sc2mr_preparer(spec, &iso, &p) <= 0) return -1;
    }
    g_fen = (__bridge NSWindow *)fenetre;
    g_vue = (__bridge NSView *)vue;
    /* ATTENDRE_LIGNE : la sortie standard (journal de l'app, ou le fichier de « > run.log »), lue par pread depuis sa fin */
    g_suivi_ok = !sc2en_suivi_ouvrir(&g_j, "-", -1);
    if (!g_suivi_ok)
        sc2_log("[MACEV] sortie standard illisible (%s) : ATTENDRE_LIGNE finira en délai dépassé", strerror(errno));
    g_t0 = sc2mf_ms();
    sc2en_demarrer(&g_r, &g_s, instant(), g_suivi_ok ? sc2en_suivi_rappel : NULL, &g_j);
    atomic_store(&g_actif, 1);
    g_minuteur = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(g_minuteur, DISPATCH_TIME_NOW, 4 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_minuteur, ^{ pas(); });
    dispatch_resume(g_minuteur);
    sc2_log("[MACEV] rejeu armé : %d primitive(s), horloge t=0 à la fenêtre prête, Present n°%u", g_s.n, sc2a_presents());
    return 0;
}
