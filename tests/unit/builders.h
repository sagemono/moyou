#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace moyou::test {

using Bytes = std::vector<uint8_t>;

inline void be32(Bytes &d, size_t at, uint32_t v) {
    if (d.size() < at + 4) d.resize(at + 4);
    d[at] = uint8_t(v >> 24); d[at + 1] = uint8_t(v >> 16); d[at + 2] = uint8_t(v >> 8); d[at + 3] = uint8_t(v);
}
inline void le32(Bytes &d, size_t at, uint32_t v) {
    if (d.size() < at + 4) d.resize(at + 4);
    d[at] = uint8_t(v); d[at + 1] = uint8_t(v >> 8); d[at + 2] = uint8_t(v >> 16); d[at + 3] = uint8_t(v >> 24);
}
inline void appendStr(Bytes &d, const std::string &s) {
    d.insert(d.end(), s.begin(), s.end());
    d.push_back(0);
}
inline Bytes bytesOf(const std::string &s) { return Bytes(s.begin(), s.end()); }

// An uncompressed QRCF container laid out like the firmware's: a "qrc" root, a "file-table" child, and one "file" node
// per entry in order. The header holds (offset, size) pairs from 0x08: tree, ids, names, two empty tables, data. A
// node is name, attribute count, parent, previous, next, first child, last child (tree offsets, -1 for none), then
// 16-byte attributes (name, type, value, size): "src" (type 6, payload offset and size) and "id" (type 7, offset of
// its id entry: the node's offset, then the string, padded to 4 bytes).
inline Bytes makeQrcf(const std::vector<std::pair<std::string, Bytes>> &files) {
    constexpr uint32_t kNone = 0xffffffffu;
    Bytes names;
    const uint32_t nQrc = 0;
    appendStr(names, "qrc");
    const uint32_t nTable = uint32_t(names.size());
    appendStr(names, "file-table");
    const uint32_t nFile = uint32_t(names.size());
    appendStr(names, "file");
    const uint32_t nSrc = uint32_t(names.size());
    appendStr(names, "src");
    const uint32_t nId = uint32_t(names.size());
    appendStr(names, "id");

    constexpr uint32_t kRoot = 0, kTable = 28, kFirst = 56, kFile = 28 + 2 * 16;
    auto fileNode = [](size_t i) { return kFirst + kFile * uint32_t(i); };
    Bytes ids, data;
    std::vector<uint32_t> idOff, dataOff;
    for (size_t i = 0; i < files.size(); i++) {
        while (ids.size() % 4) ids.push_back(0);
        idOff.push_back(uint32_t(ids.size()));
        be32(ids, ids.size(), fileNode(i));
        appendStr(ids, files[i].first);
        while (data.size() % 16) data.push_back(0);
        dataOff.push_back(uint32_t(data.size()));
        data.insert(data.end(), files[i].second.begin(), files[i].second.end());
    }

    Bytes tree(kFirst + kFile * files.size(), 0);
    auto node = [&](uint32_t o, uint32_t name, uint32_t nattr, uint32_t parent, uint32_t prev, uint32_t next,
                    uint32_t first, uint32_t last) {
        for (uint32_t v : {name, nattr, parent, prev, next, first, last}) { be32(tree, o, v); o += 4; }
    };
    const uint32_t firstFile = files.empty() ? kNone : fileNode(0), lastFile = files.empty() ? kNone : fileNode(files.size() - 1);
    node(kRoot, nQrc, 0, kNone, kNone, kNone, kTable, kTable);
    node(kTable, nTable, 0, kRoot, kNone, kNone, firstFile, lastFile);
    for (size_t i = 0; i < files.size(); i++) {
        const uint32_t o = fileNode(i);
        node(o, nFile, 2, kTable, i ? fileNode(i - 1) : kNone, i + 1 < files.size() ? fileNode(i + 1) : kNone, kNone, kNone);
        be32(tree, o + 28, nSrc);
        be32(tree, o + 32, 6);
        be32(tree, o + 36, dataOff[i]);
        be32(tree, o + 40, uint32_t(files[i].second.size()));
        be32(tree, o + 44, nId);
        be32(tree, o + 48, 7);
        be32(tree, o + 52, idOff[i]);
        be32(tree, o + 56, 0);
    }

    Bytes out = {'Q', 'R', 'C', 'F'};
    out.resize(0x40, 0);
    be32(out, 0x04, 0x110);
    uint32_t at[3];
    const Bytes *secs[3] = {&tree, &ids, &names};
    for (int k = 0; k < 3; k++) {
        while (out.size() % 16) out.push_back(0);
        at[k] = uint32_t(out.size());
        out.insert(out.end(), secs[k]->begin(), secs[k]->end());
        be32(out, 0x08 + 8 * size_t(k), at[k]);
        be32(out, 0x0c + 8 * size_t(k), uint32_t(secs[k]->size()));
    }
    while (out.size() % 16) out.push_back(0);
    const uint32_t dataStart = uint32_t(out.size());
    out.insert(out.end(), data.begin(), data.end());
    for (size_t pos : {0x20, 0x28, 0x30}) be32(out, pos, dataStart);   // the two empty tables sit at the data offset
    be32(out, 0x34, uint32_t(data.size()));
    return out;
}

// A 128-byte DDS header for a w x h top level: A8R8G8B8 (fourcc 0) or a DXTn fourcc such as "DXT1".
inline Bytes makeDdsHeader(int w, int h, const char *fourcc = nullptr) {
    Bytes d(128, 0);
    d[0] = 'D'; d[1] = 'D'; d[2] = 'S'; d[3] = ' ';
    le32(d, 12, uint32_t(h));
    le32(d, 16, uint32_t(w));
    if (fourcc) {
        le32(d, 80, 0x4);
        for (int i = 0; i < 4; i++) d[size_t(84 + i)] = uint8_t(fourcc[i]);
    } else {
        le32(d, 80, 0x41);
        le32(d, 88, 32);
        le32(d, 92, 0x00ff0000);
        le32(d, 96, 0x0000ff00);
        le32(d, 100, 0x000000ff);
        le32(d, 104, 0xff000000);
    }
    return d;
}

// An uncompressed truecolour TGA header (18 bytes) for w x h at 24 or 32 bits.
inline Bytes makeTgaHeader(int w, int h, int bpp) {
    Bytes d(18, 0);
    d[2] = 2;
    d[12] = uint8_t(w); d[13] = uint8_t(w >> 8);
    d[14] = uint8_t(h); d[15] = uint8_t(h >> 8);
    d[16] = uint8_t(bpp);
    return d;
}

// A CgBinary fragment program (sce_fp_rsx) with no microcode: a parameter table with one texture input (uv,
// TEXCOORD0) and one sampler (_Tex, TEXUNIT0), and a zeroed program header. rsx::assemble fills in the rest.
inline Bytes makeEmptyFragmentProgram() {
    constexpr uint32_t kParams = 2, kTable = 32, kBlob = kTable + 48 * kParams;
    Bytes d(kBlob, 0);
    be32(d, 0, 7004);
    be32(d, 12, kParams);
    be32(d, 16, kTable);
    const uint32_t nUv = uint32_t(d.size());
    appendStr(d, "uv");
    const uint32_t sUv = uint32_t(d.size());
    appendStr(d, "TEXCOORD0");
    const uint32_t nTex = uint32_t(d.size());
    appendStr(d, "_Tex");
    // type, resource, variability, resIndex, name, defaults, embedded, semantic, direction, paramno, referenced
    const uint32_t uv[11] = {1046, 3220, 4101, 0, nUv, 0, 0, sUv, 4097, 0, 1};
    const uint32_t tex[11] = {1066, 2048, 4102, 0, nTex, 0, 0, 0, 4097, 1, 1};
    for (int k = 0; k < 11; k++) {
        be32(d, kTable + 4 * size_t(k), uv[k]);
        be32(d, kTable + 48 + 4 * size_t(k), tex[k]);
    }
    while (d.size() % 16) d.push_back(0);
    const uint32_t progOff = uint32_t(d.size());
    d.resize(progOff + 32, 0);
    be32(d, 8, uint32_t(d.size()));
    be32(d, 20, progOff);
    be32(d, 24, 0);
    be32(d, 28, uint32_t(d.size()));
    return d;
}

}  // namespace moyou::test
