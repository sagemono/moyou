#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace moyou::mnu {

struct Entry {
    std::string name;
    std::string type;    // "int" or "float"
    double value = 0;
};

std::vector<Entry> parse(const std::string &text);
std::string fromBytes(const std::vector<uint8_t> &bytes);   // latin-1
std::vector<uint8_t> toBytes(const std::string &text);

std::string formatValue(double v, bool isInt);

std::string patch(const std::string &text, const std::map<std::string, double> &values, std::vector<std::string> *found = nullptr, bool appendMissing = false);

}  // namespace moyou::mnu
