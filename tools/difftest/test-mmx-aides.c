/* test-mmx-aides.c — passe 9r : rejoue les vecteurs de difftest-mmx-aides.py (Unicorn) avec les aides MMX de
 * runtime/cpu.h et compare. cc -O1 -Wall -I runtime tools/difftest/test-mmx-aides.c -o /tmp/t && /tmp/t vecteurs.txt */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
uintptr_t g_hote_delta;
#include "cpu.h"
#define B(n) { #n, mmx_##n, NULL }
#define S(n) { #n, NULL, mmx_##n }
static const struct { const char *nom; uint64_t (*bin)(uint64_t, uint64_t); uint64_t (*dec)(uint64_t, uint64_t); } T[] = {
    B(paddb), B(paddw), B(paddd), B(paddq), B(psubb), B(psubw), B(psubd), B(psubq), B(paddsb), B(paddsw), B(paddusb),
    B(paddusw), B(psubsb), B(psubsw), B(psubusb), B(psubusw), B(pmullw), B(pmulhw), B(pmulhuw), B(pmaddwd), B(pmuludq),
    B(pmaxsw), B(pminsw), B(pmaxub), B(pminub), B(pavgb), B(pavgw), B(psadbw), B(pcmpeqb), B(pcmpeqw), B(pcmpeqd),
    B(pcmpgtb), B(pcmpgtw), B(pcmpgtd), B(punpcklbw), B(punpcklwd), B(punpckldq), B(punpckhbw), B(punpckhwd),
    B(punpckhdq), B(packsswb), B(packuswb), B(packssdw), B(pand), B(pandn), B(por), B(pxor),
    S(psllw), S(pslld), S(psllq), S(psrlw), S(psrld), S(psrlq), S(psraw), S(psrad),
};
int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "vecteurs.txt", "r");
    if (!f) { perror("vecteurs"); return 2; }
    char nom[32]; uint64_t a, b, att; unsigned imm; int n = 0, ko = 0, par_ko[64] = { 0 }, par_n[64] = { 0 };
    while (fscanf(f, "%31s %" SCNx64 " %" SCNx64 " %x %" SCNx64, nom, &a, &b, &imm, &att) == 5) {
        uint64_t r; int k = -1;
        if (!strcmp(nom, "pshufw")) r = mmx_pshufw(b, imm), k = 63;
        else {
            for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(T[i].nom, nom)) k = (int)i;
            if (k < 0) { printf("inconnu : %s\n", nom); return 2; }
            r = T[k].bin ? T[k].bin(a, b) : T[k].dec(a, b);
        }
        n++; par_n[k]++;
        if (r != att) { ko++; if (par_ko[k]++ < 2) printf("ÉCART %-10s a=%016" PRIx64 " b=%016" PRIx64 " imm=%02x : C %016" PRIx64 " ≠ Unicorn %016" PRIx64 "\n", nom, a, b, imm, r, att); }
    }
    int ops_ko = 0; for (int i = 0; i < 64; i++) if (par_ko[i]) ops_ko++;
    printf("%s : %d vecteurs, %d écarts, %d opcode(s) en écart\n", ko ? "ÉCHEC" : "PASS", n, ko, ops_ko);
    return ko != 0;
}
