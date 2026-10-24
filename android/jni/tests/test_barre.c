/* test_barre.c — clavier de raccourcis (android/jni/sc2_barre.h), sur l'hôte :
 *   cc -std=gnu17 -Wall -Wextra -I android/jni -o /tmp/test_barre android/jni/tests/test_barre.c && /tmp/test_barre [dossier]
 * Écrit aussi barre-fold.ppm (écran intérieur, page commandes) et barre-lettres.ppm dans [dossier] (défaut .). */
#include <stdio.h>
#include <stdlib.h>
#include "sc2_barre.h"

static int echecs;
#define VERIF(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("ÉCHEC " __VA_ARGS__); echecs++; } printf("\n"); } while (0)

static void ppm(const char *dossier, const char *nom, const barre_geo *g, const barre_etat *e)
{
    uint8_t *px = malloc((size_t)g->barre.w * g->barre.h * 4);
    barre_dessiner(px, g, e, 0);
    char ch[512]; snprintf(ch, sizeof ch, "%s/%s", dossier, nom);
    FILE *f = fopen(ch, "wb"); fprintf(f, "P6\n%d %d\n255\n", g->barre.w, g->barre.h);
    for (size_t i = 0; i < (size_t)g->barre.w * g->barre.h; i++) fwrite(px + 4 * i, 1, 3, f);
    fclose(f); free(px);
}

int main(int argc, char **argv)
{
    const char *dossier = argc > 1 ? argv[1] : ".";
    /* écran intérieur du Fold, jeu 1280x1024 : image 2504x2003 en haut, barre 2504x253 en bas */
    barre_geo g = barre_disposer(1280, 1024, 2504, 2256, 1);
    VERIF(g.active && !g.verticale && g.jeu.y == 0 && g.jeu.h == 2003 && g.barre.y == 2003 && g.barre.h == 253,
          "Fold intérieur : image en haut 2504x%d, barre %dx%d en %d", g.jeu.h, g.barre.w, g.barre.h, g.barre.y);
    barre_geo c = barre_disposer(1280, 1024, 2504, 2256, 0);
    VERIF(!c.active && c.jeu.y == 126, "sans barre voulue : image centrée (y = %d)", c.jeu.y);
    /* écran extérieur 2520x1080, jeu 1280x720 : bande à droite */
    barre_geo v = barre_disposer(1280, 720, 2520, 1080, 1);
    VERIF(v.active && v.verticale && v.jeu.x == 0 && v.barre.x == 1920 && v.barre.w == 600,
          "écran extérieur : barre verticale %dx%d en x = %d", v.barre.w, v.barre.h, v.barre.x);
    /* écran au bon rapport : pas de barre (moins de 8 % libre) */
    barre_geo p = barre_disposer(1280, 720, 1920, 1080, 1);
    VERIF(!p.active, "16:9 sur 16:9 : pas de barre");

    /* détection : la touche « 3 » (rang 0, colonne 2) et « CTRL » (rang 1, colonne 0) */
    int32_t kw = g.barre.w / 13, kh = g.barre.h / 2;
    int i3 = barre_toucher(&g, 0, 2 * kw + kw / 2, g.barre.y + kh / 2), ic = barre_toucher(&g, 0, kw / 2, g.barre.y + kh + kh / 2);
    VERIF(i3 == 2 && ic == 16, "détection : « 3 » → %d, « CTRL » → %d", i3, ic);
    VERIF(barre_toucher(&g, 0, 100, 100) == -1 && !barre_dedans(&g, 100, 100), "toucher dans l'image : hors barre");

    /* Ctrl verrouillé puis « 3 » : Ctrl↓ 3↓ 3↑ Ctrl↑, Ctrl relâché ensuite */
    barre_etat e = { 0 }; uint16_t vk[8]; uint8_t bas[8];
    int n = barre_appuyer(&e, barre_def_de(0, ic), vk, bas, 8);
    VERIF(n == 0 && e.modifs == BM_CTRL, "Ctrl verrouillé (aucun événement)");
    ppm(dossier, "barre-ctrl.ppm", &g, &e);
    n = barre_appuyer(&e, barre_def_de(0, i3), vk, bas, 8);
    VERIF(n == 4 && vk[0] == 0x11 && bas[0] && vk[1] == '3' && bas[1] && vk[2] == '3' && !bas[2] && vk[3] == 0x11 && !bas[3]
          && e.modifs == 0, "Ctrl+3 : %d événements, Ctrl relâché", n);
    /* page lettres et retour */
    n = barre_appuyer(&e, barre_def_de(0, 1 * 16 + 12), vk, bas, 8);
    VERIF(n == 0 && e.page == 1, "ABC → page lettres");
    ppm(dossier, "barre-lettres.ppm", &g, &e);
    n = barre_appuyer(&e, barre_def_de(1, 2 * 16 + 9), vk, bas, 8);
    VERIF(e.page == 0, "123 → page commandes");
    ppm(dossier, "barre-fold.ppm", &g, &e);
    ppm(dossier, "barre-exterieur.ppm", &v, &e);
    printf("%s (%d échec(s))\n", echecs ? "ÉCHEC" : "TOUT OK", echecs);
    return echecs != 0;
}
