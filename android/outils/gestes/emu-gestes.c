/* emu-gestes.c (21/10) — injecteur de gestes MULTIPOINTS pour l'émulateur (root), protocole B, sans le coût d'un
 * processus sendevent par événement (≈ 100 ms chacun : un 2e doigt arrivait 550 ms après le premier).
 *   emu-gestes <périph> <largeur> <hauteur> tape x y | deux x1 y1 x2 y2 | glisse x1 y1 x2 y2
 * Coordonnées en pixels d'écran, converties vers 0..32767. Compilé statique avec le NDK (android/outils/gestes/). */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fd, L, H;
static void ev(int type, int code, int val)
{
    struct input_event e; memset(&e, 0, sizeof e);
    e.type = (unsigned short)type; e.code = (unsigned short)code; e.value = val;
    if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) perror("write");
}
static void syn(void) { ev(EV_SYN, SYN_REPORT, 0); }
static void dodo(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static void doigt(int slot, int id, int x, int y)
{
    ev(EV_ABS, ABS_MT_SLOT, slot); ev(EV_ABS, ABS_MT_TRACKING_ID, id);
    ev(EV_ABS, ABS_MT_POSITION_X, x * 32767 / L); ev(EV_ABS, ABS_MT_POSITION_Y, y * 32767 / H);
    ev(EV_ABS, ABS_MT_PRESSURE, 512); ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 6);
}
static void bouge(int slot, int x, int y)
{
    ev(EV_ABS, ABS_MT_SLOT, slot); ev(EV_ABS, ABS_MT_POSITION_X, x * 32767 / L); ev(EV_ABS, ABS_MT_POSITION_Y, y * 32767 / H);
}
static void leve(int slot) { ev(EV_ABS, ABS_MT_SLOT, slot); ev(EV_ABS, ABS_MT_TRACKING_ID, -1); }

int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage : emu-gestes <périph> <L> <H> tape x y | deux x1 y1 x2 y2 | glisse x1 y1 x2 y2\n"); return 2; }
    fd = open(argv[1], O_WRONLY); if (fd < 0) { perror(argv[1]); return 1; }
    L = atoi(argv[2]); H = atoi(argv[3]);
    const char *g = argv[4]; int a[4] = { 0 };
    for (int i = 0; i < 4 && 5 + i < argc; i++) a[i] = atoi(argv[5 + i]);
    static int id = 300; id += (int)(time(NULL) % 1000);
    if (!strcmp(g, "tape")) {
        doigt(0, id, a[0], a[1]); ev(EV_KEY, BTN_TOUCH, 1); syn(); dodo(60);
        leve(0); ev(EV_KEY, BTN_TOUCH, 0); syn();
    } else if (!strcmp(g, "deux")) {
        doigt(0, id, a[0], a[1]); ev(EV_KEY, BTN_TOUCH, 1); syn(); dodo(30);
        doigt(1, id + 1, a[2], a[3]); syn(); dodo(80);
        leve(1); syn(); dodo(10); leve(0); ev(EV_KEY, BTN_TOUCH, 0); syn();
    } else if (!strcmp(g, "glisse")) {
        doigt(0, id, a[0], a[1]); ev(EV_KEY, BTN_TOUCH, 1); syn();
        for (int i = 1; i <= 10; i++) { dodo(25); bouge(0, a[0] + (a[2] - a[0]) * i / 10, a[1] + (a[3] - a[1]) * i / 10); syn(); }
        dodo(30); leve(0); ev(EV_KEY, BTN_TOUCH, 0); syn();
    } else { fprintf(stderr, "geste inconnu : %s\n", g); return 2; }
    close(fd);
    return 0;
}
