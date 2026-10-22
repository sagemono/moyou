
// RSX sampler models, prepended to every patched program. Wrap modes follow the GCM enum: 1 WRAP,
// 3 CLAMP_TO_EDGE ([1/2N, 1-1/2N]), 5 CLAMP ([0,1], border colour 0). Filtering uses 8-bit interpolation
// coefficients; results are rounded to 8-bit or FP16 as the RSX texture unit does for the texel format
// (RSX User's Manual 7.1, 8.1).
vec4 rsx_q8(vec4 v) { return floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5) / 255.0; }
vec4 rsx_f16(vec4 v) { return vec4(unpackHalf2x16(packHalf2x16(v.xy)), unpackHalf2x16(packHalf2x16(v.zw))); }
int rsx_wrapi(int i, int n, int mode) { return mode == 1 ? ((i % n) + n) % n : clamp(i, 0, n - 1); }
vec4 rsx_texel(sampler2D s, ivec2 p, ivec2 n, ivec2 mode) {
  if ((mode.x == 5 && (p.x < 0 || p.x >= n.x)) || (mode.y == 5 && (p.y < 0 || p.y >= n.y))) return vec4(0.0);
  return texelFetch(s, ivec2(rsx_wrapi(p.x, n.x, mode.x), rsx_wrapi(p.y, n.y, mode.y)), 0);
}
vec4 rsx_bilerp(sampler2D s, vec2 uv, ivec2 mode) {
  ivec2 n = textureSize(s, 0);
  vec2 nf = vec2(n);
  if (mode.x == 5) uv.x = clamp(uv.x, 0.0, 1.0);
  if (mode.y == 5) uv.y = clamp(uv.y, 0.0, 1.0);
  if (mode.x == 3) uv.x = clamp(uv.x, 0.5 / nf.x, 1.0 - 0.5 / nf.x);
  if (mode.y == 3) uv.y = clamp(uv.y, 0.5 / nf.y, 1.0 - 0.5 / nf.y);
  vec2 p = uv * nf - 0.5;
  vec2 i = floor(p);
  vec2 f = floor((p - i) * 256.0) / 256.0;
  ivec2 i0 = ivec2(i);
  vec4 a = rsx_texel(s, i0, n, mode), b = rsx_texel(s, i0 + ivec2(1, 0), n, mode);
  vec4 c = rsx_texel(s, i0 + ivec2(0, 1), n, mode), d = rsx_texel(s, i0 + ivec2(1, 1), n, mode);
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
vec4 rsx_aniso(sampler2D s, vec2 uv, float maxAniso, bool f16, ivec2 mode) {
  vec2 n = vec2(textureSize(s, 0));
  vec2 dx = dFdx(uv), dy = dFdy(uv);
  float la = length(dx * n), lc = length(dy * n);
  vec2 major = la >= lc ? dx : dy;
  float N = clamp(ceil(max(la, lc) / max(min(la, lc), 1e-6)), 1.0, maxAniso);
  vec4 acc = vec4(0.0);
  for (int i = 0; i < 16; i++) {
    if (float(i) >= N) break;
    vec4 v = rsx_bilerp(s, uv + major * ((float(i) + 0.5) / N - 0.5), mode);
    acc += f16 ? rsx_f16(v) : rsx_q8(v);
  }
  return acc / N;
}
vec4 rsx_gauss3(sampler2D s, vec2 uv, ivec2 mode) {
  ivec2 n = textureSize(s, 0);
  ivec2 c = ivec2(floor(uv * vec2(n)));
  vec4 acc = vec4(0.0);
  for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++)
    acc += float((2 - abs(i)) * (2 - abs(j))) * rsx_texel(s, c + ivec2(i, j), n, mode);
  return acc / 16.0;
}
