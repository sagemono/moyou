#pragma once
// Parameters (lines.qrc .mnu files), the time-of-day schedule and the VSH clock.
//
// Keys are "FILE.NAME" ("LINE1.LENGTH", "PARTICLES.emit vel min"). Presets come straight from the archive:
// base = LINE1/BACKGROUND/HDR/PARTICLES.mnu, and night/yoake/day/higure = override/<preset>/*.mnu (complete files).
// The schedule (custom_render_plugin 0x9c98c) blends presets by hour with a smoothstep; the VSH samples the RTC
// once a second and pushes each new time with a 1 s fade (5 s for the first), and parameters glide over that fade.

#include "formats/qrc.h"

#include <map>
#include <string>
#include <vector>

namespace moyou::sim {

using Table = std::map<std::string, double>;

struct Settings {
    bool follow = true;          // follow the local clock
    double dayFrac = 0.5;        // time of day in [0, 1)
    double month = 9.5;          // month index + day fraction ((mon-1) + (day-1)/daysInMonth)
    double lapse = 1;            // time-lapse multiplier when not following
    std::string preset = "schedule";   // or a fixed preset name
    int view = 0;                // 0 display, 1 wave buffer, 2 background map, 3 glare
    bool showFfd = false, showGrid = false;
    int state = 1;               // rope start state 0..2
    double simSpeed = 1;
    bool paused = false;
    bool glareOn = true, particlesOn = true;
    int brightLevel = 0;         // Theme Settings > Brightness, 0..5
    bool runClock100 = false;    // firmware 1.00: run the decompiled day cycle (the console's plugin clock stays at 00:00)
};

struct Segment { std::string a, b; double s = 0; };

// Writes edits ("FILE.NAME" -> value) into the archive the way the port means them: the value holds at every time
// of day, so FILE.mnu and override/{night,yoake,day,higure}/FILE.mnu all get it.
// allOverrides writes every copy of the file in the archive instead, whatever directory it sits in: lines.qrc's
// event presets (gameboot, welcome, coldboot, ...), the 21 in icons.qrc, earth.qrc's earth/presets/*.
// Returns the keys that matched no entry in any of those files.
std::vector<std::string> applyEdits(qrc::Archive &archive, const Table &edits, bool allOverrides = false);

class Params {
public:
    static const std::vector<std::string> &presetNames();   // night, yoake, day, higure, base
    void load(const qrc::Archive &archive);                  // throws qrc::FormatError if files are missing
    int firmware = 493;                                      // 100 for a firmware 1.00 lines.qrc (no Gaussian.vpo)

    Settings S;
    Table edits;

    double P(const std::string &key) const;                  // edit > schedule
    double scheduled(const std::string &key) const;          // NaN if the key is in no preset
    bool has(const std::string &key) const;
    const Table &preset(const std::string &name) const;

    // Clock (per frame). Returns true on the frame of an RTC tick.
    void syncClockFromDate();
    void advance(double dt);
    void forceTick() { clk.force = true; }

    struct Clock {
        double acc = 1e9; bool first = true; float dayFrac = 0.5f; double month = 9.5;
        float fadeT = 0; double fadeDur = 5, paramT = 0; bool snap = true, force = false;
        double bootFade = 5;
    } clk;
    Segment seg, segPrev;
    bool hasPrev = false;

    Segment schedule(double dayFrac) const;                  // the firmware's key table
    std::string segmentLabel() const;
    bool clockHeld() const { return firmware == 100 && !S.runClock100; }   // 1.00 as on the console: always 00:00

private:
    void clockTick();
    double segValue(const Segment &s, const std::string &key) const;
    std::map<std::string, Table> presets_;
};

}  // namespace moyou::sim
