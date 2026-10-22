#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace moyou::render {

struct EffectParam {
    const char *key, *label;
    double min, max, step, def;
};

struct EffectInfo {
    const char *id, *name, *description;
    std::vector<EffectParam> params;
};

// In the order they are applied: wave sample, wave colour, before the tone lookup, after it.
const std::vector<EffectInfo> &linesControllerEffects();

struct EffectSettings {
    std::map<std::string, bool> on;           // effect id -> enabled
    std::map<std::string, double> values;     // "id.key" -> value (missing: the default)
    double value(const std::string &id, const EffectParam &p) const;
    bool any() const;
};

// "vignette, scanlines.dark=0.3, rainbow.cycles=2": naming an effect (or one of its values) enables it.
// Throws std::invalid_argument on an unknown effect or value.
EffectSettings parseEffectSpec(const std::string &spec);
std::string effectSpec(const EffectSettings &s);   // the inverse (enabled effects, non-default values)

// The 4.93 LinesController with the enabled effects, as rsx::listing syntax. The stock instructions are taken from
// `program`, the file's own LinesController.fpo: the stock one, or one this function generated. Any other (a mod's
// own program) throws std::runtime_error, since the effects would replace what it changed.
std::string linesControllerSource(const EffectSettings &s, std::span<const uint8_t> program);

}  // namespace moyou::render
