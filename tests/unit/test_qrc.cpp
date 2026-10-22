#include "builders.h"

#include "formats/qrc.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using moyou::qrc::Archive;
using moyou::qrc::FormatError;
using namespace moyou::test;

namespace {
const std::vector<std::pair<std::string, Bytes>> kFiles{
    {"LINE1.mnu", bytesOf("#MNU_1.0\nLENGTH:float:0.5\n")},
    {"override/night/LINE1.mnu", bytesOf("#MNU_1.0\nLENGTH:float:0.25\n")},
    {"lib/moyou/lines1.fpo", Bytes{1, 2, 3, 4, 5, 6, 7}},
};
}  // namespace

TEST_CASE("a QRCF container lists its files in tree order with their contents", "[qrc]") {
    const Archive a = Archive::fromBytes(makeQrcf(kFiles));
    REQUIRE(a.ids() == std::vector<std::string>{"LINE1.mnu", "override/night/LINE1.mnu", "lib/moyou/lines1.fpo"});
    for (const auto &[id, payload] : kFiles) CHECK(a.file(id) == payload);
    CHECK(a.contains("LINE1.mnu"));
    CHECK_FALSE(a.contains("LINE2.mnu"));
    CHECK_THROWS_AS(a.file("LINE2.mnu"), FormatError);
    CHECK_FALSE(a.modified());
}

TEST_CASE("an unchanged archive rebuilds byte for byte", "[qrc]") {
    const Bytes raw = makeQrcf(kFiles);
    CHECK(Archive::fromBytes(raw).qrcf() == raw);
}

TEST_CASE("replaced files survive a rebuild, including a size change", "[qrc]") {
    Archive a = Archive::fromBytes(makeQrcf(kFiles));
    const Bytes longer(100, 0xab);
    a.replace("lib/moyou/lines1.fpo", longer);
    a.replace("LINE1.mnu", bytesOf("#MNU_1.0\nLENGTH:float:0.75\n"));
    CHECK(a.modified());
    const Archive b = Archive::fromBytes(a.qrcf());
    CHECK(b.ids() == a.ids());
    CHECK(b.file("lib/moyou/lines1.fpo") == longer);
    CHECK(b.file("LINE1.mnu") == bytesOf("#MNU_1.0\nLENGTH:float:0.75\n"));
    CHECK(b.file("override/night/LINE1.mnu") == kFiles[1].second);
    CHECK_THROWS_AS(a.replace("missing.mnu", {}), FormatError);
}

TEST_CASE("the compressed QRCC form round-trips through save and load", "[qrc]") {
    const Archive a = Archive::fromBytes(makeQrcf(kFiles));
    const Bytes qrcc = a.save();
    REQUIRE(qrcc.size() > 8);
    CHECK(std::string(qrcc.begin(), qrcc.begin() + 4) == "QRCC");
    CHECK(Archive::fromBytes(qrcc).qrcf() == a.qrcf());

    const auto path = std::filesystem::temp_directory_path() / "moyou_unit_test.qrc";
    a.saveTo(path);
    const Archive b = Archive::load(path);
    std::filesystem::remove(path);
    CHECK(b.file("LINE1.mnu") == kFiles[0].second);
}

TEST_CASE("a new file is added at the end of the file table", "[qrc][add]") {
    Archive a = Archive::fromBytes(makeQrcf(kFiles));
    const Bytes note = bytesOf("Built with moyou\n");
    a.add("moyou.txt", note);
    CHECK(a.modified());

    const Archive b = Archive::fromBytes(a.save());
    REQUIRE(b.ids() == std::vector<std::string>{"LINE1.mnu", "override/night/LINE1.mnu", "lib/moyou/lines1.fpo", "moyou.txt"});
    CHECK(b.file("moyou.txt") == note);
    for (const auto &[id, payload] : kFiles) CHECK(b.file(id) == payload);

    // the new node is linked the way the firmware's are: the old last file points to it, the table's last child is it
    const Bytes raw = b.qrcf();
    auto rd = [&](size_t o) { return uint32_t(raw[o]) << 24 | uint32_t(raw[o + 1]) << 16 | uint32_t(raw[o + 2]) << 8 | raw[o + 3]; };
    const uint32_t tree = rd(0x08), table = rd(tree + 20), last = rd(tree + table + 24), prev = rd(tree + last + 12);
    CHECK(rd(tree + prev + 16) == last);
    CHECK(rd(tree + last + 8) == table);
    CHECK(rd(tree + last + 16) == 0xffffffffu);
    const uint32_t ids = rd(0x10), idEntry = rd(tree + last + 28 + 16 + 8);
    CHECK(rd(ids + idEntry) == last);   // the id entry points back at its node
}

TEST_CASE("adding twice, or adding an existing id, keeps one copy", "[qrc][add]") {
    Archive a = Archive::fromBytes(makeQrcf(kFiles));
    a.add("moyou.txt", bytesOf("one"));
    a.add("moyou.txt", bytesOf("two"));
    a.add("LINE1.mnu", bytesOf("#MNU_1.0\n"));
    const Archive b = Archive::fromBytes(a.qrcf());
    CHECK(b.ids().size() == kFiles.size() + 1);
    CHECK(b.file("moyou.txt") == bytesOf("two"));
    CHECK(b.file("LINE1.mnu") == bytesOf("#MNU_1.0\n"));

    Archive c = Archive::fromBytes(b.qrcf());   // and again on a file that already has it
    c.add("second.txt", bytesOf("x"));
    CHECK(Archive::fromBytes(c.qrcf()).file("moyou.txt") == bytesOf("two"));
    CHECK(Archive::fromBytes(c.qrcf()).file("second.txt") == bytesOf("x"));
}

TEST_CASE("malformed containers are refused instead of crashing or hanging", "[qrc][malformed]") {
    SECTION("not a container") {
        CHECK_THROWS_AS(Archive::fromBytes(bytesOf("hello")), FormatError);
        CHECK_THROWS_AS(Archive::fromBytes(Bytes{}), FormatError);
    }
    SECTION("truncated") {
        Bytes raw = makeQrcf(kFiles);
        raw.resize(0x60);
        CHECK_THROWS_AS(Archive::fromBytes(raw), FormatError);
    }
    SECTION("a payload past the end of the file") {
        Bytes raw = makeQrcf(kFiles);
        raw.resize(raw.size() - 4);
        CHECK_THROWS_AS(Archive::fromBytes(raw), FormatError);
    }
    SECTION("sibling links that loop") {
        Bytes raw = makeQrcf(kFiles);
        const uint32_t tree = uint32_t(raw[8]) << 24 | uint32_t(raw[9]) << 16 | uint32_t(raw[10]) << 8 | raw[11];
        be32(raw, tree + 16, 0);   // the root's next sibling is the root
        CHECK_THROWS_AS(Archive::fromBytes(raw), FormatError);
    }
    SECTION("a compressed header that claims 4 GB") {
        Bytes qrcc = Archive::fromBytes(makeQrcf(kFiles)).save();
        be32(qrcc, 4, 0xffffffffu);
        CHECK_THROWS_AS(Archive::fromBytes(qrcc), FormatError);
    }
    SECTION("a corrupt zlib stream") {
        Bytes qrcc = Archive::fromBytes(makeQrcf(kFiles)).save();
        for (size_t i = 10; i < qrcc.size(); i++) qrcc[i] ^= 0x5a;
        CHECK_THROWS_AS(Archive::fromBytes(qrcc), FormatError);
    }
}
