/* sc2_horloge.h — passe 9j : horloge des temps Win32 (ms, CLOCK_MONOTONIC tronqué à 32 bits comme GetTickCount),
 * partagée par GetTickCount / timeGetTime (sc2_iat.c), GetMessageTime (sc2_iat_p4.c, sc2_input_sim.c) et le champ
 * MSG.time (sc2_input_sim.c). En-tête seul : testable sur l'hôte (tests/test_horloge_9j.c), aucun fichier de build. */
#ifndef SC2_HORLOGE_H
#define SC2_HORLOGE_H
#include <stdint.h>
#include <time.h>
static inline uint32_t sc2_tick_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}
#endif
