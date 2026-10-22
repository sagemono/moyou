// RSX assembler: listing -> assemble -> write reproduces every program byte for byte.
// Programs come from every qgl archive of 4.93 and 1.00 and, when present, the Lineschive archive (work/lineschive).
// Usage: test_asm <research_dir>   (exit 77 when no lines.qrc is available)

#include "formats/qrc.h"
#include "rsx/asm.h"
#include "rsx/rsx.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace moyou;

namespace {
int failures = 0, shown = 0;
void fail(const std::string &id, const std::string &what) {
    failures++;
    if (shown++ < 25) std::printf("FAIL  %s: %s\n", id.c_str(), what.c_str());
}

std::string paramDiff(const rsx::Param &a, const rsx::Param &b) {
    if (a.name != b.name) return "name";
    if (a.referenced != b.referenced) return "referenced " + std::to_string(a.referenced) + "->" + std::to_string(b.referenced);
    if (a.embedded != b.embedded) return "embedded offsets";
    if (a.defaults != b.defaults) return "defaults";
    if (a.resIndex != b.resIndex || a.resource != b.resource) return "resource";
    return {};
}
}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { std::puts("usage: test_asm <research_dir>"); return 77; }
    const fs::path root = argv[1];
    // every archive in the qgl folder (lines, icons, earth, canyon, raf, rhm, store) of both firmwares
    std::vector<fs::path> qrcs;
    for (const char *rel : {"work/dev_flash_root/dev_flash/vsh/resource/qgl", "work/fw100/dev_flash_root/dev_flash/vsh/resource/qgl"})
        if (fs::exists(root / rel))
            for (const auto &e : fs::directory_iterator(root / rel))
                if (e.path().extension() == ".qrc") qrcs.push_back(e.path());
    if (qrcs.empty()) { std::puts("skipped: no extracted lines.qrc"); return 77; }
    if (fs::exists(root / "work/lineschive"))
        for (const auto &e : fs::recursive_directory_iterator(root / "work/lineschive"))
            if (e.path().extension() == ".qrc") qrcs.push_back(e.path());

    std::set<std::vector<uint8_t>> seen;
    int programs = 0, withXor = 0, xorLines = 0, lines = 0;
    std::map<std::string, int> xorByProgram;
    for (const auto &q : qrcs) {
        qrc::Archive a = qrc::Archive::load(q);
        for (const auto &id : a.ids()) {
            if (!id.ends_with(".vpo") && !id.ends_with(".fpo")) continue;
            const auto &bin = a.file(id);
            if (!seen.insert(bin).second) continue;
            programs++;
            try {
                const rsx::Program p = rsx::parse(bin);
                if (rsx::write(p, bin) != bin) { fail(id, "write(parse(f)) != f"); continue; }
                const std::string text = rsx::listing(p, id);
                int nx = 0, nl = 0;
                for (size_t b = 0, e; b < text.size(); b = e + 1) {
                    e = text.find('\n', b);
                    if (e == std::string::npos) e = text.size();
                    const std::string l = text.substr(b, e - b);
                    if (!l.empty() && l.rfind("//", 0) != 0) { nl++; if (l.find("!xor(") != std::string::npos) nx++; }
                }
                lines += nl;
                xorLines += nx;
                if (nx) { withXor++; xorByProgram[id] = nx; }
                const rsx::Program r = rsx::assemble(text, p);
                if (r.ucode != p.ucode) { fail(id, "assembled microcode differs"); continue; }
                if (r.params.size() != p.params.size()) { fail(id, "parameter count differs"); continue; }
                bool ok = true;
                for (size_t k = 0; k < p.params.size() && ok; k++)
                    if (auto d = paramDiff(p.params[k], r.params[k]); !d.empty()) {
                        fail(id, "parameter " + p.params[k].name + ": " + d);
                        ok = false;
                    }
                if (!ok) continue;
                const auto out = rsx::write(r, bin);
                if (out != bin) {
                    size_t k = 0;
                    while (k < out.size() && k < bin.size() && out[k] == bin[k]) k++;
                    char m[200];
                    if (p.vertex)
                        std::snprintf(m, sizeof m, "rebuilt file differs at byte 0x%zx: vp regs %u/%u in 0x%x/0x%x out 0x%x/0x%x", k,
                                      r.vp.registerCount, p.vp.registerCount, r.vp.inputMask, p.vp.inputMask, r.vp.outputMask, p.vp.outputMask);
                    else
                        std::snprintf(m, sizeof m, "rebuilt file differs at byte 0x%zx: fp slots %u/%u regs %u/%u in 0x%x/0x%x tc 0x%x/0x%x 2d 0x%x/0x%x", k,
                                      r.fp.instructionCount, p.fp.instructionCount, r.fp.registerCount, p.fp.registerCount, r.fp.inputMask,
                                      p.fp.inputMask, r.fp.texcoordMask, p.fp.texcoordMask, r.fp.texcoord2d, p.fp.texcoord2d);
                    fail(id, m);
                    continue;
                }
            } catch (const rsx::AsmError &e) {
                fail(id, "line " + std::to_string(e.line()) + ": " + e.what());
            } catch (const std::exception &e) {
                fail(id, e.what());
            }
        }
    }
    std::printf("%d distinct programs, %d instruction lines, %d with !xor (%d programs)\n", programs, lines, xorLines, withXor);
    int k = 0;
    for (const auto &[id, n] : xorByProgram) if (k++ < 12) std::printf("  !xor %3d  %s\n", n, id.c_str());
    std::printf("%s  listing -> assemble -> write round trip (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
