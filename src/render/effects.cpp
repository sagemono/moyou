#include "render/effects.h"

#include "rsx/asm.h"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace moyou::render {

namespace {
constexpr double PI = 3.14159265358979323846;

// The sections of linescontroller.asm by name: "#name" starts one, "# text" and a bare "#" are template comments.
const std::map<std::string, std::string> &asmSections() {
    static const std::map<std::string, std::string> sections = [] {
        QFile f(":/moyou/render/linescontroller.asm");
        if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("render/linescontroller.asm is not in the resources");
        const QByteArray all = f.readAll();
        std::map<std::string, std::string> out;
        std::string name, body;
        auto flush = [&] {
            if (name.empty()) return;
            while (body.size() >= 2 && body[body.size() - 1] == '\n' && body[body.size() - 2] == '\n') body.pop_back();
            out[name] = body;
        };
        for (const QByteArray &raw : all.split('\n')) {
            const std::string line(raw.constData(), size_t(raw.size()));
            if (line.size() > 1 && line[0] == '#' && line[1] != ' ') {
                flush();
                name = line.substr(1);
                body.clear();
            } else if (!line.empty() && line[0] == '#') {
                continue;
            } else if (!name.empty()) {
                body += line + "\n";
            }
        }
        flush();
        return out;
    }();
    return sections;
}

std::string expand(const std::string &section, const std::map<std::string, std::string> &values) {
    auto it = asmSections().find(section);
    if (it == asmSections().end()) throw std::runtime_error("linescontroller.asm has no section #" + section);
    std::string out = it->second;
    for (const auto &[k, v] : values) {
        const std::string ph = "${" + k + "}";
        for (size_t p = out.find(ph); p != std::string::npos; p = out.find(ph, p + v.size()))
            out.replace(p, ph.size(), v);
    }
    if (const size_t p = out.find("${"); p != std::string::npos)
        throw std::runtime_error("linescontroller.asm #" + section + ": no value for " + out.substr(p, out.find('}', p) + 1 - p));
    return out;
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.8g", v);
    return b;
}
std::string vec(double x, double y = 0, double z = 0, double w = 0) {
    return "{" + num(x) + ", " + num(y) + ", " + num(z) + ", " + num(w) + "}";
}
std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool startsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

std::string joinLines(const std::vector<std::string> &lines) {
    std::string out;
    for (const auto &l : lines) out += (out.empty() ? "" : "\n") + l;
    return out;
}

// The stock parts of the 4.93 LinesController, cut out of the file's own program so that moyou ships none of it:
// the head up to the wave intensity decode (H2.w), and the exposure / tone lookup and dither after the compose.
// The program can be the stock one or one this file generated: both have the same head and tone instructions.
struct StockParts {
    std::vector<std::string> head, tone;
};

StockParts stockParts(std::span<const uint8_t> program) {
    std::vector<std::string> ins;
    const std::string text = rsx::listing(rsx::parse(program), "LinesController.fpo");
    for (size_t b = 0; b < text.size();) {
        size_t e = text.find('\n', b);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(b, e - b);
        if (const size_t c = line.find("//"); c != std::string::npos) line.erase(c);
        line = trim(line);
        if (!line.empty()) ins.push_back(line);
        b = e + 1;
    }
    const auto notStock = [] {
        return std::runtime_error("the file's LinesController is not the firmware 4.93 one, so the effects cannot rebuild it");
    };
    StockParts s;
    size_t i = 0;
    for (; i < ins.size(); i++) {
        if (ins[i] != "MOVR R4.xy, R1;") s.head.push_back(ins[i]);   // the screen uv copy this file adds
        if (startsWith(ins[i], "MULR R1.xy, ")) s.head.push_back("MOVR R4.xy, R1;");
        if (startsWith(ins[i], "MADR H2.w, ")) break;
    }
    if (i == ins.size() || std::count(s.head.begin(), s.head.end(), "MOVR R4.xy, R1;") != 1) throw notStock();
    // Between head and tone: the stock compose, or the one this file generates (then effects may follow the tone).
    std::vector<std::string> middle;
    for (i++; i < ins.size() && !startsWith(ins[i], "TEXR R2.xy, R1, preexpose_ExposeTex0;"); i++) middle.push_back(ins[i]);
    const bool generated = std::count(middle.begin(), middle.end(), "MADR_m8 R1.xyz, R8, R6, R2;") == 1;
    if (!generated && middle != std::vector<std::string>{"MADR_m8 R1.xyz, H2.w, f[TEX3], R2;"}) throw notStock();
    for (; i < ins.size(); i++) {
        s.tone.push_back(ins[i]);
        if (startsWith(ins[i], "MADR R0.xy, ")) break;
    }
    if (i == ins.size() || s.tone.size() > 8 || (!generated && i + 1 != ins.size())) throw notStock();
    return s;
}
}  // namespace

const std::vector<EffectInfo> &linesControllerEffects() {
    static const std::vector<EffectInfo> fx{
        {"chroma", "Chromatic aberration", "Samples the wave three times, shifted sideways for red and blue.",
         {{"offset", "Offset (pixels)", 0, 24, 0.5, 4}}},
        {"tint", "Wave tint", "Multiplies the wave colour (the time-of-day gradient) by a colour.",
         {{"r", "Red", 0, 2, 0.01, 1}, {"g", "Green", 0, 2, 0.01, 0.55}, {"b", "Blue", 0, 2, 0.01, 0.25}}},
        {"rainbow", "Rainbow wave", "A cosine rainbow across the screen, multiplied into the wave colour.",
         {{"cycles", "Cycles across the screen", 0.1, 6, 0.05, 1}, {"phase", "Phase", 0, 1, 0.01, 0},
          {"saturation", "Saturation", 0, 1, 0.01, 1}}},
        {"vignette", "Vignette", "Darkens towards the corners (before the HDR tone lookup, so glare follows).",
         {{"strength", "Strength", 0, 2, 0.01, 1.2}}},
        {"scanlines", "Scanlines", "Darkens every other band of rows. Sharp lines bend visibly through the glass icons.",
         {{"period", "Period (pixels)", 2, 32, 1, 4}, {"dark", "Darkness", 0, 1, 0.01, 0.22}}},
        {"saturation", "Saturation", "After tone mapping: 0 grey, 1 unchanged, above 1 more colourful.",
         {{"amount", "Amount", 0, 3, 0.01, 0.4}}},
        {"contrast", "Contrast", "After tone mapping, around mid grey.", {{"amount", "Amount", 0.25, 3, 0.01, 1.3}}},
        {"posterize", "Posterise", "After tone mapping: rounds each channel to a few levels.",
         {{"levels", "Levels", 2, 32, 1, 6}}},
        {"invert", "Invert", "After tone mapping.", {{"mix", "Mix", 0, 1, 0.01, 1}}},
        {"grain", "Film grain", "Animated: the XMB's per-frame dither noise, moved by _NoiseJitter every frame. Added last.",
         {{"amount", "Amount", 0, 0.5, 0.005, 0.08}}},
    };
    return fx;
}

double EffectSettings::value(const std::string &id, const EffectParam &p) const {
    auto it = values.find(id + "." + p.key);
    return it != values.end() ? it->second : p.def;
}

bool EffectSettings::any() const {
    for (const auto &[id, enabled] : on) if (enabled) return true;
    return false;
}

EffectSettings parseEffectSpec(const std::string &spec) {
    EffectSettings s;
    size_t b = 0;
    while (b <= spec.size()) {
        size_t e = spec.find(',', b);
        const std::string tok = trim(spec.substr(b, e == std::string::npos ? std::string::npos : e - b));
        if (!tok.empty()) {
            const size_t eq = tok.find('='), dot = tok.find('.');
            const std::string id = trim(tok.substr(0, std::min(dot, eq)));
            const EffectInfo *info = nullptr;
            for (const auto &f : linesControllerEffects()) if (id == f.id) info = &f;
            if (!info) throw std::invalid_argument("unknown effect \"" + id + "\"");
            s.on[id] = true;
            if (dot != std::string::npos && dot < eq) {
                if (eq == std::string::npos) throw std::invalid_argument("\"" + tok + "\" needs a value (id.key=value)");
                const std::string key = trim(tok.substr(dot + 1, eq - dot - 1));
                bool known = false;
                for (const auto &p : info->params) if (key == p.key) known = true;
                if (!known) throw std::invalid_argument("effect " + id + " has no value \"" + key + "\"");
                s.values[id + "." + key] = std::stod(tok.substr(eq + 1));
            }
        }
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return s;
}

std::string effectSpec(const EffectSettings &s) {
    std::string out;
    for (const auto &f : linesControllerEffects()) {
        auto it = s.on.find(f.id);
        if (it == s.on.end() || !it->second) continue;
        std::string part = f.id;
        for (const auto &p : f.params) {
            const double v = s.value(f.id, p);
            if (v != p.def) part += ", " + std::string(f.id) + "." + p.key + "=" + num(v);
        }
        out += (out.empty() ? "" : ", ") + part;
    }
    return out;
}

std::string linesControllerSource(const EffectSettings &s, std::span<const uint8_t> program) {
    const StockParts stock = stockParts(program);
    auto on = [&](const char *id) { auto it = s.on.find(id); return it != s.on.end() && it->second; };
    auto val = [&](const char *id, const char *key) {
        for (const auto &f : linesControllerEffects())
            if (std::string(f.id) == id)
                for (const auto &p : f.params) if (std::string(p.key) == key) return s.value(id, p);
        return 0.0;
    };
    std::string o;
    auto put = [&](const std::string &section, const std::map<std::string, std::string> &values = {}) {
        o += expand(section, values);
    };

    put("head", {{"spec", s.any() ? effectSpec(s) : std::string("no effects")}, {"stock", joinLines(stock.head)}});
    if (on("chroma")) {
        const double d = val("chroma", "offset") / 1920.0;
        put("chroma", {{"plus", vec(d)}, {"minus", vec(-d)}, {"edge", vec(0.5 / 1920, 1 - 0.5 / 1920)}});
    } else {
        put("chroma.off");
    }
    put("colour");
    if (on("tint")) put("tint", {{"rgb", vec(val("tint", "r"), val("tint", "g"), val("tint", "b"))}});
    if (on("rainbow")) {
        const double w = 2 * PI * val("rainbow", "cycles"), ph = 2 * PI * val("rainbow", "phase"), sat = val("rainbow", "saturation");
        put("rainbow", {{"freq", vec(w)}, {"phases", vec(ph, ph + 2 * PI / 3, ph + 4 * PI / 3)}, {"amp", vec(0.7 * sat, 1 - 0.4 * sat)}});
    }
    put("compose");
    if (on("vignette")) put("vignette", {{"k", vec(-val("vignette", "strength"), 1)}});
    if (on("scanlines")) {
        const double per = std::max(2.0, val("scanlines", "period")), dark = val("scanlines", "dark");
        put("scanlines", {{"step", vec(1 / per)}, {"dark", vec(-dark, 1)}, {"halfperiod", num(per / 2)}, {"percent", num(100 * (1 - dark))}});
    }
    put("tone", {{"stock", joinLines(stock.tone)}});
    if (on("saturation")) put("saturation", {{"amount", vec(val("saturation", "amount"))}});
    if (on("contrast")) {
        const double c = val("contrast", "amount");
        put("contrast", {{"c", vec(c, 0.5 - 0.5 * c)}});
    }
    if (on("posterize")) {
        const double n = std::max(2.0, std::round(val("posterize", "levels")));
        put("posterize", {{"n", vec(n - 0.001, 1 / (n - 1))}, {"levels", num(n)}});
    }
    if (on("invert")) put("invert", {{"m", vec(-2, 1, val("invert", "mix"))}});
    if (on("grain")) {
        const double a = val("grain", "amount");
        put("grain", {{"scale1", vec(15.9375, -0.5, a)}, {"scale2", vec(15.9375, -0.5, a * 1.6)}});
    }
    return o;
}

}  // namespace moyou::render
