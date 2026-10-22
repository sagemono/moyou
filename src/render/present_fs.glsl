#version 300 es
precision highp float;
uniform sampler2D uTex; uniform int uMode;
in vec4 v_TEXCOORD0; out vec4 o;
void main() {
  vec2 uv = v_TEXCOORD0.xy;
  if (uMode == 0 || uMode == 3) uv.y = 1.0 - uv.y;      // display buffer memory row 0 is the top of the screen
  vec4 t = texture(uTex, uv);
  if (uMode == 1) t = vec4(vec3((t.y * 0.5 + t.x * 0.03125) * 1.9), 1.0);
  o = vec4(t.rgb, 1.0);
}
