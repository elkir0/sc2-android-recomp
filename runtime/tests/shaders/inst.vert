#version 450
// V2.2 (22/10) — test GPU de l'instanciation (tests/test_d3d9v.c) : nos shaders, aucun contenu du jeu.
// Liaison 0 par sommet : coin du quad ; liaison 1 (par instance, ou par sommet dans les dessins de référence) :
// centre xy et demi-taille xy en NDC, teinte RGBA8.
layout(location = 0) in vec2 coin;
layout(location = 1) in vec2 centre;
layout(location = 2) in vec2 demi;
layout(location = 3) in vec4 teinte;
layout(location = 0) out vec4 c;
void main() { gl_Position = vec4(centre + coin * demi, 0.5, 1.0); c = teinte; }
