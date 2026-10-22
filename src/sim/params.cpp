#include "sim/params.h"

#include "formats/mnu.h"

#include <QDateTime>

#include <cmath>
#include <limits>
#include <set>

namespace moyou::sim {

namespace {
const char *const kFiles[] = {"LINE1", "BACKGROUND", "HDR", "PARTICLES", "LINE2"};
bool optional(const std::string &f) { return f == "PARTICLES" || f == "LINE2"; }   // 1.00 has LINE2, no PARTICLES
const double kHours[] = {0, 1, 5, 7, 11, 13, 17, 19, 23, 24};
const double kHours100[] = {0, 4.8, 6, 7.2, 8.4, 16.8, 18, 19.2, 20.4, 24};   // 1.00 table at 0x13669c
const char *const kSched[] = {"night", "night", "yoake", "yoake", "day", "day", "higure", "higure", "night", "night"};
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
}  // namespace

std::vector<std::string> applyEdits(qrc::Archive &a, const Table &edits, bool allOverrides) {
    std::map<std::string, std::map<std::string, double>> byFile;
    for (const auto &[k, v] : edits) {
        const auto dot = k.find('.');
        if (dot != std::string::npos) byFile[k.substr(0, dot)][k.substr(dot + 1)] = v;
    }
    std::vector<std::string> missing;
    for (const auto &[file, kv] : byFile) {
        std::set<std::string> hit;
        std::vector<std::string> ids{file + ".mnu"};
        if (allOverrides) {
            // Every copy of the file, whatever directory it sits in: lines.qrc also has the event presets
            // (gameboot, welcome, coldboot, ...), icons.qrc has 21 and earth.qrc nests them under earth/presets/.
            const std::string leaf = file + ".mnu";
            for (const auto &id : a.ids()) {
                const auto slash = id.find_last_of('/');
                if (slash != std::string::npos && id.substr(slash + 1) == leaf) ids.push_back(id);
            }
        } else {
            for (const char *p : {"night", "yoake", "day", "higure"}) ids.push_back(std::string("override/") + p + "/" + file + ".mnu");
        }
        for (const auto &id : ids) {
            if (!a.contains(id)) continue;
            std::vector<std::string> found;
            a.replace(id, mnu::toBytes(mnu::patch(mnu::fromBytes(a.file(id)), kv, &found)));
            hit.insert(found.begin(), found.end());
        }
        for (const auto &[k, v] : kv) if (!hit.count(k)) missing.push_back(file + "." + k);
    }
    return missing;
}

const std::vector<std::string> &Params::presetNames() {
    static const std::vector<std::string> n{"night", "yoake", "day", "higure", "base"};
    return n;
}

void Params::load(const qrc::Archive &a) {
    presets_.clear();
    firmware = a.contains("lib/glutils/Gaussian.vpo") ? 493 : 100;
    clk.bootFade = firmware == 100 ? 1 : 5;   // 1.00 has no 5 s boot fade
    for (const auto &name : presetNames()) {
        Table t;
        for (const char *f : kFiles) {
            const std::string id = name == "base" ? std::string(f) + ".mnu" : "override/" + name + "/" + f + ".mnu";
            if (!a.contains(id)) {
                if (name == "base" && !optional(f)) throw qrc::FormatError("lines.qrc has no " + id);
                continue;
            }
            for (const auto &e : mnu::parse(mnu::fromBytes(a.file(id)))) t[std::string(f) + "." + e.name] = e.value;
        }
        if (name == "base" && firmware != 100) {   // BACKGROUND keys absent from the base file use the compiled defaults (BackMenu ctor)
            const Table def{{"BACKGROUND.NIGHT BLEND", 1}, {"BACKGROUND.NIGHT2DAY BEGIN", 4}, {"BACKGROUND.NIGHT2DAY END", 6},
                            {"BACKGROUND.DAY2NIGHT BEGIN", 18}, {"BACKGROUND.DAY2NIGHT END", 20},
                            {"BACKGROUND.DAYSPREAD", 0}, {"BACKGROUND.NIGHT WHIT BIAS", 0.5}};
            for (const auto &[k, v] : def) t.emplace(k, v);
        }
        presets_[name] = std::move(t);
    }
    for (const auto &name : presetNames())   // a preset missing a file falls back to the base values
        for (const auto &[k, v] : presets_["base"]) presets_[name].emplace(k, v);
}

const Table &Params::preset(const std::string &name) const {
    static const Table empty;                       // nothing loaded yet: every key reads as missing (NaN)
    auto it = presets_.find(name);
    if (it != presets_.end()) return it->second;
    auto day = presets_.find("day");
    return day != presets_.end() ? day->second : empty;
}

bool Params::has(const std::string &key) const { return presets_.count("base") && presets_.at("base").count(key); }

Segment Params::schedule(double dayFrac) const {
    const double h = dayFrac * 24;
    const double *hours = firmware == 100 ? kHours100 : kHours;
    int k = 0;
    while (k < 8 && h >= hours[k + 1]) k++;
    const double t = std::min(1.0, std::max(0.0, (h - hours[k]) / (hours[k + 1] - hours[k])));
    return {kSched[k], kSched[k + 1], t * t * (3 - 2 * t)};
}

double Params::segValue(const Segment &s, const std::string &key) const {
    const Table &A = preset(s.a), &B = preset(s.b);
    auto ia = A.find(key), ib = B.find(key);
    if (ia == A.end() || ib == B.end()) return ia != A.end() ? ia->second : ib != B.end() ? ib->second : NaN;
    return ia->second + (ib->second - ia->second) * s.s;
}

double Params::scheduled(const std::string &key) const {
    if (S.preset != "schedule") {
        const Table &t = preset(S.preset);
        auto it = t.find(key);
        return it != t.end() ? it->second : NaN;
    }
    const double v = segValue(seg, key);
    if (!hasPrev || clk.paramT >= clk.fadeDur) return v;
    const double v0 = segValue(segPrev, key);
    return v0 + (v - v0) * (clk.paramT / clk.fadeDur);
}

double Params::P(const std::string &key) const {
    auto it = edits.find(key);
    return it != edits.end() ? it->second : scheduled(key);
}

void Params::syncClockFromDate() {
    const QDateTime now = QDateTime::currentDateTime();
    const QTime t = now.time();
    const QDate d = now.date();
    S.dayFrac = (t.hour() * 3600 + t.minute() * 60 + t.second() + t.msec() / 1000.0) / 86400.0;
    S.month = (d.month() - 1) + double(d.day() - 1) / d.daysInMonth();   // 0x10900
}

void Params::clockTick() {
    // A real 1.00 never moves its plugin clock off 00:00: black background map, night preset, every month alike
    const double secs = clockHeld() ? 0 : std::fmod(std::floor(S.dayFrac * 86400 + 1e-6), 86400.0);
    clk.dayFrac = float(secs / 86400);
    clk.month = S.month;
    hasPrev = !clk.first;
    if (hasPrev) segPrev = seg;
    seg = schedule(clk.dayFrac);
    clk.paramT = 0;
    clk.fadeT = 0;
    clk.fadeDur = clk.first ? clk.bootFade : 1;
    clk.first = false;
    clk.snap = true;
}

void Params::advance(double dt) {
    if (S.follow) syncClockFromDate();
    else if (S.lapse > 0) {
        S.dayFrac += dt * S.lapse / 86400;
        if (S.dayFrac >= 1) { S.dayFrac -= 1; S.month = std::fmod(S.month + 1.0 / 30, 12.0); }
    }
    clk.acc += dt;
    if (clk.acc >= 1 || clk.force) {
        clk.acc = clk.force ? 0 : std::fmod(clk.acc, 1.0);
        clk.force = false;
        clockTick();
    } else {
        clk.paramT = std::min(clk.fadeDur, clk.paramT + dt);
    }
}

std::string Params::segmentLabel() const {
    if (S.preset != "schedule") return S.preset + " (fixed)";
    if (clockHeld()) return seg.a + " (1.00 clock held at 00:00)";
    if (seg.a == seg.b || seg.s == 0) return seg.a;
    if (seg.s == 1) return seg.b;
    return seg.a + " → " + seg.b + " " + std::to_string(int(std::lround(seg.s * 100))) + "%";
}

}  // namespace moyou::sim
