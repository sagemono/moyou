#version 300 es
layout(location = 0) in vec4 a_pos;
layout(location = 8) in vec4 a_tex;
out vec4 v_TEXCOORD0;
void main() { v_TEXCOORD0 = a_tex; gl_Position = vec4(a_pos.xy, 0.0, 1.0); }
