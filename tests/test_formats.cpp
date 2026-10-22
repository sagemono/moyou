// lines.qrc container, .mnu patching, presets, textures and rope tables against the research checkout.
// Usage: test_formats <research_dir>   (exit 77 when the extracted firmware files are missing)

#include "formats/image.h"
#include "formats/mnu.h"
#include "formats/qrc.h"
#include "sim/params.h"
#include "sim/wave.h"

#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace moyou;

static int failures = 0;
static void check(const char *name, bool ok, const std::string &detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) failures++;
}

static std::vector<uint8_t> readFile(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int main(int argc, char **argv) {
    if (argc < 2) { std::puts("usage: test_formats <research_dir> [python_exe]"); return 77; }
    const fs::path root = argv[1];
    const fs::path systemPython = argc > 2 ? fs::path(argv[2]) : fs::path();
    const fs::path qrcPath = root / "work/dev_flash_root/dev_flash/vsh/resource/qgl/lines.qrc";
    if (!fs::exists(qrcPath)) { std::puts("skipped: no extracted lines.qrc"); return 77; }

    // 1. container round trip: the rebuilt QRCF equals the original, and save/load preserves every payload
    const auto raw = readFile(qrcPath);
    std::vector<uint8_t> orig(size_t(raw[4]) << 24 | size_t(raw[5]) << 16 | size_t(raw[6]) << 8 | raw[7]);
    uLongf n = uLongf(orig.size());
    uncompress(orig.data(), &n, raw.data() + 8, uLong(raw.size() - 8));
    qrc::Archive a = qrc::Archive::load(qrcPath);
    check("QRCF rebuilt byte-identical", a.qrcf() == orig, std::to_string(a.ids().size()) + " files");
    qrc::Archive b = qrc::Archive::fromBytes(a.save());
    bool same = b.ids() == a.ids();
    for (const auto &id : a.ids()) same = same && b.file(id) == a.file(id);
    check("save -> load keeps every payload", same);

    // 2. presets straight from the archive, against the values the WebGL port was validated with
    sim::Params p;
    p.load(a);
    auto near = [](double x, double y) { return std::fabs(x - y) < 1e-5; };
    check("night LINE1.SPACING", near(p.preset("night").at("LINE1.SPACING"), 408.298));
    check("day HDR.EXPOSURE", near(p.preset("day").at("HDR.EXPOSURE"), 1.05));
    check("yoake LINE1.ANG ROT", near(p.preset("yoake").at("LINE1.ANG ROT"), 18.1208));
    check("base LINE1.TIMESTEP", near(p.preset("base").at("LINE1.TIMESTEP"), 4));
    check("base BACKGROUND.NIGHT BLEND (compiled default)", near(p.preset("base").at("BACKGROUND.NIGHT BLEND"), 1));
    check("day BACKGROUND.NIGHT BLEND (override file)", near(p.preset("day").at("BACKGROUND.NIGHT BLEND"), 0.499947));
    check("higure PARTICLES.size middle present", p.preset("higure").count("PARTICLES.size middle") == 1);
    p.S.follow = false;
    p.S.preset = "schedule";
    p.S.dayFrac = 12.0 / 24;
    p.forceTick();
    p.advance(0);
    check("schedule at noon = day", p.seg.a == "day" && near(p.P("LINE1.SPACING"), p.preset("day").at("LINE1.SPACING")));

    // 3. edits -> archive, compared with tools/qrc_patch.py on the same edits
    const sim::Table edits{{"LINE1.LENGTH", 0.33}, {"PARTICLES.emit vel min", 0.25}, {"BACKGROUND.1 RED", 0.7},
                           {"HDR.EXPOSURE", 1.3}, {"LINE1.STATE", 2}, {"BACKGROUND.NIGHT2DAY BEGIN", 5}};
    qrc::Archive c = a;
    const auto missing = sim::applyEdits(c, edits);
    check("all edits matched an entry", missing.empty());
    const fs::path tmp = fs::temp_directory_path() / "moyou_test_formats";
    fs::create_directories(tmp);
    {
        std::ofstream e(tmp / "edits.txt");
        for (const auto &[k, v] : edits) e << k << " = " << mnu::formatValue(v, false) << "\n";
    }
    // qrc_patch.py only needs the standard library, so the research .venv or any python will do. The research
    // directory can be shared between a Windows and a Linux boot, so only look for this platform's venv layout.
#ifdef _WIN32
    fs::path py = root / ".venv/Scripts/python.exe";
#else
    fs::path py = root / ".venv/bin/python";
#endif
    if (!fs::exists(py)) py = systemPython;
    if (!py.empty() && fs::exists(py)) {
        const std::string line = "\"" + py.string() + "\" \"" + (root / "tools/qrc_patch.py").string() + "\" \"" + qrcPath.string() +
                                 "\" \"" + (tmp / "edits.txt").string() + "\" \"" + (tmp / "py.qrc").string() + "\" > \"" +
                                 (tmp / "py.log").string() + "\" 2>&1";
#ifdef _WIN32
        const std::string cmd = "\"" + line + "\"";   // cmd.exe /c strips one level of outer quotes
#else
        const std::string &cmd = line;
#endif
        const int rc = std::system(cmd.c_str());
        if (rc == 0 && fs::exists(tmp / "py.qrc")) {
            qrc::Archive ref = qrc::Archive::load(tmp / "py.qrc");
            std::string diff;
            for (const auto &id : ref.ids())
                if (ref.file(id) != c.file(id)) {
                    diff += id + ": python '" + mnu::fromBytes(ref.file(id)).substr(0, 0) + "' ";
                    const std::string x = mnu::fromBytes(ref.file(id)), y = mnu::fromBytes(c.file(id));
                    size_t k = 0;
                    while (k < x.size() && k < y.size() && x[k] == y[k]) k++;
                    diff += "at byte " + std::to_string(k) + " py=[" + x.substr(k > 20 ? k - 20 : 0, 40) + "] c++=[" +
                            y.substr(k > 20 ? k - 20 : 0, 40) + "]; ";
                }
            check("applyEdits == tools/qrc_patch.py (QRCF bytes)", ref.qrcf() == c.qrcf(), diff);
        } else {
            check("run tools/qrc_patch.py", false, "exit " + std::to_string(rc));
        }
    } else {
        std::puts("note: no python found, skipping the qrc_patch.py comparison");
    }
    sim::Params q;
    q.load(c);
    check("edited value in every scheduled preset", near(q.preset("night").at("LINE1.LENGTH"), 0.33) &&
                                                    near(q.preset("higure").at("LINE1.LENGTH"), 0.33) &&
                                                    near(q.preset("base").at("LINE1.LENGTH"), 0.33));
    check("event presets untouched", !c.contains("override/coldboot1/LINE1.mnu") ||
                                         mnu::fromBytes(c.file("override/coldboot1/LINE1.mnu")) ==
                                             mnu::fromBytes(a.file("override/coldboot1/LINE1.mnu")));

    // 4. textures
    bool dds = true;
    for (int k = 0; k < 2; k++)
        for (int mo = 1; mo <= 12; mo++) {
            char id[64];
            std::snprintf(id, sizeof id, "textures/month_bg/%s/%02d.dds", k ? "night" : "rgb", mo);
            const auto img = image::decodeDds(a.file(id));
            dds = dds && img.width == 64 && img.height == 32 && img.pixels.size() == 64 * 32 * 4;
        }
    check("24 month DDS decode to 64x32", dds);
    const auto fres = image::decodeTga(a.file("textures/TGA/freslut1.tga"));
    const auto part = image::decodeTga(a.file("particles/proc_iridescent.tga"));
    check("freslut1.tga 512x1", fres.width == 512 && fres.height == 1);
    check("proc_iridescent.tga 128x128", part.width == 128 && part.height == 128);

    // 5. rope start states from a decrypted custom_render_plugin, against the flattened image the port used
    const fs::path elf = root / "work/scetool_out/custom_render_plugin.elf";
    const fs::path bin = root / "work/ppu/crp.bin";
    if (fs::exists(elf) && fs::exists(bin)) {
        std::string err;
        const auto st = sim::RopeStates::fromPrx(elf, &err);
        bool ok = st.valid();
        if (ok) {
            const auto img = readFile(bin);
            const uint32_t tables[3][2] = {{0x95a40, 0x96b2c}, {0x97c18, 0x98d04}, {0x99df0, 0x9aedc}};
            size_t k = 0;
            for (const auto &t : tables)
                for (uint32_t va : t)
                    for (int i = 0; i < sim::NN * 3; i++, k++) {
                        const uint8_t *b4 = &img[va + size_t(i) * 4];
                        const uint32_t u = uint32_t(b4[0]) << 24 | uint32_t(b4[1]) << 16 | uint32_t(b4[2]) << 8 | b4[3];
                        float f;
                        std::memcpy(&f, &u, 4);
                        ok = ok && f == st.data[k];
                    }
        }
        check("rope tables from the decrypted PRX", ok, err);
    }
    fs::remove_all(tmp);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
