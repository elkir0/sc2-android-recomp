/* test_souris_mac.c — souris et molette macOS (mac/sc2_souris_mac.c), sur l'hôte (V3, lot M2, 22/10).
 *   cc -Wall -Wextra -I mac mac/tests/test_souris_mac.c mac/sc2_souris_mac.c -o /tmp/t && /tmp/t
 * (ou ctest dans build-tests : tests-hote/v3/clavier_souris.cmake). Code de sortie 0 : tout conforme.
 *   A. boutons NSEvent 0, 1, 2 → runtime 0, 2, 1 ; ≥ 3 ignorés et comptés ;
 *   B. Ctrl+clic = clic droit, mémorisé jusqu'au relâché ; relâché orphelin ; perte du focus ;
 *   C. molette : 3 crans → −360 ; trackpad 36 points → 3 crans (12 points par cran), reste conservé ; inversion,
 *      réglage, axes séparés, arrondis, bornes ;
 *   D. points → pixels (vue retournée), aller-retour exact à toute échelle ;
 *   E. avec sc2_geo (android/jni) : vue → client à l'échelle 2 avec bandes noires (1280×720 dans 3200×1800,
 *      1280×1024 dans 2560×1440), aller-retour exhaustif au pixel près, bandes ramenées au bord. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sc2_souris_mac.h"
#include "../../android/jni/sc2_geo.h"

static int g_ko, g_n;
#define VERIF(c, ...) do { g_n++; if (!(c)) { g_ko++; if (g_ko < 40) { printf("  ÉCHEC : "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* E. client → pixel de backing (centre) → points (événement synthétique) → pixel → client, pour tout pixel client. */
static void vue_client(const char *nom, uint32_t jw, uint32_t jh, uint32_t fw, uint32_t fh, double echelle)
{
    sc2_geo g = sc2_geo_swapchain(jw, jh, fw, fh, fw, fh);
    long faux = 0;
    VERIF(g.ok, "%s : géométrie refusée", nom);
    for (int32_t y = 0; y < (int32_t)jh; y++)
        for (int32_t x = 0; x < (int32_t)jw; x++) {
            int32_t ex, ey, cx, cy;
            double px, py, fx, fy;
            sc2_geo_vers_ecran(&g, x, y, &ex, &ey);
            sc2s_pixels_vers_points(ex, ey, echelle, &px, &py);
            sc2s_points_vers_pixels(px, py, echelle, &fx, &fy);
            int d = sc2_geo_vers_jeu(&g, fx, fy, &cx, &cy);
            if (fx != ex || fy != ey || !d || cx != x || cy != y) {
                if (faux++ < 3) VERIF(0, "%s : client (%d,%d) → px (%d,%d) → pt (%.3f,%.3f) → px (%.0f,%.0f) → (%d,%d)",
                                      nom, x, y, ex, ey, px, py, fx, fy, cx, cy);
            }
        }
    VERIF(faux == 0, "%s : %ld pixels client sur %u ne reviennent pas", nom, faux, jw * jh);
    /* un événement réel tombe n'importe où dans le pixel : coin et bord opposé du pixel de backing de (767, 301) */
    int32_t ex, ey, cx, cy;
    double fx, fy;
    sc2_geo_vers_ecran(&g, 767, 301, &ex, &ey);
    sc2s_points_vers_pixels(ex / echelle, ey / echelle, echelle, &fx, &fy);
    sc2_geo_vers_jeu(&g, fx, fy, &cx, &cy);
    VERIF(cx == 767 && cy == 301, "%s : coin du pixel → (%d,%d)", nom, cx, cy);
    sc2s_points_vers_pixels((ex + 0.999) / echelle, (ey + 0.999) / echelle, echelle, &fx, &fy);
    sc2_geo_vers_jeu(&g, fx, fy, &cx, &cy);
    VERIF(cx == 767 && cy == 301, "%s : bord opposé du pixel → (%d,%d)", nom, cx, cy);
    /* bandes noires et hors de la vue : ramené au bord, signalé */
    sc2s_points_vers_pixels(g.x0 > 0 ? 1.0 : -3.0, 10.0, echelle, &fx, &fy);
    int d = sc2_geo_vers_jeu(&g, fx, fy, &cx, &cy);
    VERIF(!d && cx == 0, "%s : bord gauche (bande ou hors vue) → dedans %d, x %d", nom, d, cx);
    sc2s_points_vers_pixels(fw / echelle + 5.0, fh / echelle + 5.0, echelle, &fx, &fy);
    d = sc2_geo_vers_jeu(&g, fx, fy, &cx, &cy);
    VERIF(!d && cx == (int32_t)jw - 1 && cy == (int32_t)jh - 1, "%s : au-delà du coin bas-droit → (%d,%d)", nom, cx, cy);
}

int main(void)
{
    /* ---- A. boutons ---- */
    VERIF(sc2s_bouton(0) == 0 && sc2s_bouton(1) == 2 && sc2s_bouton(2) == 1, "NSEvent 0, 1, 2 → 0, 2, 1 : %d %d %d",
          sc2s_bouton(0), sc2s_bouton(1), sc2s_bouton(2));
    VERIF(sc2s_bouton(3) == -1 && sc2s_bouton(4) == -1 && sc2s_bouton(-1) == -1, "boutons ≥ 3 et négatifs ignorés");

    /* ---- B. appuis, Ctrl+clic, relâchés ---- */
    {
        sc2s_boutons b;
        memset(&b, 0, sizeof b);
        VERIF(sc2s_bouton_appui(&b, 0, 0) == 0 && sc2s_bouton_relache(&b, 0) == 0, "gauche");
        VERIF(sc2s_bouton_appui(&b, 1, 0) == 2 && sc2s_bouton_relache(&b, 1) == 2, "droit (NSEvent 1)");
        VERIF(sc2s_bouton_appui(&b, 2, 0) == 1 && sc2s_bouton_relache(&b, 2) == 1, "milieu (NSEvent 2)");
        VERIF(sc2s_bouton_appui(&b, 0, 1) == 2 && sc2s_boutons_tenus(&b) == 1, "Ctrl+clic gauche → droit");
        VERIF(sc2s_bouton_relache(&b, 0) == 2 && b.ctrl_clics == 1, "relâché du Ctrl+clic → droit (Ctrl déjà lâché)");
        VERIF(sc2s_bouton_appui(&b, 0, 0) == 0 && sc2s_bouton_relache(&b, 0) == 0, "Ctrl pris après l'appui : gauche");
        VERIF(sc2s_bouton_appui(&b, 1, 1) == 2 && sc2s_bouton_relache(&b, 1) == 2, "Ctrl+clic droit : droit");
        VERIF(sc2s_bouton_appui(&b, 2, 1) == 1 && sc2s_bouton_relache(&b, 2) == 1, "Ctrl+clic milieu : milieu");
        VERIF(sc2s_bouton_appui(&b, 3, 0) == -1 && sc2s_bouton_relache(&b, 3) == -1 && b.ignores == 2,
              "bouton 3 ignoré et compté (%llu)", (unsigned long long)b.ignores);
        VERIF(sc2s_bouton_relache(&b, 0) == -1 && b.relaches_orphelins == 1, "relâché sans appui : orphelin");
        sc2s_bouton_appui(&b, 0, 1); sc2s_bouton_appui(&b, 2, 0);
        VERIF(sc2s_boutons_tenus(&b) == 2, "deux tenus");
        sc2s_boutons_reinitialiser(&b);
        VERIF(sc2s_boutons_tenus(&b) == 0 && sc2s_bouton_relache(&b, 0) == -1 && b.appuis == 9,
              "perte du focus : rien de tenu, compteurs gardés (%llu appuis)", (unsigned long long)b.appuis);
    }

    /* ---- C. molette ---- */
    {
        sc2s_cumul c = { 0, 0 };
        int s = 0;
        for (int i = 0; i < 3; i++) { int d = sc2s_crans(&c, -1.0, 0, 0, 12, 0); VERIF(d == -120, "cran %d → %d", i, d); s += d; }
        VERIF(s == -360 && c.v == 0, "3 crans vers l'arrière → %d (reste %g)", s, c.v);
        VERIF(sc2s_crans(&c, -3.0, 0, 0, 12, 0) == -360, "un événement de 3 crans → −360");
        VERIF(sc2s_crans(&c, 1.0, 0, 0, 12, 0) == 120, "1 cran vers l'avant → 120");
        VERIF(sc2s_crans(&c, -1.0, 0, 0, 12, 1) == 120, "inversé : −1 cran → 120");
        VERIF(sc2s_crans(&c, 0.5, 0, 0, 12, 0) == 0 && sc2s_crans(&c, 0.5, 0, 0, 12, 0) == 120, "demi-crans cumulés");
        VERIF(sc2s_crans(&c, 1.0, 1, 0, 12, 0) == 120 && c.v == 0, "axe horizontal séparé");
        VERIF(sc2s_crans(&c, 0.25, 1, 0, 12, 0) == 0 && c.h == 30 && c.v == 0, "reste horizontal gardé à part (%g)", c.h);

        sc2s_cumul t = { 0, 0 };
        static const double pas[] = { 5, 5, 5, 5, 5, 5, 5, 1 };   /* 36 points */
        static const int att[] = { 0, 0, 120, 0, 120, 0, 0, 120 };
        s = 0;
        for (int i = 0; i < 8; i++) {
            int d = sc2s_crans(&t, pas[i], 0, 1, 12, 0);
            VERIF(d == att[i], "trackpad pas %d (%g points) → %d, attendu %d", i, pas[i], d, att[i]);
            s += d;
        }
        VERIF(s == 360 && fabs(t.v) < 1e-6, "trackpad 36 points → %d (3 crans), reste %g", s, t.v);
        VERIF(sc2s_crans(&t, 40, 0, 1, 12, 0) == 360 && fabs(t.v - 40) < 1e-9, "40 points → 3 crans, reste 4 points (%g)", t.v);
        VERIF(sc2s_crans(&t, 8, 0, 1, 12, 0) == 120 && fabs(t.v) < 1e-9, "reste conservé : +8 points → 4e cran");
        VERIF(sc2s_crans(&t, -36, 0, 1, 12, 0) == -360, "trackpad −36 points → −360");
        VERIF(sc2s_crans(&t, 6, 0, 1, 12, 0) == 0 && sc2s_crans(&t, -6, 0, 1, 12, 0) == 0 && fabs(t.v) < 1e-9,
              "aller-retour de 6 points : aucun cran");
        VERIF(sc2s_crans(&t, 12, 0, 1, 0, 0) == 120 && sc2s_crans(&t, 12, 0, 1, -5, 0) == 120, "réglage ≤ 0 → 12 points");
        VERIF(sc2s_crans(&t, 36, 0, 1, 20, 0) == 120 && fabs(t.v - 96) < 1e-9, "20 points par cran : 36 → 1 cran, reste 16");
        t.v = 0;
        s = 0;
        for (int i = 0; i < 360; i++) s += sc2s_crans(&t, 0.1, 0, 1, 12, 0);
        VERIF(s == 360, "360 pas de 0,1 point → %d (arrondis)", s);
        s = 0;
        for (int i = 0; i < 108; i++) s += sc2s_crans(&t, 1.0 / 3.0, 0, 1, 12, 0);
        VERIF(s == 360, "108 pas de 1/3 point → %d (arrondis)", s);
        t.v = 0;
        VERIF(sc2s_crans(&t, 1000, 0, 0, 12, 0) == 273 * 120 && t.v == 0, "1000 crans bornés à 273 (SHORT)");
        VERIF(sc2s_crans(&t, -1e9, 0, 1, 12, 0) == -273 * 120 && t.v == 0, "−1e9 points bornés");
        VERIF(sc2s_crans(&t, NAN, 0, 0, 12, 0) == 0 && sc2s_crans(&t, INFINITY, 0, 0, 12, 0) == 0 && t.v == 0, "NaN, ∞ ignorés");
        VERIF(sc2s_crans(NULL, 1, 0, 0, 12, 0) == 0, "cumul NULL");
    }

    /* ---- D. points → pixels ---- */
    {
        double fx, fy, px, py;
        sc2s_points_vers_pixels(767, 301, 2, &fx, &fy);
        VERIF(fx == 1534 && fy == 602, "(767, 301) pt × 2 → (%g, %g)", fx, fy);
        sc2s_points_vers_pixels(767.5, 301.25, 2, &fx, &fy);
        VERIF(fx == 1535 && fy == 602, "(767,5 ; 301,25) pt × 2 → (%g, %g)", fx, fy);
        sc2s_points_vers_pixels(-0.25, 0, 2, &fx, &fy);
        VERIF(fx == -1 && fy == 0, "hors vue à gauche → (%g, %g)", fx, fy);
        sc2s_points_vers_pixels(10.6, 3.2, 0, &fx, &fy);
        VERIF(fx == 10 && fy == 3, "échelle 0 → 1 : (%g, %g)", fx, fy);
        sc2s_pixels_vers_points(1534, 602, 2, &px, &py);
        VERIF(px == 767.25 && py == 301.25, "pixel (1534, 602) × 2 → centre (%g, %g) pt", px, py);
        static const double ech[] = { 1, 2, 3, 1.5, 1.25 };
        long faux = 0;
        for (int e = 0; e < 5; e++)
            for (int f = -8; f < 6000; f++) {
                sc2s_pixels_vers_points(f, 5999 - f, ech[e], &px, &py);
                sc2s_points_vers_pixels(px, py, ech[e], &fx, &fy);
                if (fx != f || fy != 5999 - f) faux++;
            }
        VERIF(faux == 0, "aller-retour pixel → points → pixel : %ld écarts", faux);
    }

    /* ---- E. vue → client avec sc2_geo ---- */
    vue_client("1280x720 dans 3200x1800 (échelle 2)", 1280, 720, 3200, 1800, 2);
    vue_client("1280x1024 dans 2560x1440 (échelle 2, bandes)", 1280, 1024, 2560, 1440, 2);
    vue_client("1280x720 dans 1600x1000 (échelle 2, bandes)", 1280, 720, 1600, 1000, 2);
    vue_client("1280x720 dans 1280x720 (échelle 1)", 1280, 720, 1280, 720, 1);

    printf("RÉSULTAT test_souris_mac : %d vérifications, %d échec(s)\n", g_n, g_ko);
    return g_ko != 0;
}
