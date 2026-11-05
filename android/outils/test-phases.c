/* test-phases.c — test hôte de android/jni/sc2_phases.h (passe 7r), sur de VRAIS journaux du runtime.
 *
 *   cc -O2 -Wall -Wextra -I android/jni android/outils/test-phases.c -o /tmp/test-phases
 *   /tmp/test-phases                         tests unitaires seuls
 *   /tmp/test-phases <run.log> [SC2_PHASES]  + rejoue un journal : les lignes « FRAME n°N présentée » font
 *                                            les frames (k = N ; temps fictif = N / 10 s), les autres
 *                                            alimentent sc2_phases_ligne, comme fil_log sur l'appareil.
 * Sortie : les passages [PHASE] et le [BILAN] [FPS] tels que l'APK les écrit. */
#include <stdio.h>
#include <stdlib.h>
#include "sc2_phases.h"

static int g_ko;
#define VERIF(c, ...) do { if (!(c)) { g_ko++; printf("  ÉCHEC : "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void unitaires(void)
{
    sc2_phases p; char s[256];
    sc2_phases_init(&p, NULL);
    VERIF(p.n == 4 && !strcmp(p.nom[2], "menu") && !strcmp(p.nom[3], "chargement"), "défaut : %d phases", p.n);
    VERIF(!strcmp(p.motif[3], "\xe2\x86\x92 Maps\\"), "motif chargement « %s »", p.motif[3]);
    VERIF(sc2_phases_ligne(&p, "[MPQ] x → Assets\\Textures\\ui_camapaignmenu_icon.dds", 1, 0) == 2,
          "avant la 1re frame, une règle fait avancer (démarrage → menu)");
    VERIF(sc2_phases_frame(&p, 2.0, 1) == -1, "1re frame après menu : pas de recul vers titre");
    sc2_phases_init(&p, NULL);
    VERIF(sc2_phases_frame(&p, 2.0, 1) == 1 && p.courante == 1, "1re frame → titre");
    VERIF(sc2_phases_frame(&p, 2.1, 2) == -1, "2e frame : pas de changement");
    VERIF(sc2_phases_ligne(&p, "[FS] GetFileAttributesW(\"Maps/Campaign/TRaynor01.SC2Map\") → absent", 3, 5) == -1,
          "chemin à / : pas le motif chargement");
    VERIF(sc2_phases_ligne(&p, "[MPQ] Mods\\Core.SC2Mod\\Base.SC2Assets → Assets\\Textures\\ui_camapaignmenu_frame_backdrop.dds"
                           " (4921216 o) — 1re lecture", 12.0, 101) == 2, "texture du menu → menu");
    sc2_phases_texte_passage(&p, s, sizeof s);
    VERIF(strstr(s, "[PHASE] menu à 12.0 s (frame affichée 102) ; titre : 101 images en 10.0 s = 10.1 images/s") != NULL,
          "passage : %s", s);
    VERIF(sc2_phases_ligne(&p, "ui_camapaignmenu encore", 13, 110) == -1, "pas de re-passage");
    VERIF(sc2_phases_ligne(&p, "[MPQ] Campaigns\\Liberty.SC2Campaign\\Base.SC2Maps → Maps\\Campaign\\TRaynor01.SC2Map\\"
                           "DocumentHeader (720 o) — 1re lecture", 22.0, 201) == 3, "carte → chargement");
    VERIF(sc2_phases_ligne(&p, "ui_camapaignmenu après", 23, 210) == -1, "pas de recul");
    char b[8][160]; int m = sc2_phases_bilan(&p, 32.0, 301, b, 8);
    VERIF(m == 4, "bilan : %d lignes", m);
    VERIF(m == 4 && strstr(b[3], "chargement :    100 images en    10.0 s =  10.0 images/s (de 22.0 s, en cours)"), "%s", b[3]);
    sc2_phases_init(&p, "intro=ScreenLoading;jeu=t3Terrain.xml;mauvaise;=x;longue=");
    VERIF(p.n == 4 && !strcmp(p.nom[3], "jeu") && !strcmp(p.motif[2], "ScreenLoading"), "SC2_PHASES : %d phases", p.n);
    sc2_phases_init(&p, NULL);   /* saut de phase : titre → chargement directement (mode -run) */
    sc2_phases_frame(&p, 1, 1);
    VERIF(sc2_phases_ligne(&p, "Base.SC2Maps → Maps\\Campaign\\X", 2, 3) == 3, "saut titre → chargement");
    sc2_phases_texte_passage(&p, s, sizeof s);
    VERIF(strstr(s, "; titre : 3 images") != NULL, "passage après saut : %s", s);
}

int main(int argc, char **argv)
{
    unitaires();
    printf("tests unitaires : %s (%d échec(s))\n", g_ko ? "ÉCHEC" : "OK", g_ko);
    if (argc < 2) return g_ko != 0;
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }
    sc2_phases p; sc2_phases_init(&p, argc > 2 ? argv[2] : getenv("SC2_PHASES"));
    char l[8192], s[256]; unsigned k = 0; long nl = 0;
    while (fgets(l, sizeof l, f)) {
        nl++;
        const char *q = strstr(l, "FRAME n\xc2\xb0");
        if (q && strstr(l, "présentée")) {
            k = (unsigned)strtoul(q + strlen("FRAME n\xc2\xb0"), NULL, 10);
            if (sc2_phases_frame(&p, k / 10.0, k) > 0) { sc2_phases_texte_passage(&p, s, sizeof s); printf("l.%-6ld %s\n", nl, s); }
            continue;
        }
        if (sc2_phases_ligne(&p, l, k / 10.0, k) > 0) {
            sc2_phases_texte_passage(&p, s, sizeof s);
            l[strcspn(l, "\n")] = 0;
            printf("l.%-6ld %s\n         ← %.150s\n", nl, s, l);
        }
    }
    fclose(f);
    char b[SC2_PHASES_MAX + 2][160]; int m = sc2_phases_bilan(&p, k / 10.0, k, b, SC2_PHASES_MAX + 2);
    for (int i = 0; i < m; i++) printf("%s\n", b[i]);
    return g_ko != 0;
}
