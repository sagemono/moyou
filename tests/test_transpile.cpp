// RSX transpiler / disassembler: byte-identical to the Python tools for every program extracted from lines.qrc.
// Usage: test_transpile <research_dir> <python_exe>
// References are generated at run time by running tools/rsx_to_glsl.py, tools/rsx_cgdis.py and the post_vp/post_fp
// functions of tools/gen_particles_js.py; nothing firmware-derived is stored in this repository.
// Exit codes: 0 all identical, 1 mismatch, 77 research data or python missing.
#include "rsx/rsx.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#define MOYOU_POPEN_READ "rb"   // _popen needs the explicit binary mode; glibc rejects anything but "r"/"w"
#else
#define MOYOU_POPEN_READ "r"
#endif

namespace fs = std::filesystem;
using namespace moyou::rsx;

namespace {

std::string quote(const std::string &s) { return "\"" + s + "\""; }

// Runs a command and returns its stdout with CRLF folded to LF (Python's text-mode stdout on Windows).
std::string run(const std::string &cmd, int *status = nullptr) {
#ifdef _WIN32
    std::string full = "\"" + cmd + "\"";  // cmd.exe /c strips one level of outer quotes
#else
    const std::string &full = cmd;
#endif
    std::string out;
    if (FILE *p = popen(full.c_str(), MOYOU_POPEN_READ)) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0)
            out.append(buf, n);
        int st = pclose(p);
        if (status)
            *status = st;
    } else if (status) {
        *status = -1;
    }
    std::string r;
    r.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i)
        if (!(out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n'))
            r += out[i];
    return r;
}

std::vector<uint8_t> readFile(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void writeFile(const fs::path &p, const std::string &s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

std::vector<std::string> splitLines(const std::string &s) {
    std::vector<std::string> l;
    std::istringstream in(s);
    for (std::string line; std::getline(in, line);)
        l.push_back(line);
    return l;
}

// Python declares the VP output locals in set iteration order (depends on PYTHONHASHSEED); sort them into
// RSX output-register order, which is what the C++ port emits.
std::string canonicalVpOutputs(const std::string &s) {
    static const std::vector<std::string> order = {
        "HPOS", "COL0", "COL1", "BFC0", "BFC1", "FOGC", "PSIZE", "TEXCOORD0", "TEXCOORD1", "TEXCOORD2",
        "TEXCOORD3", "TEXCOORD4", "TEXCOORD5", "TEXCOORD6", "TEXCOORD7", "TEXCOORD8"};
    const std::string pre = "    vec4 o_", post = " = vec4(0.0, 0.0, 0.0, 1.0);";
    auto key = [&](const std::string &line) -> int {
        if (!line.starts_with(pre) || !line.ends_with(post))
            return -1;
        auto name = line.substr(pre.size(), line.size() - pre.size() - post.size());
        auto it = std::find(order.begin(), order.end(), name);
        return it == order.end() ? -1 : int(it - order.begin());
    };
    auto lines = splitLines(s);
    for (size_t i = 0; i < lines.size();) {
        size_t j = i;
        while (j < lines.size() && key(lines[j]) >= 0)
            ++j;
        if (j > i + 1)
            std::sort(lines.begin() + ptrdiff_t(i), lines.begin() + ptrdiff_t(j),
                      [&](const std::string &a, const std::string &b) { return key(a) < key(b); });
        i = j == i ? i + 1 : j;
    }
    std::string r;
    for (auto &l : lines)
        r += l + "\n";
    if (!s.empty() && s.back() != '\n' && !r.empty())
        r.pop_back();
    return r;
}

struct Stats {
    int comparisons = 0, mismatches = 0;
};

void compare(Stats &st, const std::string &what, const std::string &expected, const std::string &got) {
    ++st.comparisons;
    if (expected == got)
        return;
    ++st.mismatches;
    auto e = splitLines(expected), g = splitLines(got);
    size_t i = 0;
    while (i < e.size() && i < g.size() && e[i] == g[i])
        ++i;
    std::cout << "MISMATCH " << what << " at line " << i + 1 << "\n"
              << "  python: " << (i < e.size() ? e[i] : "<eof>") << "\n"
              << "  c++   : " << (i < g.size() ? g[i] : "<eof>") << "\n";
    if (e.size() == g.size() && i == e.size())
        std::cout << "  (difference in trailing newlines: python " << expected.size() << " bytes, c++ " << got.size()
                  << " bytes)\n";
}

const char *kPostHelper = R"(import sys
sys.path.insert(0, sys.argv[1])
import gen_particles_js as g
src = open(sys.argv[3], encoding='latin-1', newline='').read()
try:
    out = g.post_vp(src) if sys.argv[2] == 'vp' else g.post_fp(src)
except Exception:
    out = '<<error>>'
sys.stdout.buffer.write(out.encode('latin-1'))
)";

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: test_transpile <research_dir> <python_exe>\n";
        return 77;
    }
    const fs::path research = argv[1];
    const std::string python = argv[2];
    const fs::path lib = research / "work" / "lines" / "lib";
    const fs::path tools = research / "tools";
    if (python.empty() || !fs::exists(python) || !fs::is_directory(lib) || !fs::exists(tools / "rsx_to_glsl.py")) {
        std::cout << "skipped: research data or python not found\n";
        return 77;
    }
    std::vector<fs::path> files;
    for (auto &e : fs::recursive_directory_iterator(lib))
        if (e.is_regular_file() && (e.path().extension() == ".vpo" || e.path().extension() == ".fpo"))
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cout << "skipped: no .vpo/.fpo under " << lib.string() << "\n";
        return 77;
    }

    const fs::path tmp = fs::temp_directory_path() / "moyou_test_transpile";
    fs::create_directories(tmp);
    const fs::path helper = tmp / "post_helper.py";
    writeFile(helper, kPostHelper);

    const std::string py = quote(python);
    const std::string glslTool = quote((tools / "rsx_to_glsl.py").generic_string());
    const std::string disTool = quote((tools / "rsx_cgdis.py").generic_string());
    Stats st;
    int particlePrograms = 0;
    for (const fs::path &f : files) {
        const std::string path = f.generic_string(), base = f.filename().string();
        Program prog;
        try {
            auto data = readFile(f);
            prog = parse(data);
        } catch (const std::exception &ex) {
            ++st.mismatches;
            std::cout << "PARSE ERROR " << path << ": " << ex.what() << "\n";
            continue;
        }
        struct Variant { const char *flags; Options opt; };
        for (const Variant &v : {Variant{"--hwprec", {true, false}}, Variant{"", {false, false}},
                                 Variant{"--hwprec --gl330", {true, true}}}) {
            std::string expected = run(py + " " + glslTool + " " + v.flags + " " + quote(path));
            std::string got = transpile(prog, Options(v.opt), base) + "\n";  // + print()'s newline
            if (prog.vertex)
                expected = canonicalVpOutputs(expected);
            compare(st, path + " [transpile " + v.flags + "]", expected, got);
        }
        compare(st, path + " [disasm]", run(py + " " + disTool + " " + quote(path)),
                disassemble(prog, path) + "\n");

        if (f.parent_path().filename() == "particles") {
            ++particlePrograms;
            for (bool hw : {false, true}) {
                std::string glsl = transpile(prog, Options{hw, false}, base);
                const fs::path in = tmp / (base + ".glsl");
                writeFile(in, glsl);
                std::string expected = run(py + " " + quote(helper.generic_string()) + " " +
                                           quote(tools.generic_string()) + " " + (prog.vertex ? "vp " : "fp ") +
                                           quote(in.generic_string()));
                std::string got;
                try {
                    got = prog.vertex ? postParticleVP(glsl) : postParticleFP(glsl);
                } catch (const FormatError &) {
                    got = "<<error>>";
                }
                compare(st, path + (prog.vertex ? " [post_vp" : " [post_fp") + (hw ? " hwprec]" : "]"), expected,
                        got);
            }
        }
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);

    std::cout << files.size() << " programs (" << particlePrograms << " particle), " << st.comparisons
              << " comparisons, " << st.mismatches << " mismatches\n";
    return st.mismatches ? 1 : 0;
}
