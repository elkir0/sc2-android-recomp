#version 450
// Passe 5c — « mode lumière » : sommets du triangle DÉGÉNÉRÉ (les trois au même
// point, hors du volume de clip) : le pipeline est réel et exécuté, aucun pixel
// n'est produit. Les shaders SM2/3 du jeu → SPIR-V : passe suivante.
void main() { gl_Position = vec4(2.0, 2.0, 0.5, 1.0); }
