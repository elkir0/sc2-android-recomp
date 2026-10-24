/* mac/sc2_souris_mac.c — souris et molette macOS → boutons, crans et pixels du runtime, en C pur (V3, lot M2 ;
 * cf. sc2_souris_mac.h). Modèle des crans : crans() de android/jni/sc2_entrees_bt.c:54-60, ici en crans entiers. */
#include <math.h>
#include <string.h>

#include "sc2_souris_mac.h"

int sc2s_bouton(int ns_bouton)
{
    switch (ns_bouton) {
    case 0: return 0;               /* gauche */
    case 1: return 2;               /* NSEvent 1 = droit (dont le clic à deux doigts) → runtime 2 */
    case 2: return 1;               /* NSEvent 2 = milieu → runtime 1 */
    default: return -1;             /* ≥ 3 : pas de WM_XBUTTON* */
    }
}

int sc2s_bouton_appui(sc2s_boutons *b, int ns_bouton, int ctrl_clic)
{
    int r = sc2s_bouton(ns_bouton);
    if (r < 0) { b->ignores++; return -1; }
    if (ns_bouton == 0 && ctrl_clic) { r = 2; b->ctrl_clics++; }   /* Ctrl physique + clic gauche = clic droit */
    b->joue[ns_bouton] = (int8_t)(r + 1);
    b->appuis++;
    return r;
}

int sc2s_bouton_relache(sc2s_boutons *b, int ns_bouton)
{
    if (ns_bouton < 0 || ns_bouton > 2) { b->ignores++; return -1; }
    if (!b->joue[ns_bouton]) { b->relaches_orphelins++; return -1; }   /* appui jamais vu (avant le focus…) */
    int r = b->joue[ns_bouton] - 1;
    b->joue[ns_bouton] = 0;
    return r;
}

int sc2s_boutons_tenus(const sc2s_boutons *b)
{
    return (b->joue[0] != 0) + (b->joue[1] != 0) + (b->joue[2] != 0);
}

void sc2s_boutons_reinitialiser(sc2s_boutons *b)
{
    memset(b->joue, 0, sizeof b->joue);
}

#define CRANS_MAX 273               /* 273 × 120 = 32 760 ≤ SHORT_MAX */

int sc2s_crans(sc2s_cumul *c, double d, int horiz, int precis, double points_par_cran, int inverse)
{
    if (!c || !isfinite(d)) return 0;
    if (inverse) d = -d;
    if (!(points_par_cran > 0)) points_par_cran = 12.0;
    double *a = horiz ? &c->h : &c->v;
    *a += precis ? d * 120.0 / points_par_cran : d * 120.0;
    double q = *a / 120.0;
    if (fabs(q) > CRANS_MAX) { *a = 0; return q > 0 ? CRANS_MAX * 120 : -CRANS_MAX * 120; }   /* excédent abandonné */
    long k = (long)(q + (q >= 0 ? 1e-9 : -1e-9));   /* tronqué vers 0, tolérance d'arrondi (3 × 12 points = 3 crans) */
    if (!k) return 0;
    *a -= (double)k * 120.0;
    return (int)(k * 120);
}

void sc2s_points_vers_pixels(double px, double py, double echelle, double *fx, double *fy)
{
    if (!(echelle > 0)) echelle = 1.0;
    *fx = floor(px * echelle + 1e-7);
    *fy = floor(py * echelle + 1e-7);
}

void sc2s_pixels_vers_points(double fx, double fy, double echelle, double *px, double *py)
{
    if (!(echelle > 0)) echelle = 1.0;
    *px = (fx + 0.5) / echelle;
    *py = (fy + 0.5) / echelle;
}
