// moyou-cli: command-line tools on moyou_core. Usage: moyou-cli <command> [args...]
#include "formats/qrc.h"
#include "render/effects.h"
#include "rsx/asm.h"
#include "rsx/rsx.h"
#include "sim/params.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string baseName(const std::string &path) {
    auto p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

// transpile [--no-hwprec] [--gl330] file.vpo|fpo
int cmdTranspile(const std::vector<std::string> &args) {
    moyou::rsx::Options opt;
    std::vector<std::string> files;
    for (const auto &a : args) {
        if (a == "--no-hwprec") opt.hwprec = false;
        else if (a == "--hwprec") opt.hwprec = true;
        else if (a == "--gl330") opt.gl330 = true;
        else files.push_back(a);
    }
    if (files.empty()) {
        std::cerr << "usage: moyou-cli transpile [--no-hwprec] [--gl330] file.vpo|fpo...\n";
        return 2;
    }
    for (const auto &f : files) {
        auto prog = moyou::rsx::parse(readFile(f));
        std::cout << moyou::rsx::transpile(prog, opt, baseName(f)) << '\n';
    }
    return 0;
}

// disasm file.vpo|fpo...
int cmdDisasm(const std::vector<std::string> &args) {
    if (args.empty()) {
        std::cerr << "usage: moyou-cli disasm file.vpo|fpo...\n";
        return 2;
    }
    for (const auto &f : args)
        std::cout << moyou::rsx::disassemble(moyou::rsx::parse(readFile(f)), f) << '\n';
    return 0;
}

// list file.vpo|fpo: the editable assembly listing
int cmdList(const std::vector<std::string> &args) {
    if (args.size() != 1) {
        std::cerr << "usage: moyou-cli list file.vpo|fpo\n";
        return 2;
    }
    std::cout << moyou::rsx::listing(moyou::rsx::parse(readFile(args[0])), args[0]);
    return 0;
}

// effects lines.qrc "spec" out.qrc | effects --list | effects --source lines.qrc "spec"
int cmdEffects(const std::vector<std::string> &args) {
    if (args.size() == 1 && args[0] == "--list") {
        for (const auto &f : moyou::render::linesControllerEffects()) {
            std::cout << f.id << "  " << f.name << ": " << f.description << "\n";
            for (const auto &p : f.params)
                std::cout << "    " << f.id << "." << p.key << " = " << p.def << "   (" << p.label << ", " << p.min << " .. " << p.max << ")\n";
        }
        return 0;
    }
    const bool sourceOnly = args.size() == 3 && args[0] == "--source";
    if (args.size() != 3) {
        std::cerr << "usage: moyou-cli effects lines.qrc \"vignette, scanlines.dark=0.3\" out.qrc   (--list, --source lines.qrc \"spec\")\n";
        return 2;
    }
    const char *id = "lib/moyou/LinesController.fpo";
    auto a = moyou::qrc::Archive::load(std::filesystem::path(args[sourceOnly ? 1 : 0]));
    if (!a.contains(id) || !a.contains("lib/glutils/Gaussian.vpo")) {
        std::cerr << "effects need a firmware 4.93 lines.qrc\n";
        return 1;
    }
    const auto &orig = a.file(id);
    std::string src;
    try {
        src = moyou::render::linesControllerSource(moyou::render::parseEffectSpec(args[sourceOnly ? 2 : 1]), orig);
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    if (sourceOnly) {
        std::cout << src;
        return 0;
    }
    try {
        a.replace(id, moyou::rsx::write(moyou::rsx::assemble(src, moyou::rsx::parse(orig)), orig));
    } catch (const moyou::rsx::AsmError &e) {
        std::cerr << "effect source line " << e.line() << ": " << e.what() << "\n";
        return 1;
    }
    moyou::qrc::addNote(a);
    a.saveTo(std::filesystem::path(args[2]));
    std::cout << "wrote " << args[2] << "\n";
    return 0;
}

// replace lines.qrc id file out.qrc: swap one file inside the archive (e.g. an assembled shader)
int cmdReplace(const std::vector<std::string> &args) {
    if (args.size() != 4) {
        std::cerr << "usage: moyou-cli replace lines.qrc id file out.qrc\n";
        return 2;
    }
    auto a = moyou::qrc::Archive::load(std::filesystem::path(args[0]));
    if (!a.contains(args[1])) {
        std::cerr << args[0] << " has no " << args[1] << "\n";
        return 1;
    }
    a.replace(args[1], readFile(args[2]));
    moyou::qrc::addNote(a);
    a.saveTo(std::filesystem::path(args[3]));
    std::cout << "wrote " << args[3] << "\n";
    return 0;
}

// asm source.txt original.vpo|fpo out.vpo|fpo: assemble against the original's parameter table
int cmdAsm(const std::vector<std::string> &args) {
    if (args.size() != 3) {
        std::cerr << "usage: moyou-cli asm source.txt original.vpo|fpo out.vpo|fpo\n";
        return 2;
    }
    const auto srcBytes = readFile(args[0]);
    const std::string src(srcBytes.begin(), srcBytes.end());
    const auto orig = readFile(args[1]);
    try {
        const auto prog = moyou::rsx::assemble(src, moyou::rsx::parse(orig));
        const auto out = moyou::rsx::write(prog, orig);
        std::ofstream(args[2], std::ios::binary).write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
        std::cout << "wrote " << args[2] << " (" << out.size() << " bytes, " << prog.ucode.size() / 16 << " slots)\n";
    } catch (const moyou::rsx::AsmError &e) {
        std::cerr << args[0] << ":" << e.line() << ": " << e.what() << "\n";
        return 1;
    }
    return 0;
}

std::filesystem::path fsPath(const std::string &s) { return std::filesystem::path(s); }   // argv encoding

// info lines.qrc
int cmdInfo(const std::vector<std::string> &args) {
    if (args.size() != 1) { std::cerr << "usage: moyou-cli info lines.qrc\n"; return 2; }
    const auto a = moyou::qrc::Archive::load(fsPath(args[0]));
    size_t total = 0;
    for (const auto &id : a.ids()) {
        std::printf("%9zu  %s\n", a.file(id).size(), id.c_str());
        total += a.file(id).size();
    }
    std::printf("%zu files, %zu bytes of payload\n", a.ids().size(), total);
    return 0;
}

// extract lines.qrc outdir
int cmdExtract(const std::vector<std::string> &args) {
    if (args.size() != 2) { std::cerr << "usage: moyou-cli extract lines.qrc outdir\n"; return 2; }
    const auto a = moyou::qrc::Archive::load(fsPath(args[0]));
    const auto root = std::filesystem::weakly_canonical(fsPath(args[1]));
    for (const auto &id : a.ids()) {
        const auto dest = std::filesystem::weakly_canonical(root / fsPath(id));
        const auto rel = dest.lexically_relative(root);
        if (rel.empty() || *rel.begin() == "..") { std::cerr << "skip " << id << "\n"; continue; }   // no path escapes
        std::filesystem::create_directories(dest.parent_path());
        std::ofstream f(dest, std::ios::binary);
        const auto &d = a.file(id);
        f.write(reinterpret_cast<const char *>(d.data()), std::streamsize(d.size()));
        std::printf("%9zu  %s\n", d.size(), id.c_str());
    }
    return 0;
}

// patch lines.qrc edits.txt out.qrc   (edits: "FILE.NAME = value" lines, as exported by the app)
int cmdPatch(const std::vector<std::string> &args) {
    std::vector<std::string> pos;
    bool allOverrides = false;
    for (const auto &s : args) (s == "--all-overrides" ? (void)(allOverrides = true) : (void)pos.push_back(s));
    if (pos.size() != 3) {
        std::cerr << "usage: moyou-cli patch lines.qrc edits.txt out.qrc [--all-overrides]\n";
        return 2;
    }
    auto a = moyou::qrc::Archive::load(fsPath(pos[0]));
    std::ifstream in(fsPath(pos[1]));
    if (!in) throw std::runtime_error("cannot open " + pos[1]);
    moyou::sim::Table edits;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) throw std::runtime_error("bad edit line: " + line);
        auto trim = [](std::string s) { s.erase(0, s.find_first_not_of(" \t")); s.erase(s.find_last_not_of(" \t") + 1); return s; };
        edits[trim(line.substr(0, eq))] = std::stod(trim(line.substr(eq + 1)));
    }
    for (const auto &k : moyou::sim::applyEdits(a, edits, allOverrides))
        std::cerr << "warning: " << k << " is not in the archive, skipped\n";
    moyou::qrc::addNote(a);
    a.saveTo(fsPath(pos[2]));
    std::printf("wrote %s (%zu edits%s)\n", pos[2].c_str(), edits.size(), allOverrides ? ", all overrides" : "");
    return 0;
}

struct Command {
    std::string_view name, help;
    int (*run)(const std::vector<std::string> &);
};

const Command kCommands[] = {
    {"info", "lines.qrc                              list the files in a .qrc", cmdInfo},
    {"extract", "lines.qrc outdir                    unpack a .qrc", cmdExtract},
    {"patch", "file.qrc edits.txt out.qrc [--all-overrides]   apply exported edits (base + time-of-day presets)", cmdPatch},
    {"transpile", "[--no-hwprec] [--gl330] file.vpo|fpo   RSX program -> GLSL", cmdTranspile},
    {"disasm", "file.vpo|fpo                           RSX program disassembly", cmdDisasm},
    {"list", "file.vpo|fpo                             editable RSX assembly listing", cmdList},
    {"asm", "source.txt original.vpo|fpo out.vpo|fpo  assemble a listing back into a program", cmdAsm},
    {"replace", "lines.qrc id file out.qrc               replace one file in a .qrc", cmdReplace},
    {"effects", "lines.qrc \"spec\" out.qrc              LinesController effects (--list for the effects)", cmdEffects},
};

int usage() {
    std::cerr << "usage: moyou-cli <command> [args...]\n";
    for (const auto &c : kCommands)
        std::cerr << "  " << c.name << " " << c.help << "\n";
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2)
        return usage();
    const std::string_view cmd = argv[1];
    const std::vector<std::string> args(argv + 2, argv + argc);
    for (const auto &c : kCommands) {
        if (c.name != cmd)
            continue;
        try {
            return c.run(args);
        } catch (const std::exception &e) {
            std::cerr << "moyou-cli " << cmd << ": " << e.what() << "\n";
            return 1;
        }
    }
    return usage();
}
