# Using moyou

## What you need

- **`lines.qrc`** from firmware 4.93: `dev_flash/vsh/resource/qgl/lines.qrc`. With RPCS3, install the PS3 firmware (File > Install Firmware) and the file is in RPCS3's `dev_flash` folder.
- Firmware 1.00's `lines.qrc` also loads, and gets the 1.00 frame graph decompiled from its custom_render_plugin: no particles, no night colour maps, its own camera near plane, glare source and tone mapping. As on a real 1.00 console the wave's clock stays at 00:00: a static black background and the night preset, whatever the date. Clock > Run the 1.00 day cycle runs the time-of-day code that is in the 1.00 files anyway.
- **Optional:** a decrypted `custom_render_plugin` (`.prx` or `.elf`) for the wave's exact start states. RPCS3 makes one with Utilities > Decrypt PS3 Binaries on `dev_flash/vsh/module/custom_render_plugin.sprx`. Without it, the rope starts straight and is run until it moves like the PS3's (about half a second at load).
- A GPU with OpenGL 4.3.

## The window

| | |
|---|---|
| File > Open lines.qrc / Open from RPCS3 | load the file (RPCS3: pick the folder with `rpcs3.exe`), or drop `lines.qrc` on the window |
| Parameters dock | tabs for the clock and time-of-day presets, Wave / Background / HDR / Particles sliders, **lines.qrc** (every value in the file in an editable, filterable tree) and debug views |
| Bold labels | your edits; the reset button (or Edit > Revert All Edits) hands values back to the time-of-day schedule |
| Simulation menu | pause (Space), step (.), reset the rope, restart the particles |
| File > Save / Save As | write the edits into the file: the base `.mnu` and the four time-of-day presets get each value. The first save over a file keeps the original as `.orig`. Saved files also get a small `moyou.txt` inside crediting moyou; the XMB never reads it |
| File > Install to RPCS3 | save into RPCS3's `dev_flash` (restart the XMB to see it) |
| View > Shader Editor (Ctrl+E) | edit the wave's GPU programs as RSX assembly (below) |
| F12 | save a 1920×1080 screenshot of the display buffer |
| M / H / F11, double-click | parameters dock / hide all UI / full screen |

Every value in the file is modelled, including the non-default ones (FFD shaders 0-3 with the console's cross-fade, the procedural and black background shaders, HDR on/off, glare on/off and glare-only, the wave's draw switches). The eight entries shown greyed in the lines.qrc tab change nothing on a PS3 either; their tooltips say why.

## Shader editor

The wave is drawn by RSX programs stored in `lines.qrc` (`.vpo` vertex and `.fpo` fragment programs), and the console runs whatever is in the file. The Shader Editor shows each one as editable assembly in the NV40 syntax `sce-cgcdisasm` prints, with names where the binary has them:

```
TEXR R1.xyz, f[TEX0], _Back;                      // sample the background map
MADR_m8 R1.xyz, H2.w, {1, 0.35, 0.1, 0}, R2;      // orange wave instead of the colour gradient
MOV R0.y, _Brightness.x  &  RSQ R1.x, R1.x;       // vertex: vector and scalar unit in one slot
```

Apply (Ctrl+Return) assembles the program and swaps it into the live view. File > Save writes it into `lines.qrc`, ready for testing inside RPCS3 or a console. The list shows which programs the XMB actually draws: editing the others changes nothing. Uniforms keep working by name, literals `{x, y, z, w}` get constant registers allocated, and a fragment program can use a uniform the compiler had dropped (listed as unused), which the XMB then fills in.

No compiler is involved, and none ships with moyou: the assembler is moyou's own, the exact inverse of its disassembler. Every program in the 4.93 and 1.00 `lines.qrc` and in the Lineschive collection lists and assembles back to the identical file.

## Effects

The Effects tab rebuilds `LinesController` (the pass that puts the wave over the background) from the stock program with effects switched on by check boxes and sliders: wave tint, rainbow, chromaticaberration, vignette, scanlines, saturation, contrast, posterise, invert and animated film grain. 

It applies live and saves like any other edit; Open in Assembly shows the generated program. A file whose LinesController was already modified by a mod can't be rebuilt, and the tab says so.

## Command line

```
moyou-cli info lines.qrc                        list the files in the archive
moyou-cli extract lines.qrc outdir              unpack it
moyou-cli patch file.qrc edits.txt out.qrc      apply "FILE.NAME = value" edits (File > Copy edits as text)
                      [--all-overrides]         every preset in the archive, not just the time-of-day ones
moyou-cli transpile file.vpo|fpo                RSX program -> GLSL
moyou-cli disasm file.vpo|fpo                   RSX program disassembly
moyou-cli list file.vpo|fpo                     editable assembly listing
moyou-cli asm source.txt orig.fpo out.fpo       assemble a listing against the original's parameters
moyou-cli replace lines.qrc id file out.qrc     swap one file in a .qrc (e.g. lib/moyou/lines1.fpo)
moyou-cli effects lines.qrc "spec" out.qrc      LinesController effects, e.g. "vignette, grain.amount=0.1"
                      (--list, --source lines.qrc "spec")
moyou --snapshot lines.qrc out.png [--time HH:MM] [--month M] [--frames N] [--prx file] [--edit KEY=V] [--no-particles]
```

## Known differences from the console

Controller interaction (icon wind, d-pad, SIXAXIS) and the boot animations are not modelled. Particle quads run the PS3 vertex program as written; RPCS3 draws them twice as long along their u axis, which no reading of the program reproduces.