#include "formats/mnu.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace moyou::mnu {

namespace {
std::vector<std::string> split(const std::string &s, char c) {
    std::vector<std::string> v;
    size_t a = 0;
    for (size_t b; (b = s.find(c, a)) != std::string::npos; a = b + 1) v.push_back(s.substr(a, b - a));
    v.push_back(s.substr(a));
    return v;
}
}  // namespace

std::vector<Entry> parse(const std::string &text) {
    std::vector<Entry> out;
    for (std::string line : split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto parts = split(line, ':');
        if (parts.size() < 3) continue;
        Entry e{parts[0], parts[1], 0};
        try { e.value = std::stod(parts[2]); } catch (...) { continue; }
        out.push_back(std::move(e));
    }
    return out;
}

std::string fromBytes(const std::vector<uint8_t> &bytes) { return std::string(bytes.begin(), bytes.end()); }
std::vector<uint8_t> toBytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

std::string formatValue(double v, bool isInt) {
    if (isInt) return std::to_string(std::llround(v));
    const float f = float(v);
    char buf[64];
    for (int p = 6; p <= 9; p++) {                 // the shortest %.*g that reads back as the same float
        std::snprintf(buf, sizeof buf, "%.*g", p, double(f));
        if (std::strtof(buf, nullptr) == f) return buf;
    }
    return buf;
}

std::string patch(const std::string &text, const std::map<std::string, double> &values,
                  std::vector<std::string> *found, bool appendMissing) {
    auto lines = split(text, '\n');
    std::set<std::string> hit;
    for (auto &line : lines) {
        const bool cr = !line.empty() && line.back() == '\r';
        std::string body = cr ? line.substr(0, line.size() - 1) : line;
        const auto parts = split(body, ':');
        if (parts.size() < 3 || body.empty() || body[0] == '#') continue;
        auto it = values.find(parts[0]);
        if (it == values.end()) continue;
        line = parts[0] + ":" + parts[1] + ":" + formatValue(it->second, parts[1] == "int") + (cr ? "\r" : "");
        hit.insert(parts[0]);
    }
    if (appendMissing) {
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        for (const auto &[k, v] : values)
            if (!hit.count(k)) { lines.push_back(k + ":float:" + formatValue(v, false)); hit.insert(k); }
        lines.push_back("");
    }
    if (found) found->assign(hit.begin(), hit.end());
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) out += (i ? "\n" : "") + lines[i];
    return out;
}

}  // namespace moyou::mnu
