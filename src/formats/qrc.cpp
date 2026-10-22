#include "formats/qrc.h"

#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <set>

namespace moyou::qrc {

namespace {

uint32_t be32(const std::vector<uint8_t> &d, size_t o) {
    if (o + 4 > d.size()) throw FormatError("qrc: truncated");
    return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}
void put32(std::vector<uint8_t> &d, size_t o, uint32_t v) {
    d[o] = uint8_t(v >> 24); d[o + 1] = uint8_t(v >> 16); d[o + 2] = uint8_t(v >> 8); d[o + 3] = uint8_t(v);
}
std::string cstr(const std::vector<uint8_t> &d, size_t o) {
    std::string s;
    while (o < d.size() && d[o]) s.push_back(char(d[o++]));
    return s;
}

constexpr size_t kMaxInflated = size_t(256) << 20;   // stock lines.qrc inflates to a few MB; the header is untrusted

std::vector<uint8_t> inflate(std::span<const uint8_t> in, size_t expect) {
    if (expect > kMaxInflated) throw FormatError("qrc: implausible uncompressed size");
    std::vector<uint8_t> out(std::max<size_t>(expect ? expect : in.size() * 4, 4096));
    z_stream z{};
    if (inflateInit(&z) != Z_OK) throw FormatError("qrc: zlib init failed");
    z.next_in = const_cast<Bytef *>(in.data());
    z.avail_in = uInt(in.size());
    int rc;
    do {
        if (z.total_out == out.size()) {
            if (out.size() >= kMaxInflated) { inflateEnd(&z); throw FormatError("qrc: uncompressed data too large"); }
            out.resize(std::min(out.size() * 2, kMaxInflated));
        }
        z.next_out = out.data() + z.total_out;
        z.avail_out = uInt(out.size() - z.total_out);
        rc = ::inflate(&z, Z_NO_FLUSH);
    } while (rc == Z_OK);
    inflateEnd(&z);
    if (rc != Z_STREAM_END) throw FormatError("qrc: corrupt zlib stream");
    out.resize(z.total_out);
    return out;
}

}  // namespace

Archive Archive::load(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw FormatError("cannot open " + path.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return fromBytes(bytes);
}

Archive Archive::fromBytes(std::span<const uint8_t> bytes) {
    Archive a;
    if (bytes.size() >= 8 && std::equal(bytes.begin(), bytes.begin() + 4, "QRCC")) {
        size_t size = size_t(bytes[4]) << 24 | size_t(bytes[5]) << 16 | size_t(bytes[6]) << 8 | bytes[7];
        a.raw_ = inflate(bytes.subspan(8), size);
    } else {
        a.raw_.assign(bytes.begin(), bytes.end());
    }
    const auto &d = a.raw_;
    if (d.size() < 0x38 || !std::equal(d.begin(), d.begin() + 4, "QRCF")) throw FormatError("not a QRC container");
    const uint32_t treeOff = be32(d, 8), idsOff = be32(d, 0x10), namesOff = be32(d, 0x18), dataOff = be32(d, 0x30);

    std::set<size_t> seen;                             // a node reached twice means the links loop
    std::function<void(int32_t, int)> walk = [&](int32_t rel, int depth) {
        if (depth > 64) throw FormatError("qrc: tree too deep");
        while (rel != -1) {
            const size_t o = treeOff + uint32_t(rel);
            if (!seen.insert(o).second) throw FormatError("qrc: tree links loop");
            const uint32_t name = be32(d, o), nattr = be32(d, o + 4);
            const int32_t next = int32_t(be32(d, o + 16)), first = int32_t(be32(d, o + 20));
            std::string id;
            size_t srcPos = 0;
            uint32_t off = 0, size = 0;
            for (uint32_t i = 0; i < nattr; i++) {
                const size_t ao = o + 28 + 16 * i;
                const std::string an = cstr(d, namesOff + be32(d, ao));
                if (an == "id") id = cstr(d, idsOff + be32(d, ao + 8) + 4);
                else if (an == "src") { srcPos = ao; off = be32(d, ao + 8); size = be32(d, ao + 12); }
            }
            if (cstr(d, namesOff + name) == "file" && srcPos) {
                if (size_t(dataOff) + off + size > d.size()) throw FormatError("qrc: payload out of range");
                a.entries_.push_back({id, srcPos, off, size, uint32_t(rel)});
                a.payload_[id].assign(d.begin() + dataOff + off, d.begin() + dataOff + off + size);
            }
            if (first != -1) walk(first, depth + 1);
            rel = next;
        }
    };
    walk(0, 0);
    return a;
}

std::vector<std::string> Archive::ids() const {
    std::vector<std::string> v;
    for (const auto &e : entries_) v.push_back(e.id);
    return v;
}

bool Archive::contains(const std::string &id) const { return payload_.count(id) != 0; }

const std::vector<uint8_t> &Archive::file(const std::string &id) const {
    auto it = payload_.find(id);
    if (it == payload_.end()) throw FormatError("lines.qrc has no " + id);
    return it->second;
}

void Archive::replace(const std::string &id, std::vector<uint8_t> data) {
    auto it = payload_.find(id);
    if (it == payload_.end()) throw FormatError("lines.qrc has no " + id);
    if (it->second != data) { it->second = std::move(data); modified_ = true; }
}

// The container keeps six sections, each an (offset, size) pair in the header from 0x08: the node tree, the id
// strings, the attribute names, two tables lines.qrc leaves empty, and the payloads. A node is 28 bytes (name,
// attribute count, then parent, previous sibling, next sibling, first child and last child, all offsets into the
// tree) followed by 16-byte attributes (name, type, value, size). A file node has a "src" attribute (type 6: offset
// and size in the payloads) and an "id" attribute (type 7: offset of its entry in the id strings, which is the
// node's own offset followed by the string, padded to 4 bytes). Every offset is relative to its section, so a new
// node and id can be appended and the sections laid out again with only the header changing.
void Archive::add(const std::string &id, std::vector<uint8_t> data) {
    if (contains(id)) { replace(id, std::move(data)); return; }
    if (entries_.empty()) throw FormatError("qrc: no file table to add " + id + " to");
    auto &d = raw_;
    uint32_t off[6], size[6];
    for (int k = 0; k < 6; k++) { off[k] = be32(d, 8 + 8 * size_t(k)); size[k] = be32(d, 12 + 8 * size_t(k)); }
    std::vector<uint8_t> sec[6];
    for (int k = 0; k < 5; k++) {
        if (size_t(off[k]) + size[k] > d.size()) throw FormatError("qrc: section out of range");
        sec[k].assign(d.begin() + off[k], d.begin() + off[k] + size[k]);
    }

    if (size_t(off[5]) + size[5] > d.size()) throw FormatError("qrc: section out of range");

    // a copy of an existing file node, linked in as the last child of the same parent (the file table)
    auto rel32 = [&](uint32_t rel, size_t at) { return be32(sec[0], rel + at); };
    const uint32_t tmpl = entries_.front().node;
    const uint32_t nattr = rel32(tmpl, 4), parent = rel32(tmpl, 8);
    if (parent == 0xffffffffu || parent + 28 > size[0]) throw FormatError("qrc: file node has no parent");
    const uint32_t last = rel32(parent, 24);
    while (sec[0].size() % 4) sec[0].push_back(0);
    const uint32_t node = uint32_t(sec[0].size());
    while (sec[1].size() % 4) sec[1].push_back(0);
    const uint32_t idRel = uint32_t(sec[1].size());
    sec[1].resize(idRel + 4);
    put32(sec[1], idRel, node);
    sec[1].insert(sec[1].end(), id.begin(), id.end());
    sec[1].push_back(0);

    sec[0].resize(node + 28 + 16 * size_t(nattr), 0);
    put32(sec[0], node, rel32(tmpl, 0));
    put32(sec[0], node + 4, nattr);
    put32(sec[0], node + 8, parent);
    put32(sec[0], node + 12, last);
    put32(sec[0], node + 16, 0xffffffffu);
    put32(sec[0], node + 20, 0xffffffffu);
    put32(sec[0], node + 24, 0xffffffffu);
    uint32_t srcAttr = 0;
    const std::string names(sec[2].begin(), sec[2].end());
    for (uint32_t i = 0; i < nattr; i++) {
        const uint32_t a = tmpl + 28 + 16 * i, b = node + 28 + 16 * i;
        const uint32_t nameOff = rel32(a, 0);
        const std::string an = nameOff < names.size() ? names.c_str() + nameOff : "";
        put32(sec[0], b, nameOff);
        put32(sec[0], b + 4, rel32(a, 4));
        if (an == "id") put32(sec[0], b + 8, idRel);
        if (an == "src") srcAttr = b;
    }
    if (!srcAttr) throw FormatError("qrc: file nodes have no src attribute");
    if (last != 0xffffffffu) put32(sec[0], last + 16, node);
    else put32(sec[0], parent + 20, node);
    put32(sec[0], parent + 24, node);

    // lay the sections out again (16-byte aligned, as the firmware's files are); payloads stay where they are
    std::vector<uint8_t> out(d.begin(), d.begin() + 0x40);
    for (int k = 0; k < 5; k++) {
        while (out.size() % 16) out.push_back(0);
        put32(out, 8 + 8 * size_t(k), uint32_t(out.size()));
        put32(out, 12 + 8 * size_t(k), uint32_t(sec[k].size()));
        out.insert(out.end(), sec[k].begin(), sec[k].end());
    }
    while (out.size() % 16) out.push_back(0);
    const uint32_t newTree = be32(out, 8), dataOff = uint32_t(out.size());
    for (auto &e : entries_) e.attrPos = e.attrPos - off[0] + newTree;
    put32(out, 0x30, dataOff);
    put32(out, 0x34, size[5]);
    out.insert(out.end(), d.begin() + off[5], d.begin() + off[5] + size[5]);
    raw_ = std::move(out);

    // sorts after every payload; 128-aligned so it does not lower the alignment qrcf() keeps; size 0: no tail padding
    entries_.push_back({id, newTree + srcAttr, (size[5] + 127) & ~127u, 0, node});
    payload_[id] = std::move(data);
    modified_ = true;
}

std::vector<uint8_t> Archive::qrcf() const {
    std::vector<uint8_t> d = raw_;
    const uint32_t dataOff = be32(d, 0x30), dataSize = be32(d, 0x34);
    std::vector<const Entry *> order;
    for (const auto &e : entries_) order.push_back(&e);
    std::sort(order.begin(), order.end(), [](const Entry *a, const Entry *b) { return a->offset < b->offset; });
    uint32_t align = 128;                              // largest power of two dividing every original offset
    for (const Entry *e : order) while (align > 1 && e->offset % align) align /= 2;
    std::vector<uint8_t> data;
    std::map<std::string, uint32_t> newOff;
    for (const Entry *e : order) {
        while (data.size() % align) data.push_back(0);
        newOff[e->id] = uint32_t(data.size());
        const auto &p = payload_.at(e->id);
        data.insert(data.end(), p.begin(), p.end());
    }
    if (!order.empty()) {                              // keep the original padding after the last payload
        const int64_t tail = int64_t(dataSize) - (int64_t(order.back()->offset) + order.back()->size);
        if (tail > 0) data.insert(data.end(), size_t(tail), 0);
    }
    for (const auto &e : entries_) {
        put32(d, e.attrPos + 8, newOff[e.id]);
        put32(d, e.attrPos + 12, uint32_t(payload_.at(e.id).size()));
    }
    d.resize(dataOff);
    put32(d, 0x34, uint32_t(data.size()));
    d.insert(d.end(), data.begin(), data.end());
    return d;
}

std::vector<uint8_t> Archive::save() const {
    const std::vector<uint8_t> q = qrcf();
    uLongf n = compressBound(uLong(q.size()));
    std::vector<uint8_t> out(8 + n);
    if (compress2(out.data() + 8, &n, q.data(), uLong(q.size()), 9) != Z_OK) throw FormatError("qrc: compression failed");
    out.resize(8 + n);
    std::copy_n("QRCC", 4, out.begin());
    put32(out, 4, uint32_t(q.size()));
    return out;
}

void Archive::saveTo(const std::filesystem::path &path) const {
    const std::vector<uint8_t> bytes = save();
    const auto tmp = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw FormatError("cannot write " + tmp.string());
        f.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) throw FormatError("write failed: " + tmp.string());
    }
    std::filesystem::rename(tmp, path);
}

void addNote(Archive &archive) {
    const std::string note = "Built with moyou " MOYOU_VERSION "\nhttps://github.com/sagemono/moyou\n";
    archive.add(kNoteId, std::vector<uint8_t>(note.begin(), note.end()));
}

}  // namespace moyou::qrc
