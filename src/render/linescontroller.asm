# LinesController (firmware 4.93) rebuilt by the effects panel, as source rather than string literals in C++.
#
# "#name" starts a section; render/effects.cpp concatenates the ones an EffectSettings turns on, in the order
# below, and expands ${placeholder} with values it computes from the settings. BLANK LINES ARE SIGNIFICANT: a
# section that begins with one is what puts the gap between blocks in the generated listing. Blank lines at the
# end of a section are separators and are dropped. Every ${placeholder} must get a value.
#
# Lines starting "# " (and a bare "#") are comments of this template and never reach the listing.
#
# None of the stock program is in this file. ${stock} in #head and #tone is filled with the instructions of the
# LinesController in the user's lines.qrc: the head up to the wave intensity decode in H2.w (plus a copy of the
# screen uv into R4), and the exposure / tone lookup and dither that write R0. In between, the stock program
# composites with one MADR_m8 R1.xyz, H2.w, f[TEX3], R2; #chroma.off, #colour and #compose split that up so the
# effects can change the wave intensity (R8) and colour (R6).
#
# Registers: R4 screen uv, R8 wave intensity per channel, R6 wave colour, R9 dither noise; R1 shares with H2/H3.


#head
// LinesController (4.93) rebuilt by moyou's effects panel: ${spec}
// R4 screen uv, R8 wave intensity per channel, R6 wave colour, R9 dither noise; R1 is shared with H2/H3.

${stock}


#chroma

// chromatic aberration: red and blue read the wave shifted sideways
MOVR R8.y, H2.w;
ADDR R10.xy, R4, ${plus};
MAXR R10.x, R10.x, ${edge}.x;   // stay on screen (the edge reads 0)
MINR R10.x, R10.x, ${edge}.y;
TEXR R10.xy, R10, _Tex0;
MULR R10.x, R10.x, {0.03125, 0.5, 0, 0}.x;
MADR R8.x, R10.y, {0.03125, 0.5, 0, 0}.y, R10.x;
ADDR R10.xy, R4, ${minus};
MAXR R10.x, R10.x, ${edge}.x;   // stay on screen (the edge reads 0)
MINR R10.x, R10.x, ${edge}.y;
TEXR R10.xy, R10, _Tex0;
MULR R10.x, R10.x, {0.03125, 0.5, 0, 0}.x;
MADR R8.z, R10.y, {0.03125, 0.5, 0, 0}.y, R10.x;


#colour

MOVR R6.xyz, f[TEX3];                              // wave colour: horizontal gradient


#tint
MULR R6.xyz, R6, ${rgb};   // tint


#rainbow
// rainbow: base + amp * cos(2 pi (cycles u + phase) + {0, 2pi/3, 4pi/3})
MULR R5.xyz, R4.x, ${freq}.x;
ADDR R5.xyz, R5, ${phases};
COSR R7.x, R5.x;
COSR R7.y, R5.y;
COSR R7.z, R5.z;
MADR R7.xyz, R7, ${amp}.x, ${amp}.y;
MULR R6.xyz, R6, R7;


#compose
MADR_m8 R1.xyz, R8, R6, R2;                        // colour = 8 * (wave * wave colour + background)


#vignette

// vignette: max(0, 1 - strength * r^2)
ADDR R5.xy, R4, {-0.5, -0.5, 0, 0};
MULR R5.xy, R5, R5;
ADDR R5.x, R5.x, R5.y;
MADR R5.x, R5.x, ${k}.x, ${k}.y;
MAXR R5.x, R5.x, {0, 0, 0, 0}.x;
MULR R1.xyz, R1, R5.x;


#scanlines

// scanlines: every other band of ${halfperiod} rows at ${percent} %
MULR R5.y, f[WPOS].y, ${step}.x;
FRCR R5.y, R5.y;
SGER R5.y, R5.y, {0.5, 0, 0, 0}.x;
MADR R5.y, R5.y, ${dark}.x, ${dark}.y;
MULR R1.xyz, R1, R5.y;


#tone

// exposure / tone lookup and dither (stock, from the file)
${stock}


#saturation

// saturation
DP3R R5.x, R0, {0.299, 0.587, 0.114, 0};
ADDR R7.xyz, R0, -R5.x;
MADR R0.xyz, R7, ${amount}.x, R5.x;


#contrast

// contrast around 0.5
MADR R0.xyz, R0, ${c}.x, ${c}.y;


#posterize

// posterise to ${levels} levels
MULR R7.xyz, R0, ${n}.x;
FLRR R7.xyz, R7;
MULR R0.xyz, R7, ${n}.y;


#invert

// invert
MADR R7.xyz, R0, ${m}.x, ${m}.y;
MADR R0.xyz, R7, ${m}.z, R0;


#grain
# The noise is a 32x32 tile: two lookups moved by the per-frame _NoiseJitter (one at 0.73 scale) hide the repeat.

// film grain: two jittered lookups of the dither noise (0 .. 8/255 each), recentred, scaled and added
MOVR R10.xy, _NoiseJitter;
MADR R9.xy, R10, {32, 0.73, 0, 0}.x, f[WPOS];
MADR R10.zw, f[WPOS].xxxy, {32, 0.73, 0, 0}.y, R10.xxxy;
MADR R10.zw, R10.xxyx, {19, 0, 0, 0}.x, R10;
TEXR R9.x, R9, preexpose_Noise;
TEXR R9.y, R10.zwzz, preexpose_Noise;
ADDR R9.x, R9.x, R9.y;
MADR R7.x, R9.x, ${scale1}.x, ${scale1}.y;
MADR R0.xyz, R7.x, ${scale2}.z, R0;


#chroma.off
MOVR R8.xyz, H2.w;
