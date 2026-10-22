#include "builders.h"

#include "formats/mnu.h"
#include "formats/qrc.h"
#include "sim/params.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace mnu = moyou::mnu;
using namespace moyou::test;

namespace {
const std::string kFile = "#MNU_1.0\r\nSTATE:int:1\r\nLENGTH:float:0.5\r\n# a comment\r\nEND Y:float:-2.25\r\n";

double valueOf(const std::string &text, const std::string &name) {
    for (const auto &e : mnu::parse(text))
        if (e.name == name) return e.value;
    return NAN;
}
}  // namespace

TEST_CASE("parse reads NAME:type:value lines and skips the header and comments", "[mnu]") {
    const auto entries = mnu::parse(kFile);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].name == "STATE");
    CHECK(entries[0].type == "int");
    CHECK(entries[0].value == 1);
    CHECK(entries[1].name == "LENGTH");
    CHECK(entries[1].type == "float");
    CHECK(entries[1].value == 0.5);
    CHECK(entries[2].name == "END Y");   // names may contain spaces
    CHECK(entries[2].value == -2.25);
}

TEST_CASE("formatValue writes the shortest text that reads back as the same float", "[mnu]") {
    CHECK(mnu::formatValue(0.5, false) == "0.5");
    CHECK(mnu::formatValue(0.1, false) == "0.1");
    CHECK(mnu::formatValue(1.0 / 3, false) == "0.33333334");   // 7 digits would read back as a different float
    CHECK(mnu::formatValue(-2.25, false) == "-2.25");
    CHECK(mnu::formatValue(1e-5, false) == "1e-05");
    for (double v : {1.0 / 3, 0.1, 1e-5, 123456.789, -0.0078125, 3.4e38})
        CHECK(std::stof(mnu::formatValue(v, false)) == float(v));
    CHECK(mnu::formatValue(2.6, true) == "3");
    CHECK(mnu::formatValue(-1.4, true) == "-1");
}

TEST_CASE("patch rewrites only the named values and keeps every other byte", "[mnu]") {
    std::vector<std::string> found;
    const std::string out = mnu::patch(kFile, {{"LENGTH", 0.75}, {"STATE", 2}, {"NOPE", 1}}, &found);
    CHECK(out == "#MNU_1.0\r\nSTATE:int:2\r\nLENGTH:float:0.75\r\n# a comment\r\nEND Y:float:-2.25\r\n");
    CHECK(found == std::vector<std::string>{"LENGTH", "STATE"});
    CHECK(mnu::patch(kFile, {}) == kFile);
}

TEST_CASE("patch can append values the file does not have", "[mnu]") {
    const std::string out = mnu::patch("#MNU_1.0\nA:float:1\n", {{"B", 0.25}}, nullptr, true);
    CHECK(out == "#MNU_1.0\nA:float:1\nB:float:0.25\n");
}

TEST_CASE("applyEdits writes the base file and the four time-of-day presets only", "[mnu][edits]") {
    const std::string base = "#MNU_1.0\nLENGTH:float:0.5\n";
    moyou::qrc::Archive a = moyou::qrc::Archive::fromBytes(makeQrcf({
        {"LINE1.mnu", bytesOf(base)},
        {"override/night/LINE1.mnu", bytesOf(base)},
        {"override/day/LINE1.mnu", bytesOf(base)},
        {"override/coldboot1/LINE1.mnu", bytesOf(base)},
    }));
    const auto missing = moyou::sim::applyEdits(a, {{"LINE1.LENGTH", 0.25}, {"LINE1.NOPE", 1}});
    CHECK(missing == std::vector<std::string>{"LINE1.NOPE"});
    for (const char *id : {"LINE1.mnu", "override/night/LINE1.mnu", "override/day/LINE1.mnu"})
        CHECK(valueOf(mnu::fromBytes(a.file(id)), "LENGTH") == 0.25);
    CHECK(valueOf(mnu::fromBytes(a.file("override/coldboot1/LINE1.mnu")), "LENGTH") == 0.5);   // event preset

    moyou::sim::applyEdits(a, {{"LINE1.LENGTH", 0.125}}, true);
    CHECK(valueOf(mnu::fromBytes(a.file("override/coldboot1/LINE1.mnu")), "LENGTH") == 0.125);
}
