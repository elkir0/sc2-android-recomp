/* audio-preuve-7v.c — preuve de la bande son (passe 7v) : le WAV mixé par le FMOD logiciel du runtime
 * (SC2_AUDIO_DUMP) contient-il bien la musique du jeu ?
 *
 *   cc -O2 -I recomp-sc2/runtime android/outils/audio-preuve-7v.c -lm -o /tmp/audio-preuve
 *   /tmp/audio-preuve <mix.wav> [<référence.ogg> …]
 *
 * 1. Activité du mix par fenêtre de 10 s : échantillons non nuls, RMS (dBFS), crête.
 * 2. Pour chaque référence (.ogg extrait des MPQ par tools/mpqget, décodé ici par le même stb_vorbis que le
 *    runtime) : on cherche où elle se trouve dans le mix. Recherche grossière sur des enveloppes (blocs de 256
 *    frames) avec la fenêtre la plus énergique du mix (20 s), puis recherche fine au frame près (±256). Au
 *    décalage trouvé : corrélation de Pearson échantillon par échantillon (G et D) sur toute la zone commune,
 *    gain moyen (mix ≈ gain × référence) et rapport signal/résidu. Une corrélation proche de 1 dit que le mix
 *    EST cette musique (au gain du canal et du groupe près), pas un son quelconque.
 * 3. Ajustement CONJOINT : mix ≈ Σ gain_k × référence_k (moindres carrés, par tranche de 10 s) avec toutes les
 *    références localisées (musique + ambiance jouées ensemble) ; le rapport signal/résidu dit quelle part du
 *    mix ces fichiers expliquent à eux seuls (résidu ≈ arrondi 16 bits + sons non fournis).
 * Pur hôte, aucun fichier de jeu écrit. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sc2_audio_stb_vorbis.c"

typedef struct { int16_t *p; long n; int freq; } pcm_t;          /* stéréo entrelacé, n frames */

static int lire_wav(const char *f, pcm_t *w)
{
    FILE *h = fopen(f, "rb");
    if (!h) return -1;
    uint8_t e[44];
    if (fread(e, 1, 44, h) != 44 || memcmp(e, "RIFF", 4) || memcmp(e + 8, "WAVE", 4)) { fclose(h); return -1; }
    uint16_t nch; uint32_t freq; memcpy(&nch, e + 22, 2); memcpy(&freq, e + 24, 4);
    if (nch != 2) { fclose(h); return -1; }
    fseek(h, 0, SEEK_END); long octets = ftell(h) - 44; fseek(h, 44, SEEK_SET);
    w->n = octets / 4; w->freq = (int)freq; w->p = malloc((size_t)w->n * 4 + 4);
    w->n = (long)fread(w->p, 4, (size_t)w->n, h);
    fclose(h);
    return 0;
}

static int lire_ogg(const char *f, pcm_t *r)
{
    int nch = 0, freq = 0; short *p = NULL;
    int n = stb_vorbis_decode_filename(f, &nch, &freq, &p);
    if (n <= 0 || !p) return -1;
    r->n = n; r->freq = freq; r->p = malloc((size_t)n * 4 + 4);
    for (long i = 0; i < n; i++) { r->p[2 * i] = p[(size_t)i * nch]; r->p[2 * i + 1] = p[(size_t)i * nch + (nch > 1)]; }
    free(p);
    return 0;
}

#define BLOC 256
static float *enveloppe(const pcm_t *a, long *nb)
{
    *nb = a->n / BLOC;
    float *e = calloc((size_t)*nb + 1, sizeof *e);
    for (long b = 0; b < *nb; b++) {
        double s = 0;
        for (long i = b * BLOC; i < (b + 1) * BLOC; i++) s += fabs((double)a->p[2 * i] + a->p[2 * i + 1]);
        e[b] = (float)(s / BLOC);
    }
    return e;
}

/* corrélation de Pearson de x[0..n) et y[0..n) */
static double pearson(const float *x, const float *y, long n)
{
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (long i = 0; i < n; i++) { sx += x[i]; sy += y[i]; sxx += (double)x[i] * x[i]; syy += (double)y[i] * y[i]; sxy += (double)x[i] * y[i]; }
    double cx = sxx - sx * sx / n, cy = syy - sy * sy / n, cxy = sxy - sx * sy / n;
    return cx > 0 && cy > 0 ? cxy / sqrt(cx * cy) : 0;
}

/* au décalage d (frame mix = frame réf + d), corrélation par échantillon sur [m0, m1) du mix */
static double correler(const pcm_t *m, const pcm_t *r, long d, long m0, long m1, double *gain, double *snr)
{
    double sxy = 0, syy = 0, sxx = 0, sx = 0, sy = 0; long n = 0;
    for (long i = m0; i < m1; i++) {
        long j = i - d;
        if (j < 0 || j >= r->n) continue;
        for (int c = 0; c < 2; c++) {
            double x = m->p[2 * i + c], y = r->p[2 * j + c];
            sxy += x * y; syy += y * y; sxx += x * x; sx += x; sy += y; n++;
        }
    }
    if (!n) return 0;
    *gain = syy > 0 ? sxy / syy : 0;
    double res = sxx - 2 * *gain * sxy + *gain * *gain * syy;          /* Σ (x − g·y)² */
    double sig = *gain * *gain * syy;
    *snr = res > 0 ? 10 * log10(sig / res) : 99;
    double cx = sxx - sx * sx / n, cy = syy - sy * sy / n, cxy = sxy - sx * sy / n;
    return cx > 0 && cy > 0 ? cxy / sqrt(cx * cy) : 0;
}

/* moindres carrés sur [t, t1) : mix ≈ Σ g_k × réf_k[i − d_k] ; rend Σ résidu² (et Σ mix² dans *sxx) */
static double conjoint(const pcm_t *m, const pcm_t *refs, const long *decs, int K, long t, long t1, double *g, double *sxx_out)
{
    double A[16][17] = {{0}}, sxx = 0;
    for (long i = t; i < t1; i++) for (int c = 0; c < 2; c++) {
        double x = m->p[2 * i + c], y[16];
        for (int a = 0; a < K; a++) { long j = i - decs[a]; y[a] = j >= 0 && j < refs[a].n ? refs[a].p[2 * j + c] : 0; }
        for (int a = 0; a < K; a++) { for (int b = 0; b < K; b++) A[a][b] += y[a] * y[b]; A[a][K] += y[a] * x; }
        sxx += x * x;
    }
    for (int a = 0; a < K; a++) {                                         /* Gauss-Jordan, pivot partiel */
        int pv = a; for (int b = a + 1; b < K; b++) if (fabs(A[b][a]) > fabs(A[pv][a])) pv = b;
        if (fabs(A[pv][a]) < 1e-9) { for (int b = 0; b <= K; b++) A[a][b] = b == a; continue; }
        for (int b = 0; b <= K; b++) { double tmp = A[a][b]; A[a][b] = A[pv][b]; A[pv][b] = tmp; }
        for (int b = 0; b < K; b++) if (b != a) { double f = A[b][a] / A[a][a]; for (int e = a; e <= K; e++) A[b][e] -= f * A[a][e]; }
    }
    for (int a = 0; a < K; a++) g[a] = A[a][a] != 0 ? A[a][K] / A[a][a] : 0;
    double res = 0;
    for (long i = t; i < t1; i++) for (int c = 0; c < 2; c++) {
        double v = m->p[2 * i + c];
        for (int a = 0; a < K; a++) { long j = i - decs[a]; if (j >= 0 && j < refs[a].n) v -= g[a] * refs[a].p[2 * j + c]; }
        res += v * v;
    }
    if (sxx_out) *sxx_out = sxx;
    return res;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage : %s <mix.wav> [<référence.ogg> …]\n", argv[0]); return 2; }
    pcm_t m;
    if (lire_wav(argv[1], &m)) { fprintf(stderr, "%s : WAV stéréo PCM16 illisible\n", argv[1]); return 2; }
    printf("mix %s : %ld frames, %.1f s à %d Hz\n", argv[1], m.n, (double)m.n / m.freq, m.freq);
    long fen = (long)m.freq * 10, total_nn = 0;
    for (long t = 0; t < m.n; t += fen) {
        long nn = 0, f1 = t + fen < m.n ? t + fen : m.n; double sq = 0; int cr = 0;
        for (long i = 2 * t; i < 2 * f1; i++) { int v = abs(m.p[i]); nn += v != 0; sq += (double)m.p[i] * m.p[i]; if (v > cr) cr = v; }
        double rms = sqrt(sq / (2.0 * (f1 - t))) / 32768.0;
        printf("  [%5.0f s – %5.0f s] non nuls %8ld / %8ld (%5.1f %%)  RMS %6.1f dBFS  crête %5d\n", (double)t / m.freq,
               (double)f1 / m.freq, nn, 2 * (f1 - t), 100.0 * nn / (2.0 * (f1 - t)), rms > 0 ? 20 * log10(rms) : -999.0, cr);
        total_nn += nn;
    }
    printf("  total : %ld échantillons non nuls sur %ld (%.1f %%)\n", total_nn, 2 * m.n, 100.0 * total_nn / (2.0 * m.n));

    long nbm; float *em = enveloppe(&m, &nbm);
    long W = (long)(20.0 * m.freq / BLOC);                                 /* fenêtre de 20 s */
    if (W > nbm) W = nbm;
    long meil = 0; double meilE = -1;                                      /* fenêtre la plus énergique */
    for (long b = 0; b + W <= nbm; b += W / 4 + 1) { double s = 0; for (long i = b; i < b + W; i++) s += em[i]; if (s > meilE) { meilE = s; meil = b; } }
    pcm_t refs[16]; long decs[16]; int nref = 0;
    for (int k = 2; k < argc; k++) {
        pcm_t r;
        if (lire_ogg(argv[k], &r)) { printf("référence %s : décodage impossible\n", argv[k]); continue; }
        const char *nom = strrchr(argv[k], '/'); nom = nom ? nom + 1 : argv[k];
        printf("référence %s : %ld frames, %.2f s à %d Hz\n", nom, r.n, (double)r.n / r.freq, r.freq);
        if (r.freq != m.freq) { printf("  fréquence ≠ mix : pas de comparaison au frame près\n"); free(r.p); continue; }
        long nbr; float *er = enveloppe(&r, &nbr);
        /* (a) fenêtre énergique du mix glissée sur la référence (référence longue, en partie jouée) ;
         * (b) segment énergique de la référence glissé sur le mix (référence courte : voix, stinger). */
        long best = 0, bestm = meil; double bestc = -2;
        for (long j = 0; j + W <= nbr; j++) { double c = pearson(em + meil, er + j, W); if (c > bestc) { bestc = c; best = j; bestm = meil; } }
        long Wr = W < nbr * 4 / 5 ? W : nbr * 4 / 5, jr = 0; double eR = -1;
        for (long j = 0; j + Wr <= nbr; j += Wr / 8 + 1) { double e = 0; for (long i = j; i < j + Wr; i++) e += er[i]; if (e > eR) { eR = e; jr = j; } }
        if (Wr > 8) for (long b = 0; b + Wr <= nbm; b++) { double c = pearson(em + b, er + jr, Wr); if (c > bestc) { bestc = c; best = jr; bestm = b; } }
        long d0 = (bestm - best) * BLOC, d = d0; double cfin = -2, g, s;
        long Wf = bestm == meil && Wr == W ? W : Wr;
        for (long dd = d0 - BLOC; dd <= d0 + BLOC; dd++) {                 /* fin : ≤ 1 s autour du milieu de la fenêtre */
            long c0 = (bestm + Wf / 2) * BLOC, l1 = (long)m.freq < Wf * BLOC / 2 ? (long)m.freq : Wf * BLOC / 2;
            double c = correler(&m, &r, dd, c0, c0 + l1, &g, &s);
            if (c > cfin) { cfin = c; d = dd; }
        }
        long m0 = d > 0 ? d : 0, m1 = d + r.n < m.n ? d + r.n : m.n;
        double cz = correler(&m, &r, d, m0, m1, &g, &s);
        printf("  enveloppes : meilleure corrélation %.4f (fenêtre du mix %.1f–%.1f s ↔ référence %.1f s)\n", bestc,
               (double)bestm * BLOC / m.freq, (double)(bestm + Wf) * BLOC / m.freq, (double)best * BLOC / r.freq);
        printf("  au frame près : mix[t] = gain × référence[t − %.3f s] (décalage %ld frames)\n", (double)d / m.freq, d);
        printf("  zone commune %.1f s – %.1f s du mix (%.1f s, %.1f %% de la référence) : corrélation par échantillon %.4f, "
               "gain %.3f, signal/résidu %.1f dB\n", (double)m0 / m.freq, (double)m1 / m.freq, (double)(m1 - m0) / m.freq,
               100.0 * (m1 - m0) / r.n, cz, g, s);
        for (long t = m0; t + 10L * m.freq <= m1; t += 10L * m.freq) {   /* par tranche de 10 s */
            double gt, st, ct = correler(&m, &r, d, t, t + 10L * m.freq, &gt, &st);
            printf("    tranche %6.1f s : corrélation %.4f, gain %.3f, signal/résidu %5.1f dB\n", (double)t / m.freq, ct, gt, st);
        }
        free(er);
        if (cz > 0.3 && nref < 16) { refs[nref] = r; decs[nref++] = d; } else free(r.p);
    }
    if (nref > 0) {
        /* raffinement conjoint des décalages : ±16 frames chacun, au résidu minimal sur 2 s communes */
        long c0 = 0, c1 = m.n;
        for (int a = 0; a < nref; a++) { if (decs[a] > c0) c0 = decs[a]; if (decs[a] + refs[a].n < c1) c1 = decs[a] + refs[a].n; }
        if (c1 - c0 > 4L * m.freq) {
            long t0 = c0 + (c1 - c0) / 2, t1 = t0 + 2L * m.freq; double g[16];
            for (int iter = 0; iter < 3; iter++)
                for (int a = 0; a < nref; a++) {
                    long meil = decs[a]; double rmin = conjoint(&m, refs, decs, nref, t0, t1, g, NULL);
                    long base = decs[a];
                    for (long dd = base - 16; dd <= base + 16; dd++) {
                        decs[a] = dd; double r = conjoint(&m, refs, decs, nref, t0, t1, g, NULL);
                        if (r < rmin) { rmin = r; meil = dd; }
                    }
                    decs[a] = meil;
                }
        }
        printf("ajustement conjoint (%d référence(s) localisée(s), décalages raffinés :", nref);
        for (int a = 0; a < nref; a++) printf(" %ld", decs[a]);
        printf(" frames), par tranche de 10 s : mix ≈ Σ gain × référence\n");
        for (long t = 0; t < m.n; t += 10L * m.freq) {
            long t1 = t + 10L * m.freq < m.n ? t + 10L * m.freq : m.n;
            double g[16], sxx = 0, res = conjoint(&m, refs, decs, nref, t, t1, g, &sxx);
            printf("  tranche %6.1f s : gains", (double)t / m.freq);
            for (int a = 0; a < nref; a++) printf(" %.4f", g[a]);
            printf(" ; part du mix expliquée %7.3f %%, signal/résidu %5.1f dB, résidu RMS %.2f (en pas de 16 bits)\n",
                   sxx > 0 ? 100.0 * (1 - res / sxx) : 0, res > 0 && sxx > res ? 10 * log10((sxx - res) / res) : 99.0,
                   sqrt(res / (2.0 * (t1 - t))));
        }
        for (int a = 0; a < nref; a++) free(refs[a].p);
    }
    free(em); free(m.p);
    return 0;
}
