/* test_horloge_9j.c — passe 9j : horloge de GetMessageTime / GetTickCount / MSG.time (runtime/sc2_horloge.h).
 *   cc -O1 -Wall -Wextra -I runtime tests/test_horloge_9j.c -o /tmp/t && /tmp/t        (sans CMake)
 * Vérifie : valeur non nulle, croissante (jamais en arrière sur 100 000 appels), avance de ~50 ms sur un sommeil de
 * 50 ms, et égale à CLOCK_MONOTONIC en ms (à 2 ms près). Code de sortie 0 : conforme. */
#include "sc2_horloge.h"

#include <stdio.h>
#include <time.h>

static int g_ok, g_ko;
#define CHECK(cond, msg) do { if (cond) { g_ok++; printf("PASS  %s\n", msg); } else { g_ko++; printf("ÉCHEC %s\n", msg); } } while (0)

int main(void)
{
    uint32_t t0 = sc2_tick_ms();
    CHECK(t0 != 0, "valeur non nulle");
    uint32_t p = t0; int recul = 0;
    for (int i = 0; i < 100000; i++) { uint32_t v = sc2_tick_ms(); if ((int32_t)(v - p) < 0) recul++; p = v; }
    CHECK(!recul, "croissante sur 100 000 appels (aucun recul)");
    struct timespec d = { 0, 50 * 1000000 }; nanosleep(&d, NULL);
    uint32_t t1 = sc2_tick_ms(), dt = t1 - t0;
    CHECK(dt >= 50 && dt < 1000, "avance après un sommeil de 50 ms");
    struct timespec m; clock_gettime(CLOCK_MONOTONIC, &m);
    uint32_t ref = (uint32_t)((uint64_t)m.tv_sec * 1000u + (uint64_t)m.tv_nsec / 1000000u), v = sc2_tick_ms();
    CHECK(v - ref <= 2, "égale à CLOCK_MONOTONIC en ms");
    printf("t0 %u ms, t1 %u ms, écart %u ms\n%s (%d/%d)\n", t0, t1, dt, g_ko ? "ÉCHEC" : "PASS", g_ok, g_ok + g_ko);
    return g_ko != 0;
}
