/* audio-piste-7z.c — preuve de la bande son par RECONSTRUCTION (passe 7z) : le WAV mixé par le runtime
 * (SC2_AUDIO_DUMP) est-il exactement la somme des fichiers du jeu, aux positions et aux gains que le jeu a demandés ?
 *
 *   cc -O2 -I recomp-sc2/runtime android/outils/audio-piste-7z.c -lm -o /tmp/audio-piste
 *   /tmp/audio-piste <mix.wav> <piste.txt> <dossier des références .ogg> [tranche_s=10]
 *
 * piste.txt = SC2_AUDIO_PISTE du même run (sc2_audio_mix.c) : un segment contigu mixé par ligne,
 *   S <canal> <mix_début> <frames> <pos_début> <pas> <gain_g> <gain_d> <flux> <nom du son>
 *   D <canal> <mix_début> <frames> <gain_g> <gain_d>      (générateur DSP du jeu, sans référence)
 * Chaque son est cherché dans le dossier sous son nom de base (extrait des MPQ par tools/mpqget), décodé ici par
 * stb_vorbis INDÉPENDAMMENT du runtime (décodage complet, pas le flux), puis rejoué comme le mixeur : frame
 * entière pour un flux, interpolation linéaire sinon ; Σ gain × échantillon, écrêtage, arrondi 16 bits.
 * Par tranche : part du mix expliquée (1 − E[résidu²]/E[mix²]), échantillons identiques au bit près, écart max,
 * sons actifs (et ceux sans référence). Les positions viennent du runtime : l'outil prouve que le mix contient
 * les bons fichiers aux bons gains ; la localisation AVEUGLE (sans journal) reste celle d'audio-preuve-7v.c.
 * Pur hôte, aucun fichier de jeu écrit. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sc2_audio_stb_vorbis.c"

typedef struct { char nom[200]; short *pcm; int nch, rate, frames, absent; double frames_mix; } ref_t;
static ref_t REFS[512]; static int nr;

static ref_t *ref(const char *dossier, const char *nom)
{
    const char *b = strrchr(nom, '\\'); b = b ? b + 1 : nom;
    for (int i = 0; i < nr; i++) if (!strcmp(REFS[i].nom, b)) return &REFS[i];
    if (nr >= 512) return NULL;
    ref_t *r = &REFS[nr++]; snprintf(r->nom, sizeof r->nom, "%s", b);
    char f[1024]; snprintf(f, sizeof f, "%s/%s", dossier, b);
    r->frames = stb_vorbis_decode_filename(f, &r->nch, &r->rate, &r->pcm);
    if (r->frames <= 0 || !r->pcm) { r->absent = 1; r->frames = 0; }
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage : %s <mix.wav> <piste.txt> <dossier refs> [tranche_s]\n", argv[0]); return 2; }
    FILE *h = fopen(argv[1], "rb");
    uint8_t e[44];
    if (!h || fread(e, 1, 44, h) != 44 || memcmp(e, "RIFF", 4)) { fprintf(stderr, "WAV illisible\n"); return 1; }
    uint32_t freq; memcpy(&freq, e + 24, 4);
    fseek(h, 0, SEEK_END); long n = (ftell(h) - 44) / 4; fseek(h, 44, SEEK_SET);
    int16_t *mix = malloc((size_t)n * 4 + 4); n = (long)fread(mix, 4, (size_t)n, h); fclose(h);
    float *acc = calloc((size_t)n * 2 + 2, sizeof(float));
    uint8_t *dsp = calloc((size_t)n + 1, 1), *sansref = calloc((size_t)n + 1, 1);
    int ts = argc > 4 ? atoi(argv[4]) : 10; if (ts <= 0) ts = 10;
    long nt = n / ((long)freq * ts) + 1;
    /* sons actifs par tranche : bits sur les références (512 max) */
    uint64_t (*actifs)[8] = calloc((size_t)nt, sizeof *actifs);

    FILE *p = fopen(argv[2], "r");
    if (!p) { fprintf(stderr, "piste illisible\n"); return 1; }
    char l[1024]; long segs = 0, segs_dsp = 0;
    while (fgets(l, sizeof l, p)) {
        int k, flux, o = 0; unsigned long long deb; unsigned nb; double pos, pas; float gl, gr;
        if (l[0] == 'D' && sscanf(l, "D %d %llu %u %f %f", &k, &deb, &nb, &gl, &gr) == 5) {
            segs_dsp++;
            for (unsigned j = 0; j < nb && deb + j < (unsigned long long)n; j++) dsp[deb + j] = 1;
            continue;
        }
        if (l[0] != 'S' || sscanf(l, "S %d %llu %u %lf %lf %f %f %d %n", &k, &deb, &nb, &pos, &pas, &gl, &gr, &flux, &o) < 8 || !o) continue;
        l[strcspn(l, "\r\n")] = 0;
        ref_t *r = ref(argv[3], l + o);
        if (!r) continue;
        segs++;
        r->frames_mix += nb;
        for (long t = (long)(deb / ((unsigned long long)freq * ts)); t <= (long)((deb + nb) / ((unsigned long long)freq * ts)) && t < nt; t++)
            actifs[t][(r - REFS) >> 6] |= 1ull << ((r - REFS) & 63);
        double ps = pos;                                    /* comme le mixeur : ch->pos += pas, frame par frame */
        for (unsigned j = 0; j < nb; j++, ps += pas) {
            unsigned long long m = deb + j;
            if (m >= (unsigned long long)n) break;
            if (r->absent) { sansref[m] = 1; continue; }
            uint32_t f = (uint32_t)ps;
            if (f >= (uint32_t)r->frames) continue;
            const short *q = r->pcm + (size_t)f * (size_t)r->nch;
            float a = q[0] / 32768.f, b = (r->nch > 1 ? q[1] : q[0]) / 32768.f;
            if (!flux) {
                float t = (float)(ps - f);
                if (t > 0 && f + 1 < (uint32_t)r->frames) {
                    const short *q2 = q + r->nch;
                    a += t * (q2[0] / 32768.f - a); b += t * ((r->nch > 1 ? q2[1] : q2[0]) / 32768.f - b);
                }
            }
            acc[2 * m] += a * gl; acc[2 * m + 1] += b * gr;
        }
    }
    fclose(p);

    printf("mix %s : %ld frames (%.1f s à %u Hz) ; piste : %ld segments de sons, %ld segments DSP ; %d sons distincts\n",
           argv[1], n, (double)n / freq, freq, segs, segs_dsp, nr);
    for (int i = 0; i < nr; i++)
        printf("  son %-48s %s, %.1f s mixées\n", REFS[i].nom, REFS[i].absent ? "RÉFÉRENCE ABSENTE" : "référence décodée", REFS[i].frames_mix / freq);
    double tm = 0, tr = 0, tma = 0, tra = 0; long egaux = 0, nn = 0, act = 0, act95 = 0;
    printf("  tranche          part expliquée   identiques au bit   écart max   mix RMS     sons actifs\n");
    for (long t = 0; t < nt; t++) {
        long a0 = t * (long)freq * ts, a1 = a0 + (long)freq * ts; if (a1 > n) a1 = n;
        if (a0 >= a1) break;
        double em = 0, er = 0; long eg = 0, nz = 0, ndsp = 0, nsr = 0; int dmax = 0;
        for (long m = a0; m < a1; m++) {
            ndsp += dsp[m]; nsr += sansref[m];
            for (int c = 0; c < 2; c++) {
                float v = acc[2 * m + c]; v = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
                int o = (int)lrintf(v * 32767.f), x = mix[2 * m + c], d = abs(x - o);
                em += (double)x * x; er += (double)(x - o) * (x - o);
                eg += d == 0; nz += x != 0; if (d > dmax) dmax = d;
            }
        }
        tm += em; tr += er; egaux += eg; nn += 2 * (a1 - a0);
        double part = em > 0 ? 100.0 * (1.0 - er / em) : 100.0;
        int actif = nz > (a1 - a0) / 10;                     /* tranche « active » : > 5 % d'échantillons non nuls */
        if (actif) { act++; act95 += part >= 95.0; tma += em; tra += er; }
        char noms[600] = ""; size_t o = 0;
        for (int i = 0; i < nr && o < 560; i++)
            if (actifs[t][i >> 6] >> (i & 63) & 1) o += (size_t)snprintf(noms + o, sizeof noms - o, "%s%s%s", o ? ", " : "", REFS[i].nom, REFS[i].absent ? "(sans réf.)" : "");
        printf("  %5ld – %5ld s  %8.3f %%%s   %7.3f %%          %6d      %6.1f dBFS  %s%s%s\n", t * ts, (a1 / (long)freq),
               part, actif ? "" : " (·)", 100.0 * eg / (2.0 * (a1 - a0)), dmax,
               em > 0 ? 10 * log10(em / (2.0 * (a1 - a0))) - 90.309 : -999.0, noms,
               ndsp ? " +DSP du jeu" : "", nsr ? " [frames sans référence]" : "");
    }
    printf("  TOTAL : part expliquée %.3f %% (tranches actives : %.3f %%, %ld/%ld tranches actives ≥ 95 %%) ; "
           "échantillons identiques au bit près %.3f %% ; résidu RMS %.2f pas de 16 bits\n",
           tm > 0 ? 100.0 * (1 - tr / tm) : 100.0, tma > 0 ? 100.0 * (1 - tra / tma) : 100.0, act95, act,
           nn ? 100.0 * egaux / nn : 0.0, nn ? sqrt(tr / nn) : 0.0);
    return 0;
}
