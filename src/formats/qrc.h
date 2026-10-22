#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace moyou::qrc {

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Archive {
public:
    static Archive load(const std::filesystem::path &path);
    static Archive fromBytes(std::span<const uint8_t> bytes);   // QRCC or bare QRCF

    std::vector<std::string> ids() const;                       // file ids in tree order ("LINE1.mnu", "lib/moyou/lines1.fpo")
    bool contains(const std::string &id) const;
    const std::vector<uint8_t> &file(const std::string &id) const;   // throws FormatError if missing
    void replace(const std::string &id, std::vector<uint8_t> data);
    void add(const std::string &id, std::vector<uint8_t> data);   // a new file (replaces it if the id exists)
    bool modified() const { return modified_; }

    std::vector<uint8_t> qrcf() const;                          // rebuilt uncompressed container
    std::vector<uint8_t> save() const;                          // "QRCC" + size + zlib(qrcf, level 9)
    void saveTo(const std::filesystem::path &path) const;       // atomic write (temp file + rename)

private:
    struct Entry {
        std::string id;
        size_t attrPos;      // byte offset of the src attribute in raw_
        uint32_t offset;     // original payload offset in the data section
        uint32_t size;
        uint32_t node;       // offset of the file's node in the tree section
    };
    std::vector<uint8_t> raw_;               // original QRCF
    std::vector<Entry> entries_;             // tree order
    std::map<std::string, std::vector<uint8_t>> payload_;
    bool modified_ = false;
};

// The attribution note moyou adds to every lines.qrc it writes: "Built with moyou <version>" and the project page.
// The XMB only loads the files it asks for by name, so it never reads this one.
inline const std::string kNoteId = "moyou.txt";
void addNote(Archive &archive);

}  // namespace moyou::qrc
