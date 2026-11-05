/* test_clavier_mac.c — clavier macOS → VK et caractères Win32 (mac/sc2_clavier_mac.c), sur l'hôte (V3, lot M2, 22/10).
 *   cc -Wall -Wextra -I mac mac/tests/test_clavier_mac.c mac/sc2_clavier_mac.c -o /tmp/t && /tmp/t
 * (ou ctest dans build-tests : tests-hote/v3/clavier_souris.cmake). Code de sortie 0 : tout conforme.
 * Dispositions SIMULÉES (caractère de charactersByApplyingModifiers:0 donné à la main, d'après les dispositions
 * « U.S. », « Français » et « Russe » de macOS) : la séance AZERTY physique du mainteneur reste à faire (v3-critique §3.6).
 *   A. QWERTY : les 26 lettres par position, la ponctuation US ;
 *   B. AZERTY : Q/A, W/Z, M (« , » → 0xBC), « ; » « : » « = » « ù », rangée des chiffres en 0x31..0x30, ISO 0x0A/0x32 ;
 *   C. cyrillique : repli par position US ;
 *   D. table fixe : F1-F20 (F10 → 0x79), flèches, pavé (opérateurs du pavé ≠ ponctuation), Entrée du pavé, ignorées ;
 *   E. flagsChanged : gauche et droite par les bits de côté, Cmd → Ctrl, options, Verr. maj, NSEvent synthétique ;
 *   F. lecture des options ; G. caractères (Ctrl+lettre sans texte, Option, normalisations, UTF-16) ;
 *   H. VK mémorisé à l'appui ; I. type de clavier (ISO, table de KBGetLayoutType). */
#include <stdio.h>
#include <string.h>

#include "sc2_clavier_mac.h"

static int g_ko, g_n;
#define VERIF(c, ...) do { g_n++; if (!(c)) { g_ko++; if (g_ko < 40) { printf("  ÉCHEC : "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void vk(const char *nom, uint16_t kvk, uint32_t c, int iso, uint32_t attendu)
{
    uint32_t v = sc2m_vk_de_kvk(kvk, c, iso);
    VERIF(v == attendu, "%s : kVK 0x%02X car U+%04X iso %d → VK 0x%02X, attendu 0x%02X", nom, kvk, c, iso, v, attendu);
}

static void modif(const char *nom, uint16_t kvk, uint64_t d, const sc2m_clavier_options *o, uint32_t attendu, int bas_att)
{
    int bas = -1;
    uint32_t v = sc2m_vk_modificateur(kvk, d, o, &bas);
    VERIF(v == attendu && bas == bas_att, "%s : kVK 0x%02X drapeaux 0x%llX → VK 0x%02X bas %d, attendu 0x%02X bas %d",
          nom, kvk, (unsigned long long)d, v, bas, attendu, bas_att);
}

#define U16(...) (const uint16_t[]){ __VA_ARGS__ }, sizeof((const uint16_t[]){ __VA_ARGS__ }) / sizeof(uint16_t)
#define U32(...) (const uint32_t[]){ __VA_ARGS__ }, (int)(sizeof((const uint32_t[]){ __VA_ARGS__ }) / sizeof(uint32_t))
#define RIEN NULL, 0

static void cars(const char *nom, uint32_t v, uint64_t d, const sc2m_clavier_options *o, const uint16_t *c, size_t nc,
                 const uint16_t *s, size_t ns, const uint32_t *att, int na)
{
    uint32_t out[8] = { 0 };
    int n = sc2m_caracteres(v, d, o, c, nc, s, ns, out, 8);
    int ok = n == na;
    for (int i = 0; ok && i < n; i++) ok = out[i] == att[i];
    VERIF(ok, "%s : %d caractère(s) [U+%04X U+%04X …], attendu %d [U+%04X …]", nom, n, n > 0 ? out[0] : 0,
          n > 1 ? out[1] : 0, na, na > 0 ? att[0] : 0);
}

static const uint16_t KVK_LETTRE[26] = {   /* kVK_ANSI_A..Z (Events.h) */
    0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
    0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06 };

int main(void)
{
    const sc2m_clavier_options defaut = SC2M_CLAVIER_OPTIONS_DEFAUT;

    /* ---- A. QWERTY (U.S.) ---- */
    for (int i = 0; i < 26; i++) {
        vk("QWERTY lettre", KVK_LETTRE[i], 'a' + i, 0, 0x41 + i);
        vk("QWERTY lettre (Maj gardée)", KVK_LETTRE[i], 'A' + i, 0, 0x41 + i);
        vk("lettre sans caractère (repli position)", KVK_LETTRE[i], 0, 0, 0x41 + i);
    }
    vk("QWERTY ,", 0x2B, ',', 0, 0xBC); vk("QWERTY .", 0x2F, '.', 0, 0xBE); vk("QWERTY /", 0x2C, '/', 0, 0xBF);
    vk("QWERTY ;", 0x29, ';', 0, 0xBA); vk("QWERTY '", 0x27, '\'', 0, 0xDE); vk("QWERTY [", 0x21, '[', 0, 0xDB);
    vk("QWERTY ]", 0x1E, ']', 0, 0xDD); vk("QWERTY \\", 0x2A, '\\', 0, 0xDC); vk("QWERTY -", 0x1B, '-', 0, 0xBD);
    vk("QWERTY =", 0x18, '=', 0, 0xBB); vk("QWERTY `", 0x32, '`', 0, 0xC0);

    /* ---- B. AZERTY (Français, ISO) ---- */
    vk("AZERTY position Q", 0x0C, 'a', 1, 0x41);
    vk("AZERTY position A", 0x00, 'q', 1, 0x51);
    vk("AZERTY position W", 0x0D, 'z', 1, 0x5A);
    vk("AZERTY position Z", 0x06, 'w', 1, 0x57);
    vk("AZERTY position ; (m)", 0x29, 'm', 1, 0x4D);
    vk("AZERTY kVK_ANSI_M (,)", 0x2E, ',', 1, 0xBC);
    vk("AZERTY position , (;)", 0x2B, ';', 1, 0xBA);
    vk("AZERTY position . (:, repli)", 0x2F, ':', 1, 0xBE);
    vk("AZERTY position / (=)", 0x2C, '=', 1, 0xBB);
    vk("AZERTY position ' (ù, repli)", 0x27, 0xF9, 1, 0xDE);
    vk("AZERTY position [ (^ morte, repli)", 0x21, 0, 1, 0xDB);
    vk("AZERTY position ] ($)", 0x1E, '$', 1, 0xDD);
    {   /* rangée des chiffres : & é " ' ( § è ! ç à → VK_1..VK_9, VK_0 (la position l'emporte sur « ' ») */
        static const uint16_t k[10] = { 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19, 0x1D };
        static const uint32_t c[10] = { '&', 0xE9, '"', '\'', '(', 0xA7, 0xE8, '!', 0xE7, 0xE0 };
        for (int i = 0; i < 10; i++) {
            vk("AZERTY rangée des chiffres", k[i], c[i], 1, i < 9 ? 0x31 + i : 0x30);
            vk("QWERTY rangée des chiffres", k[i], i < 9 ? '1' + i : '0', 0, i < 9 ? 0x31 + i : 0x30);
        }
    }
    vk("ISO 0x0A (@, à gauche du 1)", 0x0A, '@', 1, 0xC0);
    vk("ISO 0x0A sans caractère", 0x0A, 0, 1, 0xC0);
    vk("ISO 0x32 (<, à côté de Maj)", 0x32, '<', 1, 0xE2);
    vk("ISO 0x32 sans caractère", 0x32, 0, 1, 0xE2);
    vk("ANSI 0x32 sans caractère", 0x32, 0, 0, 0xC0);
    vk("ANSI 0x0A sans caractère", 0x0A, 0, 0, 0xE2);
    vk("ISO 0x0A (§, britannique)", 0x0A, 0xA7, 1, 0xC0);

    /* ---- C. cyrillique (Russe) : repli par position US ---- */
    vk("cyrillique й (Q)", 0x0C, 0x439, 0, 0x51); vk("cyrillique ф (A)", 0x00, 0x444, 0, 0x41);
    vk("cyrillique ь (M)", 0x2E, 0x44C, 0, 0x4D); vk("cyrillique б (,)", 0x2B, 0x431, 0, 0xBC);
    vk("cyrillique ю (.)", 0x2F, 0x44E, 0, 0xBE); vk("cyrillique ж (;)", 0x29, 0x436, 0, 0xBA);
    vk("cyrillique 1", 0x12, '1', 0, 0x31);

    /* ---- D. table fixe ---- */
    {
        static const uint16_t f[20] = { 0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F,
                                        0x69, 0x6B, 0x71, 0x6A, 0x40, 0x4F, 0x50, 0x5A };
        for (int i = 0; i < 20; i++) vk("F-touche", f[i], 0xF704 + i, 0, 0x70 + i);
    }
    vk("F10", 0x6D, 0xF70D, 0, 0x79);
    vk("←", 0x7B, 0xF702, 0, 0x25); vk("→", 0x7C, 0xF703, 0, 0x27); vk("↓", 0x7D, 0xF701, 0, 0x28); vk("↑", 0x7E, 0xF700, 0, 0x26);
    vk("Entrée", 0x24, '\r', 0, 0x0D); vk("Entrée du pavé", 0x4C, 0x03, 0, 0x0D); vk("Tab", 0x30, '\t', 0, 0x09);
    vk("Espace", 0x31, ' ', 0, 0x20); vk("Retour arrière", 0x33, 0x7F, 0, 0x08); vk("Suppr", 0x75, 0xF728, 0, 0x2E);
    vk("Échap", 0x35, 0x1B, 0, 0x1B); vk("Help", 0x72, 0xF746, 0, 0x2D);
    vk("Début", 0x73, 0xF729, 0, 0x24); vk("Fin", 0x77, 0xF72B, 0, 0x23);
    vk("Pg préc.", 0x74, 0xF72C, 0, 0x21); vk("Pg suiv.", 0x79, 0xF72D, 0, 0x22);
    {
        static const uint16_t p[10] = { 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C };
        for (int i = 0; i < 10; i++) vk("pavé chiffre", p[i], '0' + i, 0, 0x60 + i);
    }
    vk("pavé . (pas 0xBE)", 0x41, '.', 0, 0x6E); vk("pavé *", 0x43, '*', 0, 0x6A); vk("pavé +", 0x45, '+', 0, 0x6B);
    vk("pavé - (pas 0xBD)", 0x4E, '-', 0, 0x6D); vk("pavé / (pas 0xBF)", 0x4B, '/', 0, 0x6F);
    vk("pavé = (rien, pas 0xBB)", 0x51, '=', 0, 0); vk("pavé Clear", 0x47, 0xF739, 0, 0x0C);
    vk("menu contextuel", 0x6E, 0, 0, 0x5D);
    for (uint16_t k = 0x36; k <= 0x3F; k++) vk("modificateur en keyDown", k, 0, 0, 0);
    vk("volume +", 0x48, 0, 0, 0); vk("sourdine", 0x4A, 0, 0, 0); vk("JIS Yen", 0x5D, 0xA5, 0, 0); vk("JIS Kana", 0x68, 0, 0, 0);
    vk("kVK inconnu sans caractère", 0x34, 0, 0, 0);
    vk("kVK hors table, lettre", 200, 'a', 0, 0x41); vk("kVK hors table, rien", 200, 0, 0, 0);
    for (int iso = 0; iso <= 1; iso++)                 /* aucun VK hors plage, aucun marqueur interne qui fuit */
        for (uint16_t k = 0; k < 128; k++) {
            uint32_t v = sc2m_vk_de_kvk(k, 0, iso);
            VERIF(v < 0xFF, "kVK 0x%02X iso %d → VK 0x%X hors plage", k, iso, v);
        }

    /* ---- E. flagsChanged ---- */
    modif("Maj gauche enfoncée", 0x38, SC2M_DRAP_MAJ | SC2M_DEV_LMAJ, NULL, 0xA0, 1);
    modif("Maj gauche relâchée", 0x38, 0, NULL, 0xA0, 0);
    modif("Maj droite enfoncée", 0x3C, SC2M_DRAP_MAJ | SC2M_DEV_RMAJ, NULL, 0xA1, 1);
    modif("Maj droite pendant la gauche", 0x3C, SC2M_DRAP_MAJ | SC2M_DEV_LMAJ | SC2M_DEV_RMAJ, NULL, 0xA1, 1);
    modif("Maj droite relâchée, gauche tenue", 0x3C, SC2M_DRAP_MAJ | SC2M_DEV_LMAJ, NULL, 0xA1, 0);
    modif("Cmd gauche → VK_LCONTROL", 0x37, SC2M_DRAP_CMD | SC2M_DEV_LCMD, NULL, 0xA2, 1);
    modif("Cmd droit → VK_RCONTROL", 0x36, SC2M_DRAP_CMD | SC2M_DEV_RCMD, &defaut, 0xA3, 1);
    modif("Cmd droit relâché, gauche tenu", 0x36, SC2M_DRAP_CMD | SC2M_DEV_LCMD, NULL, 0xA3, 0);
    modif("Option gauche → VK_LMENU", 0x3A, SC2M_DRAP_OPTION | SC2M_DEV_LOPTION, NULL, 0xA4, 1);
    modif("Option droite → VK_RMENU", 0x3D, SC2M_DRAP_OPTION | SC2M_DEV_ROPTION, NULL, 0xA5, 1);
    modif("Option droite relâchée", 0x3D, 0, NULL, 0xA5, 0);
    modif("Ctrl physique (clic) : non transmis", 0x3B, SC2M_DRAP_CTRL | SC2M_DEV_LCTRL, NULL, 0, 1);
    modif("Ctrl droit physique (clic)", 0x3E, SC2M_DRAP_CTRL | SC2M_DEV_RCTRL, NULL, 0, 1);
    {
        sc2m_clavier_options o = defaut; o.ctrl_transmis = 1;
        modif("Ctrl physique transmis, gauche", 0x3B, SC2M_DRAP_CTRL | SC2M_DEV_LCTRL, &o, 0xA2, 1);
        modif("Ctrl physique transmis, droit", 0x3E, SC2M_DRAP_CTRL | SC2M_DEV_RCTRL, &o, 0xA3, 1);
        modif("Ctrl droit relâché, gauche tenu", 0x3E, SC2M_DRAP_CTRL | SC2M_DEV_LCTRL, &o, 0xA3, 0);
        o = defaut; o.cmd_vers_ctrl = 0;
        modif("Cmd gauche → VK_LWIN (cmd)", 0x37, SC2M_DRAP_CMD | SC2M_DEV_LCMD, &o, 0x5B, 1);
        modif("Cmd droit → VK_RWIN (cmd)", 0x36, SC2M_DRAP_CMD | SC2M_DEV_RCMD, &o, 0x5C, 1);
        o = defaut; o.option_altgr = 1;
        modif("Option gauche (altgr) : non transmise", 0x3A, SC2M_DRAP_OPTION | SC2M_DEV_LOPTION, &o, 0, 1);
        modif("Option droite (altgr) : non transmise", 0x3D, SC2M_DRAP_OPTION | SC2M_DEV_ROPTION, &o, 0, 1);
    }
    modif("Verr. maj allumé", 0x39, SC2M_DRAP_VERR_MAJ, NULL, 0x14, 1);
    modif("Verr. maj éteint", 0x39, 0, NULL, 0x14, 0);
    modif("fn ignorée", 0x3F, SC2M_DRAP_FN, NULL, 0, 0);
    modif("synthétique : Maj gauche sans bits de côté", 0x38, SC2M_DRAP_MAJ, NULL, 0xA0, 1);
    modif("synthétique : Cmd droit sans bits de côté", 0x36, SC2M_DRAP_CMD, NULL, 0xA3, 1);
    modif("synthétique : relâché", 0x3A, 0, NULL, 0xA4, 0);
    VERIF(sc2m_vk_modificateur(0x38, SC2M_DRAP_MAJ | SC2M_DEV_LMAJ, NULL, NULL) == 0xA0, "bas = NULL accepté");

    /* ---- F. options ---- */
    {
        sc2m_clavier_options o;
        int r = sc2m_clavier_options_lire(&o, NULL, NULL, NULL);
        VERIF(r == 0 && o.cmd_vers_ctrl == 1 && o.ctrl_transmis == 0 && o.option_altgr == 0, "options par défaut");
        r = sc2m_clavier_options_lire(&o, "cmd", "ctrl", "altgr");
        VERIF(r == 0 && o.cmd_vers_ctrl == 0 && o.ctrl_transmis == 1 && o.option_altgr == 1, "options cmd/ctrl/altgr");
        r = sc2m_clavier_options_lire(&o, "ctrl", "clic", "alt");
        VERIF(r == 0 && o.cmd_vers_ctrl == 1 && o.ctrl_transmis == 0 && o.option_altgr == 0, "options ctrl/clic/alt");
        r = sc2m_clavier_options_lire(&o, "x", "", "ALT");
        VERIF(r == 2 && o.cmd_vers_ctrl == 1 && o.ctrl_transmis == 0 && o.option_altgr == 0,
              "valeurs inconnues comptées (%d), défauts gardés", r);
    }

    /* ---- G. caractères ---- */
    const uint64_t CMD = SC2M_DRAP_CMD | SC2M_DEV_LCMD, CTRL = SC2M_DRAP_CTRL | SC2M_DEV_LCTRL;
    const uint64_t OPT = SC2M_DRAP_OPTION | SC2M_DEV_LOPTION, MAJ = SC2M_DRAP_MAJ | SC2M_DEV_LMAJ;
    cars("a", 0x41, 0, NULL, U16('a'), U16('a'), U32('a'));
    cars("Maj+a", 0x41, MAJ, NULL, U16('A'), U16('A'), U32('A'));
    cars("Cmd+a = Ctrl+A : 0x01, aucun texte", 0x41, CMD, NULL, U16('a'), U16('a'), U32(0x01));
    cars("Cmd+z = Ctrl+Z : 0x1A", 0x5A, CMD, NULL, U16('z'), U16('z'), U32(0x1A));
    cars("Cmd+Maj+a : 0x01", 0x41, CMD | MAJ, NULL, U16('A'), U16('A'), U32(0x01));
    cars("Cmd+1 : rien", 0x31, CMD, NULL, U16('1'), U16('1'), RIEN);
    cars("Cmd+, (AZERTY M) : rien", 0xBC, CMD, NULL, U16(','), U16(','), RIEN);
    cars("Cmd+Option+a : rien", 0x41, CMD | OPT, NULL, U16(0xE6), U16('a'), RIEN);
    cars("Ctrl physique+a (clic) : rien", 0x41, CTRL, NULL, U16(0x01), U16('a'), RIEN);
    cars("Ctrl physique+1 (clic) : rien", 0x31, CTRL, NULL, U16('1'), U16('1'), RIEN);
    {
        sc2m_clavier_options o = defaut; o.ctrl_transmis = 1;
        cars("Ctrl physique+a (transmis) : 0x01", 0x41, CTRL, &o, U16(0x01), U16('a'), U32(0x01));
        cars("Ctrl physique+1 (transmis) : rien", 0x31, CTRL, &o, U16('1'), U16('1'), RIEN);
        o = defaut; o.cmd_vers_ctrl = 0;
        cars("Cmd+a (touche Windows) : rien", 0x41, CMD, &o, U16('a'), U16('a'), RIEN);
        o = defaut; o.option_altgr = 1;
        cars("Option+( AZERTY (altgr) : {", 0x35, OPT, &o, U16('{'), U16('('), U32('{'));
        cars("Option+a (altgr) : æ", 0x41, OPT, &o, U16(0xE6), U16('a'), U32(0xE6));
    }
    cars("Option+a (alt) : a, en WM_SYSCHAR par le runtime", 0x41, OPT, NULL, U16(0xE6), U16('a'), U32('a'));
    cars("Option+( AZERTY (alt) : (", 0x35, OPT, NULL, U16('{'), U16('('), U32('('));
    cars("Option+Maj+a (alt) : A", 0x41, OPT | MAJ, NULL, U16(0xC6), U16('A'), U32('A'));
    cars("Retour arrière 0x7F → 0x08", 0x08, 0, NULL, U16(0x7F), U16(0x7F), U32(0x08));
    cars("Entrée du pavé 0x03 → 0x0D", 0x0D, 0, NULL, U16(0x03), U16(0x03), U32(0x0D));
    cars("Entrée", 0x0D, 0, NULL, U16('\r'), U16('\r'), U32(0x0D));
    cars("Maj+Tab 0x19 → 0x09", 0x09, MAJ, NULL, U16(0x19), U16(0x19), U32(0x09));
    cars("Tab", 0x09, 0, NULL, U16('\t'), U16('\t'), U32(0x09));
    cars("Échap", 0x1B, 0, NULL, U16(0x1B), U16(0x1B), U32(0x1B));
    cars("Espace", 0x20, 0, NULL, U16(' '), U16(' '), U32(' '));
    cars("flèche : rien", 0x25, 0, NULL, U16(0xF702), U16(0xF702), RIEN);
    cars("F10 : rien", 0x79, 0, NULL, U16(0xF70D), U16(0xF70D), RIEN);
    cars("Suppr : rien", 0x2E, 0, NULL, U16(0xF728), U16(0xF728), RIEN);
    cars("touche morte (^) : rien", 0xDB, 0, NULL, RIEN, U16('^'), RIEN);
    cars("ê composé", 0x45, 0, NULL, U16(0xEA), U16('e'), U32(0xEA));
    cars("é AZERTY (rangée des chiffres)", 0x32, 0, NULL, U16(0xE9), U16(0xE9), U32(0xE9));
    cars("U+1F600 (paire)", 0x41, 0, NULL, U16(0xD83D, 0xDE00), RIEN, U32(0x1F600));
    cars("demi-paire haute orpheline", 0x41, 0, NULL, U16(0xD83D), RIEN, RIEN);
    cars("demi-paire basse orpheline", 0x41, 0, NULL, U16(0xDE00, 'x'), RIEN, U32('x'));
    cars("deux caractères", 0x41, 0, NULL, U16('a', 'b'), RIEN, U32('a', 'b'));
    cars("contrôle sans Ctrl : écarté", 0x41, 0, NULL, U16(0x01, 0x85, 'k'), RIEN, U32('k'));
    {
        uint32_t out[2] = { 0, 0 };
        VERIF(sc2m_caracteres(0x41, 0, NULL, U16('a', 'b', 'c'), RIEN, out, 2) == 2 && out[0] == 'a' && out[1] == 'b',
              "capacité respectée");
        VERIF(sc2m_caracteres(0x41, 0, NULL, U16('a'), RIEN, out, 0) == 0, "capacité nulle");
        VERIF(sc2m_caracteres(0x41, 0, NULL, U16('a'), RIEN, NULL, 4) == 0, "sortie NULL");
    }

    /* ---- H. VK mémorisé à l'appui ---- */
    {
        sc2m_clavier_etat e;
        memset(&e, 0, sizeof e);
        VERIF(sc2m_clavier_appui(&e, 0x0C, 0x41) == 0x41 && sc2m_clavier_tenues(&e) == 1, "appui mémorisé");
        VERIF(sc2m_clavier_appui(&e, 0x0C, 0x51) == 0x41 && e.repetitions == 1, "répétition : VK de l'appui");
        VERIF(sc2m_clavier_relache(&e, 0x0C) == 0x41 && sc2m_clavier_tenues(&e) == 0, "relâché : VK de l'appui");
        VERIF(sc2m_clavier_relache(&e, 0x0C) == 0 && e.relaches_orphelins == 1, "second relâché : orphelin");
        /* AZERTY → QWERTY pendant l'appui de kVK_ANSI_M : le relâché rejoue 0xBC, pas 0x4D */
        sc2m_clavier_appui(&e, 0x2E, sc2m_vk_de_kvk(0x2E, ',', 1));
        VERIF(sc2m_vk_de_kvk(0x2E, 'm', 0) == 0x4D && sc2m_clavier_relache(&e, 0x2E) == 0xBC, "disposition changée tenue");
        VERIF(sc2m_clavier_appui(&e, 200, 0x41) == 0 && sc2m_clavier_appui(&e, 0x00, 0) == 0, "kVK hors table, VK nul");
        sc2m_clavier_appui(&e, 0x00, 0x41); sc2m_clavier_appui(&e, 0x01, 0x53);
        VERIF(sc2m_clavier_tenues(&e) == 2 && e.appuis == 4, "deux tenues (%d), %llu appuis", sc2m_clavier_tenues(&e),
              (unsigned long long)e.appuis);
        sc2m_clavier_reinitialiser(&e);
        VERIF(sc2m_clavier_tenues(&e) == 0 && sc2m_clavier_relache(&e, 0x00) == 0 && e.appuis == 4,
              "perte du focus : rien de tenu, compteurs gardés");
    }

    /* ---- I. type de clavier ---- */
    VERIF(sc2m_iso_de_type(0x49534F20u) == 1, "'ISO ' → 1");
    VERIF(sc2m_iso_de_type(0x414E5349u) == 0 && sc2m_iso_de_type(0x4A495320u) == 0 && sc2m_iso_de_type(0x3F3F3F3Fu) == 0,
          "'ANSI', 'JIS ', inconnu → 0");
#ifdef __APPLE__
    /* table de KBGetLayoutType (HIToolbox) : 41 = ISO, 40 = ANSI (sondé le 22/10) */
    VERIF(sc2m_iso_de_clavier(41) == 1 && sc2m_iso_de_clavier(40) == 0, "KBGetLayoutType(41) ISO, (40) ANSI : %d %d",
          sc2m_iso_de_clavier(41), sc2m_iso_de_clavier(40));
#else
    VERIF(sc2m_iso_de_clavier(41) == 0, "hors macOS : jamais ISO");
#endif
    int iso = sc2m_iso();
    VERIF(iso == 0 || iso == 1, "sc2m_iso() → %d", iso);
    printf("  (LMGetKbdType de ce processus, sans événement clavier : %s)\n", iso ? "ISO" : "ANSI, JIS ou inconnu");

    printf("RÉSULTAT test_clavier_mac : %d vérifications, %d échec(s)\n", g_n, g_ko);
    return g_ko != 0;
}
