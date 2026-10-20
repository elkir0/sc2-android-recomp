/* runner-difftest.c — exécute UNE fonction lifftée avec un état injecté, pour le
 * difftest par fonction (kit 3e pilier M4, 14/10/2026).
 * Entrée : argv[1] = fichier d'état binaire (DT01). Sortie : état final sur stdout.
 * La zone hôte couvre tout l'espace 32 bits (4 Go NORESERVE) : TOUTE VA est valide.
 * Aucun stub Win32 n'est exécuté ici : le harnais Python ne soumet que des
 * fonctions dont l'arbre d'appels reste dans .text (détection Unicorn amont). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include "cpu.h"
#include "sc2_host.h"

/* Table addr → fonction lifftée, générée depuis la liste canonique.
 * recomp_sc2_list.h définit la macro LIFTED_FUNCS(X) — on l'inclut pour la
 * définition, puis on l'INSTANCIE dans l'initialiseur (pattern X-macro). */
#include "recomp_sc2_list.h"
typedef void (*RecompFn)(CPU *);
/* prototypes des fonctions lifftées (définies dans recomp_*.c) */
#define X_PROTO(a) void L_##a(CPU *c);
LIFTED_FUNCS(X_PROTO)
#undef X_PROTO
typedef struct { uint32_t addr; RecompFn fn; } Entree;
#define X(a) { 0x##a, L_##a },
static Entree g_table[] = {
    LIFTED_FUNCS(X)
};
#undef X
#define N_TABLE (sizeof g_table / sizeof g_table[0])

/* Passe 7k : un plantage nomme la fonction liftée en cours (pile fantôme) et l'adresse
 * invitée fautive, sur stderr, au lieu d'un rc anonyme. */
static void sur_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    char b[160];
    sc2_shadow *ss = g_sc2_ss;
    uint32_t fn = (ss && ss->n > 0 && ss->n <= (int)(sizeof ss->va / sizeof ss->va[0])) ? ss->va[ss->n - 1] : 0;
    uintptr_t a = (uintptr_t)si->si_addr;
    uint32_t va = (a >= g_hote_delta && a - g_hote_delta < 0x100000000ull) ? (uint32_t)(a - g_hote_delta) : 0;
    int n = snprintf(b, sizeof b, "[SIGNAL] sig %d adresse invitée 0x%08X dans 0x%08X (profondeur %d)\n",
                     sig, va, fn, ss ? ss->n : -1);
    if (write(2, b, (size_t)n) < 0) { }
    _exit(128 + sig);
}

static void *lire_exacte(FILE *f, void *p, size_t n) {
    return fread(p, 1, n, f) == n ? p : NULL;
}
#define LIRE(f, champ) lire_exacte((f), &(champ), sizeof (champ))

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s etat.bin\n", argv[0]); return 2; }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror("fopen etat"); return 2; }

    uint32_t magic; if (!LIRE(in, magic) || magic != 0x31305444u /* "DT01" LE */) {
        fprintf(stderr, "magic invalide\n"); return 2; }

    /* Zone hôte 4 Go, delta = base (VA == offset dans la zone) */
    void *zone = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (zone == MAP_FAILED) { perror("mmap zone 4G"); return 2; }
    g_hote_delta = (uintptr_t)zone;   /* GPTR(va) = zone + va, tout l'espace 32 bits */

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sur_signal; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static char pile_sig[65536]; stack_t st = { .ss_sp = pile_sig, .ss_size = sizeof pile_sig };
    sigaltstack(&st, NULL);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL); sigaction(SIGILL, &sa, NULL);

    /* Passe 7k : SC2_DT_IMAGE = image de SC2.exe telle que la voit Unicorn (harnais v2) :
     * « DTI1 », n, puis n segments (va, len, octets) — ou l'image plate depuis 0x00800000 — en-têtes + octets des sections, IAT
     * remplie de sentinelles ; le reste de l'image est nul (zone neuve). Posée AVANT les fenêtres. */
    const char *img = getenv("SC2_DT_IMAGE");
    if (img) {
        FILE *fi = fopen(img, "rb");
        if (!fi) { perror("SC2_DT_IMAGE"); return 2; }
        uint32_t m, n = 0;
        if (!LIRE(fi, m)) { fprintf(stderr, "SC2_DT_IMAGE vide\n"); return 2; }
        if (m != 0x31495444u) {          /* image plate (1re forme) : posée à 0x00800000 */
            rewind(fi);
            if (!fread(GPTR(0x00800000u), 1, 0x08000000u, fi)) { fprintf(stderr, "SC2_DT_IMAGE vide\n"); return 2; }
        } else if (!LIRE(fi, n)) { fprintf(stderr, "SC2_DT_IMAGE invalide\n"); return 2; }
        for (uint32_t k = 0; k < n; k++) {
            uint32_t va, len;
            if (!LIRE(fi, va) || !LIRE(fi, len) || !lire_exacte(fi, GPTR(va), len)) {
                fprintf(stderr, "SC2_DT_IMAGE tronquée\n"); return 2; }
        }
        fclose(fi);
    }

    CPU c; memset(&c, 0, sizeof c);
    uint32_t addr, eflags;
    if (!LIRE(in, addr) || !LIRE(in, eflags)) return 2;
    uint32_t *gpr[8] = { &c.eax, &c.ecx, &c.edx, &c.ebx, &c.esp, &c.ebp, &c.esi, &c.edi };
    for (int i = 0; i < 8; i++) if (!LIRE(in, *gpr[i])) return 2;
    if (!LIRE(in, c.fpu_top)) return 2;
    for (int i = 0; i < 8; i++) if (!LIRE(in, c.st[i])) return 2;
    if (!LIRE(in, c.fpu_sw)) return 2;
    for (int i = 0; i < 8; i++) if (!lire_exacte(in, c.xmm[i].u8, 16)) return 2;
    eflags_unpack(&c, eflags);

    /* fenêtres mémoire à poser */
    uint32_t n_fenetres;
    if (!LIRE(in, n_fenetres)) return 2;
    for (uint32_t i = 0; i < n_fenetres; i++) {
        uint32_t va, len;
        if (!LIRE(in, va) || !LIRE(in, len)) return 2;
        if (!lire_exacte(in, GPTR(va), len)) return 2;
    }
    /* adresses à relire après exécution (une à la fois) */
    uint32_t n_probes;
    if (!LIRE(in, n_probes)) return 2;

    /* recherche de la fonction */
    RecompFn fn = NULL;
    for (size_t i = 0; i < N_TABLE; i++)
        if (g_table[i].addr == addr) { fn = g_table[i].fn; break; }
    if (!fn) { fprintf(stderr, "fonction %08X absente de la table\n", addr); return 3; }

    fn(&c);   /* exécution */

    /* sortie : même format, sans fenêtres posées — juste les probes */
    FILE *out = stdout;
    uint32_t magic_out = 0x31305444u;
    fwrite(&magic_out, 4, 1, out);
    uint32_t ok = 1; fwrite(&ok, 4, 1, out);
    for (int i = 0; i < 8; i++) fwrite(gpr[i], 4, 1, out);
    uint32_t ef = eflags_pack(&c); fwrite(&ef, 4, 1, out);
    fwrite(&c.fpu_top, 4, 1, out);
    fwrite(c.st, 8, 8, out);
    fwrite(&c.fpu_sw, 4, 1, out);
    for (int i = 0; i < 8; i++) fwrite(c.xmm[i].u8, 16, 1, out);
    for (uint32_t i = 0; i < n_probes; i++) {
        uint32_t va, len;
        if (!LIRE(in, va) || !LIRE(in, len)) return 2;
        fwrite(&va, 4, 1, out); fwrite(&len, 4, 1, out);
        fwrite(GPTR(va), 1, len, out);
    }
    fclose(in); fclose(out);
    return 0;
}
