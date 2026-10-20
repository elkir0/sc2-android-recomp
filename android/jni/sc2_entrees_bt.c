/* sc2_entrees_bt.c — passe 8h : souris et clavier Android → messages Win32 (cf. sc2_entrees_bt.h).
 * Appelé par le fil principal (boucle d'événements) seulement : état sans verrou. */
#include <stdio.h>

#include "sc2_entrees_bt.h"

void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* AKEYCODE_* (android/keycodes.h) → VK Win32. Codes recopiés : le module se compile sans le NDK. */
static const struct { int16_t kc; uint8_t vk; } TABLE[] = {
    { 7, 0x30 }, { 8, 0x31 }, { 9, 0x32 }, { 10, 0x33 }, { 11, 0x34 }, { 12, 0x35 }, { 13, 0x36 }, { 14, 0x37 },
    { 15, 0x38 }, { 16, 0x39 },                                                         /* 0–9 */
    { 19, 0x26 }, { 20, 0x28 }, { 21, 0x25 }, { 22, 0x27 },                             /* flèches (DPAD) */
    { 55, 0xBC }, { 56, 0xBE },                                                         /* , . */
    { 57, 0xA4 }, { 58, 0xA5 }, { 59, 0xA0 }, { 60, 0xA1 },                             /* Alt G/D, Maj G/D */
    { 61, 0x09 }, { 62, 0x20 }, { 66, 0x0D }, { 67, 0x08 },                             /* Tab, Espace, Entrée, Retour arr. */
    { 68, 0xC0 }, { 69, 0xBD }, { 70, 0xBB }, { 71, 0xDB }, { 72, 0xDD }, { 73, 0xDC }, /* ` - = [ ] \ */
    { 74, 0xBA }, { 75, 0xDE }, { 76, 0xBF },                                           /* ; ' / */
    { 82, 0x5D }, { 92, 0x21 }, { 93, 0x22 },                                           /* Menu, Pg préc./suiv. */
    { 111, 0x1B }, { 112, 0x2E }, { 113, 0xA2 }, { 114, 0xA3 }, { 115, 0x14 }, { 116, 0x91 },   /* Échap, Suppr, Ctrl G/D, Verr. maj/défil */
    { 117, 0x5B }, { 118, 0x5C }, { 120, 0x2C }, { 121, 0x13 }, { 122, 0x24 }, { 123, 0x23 }, { 124, 0x2D },
    { 143, 0x90 }, { 154, 0x6F }, { 155, 0x6A }, { 156, 0x6D }, { 157, 0x6B }, { 158, 0x6E }, { 160, 0x0D },
};

uint32_t sc2bt_vk(int32_t kc)
{
    if (kc >= 29 && kc <= 54) return 0x41 + (uint32_t)(kc - 29);                          /* A–Z */
    if (kc >= 131 && kc <= 142) return 0x70 + (uint32_t)(kc - 131);                        /* F1–F12 */
    if (kc >= 144 && kc <= 153) return 0x60 + (uint32_t)(kc - 144);                        /* pavé 0–9 */
    for (unsigned k = 0; k < sizeof TABLE / sizeof TABLE[0]; k++) if (TABLE[k].kc == kc) return TABLE[k].vk;
    return 0;
}

static struct {
    int32_t boutons;                                 /* dernier buttonState vu */
    float cumul_v, cumul_h;                          /* crans fractionnaires en attente (unités Win32) */
    uint64_t cles, cles_inconnues, repetitions, deplacements, survols, appuis, molettes;
    unsigned journal;                                /* premières lignes journalisées (preuve sur appareil) */
} B;

int sc2bt_cle(int32_t kc, int bas, int32_t rep)
{
    uint32_t vk = sc2bt_vk(kc);
    if (!vk) {
        if (B.cles_inconnues++ < 8) sc2_log("[CLAVIER] keycode Android %d non traduit : laissé au système", kc);
        return 0;
    }
    if (bas && rep > 0) B.repetitions++; else B.cles++;
    if (B.journal < 60 && !(bas && rep > 0)) { B.journal++; sc2_log("[CLAVIER] keycode %d %s → VK 0x%02X", kc, bas ? "enfoncée" : "relâchée", vk); }
    sc2a_cle(vk, bas);
    return 1;
}

static int32_t crans(float *cumul, float axe)
{
    *cumul += axe * 120.0f;
    int32_t d = (int32_t)*cumul;                     /* tronqué vers 0 : le reste attend le cran suivant */
    *cumul -= (float)d;
    return d;
}

void sc2bt_souris(int32_t action, int32_t boutons, int32_t x, int32_t y, float vs, float hs)
{
    if (action == SC2BT_SCROLL) {
        int32_t dv = crans(&B.cumul_v, vs), dh = crans(&B.cumul_h, hs);
        if (dv) { sc2a_molette(dv, 0, x, y); B.molettes++; }
        if (dh) { sc2a_molette(dh, 1, x, y); B.molettes++; }
        if ((dv || dh) && B.journal < 60) { B.journal++; sc2_log("[SOURIS] molette (%.2f, %.2f) → delta %d / %d en (%d, %d)", vs, hs, dv, dh, x, y); }
        return;
    }
    if (action == SC2BT_CANCEL) boutons = 0;          /* geste annulé : tout relâcher */
    /* pavé tactile / souris sans état de boutons (appui « tap ») : DOWN sans bouton = primaire, UP = tout relâché */
    if (action == SC2BT_DOWN && !(boutons & 7)) boutons |= SC2BT_B_PRIMAIRE;
    if (action == SC2BT_UP && (boutons & 7) == (B.boutons & 7) && B.boutons) boutons &= ~7;
    if (action == SC2BT_HOVER_MOVE || action == SC2BT_HOVER_ENTER) B.survols++;
    /* boutons : chaque changement d'état, dans l'ordre gauche, droit, milieu ; la position exacte d'abord */
    static const int32_t masque[3] = { SC2BT_B_PRIMAIRE, SC2BT_B_SECONDAIRE, SC2BT_B_TERTIAIRE };
    static const int bouton[3] = { 0, 2, 1 };
    int32_t chg = (boutons ^ B.boutons) & (SC2BT_B_PRIMAIRE | SC2BT_B_SECONDAIRE | SC2BT_B_TERTIAIRE);
    for (int k = 0; k < 3; k++) if (chg & masque[k]) {
        int bas = (boutons & masque[k]) != 0;
        sc2a_bouton(bouton[k], bas, x, y);
        if (bas) B.appuis++;
        if (B.journal < 60) { B.journal++; sc2_log("[SOURIS] bouton %s %s en (%d, %d)", k == 0 ? "gauche" : k == 1 ? "droit" : "milieu",
                                                   bas ? "enfoncé" : "relâché", x, y); }
    }
    B.boutons = boutons & (SC2BT_B_PRIMAIRE | SC2BT_B_SECONDAIRE | SC2BT_B_TERTIAIRE);
    /* déplacements : portés par MOVE / HOVER seulement (DOWN, UP, BUTTON_* portent leur position avec le bouton) */
    if (!chg && (action == SC2BT_MOVE || action == SC2BT_HOVER_MOVE || action == SC2BT_HOVER_ENTER)) { sc2a_deplacer(x, y); B.deplacements++; }
}

void sc2bt_bilan(char *o, size_t cap)
{
    snprintf(o, cap, "souris/clavier (passe 8h) : touches %llu (+ %llu répétitions, %llu keycodes non traduits), appuis de "
             "boutons %llu, crans de molette %llu, déplacements %llu dont survols %llu",
             (unsigned long long)B.cles, (unsigned long long)B.repetitions, (unsigned long long)B.cles_inconnues,
             (unsigned long long)B.appuis, (unsigned long long)B.molettes, (unsigned long long)B.deplacements,
             (unsigned long long)B.survols);
}
