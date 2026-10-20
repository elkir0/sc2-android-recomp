/* banc-video-7w3.c — passe 7w3 : un .ogv du jeu décodé par runtime/sc2_video.c, frame par frame, TÉLÉVERSÉ en
 * texture du backend Vulkan (d3dv_texture, D3DFMT_A8R8G8B8 comme une texture dynamique du jeu) puis DESSINÉ en
 * plein écran sur une cible de rendu par d3dv_dessiner et PRÉSENTÉ par d3dv_present (PPM) — la voie écran du
 * moteur, appelée par ses fonctions publiques seulement (sc2_d3d9v.h, non modifié).
 *
 *   banc-video-7w3 <archive MPQ hôte> <nom interne> <frames> <pas> <dossier de sortie>
 *
 * Pour chaque frame décodée :
 *   - RGBA « CPU » (sc2v_lire_rgba) = référence ;
 *   - texture relue par le GPU (d3dv_texture_relire) comparée à la référence (pixels différents) ;
 *   - toutes les `pas` frames : quad plein écran échantillonné en POINT sur une cible w×h, Present → PPM,
 *     comparé à la référence (pixels différents, écart max), moyenne et écart type de l'image.
 * Bilan : [BILAN] [VIDEO-BANC] … ; code 0 si toutes les comparaisons sont exactes et ≥ 1 frame présentée.
 *
 * Shaders du quad : GLSL 450 compilé par glslangValidator -V au lancement, interface des shaders traduits
 * (set 0 : bindings 0/1 UBO, 2+n sampler n ; attributs location = registre v ; texcoord0 → location 2).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sc2_d3d9v.h"
#include "sc2_video.h"

static const char *VS =
    "#version 450\n"
    "layout(location=0) in vec4 p;\n"
    "layout(location=1) in vec2 uv;\n"
    "layout(location=2) out vec4 t0;\n"
    "void main(){ gl_Position = vec4(p.xy, 0.0, 1.0); t0 = vec4(uv, 0.0, 1.0); }\n";
static const char *PS =
    "#version 450\n"
    "layout(set=0, binding=2) uniform sampler2D s0;\n"
    "layout(location=2) in vec4 t0;\n"
    "layout(location=0) out vec4 c;\n"
    "void main(){ c = texture(s0, t0.xy); }\n";

static uint32_t *compiler(const char *src, const char *etage, const char *dir, uint32_t *nmots)
{
    char g[512], s[512], cmd[1400];
    snprintf(g, sizeof g, "%s/quad.%s", dir, etage);
    snprintf(s, sizeof s, "%s/quad-%s.spv", dir, etage);
    FILE *f = fopen(g, "w"); if (!f) return NULL; fputs(src, f); fclose(f);
    snprintf(cmd, sizeof cmd, "glslangValidator -V -S %s -o %s %s > %s/glslang-%s.log 2>&1", etage, s, g, dir, etage);
    if (system(cmd) != 0) { fprintf(stderr, "glslangValidator a échoué (%s)\n", etage); return NULL; }
    f = fopen(s, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *m = malloc((size_t)n);
    if (fread(m, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(m); return NULL; }
    fclose(f); *nmots = (uint32_t)(n / 4);
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage : %s <archive> <nom interne> <frames> <pas> <sortie>\n", argv[0]); return 2; }
    const char *arc = argv[1], *nom = argv[2], *out = argv[5];
    uint32_t nmax = (uint32_t)atoi(argv[3]), pas = (uint32_t)atoi(argv[4]);
    if (!pas) pas = 1;
    if (!d3dv_init()) { printf("[VIDEO-BANC] backend Vulkan inactif\n"); return 77; }
    printf("[VIDEO-BANC] backend : %s\n", d3dv_nom_device());
    uint32_t nvs = 0, nps = 0;
    uint32_t *svs = compiler(VS, "vert", out, &nvs), *sps = compiler(PS, "frag", out, &nps);
    if (!svs || !sps) return 1;
    int mvs = d3dv_module(svs, nvs, 0), mps = d3dv_module(sps, nps, 1);
    if (mvs < 0 || mps < 0) { printf("[VIDEO-BANC] modules refusés (%d, %d)\n", mvs, mps); return 1; }

    sc2v *v = sc2v_ouvrir_mpq(arc, nom);
    if (!v) return 1;
    uint32_t w, h; sc2v_dims(v, &w, &h);
    size_t np = (size_t)w * h;
    uint8_t *ref = malloc(np * 4), *bgra = malloc(np * 4), *gpu = malloc(np * 4);
    int cib = d3dv_cible(0x7E300000ull, w, h, 21, 0);
    if (cib < 0) { printf("[VIDEO-BANC] cible %ux%u refusée\n", w, h); return 1; }

    /* quad plein écran (bande de 2 triangles) : position xy (vec4) + uv */
    static const float sommets[4][6] = {
        { -1, -1, 0, 1, 0, 0 }, { 1, -1, 0, 1, 1, 0 }, { -1, 1, 0, 1, 0, 1 }, { 1, 1, 0, 1, 1, 1 } };
    static uint8_t cvs[D3DV_CONST_TAILLE], cps[D3DV_CONST_TAILLE];
    d3dv_dessin d; memset(&d, 0, sizeof d);
    d.vs = mvs; d.ps = mps; d.topo = 5 /* D3DPT_TRIANGLESTRIP */;
    d.nattr = 2;
    d.attr[0].loc = 0; d.attr[0].flux = 0; d.attr[0].vkfmt = 109 /* R32G32B32A32_SFLOAT */; d.attr[0].offset = 0;
    d.attr[1].loc = 1; d.attr[1].flux = 0; d.attr[1].vkfmt = 103 /* R32G32_SFLOAT */; d.attr[1].offset = 16;
    d.nflux = 1; d.flux[0].stride = 24; d.flux[0].donnees = sommets; d.flux[0].taille = sizeof sommets;
    d.nombre = 4; d.cvs = cvs; d.cps = cps;
    d.tex[0].genre_shader = 2; d.tex[0].source = D3DV_TEX_TEXTURE;
    d.tex[0].adresse[0] = d.tex[0].adresse[1] = d.tex[0].adresse[2] = 3;   /* D3DTADDRESS_CLAMP */
    d.tex[0].mag = d.tex[0].min = 1; d.tex[0].mip = 0; d.tex[0].aniso = 1; /* POINT : comparaison exacte */
    d.masque_couleur = 0xF; d.cull = 1;

    uint32_t frames = 0, tex_ok = 0, tex_ko_px = 0, presentees = 0, pres_exactes = 0, uniformes = 0;
    double ms_dec = 0, ms_tel = 0;
    struct timespec a, b;
#define MS(a, b) (((b).tv_sec - (a).tv_sec) * 1e3 + ((b).tv_nsec - (a).tv_nsec) / 1e6)
    while (frames < nmax) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        int r = sc2v_avancer(v);
        clock_gettime(CLOCK_MONOTONIC, &b); ms_dec += MS(a, b);
        if (r <= 0) break;
        frames++;
        sc2v_lire_rgba(v, ref);
        sc2v_lire_bgra(v, bgra);
        clock_gettime(CLOCK_MONOTONIC, &a);
        int tid = d3dv_texture(0x7E310000ull, 2, w, h, 1, 21, bgra, (uint32_t)(np * 4), frames);  /* gen = n° de frame */
        clock_gettime(CLOCK_MONOTONIC, &b); ms_tel += MS(a, b);
        if (tid < 0) { printf("[VIDEO-BANC] frame %u : texture refusée\n", frames); break; }
        uint32_t diff = 0;
        if (d3dv_texture_relire(tid, 0, 0, gpu) == 0) {
            for (size_t i = 0; i < np * 4; i++) if (gpu[i] != ref[i]) { diff++; i |= 3; }
            if (!diff) tex_ok++; else tex_ko_px += diff;
        } else tex_ko_px += (uint32_t)np;
        if ((frames - 1) % pas) continue;
        /* voie écran : quad → cible → Present */
        d3dv_rt(cib); d3dv_viewport(0, 0, w, h, 0.f, 1.f);
        d3dv_scene(1);
        d3dv_clear(0, NULL, D3DV_CLEAR_TARGET, 0xFFFF00FFu, 1.f, 0);    /* magenta : un trou se verrait */
        d.tex[0].id = tid;
        int dr = d3dv_dessiner(&d);
        d3dv_scene(0);
        char ch[1024]; snprintf(ch, sizeof ch, "%s/ecran_%05u.ppm", out, frames);
        double moy[3] = { 0, 0, 0 };
        if (dr != 0 || d3dv_present(cib, ch, moy) != 0) { printf("[VIDEO-BANC] frame %u : dessin/Present refusé (%d)\n", frames, dr); continue; }
        presentees++;
        /* relecture du PPM présenté : comparaison exacte avec la référence + écart type (structure) */
        FILE *f = fopen(ch, "rb"); uint32_t pw = 0, ph = 0; int mx = 0;
        uint32_t dpx = 0; int emax = 0; double s2 = 0, s1 = 0;
        if (f && fscanf(f, "P6 %u %u %d", &pw, &ph, &mx) == 3 && fgetc(f) != EOF && pw == w && ph == h) {
            uint8_t *l = malloc((size_t)w * 3);
            for (uint32_t y = 0; y < h; y++) {
                if (fread(l, 1, (size_t)w * 3, f) != (size_t)w * 3) { dpx = (uint32_t)np; break; }
                for (uint32_t x = 0; x < w; x++) {
                    int dl = 0;
                    for (int c = 0; c < 3; c++) {
                        int e = abs((int)l[x * 3 + c] - (int)ref[((size_t)y * w + x) * 4 + c]);
                        if (e > emax) emax = e;
                        if (e) dl = 1;
                        s1 += l[x * 3 + c]; s2 += (double)l[x * 3 + c] * l[x * 3 + c];
                    }
                    dpx += (uint32_t)dl;
                }
            }
            free(l);
        } else dpx = (uint32_t)np;
        if (f) fclose(f);
        double n3 = 3.0 * np, et = sqrt(s2 / n3 - (s1 / n3) * (s1 / n3));
        if (!dpx) pres_exactes++;
        if (et < 2.0) uniformes++;
        printf("[VIDEO-BANC] frame %u présentée → %s : moyenne RGB %.2f %.2f %.2f, écart type %.2f ; "
               "texture GPU %s ; écran vs décodeur : %u px différents (écart max %d)\n",
               frames, ch, moy[0], moy[1], moy[2], et, diff ? "DIFFÉRENTE" : "= décodeur", dpx, emax);
    }
    printf("[BILAN] [VIDEO-BANC] %s : %ux%u à %.3f images/s ; %u frames décodées (%llu octets lus en flux, %.2f ms/frame), "
           "%u textures téléversées identiques au décodeur (%u px différents au total, %.2f ms/téléversement) ; "
           "%u frames présentées par la voie écran (1 sur %u), %u identiques pixel à pixel, %u uniformes\n",
           nom, w, h, sc2v_fps(v), frames, (unsigned long long)sc2v_octets(v), frames ? ms_dec / frames : 0.0,
           tex_ok, tex_ko_px, frames ? ms_tel / frames : 0.0, presentees, pas, pres_exactes, uniformes);
    d3dv_bilan();
    sc2v_fermer(v);
    free(ref); free(bgra); free(gpu); free(svs); free(sps);
    return frames && tex_ok == frames && presentees && pres_exactes == presentees && !uniformes ? 0 : 1;
}
