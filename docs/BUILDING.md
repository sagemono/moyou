# Building moyou

## Requirements

- CMake 3.25 or newer
- A C++20 compiler: Visual Studio 2022 on Windows, GCC 12+ or Clang 15+ on Linux
- Qt 6.5 or newer (Core, Gui, Widgets, OpenGL, OpenGLWidgets) and zlib - either from the distribution (Linux) or
  from [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` pointing at it (`vcpkg.json` lists the features)
- For the tests, Catch2 3. When CMake finds none (most distributions still ship Catch2 2) it downloads 3.9.1;
  `-DMOYOU_FETCH_CATCH2=OFF` turns that off and `-DBUILD_TESTING=OFF` skips the tests

## Windows

```
set VCPKG_ROOT=C:\path\to\vcpkg          :: cmd
$env:VCPKG_ROOT = "C:\path\to\vcpkg"     # PowerShell (its `set` does not set environment variables)
build.bat
```

The first manifest build compiles Qt, which takes a while. If your vcpkg already has `qtbase`, `zlib` and `catch2` installed for `x64-windows`, reuse them:

```
build.bat classic
```

Output: `build\windows[-classic]\Release\moyou.exe` and `moyou-cli.exe`. A post-build step runs `windeployqt`, so the build runs in place.

To package a zip with everything needed to run it:

```
powershell -ExecutionPolicy Bypass -File packaging\package.ps1 -Preset windows-classic
```

## Linux

With the distribution's Qt 6 and zlib, which is much quicker than building Qt:

```
./build.sh # preset linux-system, output build/linux-system/moyou
```

Fedora: `sudo dnf install qt6-qtbase-devel zlib-ng-compat-devel cmake ninja-build gcc-c++`. 
Debian/Ubuntu: `sudo apt install qt6-base-dev libgl1-mesa-dev zlib1g-dev cmake ninja-build g++`.

To build the dependencies from the vcpkg manifest instead, set `VCPKG_ROOT`; `build.sh` then picks the `linux` preset (or pass one: `./build.sh linux`). Qt needs the usual X11/OpenGL development packages to build through vcpkg (on Ubuntu: `libgl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-dev libx11-xcb-dev libxcb*-dev libfontconfig1-dev`, plus `autoconf automake libtool pkg-config`).

## Tests

```
ctest --preset windows-classic      # Windows
ctest --preset linux-system         # Linux
```

There are two kinds.

**Unit tests** (`tests/unit`, Catch2) build every input in memory: small `lines.qrc` containers, `.mnu` files, DDS / DXT / TGA images, and a fragment program that goes through the assembler, the listing and the GLSL emitter. They need no firmware and run anywhere; `ctest -L unit` runs just these, and `moyou_tests` runs them directly with Catch2's options (`moyou_tests "[qrc]"`).

| test | checks |
|---|---|
| `transpile` | every RSX program in lines.qrc transpiles and disassembles byte-identically to the reference tools |
| `particles` | one SPU step and the vertex buffers bit-exact against the RPCS3 guest memory dump |
| `formats` | lines.qrc round trip, edits identical to the reference patcher, presets, textures, rope tables |
| `asm` | every RSX program of both firmwares (and the Lineschive mods) round-trips through the assembler byte for byte |
