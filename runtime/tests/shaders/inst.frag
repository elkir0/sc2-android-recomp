#version 450
// V2.2 (22/10) — test GPU de l'instanciation : la teinte interpolée (constante par quad), telle quelle.
layout(location = 0) in vec4 c;
layout(location = 0) out vec4 o;
void main() { o = c; }
