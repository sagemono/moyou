#include "builders.h"

#include "formats/image.h"

#include <catch2/catch_test_macros.hpp>

namespace image = moyou::image;
using namespace moyou::test;

namespace {
std::vector<uint8_t> px(const image::Rgba &img, int x, int y) {
    const auto at = img.pixels.begin() + (ptrdiff_t(y) * img.width + x) * 4;
    return {at, at + 4};
}
}  // namespace

TEST_CASE("an A8R8G8B8 DDS decodes to RGBA in file row order", "[image][dds]") {
    Bytes d = makeDdsHeader(2, 2);
    for (int v : {0x10, 0x20, 0x30, 0x40,  0x50, 0x60, 0x70, 0x80,  0x90, 0xa0, 0xb0, 0xc0,  0xd0, 0xe0, 0xf0, 0xff})
        d.push_back(uint8_t(v));   // stored B, G, R, A
    const auto img = image::decodeDds(d);
    REQUIRE(img.width == 2);
    REQUIRE(img.height == 2);
    CHECK(px(img, 0, 0) == std::vector<uint8_t>{0x30, 0x20, 0x10, 0x40});
    CHECK(px(img, 1, 1) == std::vector<uint8_t>{0xf0, 0xe0, 0xd0, 0xff});
}

TEST_CASE("a DXT1 block uses its two endpoints and the 1/3, 2/3 mixes", "[image][dds]") {
    Bytes d = makeDdsHeader(4, 4, "DXT1");
    // c0 = white (0xffff) > c1 = black (0x0000): four colours; indices 0, 1, 2, 3 across the first row
    for (int v : {0xff, 0xff, 0x00, 0x00, 0xe4, 0x00, 0x00, 0x00}) d.push_back(uint8_t(v));
    const auto img = image::decodeDds(d);
    CHECK(px(img, 0, 0) == std::vector<uint8_t>{255, 255, 255, 255});
    CHECK(px(img, 1, 0) == std::vector<uint8_t>{0, 0, 0, 255});
    CHECK(px(img, 2, 0) == std::vector<uint8_t>{170, 170, 170, 255});
    CHECK(px(img, 3, 0) == std::vector<uint8_t>{85, 85, 85, 255});
    CHECK(px(img, 0, 1) == std::vector<uint8_t>{255, 255, 255, 255});   // index 0 everywhere else
}

TEST_CASE("a DXT1 block with c0 <= c1 has a midpoint and transparent black", "[image][dds]") {
    Bytes d = makeDdsHeader(4, 4, "DXT1");
    for (int v : {0x00, 0x00, 0xff, 0xff, 0xe4, 0x00, 0x00, 0x00}) d.push_back(uint8_t(v));
    const auto img = image::decodeDds(d);
    CHECK(px(img, 2, 0) == std::vector<uint8_t>{127, 127, 127, 255});
    CHECK(px(img, 3, 0) == std::vector<uint8_t>{0, 0, 0, 0});
}

TEST_CASE("bad DDS files are refused", "[image][dds][malformed]") {
    CHECK_THROWS_AS(image::decodeDds(Bytes(10, 0)), image::FormatError);
    CHECK_THROWS_AS(image::decodeDds(makeDdsHeader(2, 2)), image::FormatError);           // no pixel data
    CHECK_THROWS_AS(image::decodeDds(makeDdsHeader(0, 2)), image::FormatError);           // zero width
    CHECK_THROWS_AS(image::decodeDds(makeDdsHeader(100000, 2)), image::FormatError);      // absurd width
    CHECK_THROWS_AS(image::decodeDds(makeDdsHeader(4, 4, "DXT1")), image::FormatError);   // no block
    CHECK_THROWS_AS(image::decodeDds(makeDdsHeader(4, 4, "ATI2")), image::FormatError);   // unsupported
}

TEST_CASE("truecolour TGA decodes 24 and 32 bit pixels", "[image][tga]") {
    Bytes t24 = makeTgaHeader(2, 1, 24);
    for (int v : {1, 2, 3, 4, 5, 6}) t24.push_back(uint8_t(v));   // B, G, R
    const auto a = image::decodeTga(t24);
    CHECK(px(a, 0, 0) == std::vector<uint8_t>{3, 2, 1, 255});
    CHECK(px(a, 1, 0) == std::vector<uint8_t>{6, 5, 4, 255});

    Bytes t32 = makeTgaHeader(1, 1, 32);
    for (int v : {1, 2, 3, 4}) t32.push_back(uint8_t(v));   // B, G, R, A
    CHECK(px(image::decodeTga(t32), 0, 0) == std::vector<uint8_t>{3, 2, 1, 4});
}

TEST_CASE("bad TGA files are refused", "[image][tga][malformed]") {
    CHECK_THROWS_AS(image::decodeTga(Bytes(5, 0)), image::FormatError);
    CHECK_THROWS_AS(image::decodeTga(makeTgaHeader(256, 0, 32)), image::FormatError);   // zero height
    CHECK_THROWS_AS(image::decodeTga(makeTgaHeader(2, 2, 32)), image::FormatError);     // no pixel data
    CHECK_THROWS_AS(image::decodeTga(makeTgaHeader(2, 2, 16)), image::FormatError);     // 16-bit
}
