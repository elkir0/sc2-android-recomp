/* test_entrees_mac.m — V3, lot M8 (24/10/2026) : couche d'entrées Cocoa (mac/sc2_entrees_mac.m) contre le VRAI M2
 * (sc2_clavier_mac.c, sc2_souris_mac.c reliés) et des bouchons de l'hôte pour la géométrie et la livraison : chaque
 * NSEvent synthétique remis à sc2m_entree doit ressortir en appels exacts de sc2a_* (souris, molette) ou
 * sc2_input_touche_plateforme / sc2_input_caractere (clavier). Ce qui est prouvé ici :
 *   - conversion : point de vue retournée (50, 25) → pixel client (100, 50) à l'échelle 2, géométrie 1:1 (1280×720) ;
 *     sans géométrie (aucun Present), l'événement est abandonné ;
 *   - Ctrl+clic mémorisé : un clic GAUCHE sous Ctrl est joué bouton 2, et le relâché SANS Ctrl reste bouton 2 ;
 *   - keyUp rejeu : le VK MÉMORISÉ à l'appui (jamais recalculé), et une touche relâchée après réinitialisation
 *     (perte de focus) ne produit rien ;
 *   - caractères : « a » → U+0061, Cmd+« a » → 0x01 (Ctrl+A de Win32), Ctrl physique seul → AUCUN caractère ;
 *   - AZERTY : kVK 0x0D (position « w » US) portant le caractère « z » → VK 0x5A ;
 *   - F10 : kVK 0x6D sans caractère → VK 0x79 ; Cmd (flagsChanged kVK 0x37) → VK 0xA2, bas puis haut ;
 *   - molette : 3 crans verticaux → sc2a_molette(360, 0), 2 crans horizontaux → (240, 1) ;
 *   - bilan : « entrées : … touches tenues N » sans balise (sc2m_bilan_hote la met).
 * NSApplication sans activation (politique Accessory), fenêtre jamais affichée. SKIP (77) sans session graphique. */
#import <AppKit/AppKit.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sc2_mac.h"
#include "sc2_clavier_mac.h"
#include "sc2_souris_mac.h"
#include "../android/jni/sc2_entrees_bt.h"
#include "../android/jni/sc2_vk_presente.h"

/* ---- bouchons de l'hôte : géométrie figée 1:1 à l'échelle 2, livraisons capturées --------------------------------- */
void sc2_log(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  | "); vprintf(fmt, a); va_end(a); putchar('\n'); }
static int g_geo_ok = 1;
int  sc2v_geometrie(uint32_t *sw, uint32_t *sh, uint32_t *dw, uint32_t *dh) { *sw = *dw = 1280; *sh = *dh = 720; return g_geo_ok ? 0 : 1; }
void sc2m_taille_pixels(uint32_t *fw, uint32_t *fh, double *e) { *fw = 1280; *fh = 720; *e = 2.0; }

typedef struct { int32_t x, y; }                     Livre_depl;
typedef struct { int b, bas; int32_t x, y; }         Livre_bouton;
typedef struct { int32_t d; int horiz; int32_t x, y; } Livre_molette;
typedef struct { uint32_t vk; int bas; }             Livre_touche;
static Livre_depl    g_depl[64];    static int g_n_depl;
static Livre_bouton  g_bout[64];    static int g_n_bout;
static Livre_molette g_mole[64];    static int g_n_mole;
static Livre_touche  g_touche[64];  static int g_n_touche;
static uint32_t      g_car[64];     static int g_n_car;
void sc2a_deplacer(int32_t x, int32_t y)          { if (g_n_depl < 64) g_depl[g_n_depl] = (Livre_depl){ x, y };   g_n_depl++; }
void sc2a_bouton(int b, int bas, int32_t x, int32_t y) { if (g_n_bout < 64) g_bout[g_n_bout] = (Livre_bouton){ b, bas, x, y }; g_n_bout++; }
void sc2a_molette(int32_t d, int h, int32_t x, int32_t y) { if (g_n_mole < 64) g_mole[g_n_mole] = (Livre_molette){ d, h, x, y }; g_n_mole++; }
void sc2a_cle(uint32_t vk, int bas)               { (void)vk; (void)bas; }          /* M8 ne doit JAMAIS passer par ici */
void sc2_input_touche_plateforme(uint32_t vk, int bas) { if (g_n_touche < 64) g_touche[g_n_touche] = (Livre_touche){ vk, bas }; g_n_touche++; }
void sc2_input_caractere(uint32_t cp)             { if (g_n_car < 64) g_car[g_n_car] = cp; g_n_car++; }

/* la dernière touche livrée est-elle (vk, bas) ? */
static int derniere_touche(uint32_t vk, int bas)
{
    return g_n_touche > 0 && g_touche[g_n_touche - 1].vk == vk && g_touche[g_n_touche - 1].bas == bas;
}

@interface VueRetournee : NSView
@end
@implementation VueRetournee
- (BOOL)isFlipped { return YES; }
@end

static NSWindow *g_fen;
static NSView   *g_vue;

static int g_pass, g_fail;
#define CHECK(c, ...) do { int c_ = (c); char m_[400]; snprintf(m_, sizeof m_, __VA_ARGS__); \
                           if (c_) { g_pass++; printf("PASS  %s\n", m_); } else { g_fail++; printf("FAIL  %s\n", m_); } } while (0)

/* ---- synthèse des NSEvents (techniques du rejeu M7, sc2_mac_evenements_ns.m) -------------------------------------- */

static NSEvent *souris(NSEventType t, NSPoint pt_vue, NSEventModifierFlags f, int bouton)
{
    NSPoint p = [g_vue convertPoint:pt_vue toView:nil];                    /* vue retournée → fenêtre */
    NSEvent *e = [NSEvent mouseEventWithType:t location:p modifierFlags:f timestamp:2e9 + g_n_depl + g_n_bout
                                  windowNumber:g_fen.windowNumber context:nil eventNumber:1 clickCount:1 pressure:1.0f];
    if (e && bouton) {
        /* un Right/OtherMouse* créé par mouseEventWithType porte buttonNumber 0 : la copie du CGEvent garde tout
         * (fenêtre, point) et prend le bouton — constat M4, essai du 24/10 */
        CGEventRef c = e.CGEvent ? CGEventCreateCopy(e.CGEvent) : NULL;
        if (c) {
            CGEventSetIntegerValueField(c, kCGMouseEventButtonNumber, bouton);
            NSEvent *m = [NSEvent eventWithCGEvent:c];
            CFRelease(c);
            if (m) e = m;
        }
    }
    return e;
}

static NSEvent *molette_evt(int crans, int horiz, NSPoint pt_vue)
{
    CGEventRef c = CGEventCreateScrollWheelEvent2(NULL, kCGScrollEventUnitLine, 2,
                                                  horiz ? 0 : crans, horiz ? crans : 0, 0);
    if (!c) return nil;
    NSPoint s = [g_fen convertPointToScreen:[g_vue convertPoint:pt_vue toView:nil]];
    CGFloat haut = NSMaxY([NSScreen screens].firstObject.frame);           /* CG : origine en haut à gauche */
    CGEventSetLocation(c, CGPointMake(s.x, haut - s.y));
    NSEvent *e = [NSEvent eventWithCGEvent:c];
    CFRelease(c);
    return e;
}

static NSEvent *cle(NSEventType t, uint16_t kvk, NSString *car, NSEventModifierFlags f)
{
    return [NSEvent keyEventWithType:t location:NSZeroPoint modifierFlags:f timestamp:2e9 + g_n_touche
                          windowNumber:g_fen.windowNumber context:nil characters:car
              charactersIgnoringModifiers:car isARepeat:NO keyCode:kvk];
}

/* ---- le test ------------------------------------------------------------------------------------------------------- */

int main(void)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        CFDictionaryRef s = CGSessionCopyCurrentDictionary();
        if (!s) { printf("SKIP  pas de session graphique (CGSessionCopyCurrentDictionary)\n"); return 77; }
        CFRelease(s);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        g_fen = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 360) styleMask:NSWindowStyleMaskTitled
                                              backing:NSBackingStoreBuffered defer:NO];
        g_vue = [[VueRetournee alloc] initWithFrame:NSMakeRect(0, 0, 640, 360)];
        g_fen.contentView = g_vue;
        sc2m_entrees_resync();                                             /* options lues (défauts), une seule fois */

        /* 1. conversion : point de vue (50, 25) → pixel client (100, 50) — échelle 2, géométrie 1:1 */
        sc2m_entree((__bridge void *)souris(NSEventTypeMouseMoved, NSMakePoint(50, 25), 0, 0), (__bridge void *)g_vue);
        CHECK(g_n_depl == 1 && g_depl[0].x == 100 && g_depl[0].y == 50,
              "MOVE au point de vue (50, 25) → sc2a_deplacer(%d, %d), attendu (100, 50)",
              g_n_depl ? (int)g_depl[0].x : -1, g_n_depl ? (int)g_depl[0].y : -1);

        /* 2. clic gauche : appui et relâché bouton 0, position convertie à chaque fois */
        sc2m_entree((__bridge void *)souris(NSEventTypeLeftMouseDown, NSMakePoint(50, 25), 0, 0), (__bridge void *)g_vue);
        sc2m_entree((__bridge void *)souris(NSEventTypeLeftMouseUp, NSMakePoint(100, 50), 0, 0), (__bridge void *)g_vue);
        CHECK(g_n_bout == 2 && g_bout[0].b == 0 && g_bout[0].bas == 1 && g_bout[0].x == 100 && g_bout[0].y == 50
              && g_bout[1].b == 0 && g_bout[1].bas == 0 && g_bout[1].x == 200 && g_bout[1].y == 100,
              "clic gauche : bouton 0 bas à (100, 50) puis haut à (200, 100)");

        /* 3. Ctrl+clic MÉMORISÉ : gauche sous Ctrl = bouton 2, relâché SANS Ctrl = bouton 2 encore */
        sc2m_entree((__bridge void *)souris(NSEventTypeLeftMouseDown, NSMakePoint(50, 25), NSEventModifierFlagControl, 0),
                    (__bridge void *)g_vue);
        sc2m_entree((__bridge void *)souris(NSEventTypeLeftMouseUp, NSMakePoint(60, 30), 0, 0), (__bridge void *)g_vue);
        CHECK(g_n_bout == 4 && g_bout[2].b == 2 && g_bout[2].bas == 1 && g_bout[3].b == 2 && g_bout[3].bas == 0
              && g_bout[3].x == 120 && g_bout[3].y == 60,
              "Ctrl+clic : appui bouton 2, relâché sans Ctrl TOUJOURS bouton 2 à (%d, %d)",
              (int)g_bout[3].x, (int)g_bout[3].y);

        /* 4. clic droit et milieu : buttonNumber 1 → bouton 2, buttonNumber 2 → bouton 1 (copie du CGEvent) */
        sc2m_entree((__bridge void *)souris(NSEventTypeRightMouseDown, NSMakePoint(10, 20), 0, 1), (__bridge void *)g_vue);
        sc2m_entree((__bridge void *)souris(NSEventTypeOtherMouseDown, NSMakePoint(10, 20), 0, 2), (__bridge void *)g_vue);
        CHECK(g_n_bout == 6 && g_bout[4].b == 2 && g_bout[4].bas == 1 && g_bout[5].b == 1 && g_bout[5].bas == 1,
              "buttonNumber 1 → bouton 2 (droit), buttonNumber 2 → bouton 1 (milieu)");

        /* 5. molette : crans × 120, par axe ; la position est celle du curseur converti */
        sc2m_entree((__bridge void *)molette_evt(3, 0, NSMakePoint(320, 180)), (__bridge void *)g_vue);
        sc2m_entree((__bridge void *)molette_evt(2, 1, NSMakePoint(320, 180)), (__bridge void *)g_vue);
        CHECK(g_n_mole == 2 && g_mole[0].d == 360 && g_mole[0].horiz == 0 && g_mole[1].d == 240 && g_mole[1].horiz == 1,
              "molette : 3 crans verticaux → sc2a_molette(%d, verticale), 2 horizontaux → (%d, horizontale)",
              g_n_mole ? (int)g_mole[0].d : 0, g_n_mole > 1 ? (int)g_mole[1].d : 0);

        /* 6. clavier « a » : VK 0x41 bas, caractère U+0061, puis 0x41 haut (VK mémorisé) */
        int n_car_avant = g_n_car;
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyDown, 0x00, @"a", 0), (__bridge void *)g_vue);
        CHECK(derniere_touche(0x41, 1) && g_n_car == n_car_avant + 1 && g_car[g_n_car - 1] == 0x61,
              "keyDown kVK 0x00 « a » → touche(0x41, bas), caractère U+%04X", g_n_car ? g_car[g_n_car - 1] : 0);
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyUp, 0x00, @"a", 0), (__bridge void *)g_vue);
        CHECK(derniere_touche(0x41, 0), "keyUp kVK 0x00 → touche(0x41, haut) : le VK MÉMORISÉ à l'appui");

        /* 7. AZERTY : kVK 0x0D (position « w » US) portant « z » → VK 0x5A (règle « lettre du caractère ») */
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyDown, 0x0D, @"z", 0), (__bridge void *)g_vue);
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyUp, 0x0D, @"z", 0), (__bridge void *)g_vue);
        CHECK(derniere_touche(0x5A, 0) && g_touche[g_n_touche - 2].vk == 0x5A,
              "AZERTY : kVK 0x0D « z » → VK 0x5A bas puis haut");

        /* 8. Cmd+« a » (défaut cmd_vers_ctrl) : caractère 0x01 (Ctrl+A de Win32) */
        n_car_avant = g_n_car;
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyDown, 0x00, @"a", NSEventModifierFlagCommand), (__bridge void *)g_vue);
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyUp, 0x00, @"a", NSEventModifierFlagCommand), (__bridge void *)g_vue);
        CHECK(derniere_touche(0x41, 0) && g_n_car == n_car_avant + 1 && g_car[g_n_car - 1] == 0x01,
              "Cmd+« a » → touche(0x41) et caractère 0x01 (Ctrl+A), pas de WM_CHAR US en plus");

        /* 9. Ctrl physique seul (mode « clic » par défaut) : touche OUI, caractère NON */
        n_car_avant = g_n_car;
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyDown, 0x00, @"a", NSEventModifierFlagControl), (__bridge void *)g_vue);
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyUp, 0x00, @"a", NSEventModifierFlagControl), (__bridge void *)g_vue);
        CHECK(derniere_touche(0x41, 0) && g_n_car == n_car_avant,
              "Ctrl physique + « a » → touche(0x41) mais AUCUN caractère (Ctrl réservé au Ctrl+clic)");

        /* 10. F10 : kVK 0x6D sans caractère → VK 0x79, aucun caractère */
        n_car_avant = g_n_car;
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyDown, 0x6D, @"", 0), (__bridge void *)g_vue);
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyUp, 0x6D, @"", 0), (__bridge void *)g_vue);
        CHECK(derniere_touche(0x79, 0) && g_n_car == n_car_avant,
              "F10 : kVK 0x6D → touche(0x79) bas/haut, aucun caractère");

        /* 11. Cmd en flagsChanged : kVK 0x37 → VK 0xA2 (cmd_vers_ctrl), bas puis haut (bit indépendant) */
        sc2m_entree((__bridge void *)cle(NSEventTypeFlagsChanged, 0x37, @"", NSEventModifierFlagCommand), (__bridge void *)g_vue);
        sc2m_entree((__bridge void *)cle(NSEventTypeFlagsChanged, 0x37, @"", 0), (__bridge void *)g_vue);
        CHECK(derniere_touche(0xA2, 0) && g_touche[g_n_touche - 2].vk == 0xA2,
              "flagsChanged kVK 0x37 (Cmd) → touche(0xA2) bas puis haut");

        /* 12. sans géométrie (aucun Present) : l'événement souris est abandonné, rien n'est livré */
        g_geo_ok = 0;
        sc2m_entree((__bridge void *)souris(NSEventTypeMouseMoved, NSMakePoint(50, 25), 0, 0), (__bridge void *)g_vue);
        g_geo_ok = 1;
        CHECK(g_n_depl == 1, "sans géométrie : le MOVE est abandonné (aucune livraison, %d au total)", g_n_depl);

        /* 13. perte de focus : touche tenue effacée, le keyUp d'après est orphelin (rien au runtime) */
        int n_touche_avant = g_n_touche;
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyDown, 0x00, @"a", 0), (__bridge void *)g_vue);
        sc2m_entrees_reinitialiser();
        sc2m_entree((__bridge void *)cle(NSEventTypeKeyUp, 0x00, @"a", 0), (__bridge void *)g_vue);
        CHECK(g_n_touche == n_touche_avant + 1 && derniere_touche(0x41, 1),
              "réinitialisation (perte de focus) : l'appui est parti, le keyUp d'après ne produit RIEN");

        /* 14. bilan : « entrées : … » sans balise, touches tenues retombées à 0 */
        char b[512];
        sc2m_entrees_bilan(b, sizeof b);
        CHECK(strncmp(b, "entrées : ", 10) == 0 && strstr(b, "touches tenues 0") != NULL,
              "bilan sans balise, « touches tenues 0 » : %.80s…", b);

        printf("RÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    }
    return g_fail ? 1 : 0;
}
