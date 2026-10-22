# Third-party notices

moyou's own code is under the MIT licence (see `LICENSE`). The Windows release also contains these libraries,
each under its own licence. Their full licence texts are in the `licenses` folder of the release zip, one file per
vcpkg port.

| Library | Licence | Notes |
|---|---|---|
| [Qt 6](https://www.qt.io/) (Core, Gui, Widgets, OpenGL, OpenGLWidgets) | LGPL-3.0 | Linked dynamically. You may replace the Qt DLLs with your own build of the same Qt version. Source: https://download.qt.io/official_releases/qt/ |
| [zlib](https://zlib.net/) | zlib licence | |
| Qt's own dependencies (ICU, FreeType, HarfBuzz, PCRE2, libpng, Brotli, bzip2, double-conversion, md4c, …) | various permissive licences | as built by vcpkg |

moyou contains no Sony code or data. It reads the user's own firmware files at runtime.
