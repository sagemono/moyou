#include "builders.h"

#include "render/effects.h"
#include "rsx/asm.h"
#include "rsx/rsx.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>

namespace render = moyou::render;
namespace rsx = moyou::rsx;
using namespace moyou::test;

TEST_CASE("every effect and value has an id and a sane range", "[effects]") {
    for (const auto &fx : render::linesControllerEffects()) {
        CHECK(std::string(fx.id).find_first_of(" ,.=") == std::string::npos);
        for (const auto &p : fx.params) {
            CHECK(p.min < p.max);
            CHECK(p.step > 0);
            CHECK(p.def >= p.min);
            CHECK(p.def <= p.max);
        }
    }
}

TEST_CASE("an effect spec names effects and their non-default values", "[effects]") {
    const auto s = render::parseEffectSpec(" vignette , scanlines.dark=0.3, rainbow.cycles = 2 ");
    CHECK(s.any());
    CHECK(s.on.at("vignette"));
    CHECK(s.on.at("scanlines"));
    CHECK(s.on.at("rainbow"));
    CHECK(s.values.at("scanlines.dark") == 0.3);
    CHECK(s.values.at("rainbow.cycles") == 2);
    CHECK_FALSE(render::parseEffectSpec("").any());
}

TEST_CASE("effectSpec and parseEffectSpec are inverses", "[effects]") {
    for (const char *spec : {"vignette", "grain, grain.amount=0.25", "chroma, chroma.offset=7.5, invert, invert.mix=0.5"}) {
        const auto s = render::parseEffectSpec(spec);
        CHECK(render::effectSpec(s) == spec);
        CHECK(render::effectSpec(render::parseEffectSpec(render::effectSpec(s))) == spec);
    }
}

TEST_CASE("bad effect specs say what is wrong", "[effects]") {
    CHECK_THROWS_AS(render::parseEffectSpec("sparkle"), std::invalid_argument);
    CHECK_THROWS_AS(render::parseEffectSpec("vignette.radius=2"), std::invalid_argument);
    CHECK_THROWS_AS(render::parseEffectSpec("vignette.strength"), std::invalid_argument);
}

TEST_CASE("the effects refuse a LinesController that is not the stock one", "[effects]") {
    // moyou ships none of the stock program: it is cut out of the file's own LinesController, so anything else
    // (here an unrelated program, and garbage) has to be refused rather than rebuilt as something it never was
    const Bytes empty = makeEmptyFragmentProgram();
    const Bytes other = rsx::write(rsx::assemble("TEXR R0, f[TEX0], _Tex;\n", rsx::parse(empty)), empty);
    CHECK_THROWS_WITH(render::linesControllerSource({}, other), Catch::Matchers::ContainsSubstring("not the firmware 4.93"));
    CHECK_THROWS_AS(render::linesControllerSource({}, Bytes(8, 0)), std::runtime_error);
}
