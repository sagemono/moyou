#include "builders.h"

#include "rsx/asm.h"
#include "rsx/rsx.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace rsx = moyou::rsx;
using namespace moyou::test;
using Catch::Matchers::ContainsSubstring;

namespace {
// Assembles `source` into the hand-made fragment program and returns the CgBinary.
Bytes build(const std::string &source) {
    const Bytes empty = makeEmptyFragmentProgram();
    return rsx::write(rsx::assemble(source, rsx::parse(empty)), empty);
}
}  // namespace

TEST_CASE("the hand-made fragment program parses", "[rsx]") {
    const rsx::Program p = rsx::parse(makeEmptyFragmentProgram());
    CHECK_FALSE(p.vertex);
    CHECK(p.profile == 7004);
    REQUIRE(p.params.size() == 2);
    CHECK(p.params[0].name == "uv");
    CHECK(p.params[0].semantic == "TEXCOORD0");
    CHECK(p.params[1].name == "_Tex");
    CHECK(p.ucode.empty());
}

TEST_CASE("assembled fragment code lists back as written and round-trips byte for byte", "[rsx][asm]") {
    const std::string source = "TEXR R1, f[TEX0], _Tex;\n"
                               "MULR R0, R1, {0.5, 0.25, 1, 2};\n";
    const Bytes bin = build(source);
    const rsx::Program p = rsx::parse(bin);
    CHECK(p.ucode.size() == 3 * 16);   // two instructions and the inline constant
    const std::string text = rsx::listing(p, "test.fpo");
    CHECK_THAT(text, ContainsSubstring("TEXR R1, f[TEX0], _Tex;"));
    CHECK_THAT(text, ContainsSubstring("MULR R0, R1, {0.5, 0.25, 1, 2};"));

    // listing -> assemble -> write reproduces the binary exactly, which is what makes hand edits safe
    CHECK(rsx::write(rsx::assemble(text, p), bin) == bin);
}

TEST_CASE("the fragment program transpiles to GLSL", "[rsx][glsl]") {
    const rsx::Program p = rsx::parse(build("TEXR R1, f[TEX0], _Tex;\nMULR R0, R1, {0.5, 0.25, 1, 2};\n"));
    const std::string glsl = rsx::transpile(p, {}, "test.fpo");
    CHECK_THAT(glsl, ContainsSubstring("uniform sampler2D Tex;   // TEXUNIT0"));   // the leading _ is dropped
    CHECK_THAT(glsl, ContainsSubstring("R1 = texture(Tex, (v_TEXCOORD0).xy);"));
    CHECK_THAT(glsl, ContainsSubstring("R0 = R1 * vec4(0.5, 0.25, 1.0, 2.0);"));
    CHECK_THAT(glsl, ContainsSubstring("o_COLOR0 = R0;"));
    CHECK(glsl.back() == '\n');
}

TEST_CASE("assembler errors name the line", "[rsx][asm]") {
    const rsx::Program empty = rsx::parse(makeEmptyFragmentProgram());
    try {
        rsx::assemble("MOVR R0, f[TEX0];\nFROB R0, R1;\n", empty);
        FAIL("no error for an unknown opcode");
    } catch (const rsx::AsmError &e) {
        CHECK(e.line() == 2);
    }
    CHECK_THROWS_AS(rsx::assemble("TEXR R0, f[TEX0], _Missing;\n", empty), rsx::AsmError);
}

TEST_CASE("malformed CgBinary files are refused", "[rsx][malformed]") {
    CHECK_THROWS_AS(rsx::parse(Bytes(16, 0)), rsx::FormatError);

    Bytes table = makeEmptyFragmentProgram();
    be32(table, 12, 1000);   // parameter table runs past the end
    CHECK_THROWS_AS(rsx::parse(table), rsx::FormatError);

    Bytes header = makeEmptyFragmentProgram();
    be32(header, 20, uint32_t(header.size()));   // program header past the end
    CHECK_THROWS_AS(rsx::parse(header), rsx::FormatError);

    const Bytes good = makeEmptyFragmentProgram();
    const rsx::Program p = rsx::assemble("MOVR R0, f[TEX0];\n", rsx::parse(good));
    Bytes inside = good;
    be32(inside, 20, 8);   // program header inside the file header: write() must not trust it
    CHECK_THROWS(rsx::write(p, inside));
}
