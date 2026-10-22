#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace moyou::image {

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Rgba {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;   // RGBA8, rows in file order (row 0 = first row stored in the file)
};

Rgba decodeDds(const std::vector<uint8_t> &file);
Rgba decodeTga(const std::vector<uint8_t> &file);

}  // namespace moyou::image
