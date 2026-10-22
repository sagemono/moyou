# moyou internals

A map of the source for contributors. moyou replays the XMB wave the way the PS3 draws it, so most of the code reproduces a specific piece of firmware behaviour, and the comments say which.

## Source layout

| Folder | What it holds |
|---|---|
| `src/formats` | `lines.qrc` (QRCC/QRCF container), `.mnu` parameter files, DDS and TGA textures |
| `src/rsx` | RSX vertex and fragment programs: CgBinary parsing, microcode decoding, the disassembly listing, GLSL emission (`glsl.cpp`), and moyou's own assembler (`asm.cpp`), which turns a listing back into microcode |
| `src/sim` | The parameters and time-of-day schedule (`params`), the 19×19 rope simulation, FFD deformation and camera (`wave`) |
| `src/particles` | The particle system: the PPU emitter and the SPU update step, ported bit for bit |
| `src/render` | The OpenGL replay of the frame graph (`renderer`), GL helpers, the LinesController effects, and the GLSL sources it compiles in (`*.glsl`, `linescontroller.asm`) |
| `src/app` | The Qt UI: main window, parameter dock, shader editor, effects tab, headless snapshot |
| `src/cli.cpp` | `moyou-cli` |
| `resources/icons` | `moyou.svg` is the icon; the PNGs (window icon) and `moyou.ico` (Windows executable) are rendered from it by `tools/make_icons.py` (`pip install resvg-py pillow`). Run it after editing the SVG and commit the results |

`moyou_core` (everything but `src/app` and `main.cpp`) has no widgets; the app and the CLI link it.

## How a frame is drawn

1. `sim::Params` picks the parameter values for the current time of day from the `.mnu` files.
2. `sim::Wave` steps the rope and applies the FFD deformation, giving the 19×19 control points the SPU job would.
3. `render::Renderer` runs the console's passes in order: background (`back_colours0`), the wave sheet (`lines1`), `LinesController`, the HDR glare chain (`GlareSourcePre`, `Copy`, mips, `Gaussian`, `AccGlare`), `ToneApplyDisplay`, then the particles. Every pass uses the RSX program from the user's `lines.qrc`, transpiled to GLSL at load time. `patchFP` / `patchVP` route texture fetches through models of the RSX samplers (`rsx_lib.glsl`).

Firmware 1.00 has its own frame graph (`Params::firmware == 100`); the comments mark where it differs.

## Firmware addresses in comments

Comments cite the firmware code a piece of logic was taken from, as an address like `0x4bad0`. Unless the comment says otherwise, it is a function in `custom_render_plugin.sprx` of firmware 4.93, decrypted via scetool and loaded at its default base. Comments about firmware 1.00 code refer to that firmware's `custom_render_plugin`. `func_xxxx` names are in the SPU jobs (`spline.elf`, `particles.elf`), and `qgl 0x…` is the qglbase library. Offsets like `layer+0xd0` are fields of the plugin's objects.

## Rules

- **No firmware data in the repository.** `.gitignore` blocks the usual extensions. Read what you need from the user's files at runtime, as the effects tab does with the stock LinesController.
- **Native Qt UI.** Standard widgets and the platform style: no style sheets, palettes or forced styles.
- **Bit-exact code stays bit-exact.** The particle step and the RSX models round where the hardware does. Do not rearrange their arithmetic.

## Checking a change

Run the unit tests (`ctest -L unit`), and add one in `tests/unit` for a parser or format change: `tests/unit/builders.h` makes containers, images and programs from scratch. The tests that compare against the console need firmware data that is not public (see [BUILDING.md](BUILDING.md#tests)), so also check by hand:

- `moyou --snapshot lines.qrc out.png [--time HH:MM] [--month M] [--frames N] [--prx file] [--edit KEY=V]` renders one frame without a window.
- `moyou --selftest out.png lines.qrc [--tab N] [--shaders] [--apply id file] [--effects "spec"] [--save out.qrc] [--edit "KEY=V"]... [--time HH:MM] [--size WxH] [--delay MS]` runs the real window for four seconds (or `--delay`) and saves a grab of it. The README screenshot is `--size 1600x1180 --time 15:00 --shaders --tab 2 --effects "rainbow, vignette"` with the four `BACKGROUND.n RED/GREEN/BLUE` colours edited to blue (1, 2) and violet (3, 4). It keeps its own settings, so it does not touch your last file or view options.
- `moyou-cli` covers the file formats and the assembler: `info`, `extract`, `patch`, `transpile`, `disasm`, `list`, `asm`, `replace`, `effects`.
