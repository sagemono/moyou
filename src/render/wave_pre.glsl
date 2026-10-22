
uniform highp sampler2D uCtrl;   // 19x19 control points after FFD + MVP (clip space), as DMA'd by the SPU job
vec4 bsB(float t) { float s = 1.0 - t, t2 = t * t, t3 = t2 * t;
  return vec4(s * s * s, 3.0 * t3 - 6.0 * t2 + 4.0, -3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0, t3) / 6.0; }
vec4 bsD(float t) { float s = 1.0 - t, t2 = t * t;
  return vec4(-s * s, 3.0 * t2 - 4.0 * t, -3.0 * t2 + 2.0 * t + 1.0, t2) * 0.5; }
float fadeF(float s) { return min(1.0, 10.0 * (0.5 - abs(0.5 - s))); }
