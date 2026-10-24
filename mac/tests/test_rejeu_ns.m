/* test_rejeu_ns.m — V3, lot M7 (24/10/2026) : synthèse AppKit du rejeu SC2_MAC_EVENEMENTS (mac/sc2_mac_evenements_ns.m)
 * en VOIE directe, contre le cœur de M4 (mac/sc2_mac_evenements.c) et des bouchons de l'hôte : chaque primitive doit
 * arriver à l'entrée de la couche Cocoa (sc2m_entree, M8) en UN NSEvent aux champs exacts, ou passer par le chemin des
 * notifications (FOCUS, OCCULTE). Ce que M8 lira :
 *   - souris : type, buttonNumber (0 gauche, 1 droit, 2 milieu : constaté 0 pour tous par mouseEventWithType, d'où la
 *     copie du CGEvent dans le rejeu), point en points de vue (vue retournée, géométrie 1:1 à l'échelle 2), modifierFlags ;
 *   - clavier : keyCode, characters, modifierFlags (bits de côté compris), keyDown puis keyUp ; flagsChanged de MODS ;
 *   - molette : scrollWheel, deltaY en crans ;
 *   - tous : horodatage-marqueur (≥ 1e9 s, croissant) — celui que le routeur de sendEvent: reconnaît en mode isolé.
 * NSApplication sans activation (politique Accessory), fenêtre jamais affichée. SKIP (77) sans session graphique. */
#import <AppKit/AppKit.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sc2_mac.h"
#include "sc2_mac_fenetre.h"

/* ---- bouchons de l'hôte ------------------------------------------------------------------------------------------- */
void sc2_log(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  | "); vprintf(fmt, a); va_end(a); putchar('\n'); }
unsigned sc2a_presents(void) { return 7; }
int sc2v_geometrie(uint32_t *sw, uint32_t *sh, uint32_t *dw, uint32_t *dh) { *sw = *dw = 1280; *sh = *dh = 720; return 0; }
void sc2m_taille_pixels(uint32_t *fw, uint32_t *fh, double *e) { *fw = 1280; *fh = 720; *e = 2.0; }
static int g_focus = -1, g_occ = -1, g_code = -1;
int  sc2mf_isole(void) { return 1; }
void sc2mf_focus(int actif, int simule) { g_focus = actif * 10 + simule; }
void sc2mf_occultation(int o, int simule) { g_occ = o * 10 + simule; }
void sc2mf_sortie_code(int code, const char *p) { (void)p; g_code = code; }
void sc2mf_ignores(uint64_t *m, uint64_t *n) { *m = *n = 0; }
double sc2mf_ms(void) { return CFAbsoluteTimeGetCurrent() * 1e3; }

static NSView *g_vue;
static NSMutableArray<NSEvent *> *g_recus;
void sc2m_entree(void *nsevent, void *vue)
{
    if (vue == (__bridge void *)g_vue) [g_recus addObject:(__bridge NSEvent *)nsevent];
    else printf("FAIL  sc2m_entree : vue inattendue\n");
}

@interface VueRetournee : NSView
@end
@implementation VueRetournee
- (BOOL)isFlipped { return YES; }
@end

static int g_pass, g_fail;
#define CHECK(c, ...) do { int c_ = (c); char m_[400]; snprintf(m_, sizeof m_, __VA_ARGS__); \
                           if (c_) { g_pass++; printf("PASS  %s\n", m_); } else { g_fail++; printf("FAIL  %s\n", m_); } } while (0)

static NSPoint point_vue(NSEvent *e) { return [g_vue convertPoint:e.locationInWindow fromView:nil]; }

int main(void)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        CFDictionaryRef s = CGSessionCopyCurrentDictionary();
        if (!s) { printf("SKIP  pas de session graphique (CGSessionCopyCurrentDictionary)\n"); return 77; }
        CFRelease(s);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 360) styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered defer:NO];
        g_vue = [[VueRetournee alloc] initWithFrame:NSMakeRect(0, 0, 640, 360)];
        w.contentView = g_vue;
        g_recus = [NSMutableArray array];
        const char *scen = "MOVE j 100 50; CLIC m j 200 100; DOWN d j 10 20 mods=ctrl; UP d j 10 20; "
                           "TOUCHE 0x00 \"a\" mods=shift; MOLETTE -3; MODS cmd; MODS -; FOCUS 0; OCCULTE 1; JOURNAL \"fin\"";
        int iso = -1; const char *pq = NULL;
        CHECK(sc2mr_preparer(scen, &iso, &pq) == 1 && iso == 1, "préparation : scénario prêt, isolé par défaut (%d, %s)", iso, pq);
        CHECK(sc2m_evenements_demarrer(scen, (__bridge void *)w, (__bridge void *)g_vue) == 0, "rejeu armé");
        for (int i = 0; i < 100 && sc2mr_actif(); i++)                         /* le minuteur de 4 ms, sur ce fil */
            [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        CHECK(!sc2mr_actif(), "scénario joué jusqu'au bout");
        static const NSEventType att[] = { NSEventTypeMouseMoved, NSEventTypeOtherMouseDown, NSEventTypeOtherMouseUp,
            NSEventTypeRightMouseDown, NSEventTypeRightMouseUp, NSEventTypeKeyDown, NSEventTypeKeyUp,
            NSEventTypeScrollWheel, NSEventTypeFlagsChanged, NSEventTypeFlagsChanged };
        int n = (int)g_recus.count, na = (int)(sizeof att / sizeof att[0]);
        CHECK(n == na, "%d NSEvent remis à sc2m_entree (attendu %d)", n, na);
        double ts = 0; int types_ok = 1, ts_ok = 1;
        for (int i = 0; i < n && i < na; i++) {
            NSEvent *e = g_recus[i];
            types_ok &= e.type == att[i];
            ts_ok &= e.timestamp >= 1e9 && e.timestamp > ts;
            ts = e.timestamp;
        }
        CHECK(types_ok, "types dans l'ordre : moved, other down/up, right down/up, key down/up, scroll, 2 × flagsChanged");
        CHECK(ts_ok, "horodatages-marqueurs ≥ 1e9 s et croissants (dernier %.0f)", ts);
        if (n == na) {
            NSEvent *m = g_recus[0], *o = g_recus[1], *r = g_recus[3], *k = g_recus[5], *ku = g_recus[6], *sw = g_recus[7];
            NSEvent *f1 = g_recus[8], *f2 = g_recus[9];
            NSPoint p = point_vue(m), q = point_vue(o), pr = point_vue(r);
            /* j 100 50 → pixel de vue 100 50 (1:1) → points 50 25 ; j 200 100 → 100 50 ; j 10 20 → 5 10 */
            CHECK(fabs(p.x - 50) < 0.01 && fabs(p.y - 25) < 0.01, "MOVE j 100 50 → points de vue (%.2f, %.2f), attendu (50, 25)", p.x, p.y);
            CHECK(fabs(q.x - 100) < 0.01 && fabs(q.y - 50) < 0.01 && o.buttonNumber == 2 && g_recus[2].buttonNumber == 2,
                  "CLIC m → OtherMouseDown/Up, buttonNumber %ld/%ld (attendu 2), point (%.2f, %.2f)", (long)o.buttonNumber,
                  (long)g_recus[2].buttonNumber, q.x, q.y);
            CHECK(fabs(pr.x - 5) < 0.01 && fabs(pr.y - 10) < 0.01 && r.buttonNumber == 1
                  && (r.modifierFlags & NSEventModifierFlagControl) && !(g_recus[4].modifierFlags & NSEventModifierFlagControl),
                  "DOWN d mods=ctrl → RightMouseDown bouton %ld, Ctrl porté par le seul DOWN, point (%.2f, %.2f)",
                  (long)r.buttonNumber, pr.x, pr.y);
            CHECK(k.keyCode == 0 && [k.characters isEqualToString:@"a"] && (k.modifierFlags & NSEventModifierFlagShift)
                  && ku.keyCode == 0 && !k.isARepeat, "TOUCHE 0x00 \"a\" mods=shift → keyDown/keyUp keyCode %u, characters « %s », Maj",
                  k.keyCode, k.characters.UTF8String);
            CHECK(sw.type == NSEventTypeScrollWheel && sw.deltaY == -3 && !sw.hasPreciseScrollingDeltas,
                  "MOLETTE -3 → scrollWheel deltaY %.1f en crans (précis %d)", sw.deltaY, sw.hasPreciseScrollingDeltas);
            CHECK(f1.keyCode == 0x37 && (f1.modifierFlags & NSEventModifierFlagCommand) && (f1.modifierFlags & 0x8)
                  && f2.keyCode == 0x37 && !(f2.modifierFlags & NSEventModifierFlagCommand),
                  "MODS cmd puis MODS - → flagsChanged kVK_Command (0x%X) tenu (bit de côté gauche) puis relâché", f1.keyCode);
        }
        CHECK(g_focus == 1, "FOCUS 0 → chemin des notifications, simulé (sc2mf_focus(0, 1) : %d)", g_focus);
        CHECK(g_occ == 11, "OCCULTE 1 → sc2mf_occultation(1, 1) (%d)", g_occ);
        CHECK(g_code < 0, "aucun délai dépassé (code %d)", g_code);

        printf("RÉSULTAT : %d PASS, %d FAIL\n", g_pass, g_fail);
    }
    return g_fail ? 1 : 0;
}
