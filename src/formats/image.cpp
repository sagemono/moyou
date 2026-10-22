#include "formats/image.h"

#include <algorithm>

namespace moyou::image {

namespace {
uint32_t le32(const std::vector<uint8_t> &d, size_t o) {
    if (o + 4 > d.size()) throw FormatError("image: truncated header");
    return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24;
}
}  // namespace

namespace {
// BC1 colour block (also the colour half of DXT3/5): 5:6:5 endpoints widened by bit replication, the two middle
// entries at 1/3 and 2/3. In a DXT1 block with c0 <= c1, entry 2 is the midpoint and entry 3 transparent black.
void bc1Block(const uint8_t *b, bool dxt1, uint8_t out[16][4]) {
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    auto rgb = [](uint16_t c, int *o) {
        const int r = c >> 11, g = (c >> 5) & 63, bl = c & 31;
        o[0] = r << 3 | r >> 2; o[1] = g << 2 | g >> 4; o[2] = bl << 3 | bl >> 2;
    };
    int pal[4][4];
    rgb(c0, pal[0]); rgb(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    const bool four = !dxt1 || c0 > c1;
    for (int k = 0; k < 3; k++) {
        pal[2][k] = four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
        pal[3][k] = four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
    }
    pal[2][3] = 255;
    pal[3][3] = four ? 255 : 0;
    const uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) out[i][k] = uint8_t(pal[(idx >> (2 * i)) & 3][k]);
}

// DXT1 / DXT3 / DXT5 top level (the Lineschive mods ship DXT1 month maps)
Rgba decodeDxt(const std::vector<uint8_t> &d, int w, int h, uint32_t fourcc) {
    const bool dxt1 = fourcc == 0x31545844u, dxt3 = fourcc == 0x33545844u, dxt5 = fourcc == 0x35545844u;
    if (!dxt1 && !dxt3 && !dxt5) throw FormatError("DDS: unsupported compressed format (DXT1/3/5 only)");
    const size_t bw = size_t(w + 3) / 4, bh = size_t(h + 3) / 4, bs = dxt1 ? 8 : 16;
    if (128 + bw * bh * bs > d.size()) throw FormatError("DDS: truncated pixel data");
    Rgba img;
    img.width = w; img.height = h;
    img.pixels.resize(size_t(w) * h * 4);
    for (size_t by = 0; by < bh; by++)
        for (size_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = &d[128 + (by * bw + bx) * bs];
            uint8_t px[16][4];
            bc1Block(b + (dxt1 ? 0 : 8), dxt1, px);
            if (dxt3)
                for (int i = 0; i < 16; i++) { const int a = (b[i / 2] >> (4 * (i & 1))) & 15; px[i][3] = uint8_t(a * 17); }
            if (dxt5) {
                int a[8] = {b[0], b[1]};
                for (int k = 2; k < 8; k++)
                    a[k] = a[0] > a[1] ? ((8 - k) * a[0] + (k - 1) * a[1]) / 7
                         : k < 6 ? ((6 - k) * a[0] + (k - 1) * a[1]) / 5 : (k == 6 ? 0 : 255);
                uint64_t bits = 0;
                for (int k = 0; k < 6; k++) bits |= uint64_t(b[2 + k]) << (8 * k);
                for (int i = 0; i < 16; i++) px[i][3] = uint8_t(a[(bits >> (3 * i)) & 7]);
            }
            for (int i = 0; i < 16; i++) {
                const size_t x = bx * 4 + size_t(i & 3), y = by * 4 + size_t(i >> 2);
                if (x < size_t(w) && y < size_t(h)) std::copy_n(px[i], 4, &img.pixels[(y * size_t(w) + x) * 4]);
            }
        }
    return img;
}
}  // namespace

Rgba decodeDds(const std::vector<uint8_t> &d) {
    if (d.size() < 128 || d[0] != 'D' || d[1] != 'D' || d[2] != 'S' || d[3] != ' ') throw FormatError("not a DDS file");
    Rgba img;
    img.height = int(le32(d, 12));
    img.width = int(le32(d, 16));
    const uint32_t pfFlags = le32(d, 80), bits = le32(d, 88);
    if (img.width <= 0 || img.height <= 0 || img.width > 16384 || img.height > 16384) throw FormatError("DDS: bad size");
    if (pfFlags & 0x4) return decodeDxt(d, img.width, img.height, le32(d, 84));
    if (!(pfFlags & 0x40) || bits != 32) throw FormatError("DDS: only A8R8G8B8 and DXT1/3/5 are supported");
    const uint32_t rm = le32(d, 92), gm = le32(d, 96), bm = le32(d, 100), am = le32(d, 104);
    if (rm != 0x00ff0000 || gm != 0x0000ff00 || bm != 0x000000ff || (am && am != 0xff000000))
        throw FormatError("DDS: unexpected channel masks");
    const size_t n = size_t(img.width) * img.height;
    if (img.width <= 0 || img.height <= 0 || 128 + n * 4 > d.size()) throw FormatError("DDS: truncated pixel data");
    img.pixels.resize(n * 4);
    for (size_t i = 0; i < n; i++) {               // little-endian BGRA -> RGBA
        const uint8_t *p = &d[128 + i * 4];
        img.pixels[i * 4] = p[2];
        img.pixels[i * 4 + 1] = p[1];
        img.pixels[i * 4 + 2] = p[0];
        img.pixels[i * 4 + 3] = am ? p[3] : 255;
    }
    return img;
}

Rgba decodeTga(const std::vector<uint8_t> &d) {
    if (d.size() < 18) throw FormatError("not a TGA file");
    const int idLen = d[0], cmap = d[1], type = d[2];
    Rgba img;
    img.width = d[12] | d[13] << 8;
    img.height = d[14] | d[15] << 8;
    const int bpp = d[16];
    if (type != 2 || cmap != 0 || (bpp != 24 && bpp != 32)) throw FormatError("TGA: only uncompressed 24/32-bit truecolour");
    if (img.width == 0 || img.height == 0) throw FormatError("TGA: bad size");
    const size_t n = size_t(img.width) * img.height, step = size_t(bpp / 8), base = 18 + size_t(idLen);
    if (base + n * step > d.size()) throw FormatError("TGA: truncated pixel data");
    img.pixels.resize(n * 4);
    for (size_t i = 0; i < n; i++) {               // stored B, G, R(, A)
        const uint8_t *p = &d[base + i * step];
        img.pixels[i * 4] = p[2];
        img.pixels[i * 4 + 1] = p[1];
        img.pixels[i * 4 + 2] = p[0];
        img.pixels[i * 4 + 3] = step == 4 ? p[3] : 255;
    }
    return img;
}

}  // namespace moyou::image
