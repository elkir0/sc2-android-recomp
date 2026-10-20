/* test-entrees-8h.c — passe 8h : test hôte de android/jni/sc2_entrees_bt.c (souris + clavier Android → appels de la
 * glue sc2a_*). Les sorties de la glue sont enregistrées sous forme de texte et comparées à l'attendu.
 *   cc -O1 -Wall -I android/jni android/outils/test-entrees-8h.c -o /tmp/test-entrees-8h && /tmp/test-entrees-8h
 * Code de sortie 0 : tout conforme. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../jni/sc2_entrees_bt.c"

static char g_j[4096];
static void noter(const char *fmt, ...)
{
    va_list v; va_start(v, fmt);
    size_t n = strlen(g_j);
    vsnprintf(g_j + n, sizeof g_j - n, fmt, v);
    va_end(v);
}
void sc2_log(const char *fmt, ...) { (void)fmt; }
void sc2a_deplacer(int32_t x, int32_t y) { noter("M(%d,%d) ", x, y); }
void sc2a_bouton(int b, int bas, int32_t x, int32_t y) { noter("B%d%s(%d,%d) ", b, bas ? "v" : "^", x, y); }
void sc2a_molette(int32_t d, int h, int32_t x, int32_t y) { noter("W%s%+d(%d,%d) ", h ? "h" : "", d, x, y); }
void sc2a_cle(uint32_t vk, int bas) { noter("K%02X%s ", vk, bas ? "v" : "^"); }

static int ko, total;
static void verifier(const char *nom, const char *attendu)
{
    total++;
    size_t n = strlen(g_j); if (n && g_j[n - 1] == ' ') g_j[n - 1] = 0;
    int ok = !strcmp(g_j, attendu);
    if (!ok) ko++;
    printf("[%s] %-34s %s%s%s\n", ok ? "PASS" : "ÉCHEC", nom, g_j, ok ? "" : "   ≠ attendu : ", ok ? "" : attendu);
    g_j[0] = 0;
}

int main(void)
{
    /* souris : survol, clic gauche, clic droit (BUTTON_PRESS/RELEASE), glisser, milieu */
    sc2bt_souris(SC2BT_HOVER_ENTER, 0, 10, 20, 0, 0);
    sc2bt_souris(SC2BT_HOVER_MOVE, 0, 11, 21, 0, 0);
    verifier("survol → WM_MOUSEMOVE", "M(10,20) M(11,21)");
    sc2bt_souris(SC2BT_DOWN, SC2BT_B_PRIMAIRE, 100, 200, 0, 0);
    sc2bt_souris(SC2BT_BUTTON_PRESS, SC2BT_B_PRIMAIRE, 100, 200, 0, 0);   /* même état : rien de plus */
    sc2bt_souris(SC2BT_MOVE, SC2BT_B_PRIMAIRE, 140, 230, 0, 0);           /* glisser = boîte de sélection */
    sc2bt_souris(SC2BT_BUTTON_RELEASE, 0, 140, 230, 0, 0);
    sc2bt_souris(SC2BT_UP, 0, 140, 230, 0, 0);
    verifier("clic gauche + glisser", "B0v(100,200) M(140,230) B0^(140,230)");
    sc2bt_souris(SC2BT_BUTTON_PRESS, SC2BT_B_SECONDAIRE, 300, 310, 0, 0);
    sc2bt_souris(SC2BT_BUTTON_RELEASE, 0, 300, 310, 0, 0);
    verifier("clic droit (ordre)", "B2v(300,310) B2^(300,310)");
    sc2bt_souris(SC2BT_DOWN, SC2BT_B_PRIMAIRE | SC2BT_B_SECONDAIRE, 5, 6, 0, 0);
    sc2bt_souris(SC2BT_UP, 0, 5, 6, 0, 0);
    verifier("gauche + droit ensemble", "B0v(5,6) B2v(5,6) B0^(5,6) B2^(5,6)");
    sc2bt_souris(SC2BT_BUTTON_PRESS, SC2BT_B_TERTIAIRE, 7, 8, 0, 0);
    sc2bt_souris(SC2BT_BUTTON_RELEASE, 0, 7, 8, 0, 0);
    verifier("milieu", "B1v(7,8) B1^(7,8)");
    sc2bt_souris(SC2BT_DOWN, 0, 50, 60, 0, 0);                             /* pavé tactile : tap sans buttonState */
    sc2bt_souris(SC2BT_UP, 0, 50, 60, 0, 0);
    verifier("tap de pavé tactile → gauche", "B0v(50,60) B0^(50,60)");
    sc2bt_souris(SC2BT_DOWN, SC2BT_B_SECONDAIRE, 1, 1, 0, 0);
    sc2bt_souris(SC2BT_CANCEL, SC2BT_B_SECONDAIRE, 1, 1, 0, 0);
    verifier("annulation → tout relâché", "B2v(1,1) B2^(1,1)");
    /* molette : crans entiers, fins (cumulés), horizontale */
    sc2bt_souris(SC2BT_SCROLL, 0, 640, 360, 1.0f, 0);
    sc2bt_souris(SC2BT_SCROLL, 0, 640, 360, -1.0f, 0);
    verifier("molette ±1 cran", "W+120(640,360) W-120(640,360)");
    sc2bt_souris(SC2BT_SCROLL, 0, 1, 2, 0.25f, 0);
    sc2bt_souris(SC2BT_SCROLL, 0, 1, 2, 0.25f, 0);
    sc2bt_souris(SC2BT_SCROLL, 0, 1, 2, 0, -1.0f);
    verifier("molette fine + horizontale", "W+30(1,2) W+30(1,2) Wh-120(1,2)");
    /* clavier */
    int c1 = sc2bt_cle(29, 1, 0), c2 = sc2bt_cle(29, 1, 1), c3 = sc2bt_cle(29, 0, 0);
    verifier("A (avec répétition)", "K41v K41v K41^");
    total++; if (!(c1 && c2 && c3)) { ko++; printf("[ÉCHEC] A non consommée\n"); }
    sc2bt_cle(59, 1, 0); sc2bt_cle(113, 1, 0); sc2bt_cle(57, 1, 0); sc2bt_cle(58, 0, 0); sc2bt_cle(60, 0, 0); sc2bt_cle(114, 0, 0);
    verifier("modificateurs G/D", "KA0v KA2v KA4v KA5^ KA1^ KA3^");
    sc2bt_cle(111, 1, 0); sc2bt_cle(66, 1, 0); sc2bt_cle(62, 1, 0); sc2bt_cle(61, 1, 0); sc2bt_cle(67, 1, 0); sc2bt_cle(112, 1, 0);
    verifier("Échap Entrée Espace Tab ← Suppr", "K1Bv K0Dv K20v K09v K08v K2Ev");
    sc2bt_cle(7, 1, 0); sc2bt_cle(16, 1, 0); sc2bt_cle(131, 1, 0); sc2bt_cle(142, 1, 0); sc2bt_cle(144, 1, 0); sc2bt_cle(160, 1, 0);
    verifier("0 9 F1 F12 pavé0 pavéEntrée", "K30v K39v K70v K7Bv K60v K0Dv");
    sc2bt_cle(19, 1, 0); sc2bt_cle(20, 1, 0); sc2bt_cle(21, 1, 0); sc2bt_cle(22, 1, 0); sc2bt_cle(92, 1, 0); sc2bt_cle(93, 1, 0);
    verifier("flèches, pages", "K26v K28v K25v K27v K21v K22v");
    sc2bt_cle(55, 1, 0); sc2bt_cle(56, 1, 0); sc2bt_cle(69, 1, 0); sc2bt_cle(70, 1, 0); sc2bt_cle(74, 1, 0); sc2bt_cle(76, 1, 0);
    verifier("ponctuation , . - = ; /", "KBCv KBEv KBDv KBBv KBAv KBFv");
    int v = sc2bt_cle(24, 1, 0), b = sc2bt_cle(4, 1, 0);                  /* VOLUME_UP, BACK : au système */
    verifier("volume / retour non traduits", "");
    total++; if (v || b) { ko++; printf("[ÉCHEC] touche système consommée\n"); }
    char bl[320]; sc2bt_bilan(bl, sizeof bl); printf("bilan : %s\n", bl);
    printf("%s (%d/%d)\n", ko ? "ÉCHEC" : "PASS", total - ko, total);
    return ko != 0;
}
