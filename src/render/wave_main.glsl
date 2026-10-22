void main() {
  int I = gl_VertexID % 128, J = gl_VertexID / 128;
  int pi = I >> 3, pj = J >> 3;
  float tu = float(I & 7) * 0.125, tv = float(J & 7) * 0.125;
  vec4 bu = bsB(tu), bv = bsB(tv), du = bsD(tu), dv = bsD(tv);
  vec4 pos = vec4(0.0), Pu = vec4(0.0), Pv = vec4(0.0);
  for (int r = 0; r < 4; r++) {
    vec4 row = vec4(0.0), rowu = vec4(0.0);
    for (int c = 0; c < 4; c++) {
      vec4 C = texelFetch(uCtrl, ivec2(pi + c, pj + r), 0);
      row += bu[c] * C; rowu += du[c] * C;
    }
    pos += bv[r] * row; Pu += bv[r] * rowu; Pv += dv[r] * row;
  }
  a_oldpos = pos;
  a_oldnrm = vec4(cross(Pv.xyz, Pu.xyz), 0.0);          // SPU: cross(dP/dv, dP/du)
  a_oldcrd = vec4(float(J) / 127.0, fadeF(float(I) / 127.0) * fadeF(float(J) / 127.0), 0.0, 1.0);   // 0x4d190
  rsx_vp_main();
}
