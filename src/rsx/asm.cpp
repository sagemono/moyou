#include "rsx/asm.h"

#include "rsx/decode.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>

namespace moyou::rsx {

namespace {

// ------------------------------------------------------------------ shared tables and helpers
constexpr uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1u); }
constexpr uint32_t put(uint32_t v, int lo, int n) { return (v & ((1u << n) - 1u)) << lo; }

constexpr int kCgC = 2178, kTexUnit0 = 2048, kUniform = 4102, kConst = 4103, kIn = 4097;
constexpr int kTypeSampler1D = 1065, kTypeSamplerCube = 1069;   // sampler1D .. samplerCUBE (incl. samplerRECT 1067)

const char *const kFpOps[] = {
    "NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DP4", "DST", "MIN", "MAX", "SLT", "SGE", "SLE", "SGT", "SNE",
    "SEQ", "FRC", "FLR", "KIL", "PK4", "UP4", "DDX", "DDY", "TEX", "TXP", "TXD", "RCP", "RSQ", "EX2", "LG2",
    "LIT", "LRP", "STR", "SFL", "COS", "SIN", "PK2", "UP2", "POW", "PKB", "UPB", "PK16", "UP16", "BEM", "PKG",
    "UPG", "DP2A", "TXL", "OP48", "TXB", "OP50", "TEXBEM", "TXPBEM", "BEMLUM", "REFL", "TIMESWTEX", "DP2", "NRM",
    "DIV", "DIVSQ", "LIF", "FENCT", "FENCB", "OP63", "BRK", "CAL", "IFE", "LOOP", "REP", "RET"};
// sources each fragment opcode reads (-1: unknown, print all three)
const int kFpArity[] = {0, 1, 2, 2, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 0, 1, 1, 1, 1, 1, 1, 3, 1, 1, 1, 1,
                        1, 3, 2, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 3, 1, 1, 3, 1, -1, 1, -1, 3, 3, 3, 2, 1, 2, 1,
                        2, 2, 1, 0, 0, -1, 0, 0, 0, 0, 0, 0};
const char *const kFpIn[] = {"WPOS", "COL0", "COL1", "FOGC", "TEX0", "TEX1", "TEX2", "TEX3",
                             "TEX4", "TEX5", "TEX6", "TEX7", "TEX8", "TEX9", "SSA"};
const char *const kCond[] = {"FL", "LT", "EQ", "LE", "GT", "NE", "GE", "TR"};
const char *const kPrec[] = {"R", "H", "X", "B"};
const char *const kScale[] = {"", "_m2", "_m4", "_m8", "_s4", "_d2", "_d4", "_d8"};

bool fpIsTex(int op) { return op == 23 || op == 24 || op == 25 || op == 47 || op == 49 || op == 51 || op == 52; }
bool fpIsFlow(int op) { return op >= 64 && op <= 69; }

const char *const kVpVec[] = {"NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN", "MAX", "SLT",
                              "SGE", "ARL", "FRC", "FLR", "SEQ", "SFL", "SGT", "SLE", "SNE", "STR", "SSG", "VOP23",
                              "VOP24", "TXL", "VOP26", "VOP27", "VOP28", "VOP29", "VOP30", "VOP31"};
const char *const kVpSca[] = {"NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", "BRA", "BRI", "CAL",
                              "CLI", "RET", "LG2", "EX2", "SIN", "COS", "BRB", "CLB", "PSH", "POP", "SOP21",
                              "SOP22", "SOP23", "SOP24", "SOP25", "SOP26", "SOP27", "SOP28", "SOP29", "SOP30", "SOP31"};
const char *const kVpOut[] = {"HPOS",      "COL0",      "COL1",      "BFC0",      "BFC1",      "FOGC",
                              "PSIZE",     "TEXCOORD0", "TEXCOORD1", "TEXCOORD2", "TEXCOORD3", "TEXCOORD4",
                              "TEXCOORD5", "TEXCOORD6", "TEXCOORD7", "TEXCOORD8"};

// source slots each vertex opcode reads; scalar opcodes read slot 2
std::vector<int> vpVecSlots(int op) {
    switch (op) {
    case 0: return {};
    case 1: case 13: case 14: case 15: case 22: case 25: return {0};
    case 3: return {0, 2};
    case 4: return {0, 1, 2};
    case 2: case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 16: case 17: case 18: case 19:
    case 20: case 21: return {0, 1};
    default: return {0, 1, 2};
    }
}
bool vpScaReads(int op) { return op == 1 || op == 2 || op == 3 || op == 4 || op == 5 || op == 6 || op == 7 || (op >= 13 && op <= 16); }

uint32_t rd32(std::span<const uint8_t> d, size_t o) {
    if (o + 4 > d.size()) throw FormatError("CgBinary: read past end of data");
    return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}
void wr32(std::vector<uint8_t> &d, size_t o, uint32_t v) {
    if (d.size() < o + 4) d.resize(o + 4);
    d[o] = uint8_t(v >> 24); d[o + 1] = uint8_t(v >> 16); d[o + 2] = uint8_t(v >> 8); d[o + 3] = uint8_t(v);
}
uint32_t swapHalves(uint32_t v) { return (v << 16) | (v >> 16); }

int typeWidth(int t) { return detail::typeWidth(t); }
int typeRows(int t) { return detail::typeRows(t); }
bool isSampler(int t) { return t >= kTypeSampler1D && t <= kTypeSamplerCube; }

std::string fmtFloatBits(uint32_t b) {
    const float f = std::bit_cast<float>(b);
    if (!std::isfinite(f) || (f == 0 && std::signbit(f))) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%08x", b);
        return buf;
    }
    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof buf, f);
    std::string s(buf, r.ptr);
    return s;
}

std::string swzText(const std::array<int, 4> &s) {
    if (s[0] == 0 && s[1] == 1 && s[2] == 2 && s[3] == 3) return "";
    std::string r = ".";
    if (s[0] == s[1] && s[1] == s[2] && s[2] == s[3]) return r + "xyzw"[s[0]];
    for (int c : s) r += "xyzw"[c];
    return r;
}
std::string maskText(int m) {
    if (m == 15) return "";
    std::string r = ".";
    for (int c = 0; c < 4; c++) if (m >> c & 1) r += "xyzw"[c];
    return m ? r : ".none";
}
std::string hex8(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%x", v); return b; }

// ------------------------------------------------------------------ line parsing
struct Cursor {
    std::string s;
    size_t i = 0;
    int line = 0;
    [[noreturn]] void fail(const std::string &m) const { throw AsmError(line, m); }
    void ws() { while (i < s.size() && std::isspace(uint8_t(s[i]))) i++; }
    bool eof() { ws(); return i >= s.size(); }
    bool peek(char c) { ws(); return i < s.size() && s[i] == c; }
    bool accept(char c) { if (peek(c)) { i++; return true; } return false; }
    void expect(char c) { if (!accept(c)) fail(std::string("expected '") + c + "'" + near()); }
    std::string near() const { return i < s.size() ? " near \"" + s.substr(i, 12) + "\"" : " at end of line"; }
    std::string ident() {   // letters, digits, _, and [n] / [A0.x+n] suffixes stay with the caller
        ws();
        size_t b = i;
        while (i < s.size() && (std::isalnum(uint8_t(s[i])) || s[i] == '_' || s[i] == '?')) i++;
        return s.substr(b, i - b);
    }
    long integer() {
        ws();
        size_t b = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
        while (i < s.size() && (std::isxdigit(uint8_t(s[i])) || s[i] == 'x' || s[i] == 'X')) i++;
        std::string t = s.substr(b, i - b);
        if (t.empty()) fail("expected a number" + near());
        try { return std::stol(t, nullptr, 0); } catch (...) { fail("bad number \"" + t + "\""); }
    }
    uint32_t floatBits() {   // decimal float or 0xHEX raw bits
        ws();
        size_t b = i;
        while (i < s.size() && (std::isalnum(uint8_t(s[i])) || s[i] == '.' || s[i] == '-' || s[i] == '+')) i++;
        std::string t = s.substr(b, i - b);
        if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) return uint32_t(std::stoul(t, nullptr, 16));
        float f = 0;
        auto r = std::from_chars(t.data(), t.data() + t.size(), f);
        if (r.ec != std::errc() || r.ptr != t.data() + t.size()) fail("bad number \"" + t + "\"");
        return std::bit_cast<uint32_t>(f);
    }
    std::array<int, 4> swizzle() {   // after '.', NV rule: missing components repeat the last
        ws();
        std::string t;
        while (i < s.size() && std::strchr("xyzwrgba", s[i]) && s[i]) t += s[i++];
        if (t.empty() || t.size() > 4) fail("bad swizzle" + near());
        std::array<int, 4> r{};
        for (size_t k = 0; k < 4; k++) {
            const char c = t[std::min(k, t.size() - 1)];
            r[k] = int(std::string_view("xyzw").find(c) != std::string_view::npos ? std::string_view("xyzw").find(c)
                                                                                     : std::string_view("rgba").find(c));
        }
        return r;
    }
    int mask() {
        ws();
        int m = 0;
        if (s.compare(i, 4, "none") == 0) { i += 4; return 0; }
        int last = -1;
        while (i < s.size() && std::strchr("xyzwrgba", s[i]) && s[i]) {
            const char c = s[i++];
            int k = int(std::string_view("xyzw").find(c));
            if (k < 0) k = int(std::string_view("rgba").find(c));
            if (k <= last) fail("write mask components must be in xyzw order");
            last = k;
            m |= 1 << k;
        }
        if (!m) fail("bad write mask" + near());
        return m;
    }
};

std::string stripComment(const std::string &l) {
    size_t p = l.find("//");
    std::string s = p == std::string::npos ? l : l.substr(0, p);
    while (!s.empty() && std::isspace(uint8_t(s.back()))) s.pop_back();
    return s;
}

// trailing "!xor(a, b, c, d)"
std::array<uint32_t, 4> takeXor(std::string &s, int line) {
    std::array<uint32_t, 4> x{};
    size_t p = s.find("!xor(");
    if (p == std::string::npos) return x;
    size_t e = s.find(')', p);
    if (e == std::string::npos) throw AsmError(line, "unterminated !xor(");
    std::string inner = s.substr(p + 5, e - p - 5);
    int k = 0;
    size_t b = 0;
    while (k < 4) {
        size_t c = inner.find(',', b);
        std::string t = inner.substr(b, c == std::string::npos ? std::string::npos : c - b);
        try { x[size_t(k++)] = uint32_t(std::stoul(t, nullptr, 0)); } catch (...) { throw AsmError(line, "bad !xor value"); }
        if (c == std::string::npos) break;
        b = c + 1;
    }
    if (k != 4) throw AsmError(line, "!xor needs four words");
    s = s.substr(0, p) + s.substr(e + 1);
    return x;
}

std::vector<std::string> splitTop(const std::string &s, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '{' || c == '(' || c == '[') depth++;
        if (c == '}' || c == ')' || c == ']') depth--;
        if (c == sep && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    out.push_back(cur);
    return out;
}

// ------------------------------------------------------------------ fragment programs
// per-operand precision modifier: RPCS3 RSX_FP_PRECISION_* (0 none, 1 half, 2 fx12, 3 fx9, 4 saturate, 5..7 unknown)
const char *const kSrcMod[] = {"", ":H", ":X", ":B", ":SAT", ":P5", ":P6", ":P7"};

struct FSrc {
    int type = 0, reg = 0;   // 0 temp, 1 input, 2 constant, 3 (unknown)
    int mod = 0;
    bool fp16 = false, neg = false, abs = false;
    std::array<int, 4> swz{0, 1, 2, 3};
};
struct FInst {
    int op = 0, prec = 0, scale = 0, dstReg = 0, mask = 15, attr = 0, tex = 0, cond = 7, condReg = 0;
    bool setCc = false, ccSet1 = false, sat = false, noDest = false, dstH = false;
    std::array<int, 4> condSwz{0, 1, 2, 3};
    std::array<FSrc, 3> src;
    bool hasConst = false;
    std::array<uint32_t, 4> k{};
    std::string kname;   // embedded uniform patched into this constant
    std::array<uint32_t, 4> x{};
    int line = 0;
};

FSrc decodeFSrc(uint32_t w) {
    FSrc s;
    s.type = int(bits(w, 0, 2));
    s.reg = int(bits(w, 2, 6));
    s.fp16 = bits(w, 8, 1);
    s.swz = {int(bits(w, 9, 2)), int(bits(w, 11, 2)), int(bits(w, 13, 2)), int(bits(w, 15, 2))};
    s.neg = bits(w, 17, 1);
    return s;
}
uint32_t encodeFSrc(const FSrc &s, int absBit) {
    return put(uint32_t(s.type), 0, 2) | put(uint32_t(s.reg), 2, 6) | put(s.fp16, 8, 1) | put(uint32_t(s.swz[0]), 9, 2) |
           put(uint32_t(s.swz[1]), 11, 2) | put(uint32_t(s.swz[2]), 13, 2) | put(uint32_t(s.swz[3]), 15, 2) |
           put(s.neg, 17, 1) | put(s.abs, absBit, 1);
}

FInst decodeF(const std::array<uint32_t, 4> &w) {
    FInst f;
    f.dstReg = int(bits(w[0], 1, 6));
    f.dstH = bits(w[0], 7, 1);
    f.setCc = bits(w[0], 8, 1);
    f.mask = int(bits(w[0], 9, 4));
    f.attr = int(bits(w[0], 13, 4));
    f.tex = int(bits(w[0], 17, 4));
    f.prec = int(bits(w[0], 22, 2));
    f.op = int(bits(w[0], 24, 6) | bits(w[2], 31, 1) << 6);
    f.noDest = bits(w[0], 30, 1);
    f.sat = bits(w[0], 31, 1);
    for (int k = 0; k < 3; k++) f.src[size_t(k)] = decodeFSrc(w[size_t(k + 1)]);
    f.src[0].abs = bits(w[1], 29, 1);
    f.src[1].abs = bits(w[2], 18, 1);
    f.src[2].abs = bits(w[3], 18, 1);
    for (int k = 0; k < 3; k++) f.src[size_t(k)].mod = int(bits(w[2], 19 + 3 * k, 3));
    f.cond = int(bits(w[1], 18, 3));
    f.condSwz = {int(bits(w[1], 21, 2)), int(bits(w[1], 23, 2)), int(bits(w[1], 25, 2)), int(bits(w[1], 27, 2))};
    f.ccSet1 = bits(w[1], 30, 1);
    f.condReg = int(bits(w[1], 31, 1));
    f.scale = int(bits(w[2], 28, 3));
    return f;
}

std::array<uint32_t, 4> encodeF(const FInst &f, bool end) {
    std::array<uint32_t, 4> w{};
    w[0] = put(end, 0, 1) | put(uint32_t(f.dstReg), 1, 6) | put(f.dstH, 7, 1) | put(f.setCc, 8, 1) | put(uint32_t(f.mask), 9, 4) |
           put(uint32_t(f.attr), 13, 4) | put(uint32_t(f.tex), 17, 4) | put(uint32_t(f.prec), 22, 2) | put(uint32_t(f.op), 24, 6) |
           put(f.noDest, 30, 1) | put(f.sat, 31, 1);
    w[1] = encodeFSrc(f.src[0], 29) | put(uint32_t(f.cond), 18, 3) | put(uint32_t(f.condSwz[0]), 21, 2) |
           put(uint32_t(f.condSwz[1]), 23, 2) | put(uint32_t(f.condSwz[2]), 25, 2) | put(uint32_t(f.condSwz[3]), 27, 2) |
           put(f.ccSet1, 30, 1) | put(uint32_t(f.condReg), 31, 1);
    w[2] = encodeFSrc(f.src[1], 18) | put(uint32_t(f.src[0].mod), 19, 3) | put(uint32_t(f.src[1].mod), 22, 3) |
           put(uint32_t(f.src[2].mod), 25, 3) | put(uint32_t(f.scale), 28, 3) | put(uint32_t(f.op >> 6), 31, 1);
    w[3] = encodeFSrc(f.src[2], 18);
    // an instruction reading an interpolated input carries the "no index register" pattern in src2 bits 21..29
    if (f.src[0].type == 1 || f.src[1].type == 1 || f.src[2].type == 1) w[3] |= 0x3fe00000u;
    return w;
}

struct FpNames {
    std::map<int, std::string> samplerByUnit;          // TEXUNITn -> name (referenced samplers)
    std::map<std::string, int> unitBySampler;
    std::set<std::string> uniforms;                    // names usable as embedded constants
};
FpNames fpNames(const Program &p) {
    FpNames n;
    for (const Param &q : p.params) {
        if (isSampler(q.type)) {
            if (q.resource >= kTexUnit0 && q.resource < kTexUnit0 + 16) {
                const int unit = q.resource - kTexUnit0;
                if (!n.samplerByUnit.contains(unit) || q.referenced) n.samplerByUnit[unit] = q.name;
                n.unitBySampler[q.name] = unit;
            }
        } else if (q.variability == kUniform && q.direction == kIn) {
            n.uniforms.insert(q.name);
        }
    }
    return n;
}

int fpArity(int op) { return op < int(std::size(kFpArity)) ? kFpArity[op] : -1; }
std::string fpOpName(int op) { return op < int(std::size(kFpOps)) ? kFpOps[op] : "OP" + std::to_string(op); }

std::string printF(const FInst &f, const FpNames &names) {
    std::string s = fpOpName(f.op) + kPrec[f.prec];
    if (f.setCc) s += f.ccSet1 ? "C1" : "C";
    if (f.sat) s += "_SAT";
    s += kScale[f.scale];
    const int arity = fpArity(f.op);
    std::vector<std::string> ops;
    const bool showCond = f.cond != 7 || f.condReg != 0 || f.condSwz != std::array<int, 4>{0, 1, 2, 3};
    std::string cond;
    if (showCond) cond = std::string("(") + kCond[f.cond] + (f.condReg ? "1" : "") + swzText(f.condSwz) + ")";
    const bool printDst = arity != 0 || !f.noDest || f.setCc;
    if (printDst) {
        std::string d = f.noDest ? (f.dstH ? "HC" : "RC") : (f.dstH ? "H" : "R") + std::to_string(f.dstReg);
        ops.push_back(d + maskText(f.mask) + cond);
    } else if (showCond) {
        ops.push_back(cond);
    }
    const int n = arity < 0 ? 3 : arity;
    for (int k = 0; k < n; k++) {
        const FSrc &r = f.src[size_t(k)];
        std::string t;
        if (r.type == 0) t = (r.fp16 ? "H" : "R") + std::to_string(r.reg);
        else if (r.type == 1) t = std::string("f[") + (f.attr < int(std::size(kFpIn)) ? kFpIn[f.attr] : ("IN" + std::to_string(f.attr)).c_str()) + "]";
        else if (r.type == 2) {
            const bool zero = f.k == std::array<uint32_t, 4>{};
            if (!f.kname.empty() && zero) t = f.kname;
            else {
                t = "{";
                for (int j = 0; j < 4; j++) t += (j ? ", " : "") + fmtFloatBits(f.k[size_t(j)]);
                t += "}";
                if (!f.kname.empty()) t = f.kname + "=" + t;
            }
        } else t = "T3_" + std::to_string(r.reg);
        t += swzText(r.swz);
        if (r.abs) t = "|" + t + "|";
        if (r.neg) t = "-" + t;
        ops.push_back(t + kSrcMod[r.mod]);
    }
    if (fpIsTex(f.op)) {
        auto it = names.samplerByUnit.find(f.tex);
        ops.push_back(it != names.samplerByUnit.end() ? it->second : "TEX" + std::to_string(f.tex));
    }
    for (size_t k = 0; k < ops.size(); k++) s += (k ? ", " : " ") + ops[k];
    return s + ";";
}

// parses "OPprec[C|C1][_SAT][_mN]" into f
void parseFMnemonic(Cursor &c, FInst &f) {
    std::string m = c.ident();
    if (m.empty()) c.fail("expected an instruction" + c.near());
    std::string base = m, mods;
    if (size_t u = m.find('_'); u != std::string::npos) { base = m.substr(0, u); mods = m.substr(u); }
    int found = -1;
    size_t bestLen = 0;
    std::string rest;
    for (int op = 0; op < 128; op++) {
        const std::string name = fpOpName(op);
        if (base.compare(0, name.size(), name) != 0 || name.size() < bestLen) continue;
        std::string r = base.substr(name.size());
        if (r.empty()) continue;
        const char p = r[0];
        if (!std::strchr("RHXB", p)) continue;
        std::string cc = r.substr(1);
        if (!cc.empty() && cc != "C" && cc != "C1") continue;
        found = op; bestLen = name.size(); rest = r;
    }
    if (found < 0) c.fail("unknown instruction \"" + m + "\" (opcodes need a precision suffix: R, H or X, e.g. MULR)");
    f.op = found;
    f.prec = int(std::string_view("RHXB").find(rest[0]));
    f.setCc = rest.size() > 1;
    f.ccSet1 = rest == std::string(1, rest[0]) + "C1";
    size_t b = 0;
    while (b < mods.size()) {
        size_t e = mods.find('_', b + 1);
        std::string t = mods.substr(b, e == std::string::npos ? std::string::npos : e - b);
        if (t == "_SAT") f.sat = true;
        else {
            int sc = -1;
            for (int k = 1; k < 8; k++) if (t == kScale[k]) sc = k;
            if (sc < 0) c.fail("unknown modifier \"" + t + "\" (use _SAT, _m2, _m4, _m8, _d2, _d4, _d8)");
            f.scale = sc;
        }
        if (e == std::string::npos) break;
        b = e;
    }
}

void parseCond(Cursor &c, int &cond, int &reg, std::array<int, 4> &swz) {
    c.expect('(');
    std::string t = c.ident();
    reg = 0;
    if (!t.empty() && t.back() == '1') { reg = 1; t.pop_back(); }
    cond = -1;
    for (int k = 0; k < 8; k++) if (t == kCond[k]) cond = k;
    if (cond < 0) c.fail("unknown condition \"" + t + "\" (FL LT EQ LE GT NE GE TR)");
    swz = {0, 1, 2, 3};
    if (c.accept('.')) swz = c.swizzle();
    c.expect(')');
}

struct FParseCtx {
    const FpNames *names;
    std::optional<int> attr;
};

FSrc parseFSrc(Cursor &c, FInst &f, FParseCtx &ctx) {
    FSrc r;
    if (c.accept('-')) r.neg = true;
    const bool abs = c.accept('|');
    c.ws();
    auto setConst = [&](const std::array<uint32_t, 4> &k, const std::string &name) {
        if (f.hasConst && (f.k != k || f.kname != name)) c.fail("one constant per instruction: all constant operands must be the same");
        f.hasConst = true;
        f.k = k;
        f.kname = name;
        r.type = 2;
    };
    auto braces = [&]() {
        std::array<uint32_t, 4> k{};
        c.expect('{');
        for (int j = 0; j < 4; j++) {
            if (j && !c.accept(',')) c.fail("a constant needs four values {x, y, z, w}");
            k[size_t(j)] = c.floatBits();
        }
        c.expect('}');
        return k;
    };
    if (c.peek('{')) {
        setConst(braces(), "");
    } else {
        std::string id = c.ident();
        if (id.empty()) c.fail("expected an operand" + c.near());
        if ((id[0] == 'R' || id[0] == 'H') && id.size() > 1 && std::all_of(id.begin() + 1, id.end(), ::isdigit)) {
            r.type = 0; r.fp16 = id[0] == 'H'; r.reg = std::stoi(id.substr(1));
            if (r.reg > 63) c.fail("register out of range");
        } else if (id == "f") {
            c.expect('[');
            std::string in = c.ident();
            c.expect(']');
            int a = -1;
            for (int k = 0; k < int(std::size(kFpIn)); k++) if (in == kFpIn[k]) a = k;
            if (a < 0 && in.rfind("IN", 0) == 0) a = std::stoi(in.substr(2));
            if (a < 0) c.fail("unknown input f[" + in + "] (WPOS COL0 COL1 FOGC TEX0..TEX9 SSA)");
            if (ctx.attr && *ctx.attr != a) c.fail("one input per instruction: all f[] operands must be the same");
            ctx.attr = a;
            r.type = 1;
        } else if (id.rfind("T3_", 0) == 0) {
            r.type = 3; r.reg = std::stoi(id.substr(3));
        } else if (c.peek('[') && ctx.names->uniforms.contains(id + "[" + std::to_string([&] { c.expect('['); const long n = c.integer(); c.expect(']'); return n; }()) + "]")) {
            // array element: re-read the name with its index
            std::string full = id;
            {
                size_t b = c.s.rfind('[', c.i - 1);
                full += c.s.substr(b, c.i - b);
            }
            std::array<uint32_t, 4> k{};
            if (c.accept('=')) k = braces();
            setConst(k, full);
        } else if (ctx.names->uniforms.contains(id)) {
            std::array<uint32_t, 4> k{};
            if (c.accept('=')) k = braces();
            setConst(k, id);
        } else {
            c.fail("unknown operand \"" + id + "\"");
        }
    }
    if (c.accept('.')) r.swz = c.swizzle();
    if (abs) { c.expect('|'); r.abs = true; }
    if (c.accept(':')) {
        const std::string m = ":" + c.ident();
        r.mod = -1;
        for (int k = 1; k < 8; k++) if (m == kSrcMod[k]) r.mod = k;
        if (r.mod < 0) c.fail("unknown operand modifier \"" + m + "\" (:H :X :B :SAT)");
    }
    return r;
}

FInst parseF(const std::string &line, int lineNo, const FpNames &names) {
    FInst f;
    f.line = lineNo;
    std::string s = line;
    f.x = takeXor(s, lineNo);
    Cursor c{s, 0, lineNo};
    parseFMnemonic(c, f);
    std::vector<std::string> parts;
    std::string rest = s.substr(c.i);
    while (!rest.empty() && (std::isspace(uint8_t(rest.back())) || rest.back() == ';')) rest.pop_back();
    size_t fs = rest.find_first_not_of(" \t");
    if (fs != std::string::npos) parts = splitTop(rest.substr(fs), ',');
    const int arity = fpArity(f.op);
    FParseCtx ctx{&names, std::nullopt};
    size_t pi = 0;
    // destination (or a bare condition)
    if (arity == 0) { f.noDest = true; f.dstReg = 63; }   // no-operand opcodes (FENCT, KIL, flow) write nothing
    if (pi < parts.size()) {
        Cursor d{parts[pi], 0, lineNo};
        d.ws();
        if (d.peek('(')) {
            parseCond(d, f.cond, f.condReg, f.condSwz);
            pi++;
        } else if (arity != 0 || std::isalpha(uint8_t(d.s[d.i]))) {
            std::string id = d.ident();
            if (id == "RC" || id == "HC") { f.noDest = true; f.dstH = id == "HC"; f.dstReg = 63; }
            else if ((id[0] == 'R' || id[0] == 'H') && id.size() > 1 && std::all_of(id.begin() + 1, id.end(), ::isdigit)) {
                f.noDest = false; f.dstH = id[0] == 'H'; f.dstReg = std::stoi(id.substr(1));
                if (f.dstReg > 63) d.fail("register out of range");
            } else d.fail("expected a destination register (R0..R63, H0..H63, RC or HC), got \"" + id + "\"");
            if (d.accept('.')) f.mask = d.mask();
            if (d.peek('(')) parseCond(d, f.cond, f.condReg, f.condSwz);
            if (!d.eof()) d.fail("unexpected text after the destination" + d.near());
            pi++;
        }
    }
    const int n = arity < 0 ? 3 : arity;
    for (int k = 0; k < n; k++) {
        if (pi >= parts.size()) c.fail(fpOpName(f.op) + " needs " + std::to_string(n) + " source operand(s)");
        Cursor sc{parts[pi++], 0, lineNo};
        f.src[size_t(k)] = parseFSrc(sc, f, ctx);
        if (!sc.eof()) sc.fail("unexpected text after an operand" + sc.near());
    }
    if (fpIsTex(f.op)) {
        if (pi >= parts.size()) c.fail(fpOpName(f.op) + " needs a sampler (its name or TEXn) as the last operand");
        Cursor tc{parts[pi++], 0, lineNo};
        std::string id = tc.ident();
        if (auto it = names.unitBySampler.find(id); it != names.unitBySampler.end()) f.tex = it->second;
        else if (id.rfind("TEX", 0) == 0 && id.size() > 3 && std::all_of(id.begin() + 3, id.end(), ::isdigit)) f.tex = std::stoi(id.substr(3));
        else tc.fail("unknown sampler \"" + id + "\" (samplers without a texture unit in this program cannot be used yet)");
        if (f.tex > 15) tc.fail("texture unit out of range");
    }
    if (pi < parts.size()) c.fail("too many operands for " + fpOpName(f.op));
    if (ctx.attr) f.attr = *ctx.attr;
    // unused sources keep the compiler's encoding: temp R0.xyzw
    return f;
}

// ------------------------------------------------------------------ vertex programs
struct VSrc {
    int type = 2, reg = 0;   // 1 temp, 2 input, 3 constant
    bool neg = false, abs = false;
    std::array<int, 4> swz{0, 1, 2, 3};
};
struct VInst {
    int vop = 0, sop = 0, dstTmp = 63, scaTmp = 63, dstOut = 31, vmask = 0, smask = 0, cond = 7, input = 0, konst = 0,
        addrSel = 0, addrSwz = 0;
    bool vecResult = false, sat = false, condTest = false, cc0 = false, cc1 = false, ccSel1 = false, indexConst = false;
    std::array<int, 4> condSwz{0, 1, 2, 3};
    std::array<VSrc, 3> src;
    std::array<uint32_t, 4> x{};
    int line = 0;
};

uint32_t encodeVSrc(const VSrc &s) {
    return put(uint32_t(s.type), 0, 2) | put(uint32_t(s.type == 1 ? s.reg : 0), 2, 6) | put(uint32_t(s.swz[3]), 8, 2) |
           put(uint32_t(s.swz[2]), 10, 2) | put(uint32_t(s.swz[1]), 12, 2) | put(uint32_t(s.swz[0]), 14, 2) | put(s.neg, 16, 1);
}

VInst decodeV(const std::array<uint32_t, 4> &d) {
    VInst v;
    const uint32_t raw[3] = {bits(d[2], 23, 9) | bits(d[1], 0, 8) << 9, bits(d[2], 6, 17), bits(d[3], 21, 11) | bits(d[2], 0, 6) << 11};
    v.input = int(bits(d[1], 8, 4));
    v.konst = int(bits(d[1], 12, 10));
    for (int k = 0; k < 3; k++) {
        VSrc &s = v.src[size_t(k)];
        s.type = int(bits(raw[k], 0, 2));
        s.reg = s.type == 1 ? int(bits(raw[k], 2, 6)) : s.type == 2 ? v.input : v.konst;
        s.swz = {int(bits(raw[k], 14, 2)), int(bits(raw[k], 12, 2)), int(bits(raw[k], 10, 2)), int(bits(raw[k], 8, 2))};
        s.neg = bits(raw[k], 16, 1);
        s.abs = bits(d[0], 21 + k, 1);
    }
    v.addrSwz = int(bits(d[0], 0, 2));
    v.condSwz = {int(bits(d[0], 8, 2)), int(bits(d[0], 6, 2)), int(bits(d[0], 4, 2)), int(bits(d[0], 2, 2))};
    v.cond = int(bits(d[0], 10, 3));
    v.condTest = bits(d[0], 13, 1);
    v.cc0 = bits(d[0], 14, 1);
    v.dstTmp = int(bits(d[0], 15, 6));
    v.addrSel = int(bits(d[0], 24, 1));
    v.ccSel1 = bits(d[0], 25, 1);
    v.sat = bits(d[0], 26, 1);
    v.cc1 = bits(d[0], 29, 1);
    v.vecResult = bits(d[0], 30, 1);
    v.vop = int(bits(d[1], 22, 5));
    v.sop = int(bits(d[1], 27, 5));
    v.indexConst = bits(d[3], 1, 1);
    v.dstOut = int(bits(d[3], 2, 5));
    v.scaTmp = int(bits(d[3], 7, 6));
    for (int c = 0; c < 4; c++) {
        v.vmask |= int(bits(d[3], 16 - c, 1)) << c;
        v.smask |= int(bits(d[3], 20 - c, 1)) << c;
    }
    return v;
}

std::array<uint32_t, 4> encodeV(const VInst &v, bool end) {
    uint32_t raw[3];
    for (int k = 0; k < 3; k++) raw[k] = encodeVSrc(v.src[size_t(k)]);
    std::array<uint32_t, 4> d{};
    d[0] = put(uint32_t(v.addrSwz), 0, 2) | put(uint32_t(v.condSwz[3]), 2, 2) | put(uint32_t(v.condSwz[2]), 4, 2) |
           put(uint32_t(v.condSwz[1]), 6, 2) | put(uint32_t(v.condSwz[0]), 8, 2) | put(uint32_t(v.cond), 10, 3) |
           put(v.condTest, 13, 1) | put(v.cc0, 14, 1) | put(uint32_t(v.dstTmp), 15, 6) | put(v.src[0].abs, 21, 1) |
           put(v.src[1].abs, 22, 1) | put(v.src[2].abs, 23, 1) | put(uint32_t(v.addrSel), 24, 1) | put(v.ccSel1, 25, 1) |
           put(v.sat, 26, 1) | put(v.cc1, 29, 1) | put(v.vecResult, 30, 1);
    d[1] = put(raw[0] >> 9, 0, 8) | put(uint32_t(v.input), 8, 4) | put(uint32_t(v.konst), 12, 10) | put(uint32_t(v.vop), 22, 5) |
           put(uint32_t(v.sop), 27, 5);
    d[2] = put(raw[2] >> 11, 0, 6) | put(raw[1], 6, 17) | put(raw[0], 23, 9);
    d[3] = put(end, 0, 1) | put(v.indexConst, 1, 1) | put(uint32_t(v.dstOut), 2, 5) | put(uint32_t(v.scaTmp), 7, 6) |
           put(raw[2], 21, 11);
    for (int c = 0; c < 4; c++) d[3] |= put(uint32_t(v.vmask >> c & 1), 16 - c, 1) | put(uint32_t(v.smask >> c & 1), 20 - c, 1);
    return d;
}

struct VpNames {
    bool tex9 = false;                                           // output register 6 carries TEXCOORD9 (not PSIZE)
    std::map<int, std::string> byIndex;                          // c[n] -> uniform name (rows as name[r])
    std::map<std::string, int> byName;
    std::map<int, std::array<uint32_t, 4>> literal;              // c[n] -> literal value bits
    std::map<std::array<uint32_t, 4>, std::vector<int>> literalByValue;
};
VpNames vpNames(const Program &p) {
    VpNames n;
    for (const Param &q : p.params)
        if (q.direction == 4098 && q.semantic == "TEXCOORD9") n.tex9 = true;
    for (const Param &q : p.params) {
        if (q.resource != kCgC || q.resIndex < 0 || isSampler(q.type)) continue;
        const int rows = typeRows(q.type), w = typeWidth(q.type);
        if (q.variability == kConst && !q.defaults.empty()) {
            if (!q.referenced) continue;
            for (int r = 0; r < rows; r++) {
                std::array<uint32_t, 4> v{};
                for (int k = 0; k < w && size_t(r * w + k) < q.defaults.size(); k++)
                    v[size_t(k)] = std::bit_cast<uint32_t>(q.defaults[size_t(r * w + k)]);
                n.literal[q.resIndex + r] = v;
                n.literalByValue[v].push_back(q.resIndex + r);
            }
        } else if (q.variability == kUniform) {
            if (rows == 1) {
                if (!n.byIndex.contains(q.resIndex) || q.referenced) n.byIndex[q.resIndex] = q.name;
                n.byName[q.name] = q.resIndex;
            } else {
                for (int r = 0; r < rows; r++) {
                    const std::string nm = q.name + "[" + std::to_string(r) + "]";
                    if (!n.byIndex.contains(q.resIndex + r)) n.byIndex[q.resIndex + r] = nm;
                    n.byName[nm] = q.resIndex + r;
                }
            }
        }
    }
    return n;
}

std::string vpConstText(int idx, const VpNames &n) {
    if (auto it = n.literal.find(idx); it != n.literal.end()) {
        std::string t = "{";
        for (int j = 0; j < 4; j++) t += (j ? ", " : "") + fmtFloatBits(it->second[size_t(j)]);
        t += "}";
        if (n.literalByValue.at(it->second).size() > 1) t += "@" + std::to_string(idx);
        return t;
    }
    if (auto it = n.byIndex.find(idx); it != n.byIndex.end()) return it->second;
    return "c[" + std::to_string(idx) + "]";
}

std::string printVSrc(const VInst &v, int k, const VpNames &n) {
    const VSrc &s = v.src[size_t(k)];
    std::string t;
    if (s.type == 1) t = "R" + std::to_string(s.reg);
    else if (s.type == 2) t = "v[" + std::to_string(s.reg) + "]";
    else if (s.type == 3) {
        if (v.indexConst) t = "c[A" + std::to_string(v.addrSel) + "." + "xyzw"[v.addrSwz] + " + " + std::to_string(s.reg) + "]";
        else t = vpConstText(s.reg, n);
    } else t = "U" + std::to_string(s.reg);
    t += swzText(s.swz);
    if (s.abs) t = "|" + t + "|";
    if (s.neg) t = "-" + t;
    return t;
}

std::string printV(const VInst &v, const VpNames &n) {
    std::vector<std::string> sides;
    const bool condShown = v.condTest;
    std::string cond = condShown ? std::string("(") + kCond[v.cond] + (v.ccSel1 ? "1" : "") + swzText(v.condSwz) + ")" : "";
    bool flagsPlaced = false;
    auto side = [&](bool vec) {
        const int op = vec ? v.vop : v.sop;
        std::string s = vec ? kVpVec[op] : kVpSca[op];
        // CC update: both enable bits; the suffix goes on the vector instruction (the scalar one if alone)
        const bool cc = v.cc0 && v.cc1 && (vec || !v.vop);
        if (cc) s += v.ccSel1 && !condShown ? "C1" : "C";
        if (!flagsPlaced && v.sat) s += "_SAT";
        flagsPlaced = true;
        std::vector<std::string> d;
        const int tmp = vec ? v.dstTmp : v.scaTmp;
        if (tmp != 63) d.push_back((vec && v.vop == 13 ? "A" : "R") + std::to_string(tmp));
        if (v.dstOut != 31 && v.vecResult == vec)
            d.push_back(std::string("o[") + (v.dstOut == 6 && n.tex9 ? "TEXCOORD9" : kVpOut[v.dstOut & 15]) + "]");
        std::string dst;
        for (size_t k = 0; k < d.size(); k++) dst += (k ? "|" : "") + d[k];
        if (dst.empty()) dst = "CC";
        dst += maskText(vec ? v.vmask : v.smask) + cond;
        cond.clear();
        std::vector<std::string> ops{dst};
        if (vec) for (int slot : vpVecSlots(op)) ops.push_back(printVSrc(v, slot, n));
        else if (vpScaReads(op)) ops.push_back(printVSrc(v, 2, n));
        for (size_t k = 0; k < ops.size(); k++) s += (k ? ", " : " ") + ops[k];
        return s;
    };
    if (v.vop) sides.push_back(side(true));
    else if (v.sop == 1) sides.push_back("NOP");   // a lone MOV would assemble on the vector unit
    if (v.sop) sides.push_back(side(false));
    if (sides.empty()) return "NOP;";
    return sides.size() == 1 ? sides[0] + ";" : sides[0] + "  &  " + sides[1] + ";";
}

struct VParseCtx {
    const VpNames *names;
    std::optional<int> input, konst;
    std::map<std::array<uint32_t, 4>, int> *newLiterals;   // literal value -> c index (allocated by the caller)
    std::vector<std::array<uint32_t, 4>> *wantLiterals;    // values with no c[] register yet (resolved in a 2nd pass)
};

VSrc parseVSrc(Cursor &c, VInst &v, VParseCtx &ctx) {
    VSrc s;
    if (c.accept('-')) s.neg = true;
    const bool abs = c.accept('|');
    c.ws();
    auto useConst = [&](int idx) {
        if (ctx.konst && *ctx.konst != idx) c.fail("one constant register per instruction: all constant operands must be the same c[]");
        ctx.konst = idx;
        s.type = 3; s.reg = idx;
    };
    if (c.peek('{')) {
        std::array<uint32_t, 4> k{};
        c.expect('{');
        for (int j = 0; j < 4; j++) {
            if (j && !c.accept(',')) c.fail("a constant needs four values {x, y, z, w}");
            k[size_t(j)] = c.floatBits();
        }
        c.expect('}');
        int idx = -1;
        if (c.accept('@')) idx = int(c.integer());
        else if (auto it = ctx.names->literalByValue.find(k); it != ctx.names->literalByValue.end()) idx = it->second.front();
        else if (auto jt = ctx.newLiterals->find(k); jt != ctx.newLiterals->end()) idx = jt->second;
        if (idx < 0) { ctx.wantLiterals->push_back(k); idx = 0; }   // placeholder; the caller allocates and re-parses
        useConst(idx);
    } else {
        std::string id = c.ident();
        if (id.empty()) c.fail("expected an operand" + c.near());
        if (id[0] == 'R' && id.size() > 1 && std::all_of(id.begin() + 1, id.end(), ::isdigit)) {
            s.type = 1; s.reg = std::stoi(id.substr(1));
            if (s.reg > 63) c.fail("register out of range");
        } else if (id == "v") {
            c.expect('[');
            const int r = int(c.integer());
            c.expect(']');
            if (r < 0 || r > 15) c.fail("input v[" + std::to_string(r) + "] out of range");
            if (ctx.input && *ctx.input != r) c.fail("one input per instruction: all v[] operands must be the same");
            ctx.input = r;
            s.type = 2; s.reg = r;
        } else if (id == "c") {
            c.expect('[');
            c.ws();
            if (c.peek('A')) {
                c.i++;
                v.addrSel = int(c.integer());
                c.expect('.');
                v.addrSwz = c.swizzle()[0];
                c.expect('+');
                v.indexConst = true;
            }
            const int r = int(c.integer());
            c.expect(']');
            if (r < 0 || r > 1023) c.fail("c[" + std::to_string(r) + "] out of range");
            useConst(r);
        } else if (id[0] == 'U' && id.size() > 1 && std::all_of(id.begin() + 1, id.end(), ::isdigit)) {
            s.type = 0; s.reg = std::stoi(id.substr(1));
        } else {
            std::string nm = id;
            if (c.peek('[')) {   // matrix row
                c.expect('[');
                nm += "[" + std::to_string(c.integer()) + "]";
                c.expect(']');
            }
            auto it = ctx.names->byName.find(nm);
            if (it == ctx.names->byName.end()) c.fail("unknown operand \"" + nm + "\"");
            useConst(it->second);
        }
    }
    if (c.accept('.')) s.swz = c.swizzle();
    if (abs) { c.expect('|'); s.abs = true; }
    return s;
}

int findName(const char *const *tab, int n, const std::string &s) {
    for (int k = 0; k < n; k++) if (s == tab[k]) return k;
    return -1;
}

// "MADC_SAT" -> opcode on each unit (-1 if absent), the C / C1 suffix and the _modifiers
struct VMnemonic { int vop = -1, sop = -1; bool nop = false; std::string cc, mods, text; };
VMnemonic vMnemonic(const std::string &side) {
    Cursor c{side, 0, 0};
    VMnemonic r;
    r.text = c.ident();
    if (r.text == "NOP") { r.nop = true; return r; }
    std::string base = r.text;
    if (size_t u = base.find('_'); u != std::string::npos) { r.mods = base.substr(u); base = base.substr(0, u); }
    for (const std::string suf : {"", "C", "C1"}) {
        if (base.size() <= suf.size() || base.compare(base.size() - suf.size(), suf.size(), suf) != 0) continue;
        const std::string core = base.substr(0, base.size() - suf.size());
        const int a = findName(kVpVec, 32, core), b = findName(kVpSca, 32, core);
        if (a > 0 || b > 0) { r.vop = a; r.sop = b; r.cc = suf; break; }
    }
    return r;
}

void parseVSide(const std::string &text, int lineNo, VInst &v, VParseCtx &ctx, bool vec) {
    Cursor c{text, 0, lineNo};
    const VMnemonic mn = vMnemonic(text);
    const std::string &m = mn.text;
    c.ident();
    if (mn.nop) { if (!c.eof()) c.fail("NOP takes no operands"); return; }
    if (vec) v.vop = mn.vop; else v.sop = mn.sop;
    const std::string ccs = mn.cc, mods = mn.mods;
    if (!ccs.empty()) { v.cc0 = v.cc1 = true; if (ccs == "C1") v.ccSel1 = true; }
    if (mods == "_SAT") v.sat = true;
    else if (!mods.empty()) c.fail("unknown modifier \"" + mods + "\" (vertex programs support _SAT)");
    std::string rest = text.substr(c.i);
    while (!rest.empty() && (std::isspace(uint8_t(rest.back())) || rest.back() == ';')) rest.pop_back();
    std::vector<std::string> parts;
    if (rest.find_first_not_of(" \t") != std::string::npos) parts = splitTop(rest.substr(rest.find_first_not_of(" \t")), ',');
    if (parts.empty()) c.fail(m + " needs a destination");
    // destination: R5 | A0 | o[NAME] | R5|o[NAME] | CC, then .mask and (cond)
    {
        Cursor d{parts[0], 0, lineNo};
        int mask = 15;
        for (;;) {
            std::string id = d.ident();
            if (id == "CC") {
            } else if (id == "o") {
                d.expect('[');
                std::string nm = d.ident();
                d.expect(']');
                int o = nm == "TEXCOORD9" ? 6 : findName(kVpOut, 16, nm);
                if (o < 0 && !nm.empty() && std::all_of(nm.begin(), nm.end(), ::isdigit)) o = std::stoi(nm);
                if (o < 0 || o > 15) d.fail("unknown output o[" + nm + "]");
                if (v.dstOut != 31) d.fail("one output register per instruction");
                v.dstOut = o;
                v.vecResult = vec;
            } else if ((id[0] == 'R' || id[0] == 'A') && id.size() > 1 && std::all_of(id.begin() + 1, id.end(), ::isdigit)) {
                const int r = std::stoi(id.substr(1));
                if (r > 62) d.fail("register out of range");
                (vec ? v.dstTmp : v.scaTmp) = r;
            } else d.fail("expected a destination (R0..R62, o[NAME], A0 or CC), got \"" + id + "\"");
            if (!d.accept('|')) break;
        }
        if (d.accept('.')) mask = d.mask();
        (vec ? v.vmask : v.smask) = mask;
        if (d.peek('(')) {
            int reg = 0;
            parseCond(d, v.cond, reg, v.condSwz);
            v.condTest = true;
            if (reg) v.ccSel1 = true;
        }
        if (!d.eof()) d.fail("unexpected text after the destination" + d.near());
    }
    std::vector<int> slots;
    if (vec) slots = vpVecSlots(v.vop);
    else if (vpScaReads(v.sop)) slots = {2};
    if (parts.size() - 1 != slots.size())
        c.fail(m + " takes " + std::to_string(slots.size()) + " source operand(s), got " + std::to_string(parts.size() - 1));
    for (size_t k = 0; k < slots.size(); k++) {
        Cursor sc{parts[k + 1], 0, lineNo};
        VSrc s = parseVSrc(sc, v, ctx);
        if (!sc.eof()) sc.fail("unexpected text after an operand" + sc.near());
        v.src[size_t(slots[k])] = s;
    }
}

VInst parseV(const std::string &line, int lineNo, VParseCtx &ctx) {
    VInst v;
    v.line = lineNo;
    std::string s = line;
    v.x = takeXor(s, lineNo);
    while (!s.empty() && (std::isspace(uint8_t(s.back())) || s.back() == ';')) s.pop_back();
    auto sides = splitTop(s, '&');
    if (sides.size() > 2) throw AsmError(lineNo, "at most a vector and a scalar instruction per line (A & B)");
    std::vector<VMnemonic> mn;
    for (const auto &sd : sides) {
        mn.push_back(vMnemonic(sd));
        if (!mn.back().nop && mn.back().vop <= 0 && mn.back().sop <= 0)
            throw AsmError(lineNo, "unknown vertex instruction \"" + mn.back().text + "\"");
    }
    // which unit runs each side: MOV exists on both; a lone instruction prefers the vector unit
    auto fits = [](const VMnemonic &m, bool vec) { return m.nop || (vec ? m.vop > 0 : m.sop > 0); };
    std::vector<bool> unit(sides.size(), true);
    if (sides.size() == 1) unit[0] = fits(mn[0], true);
    else if (fits(mn[0], true) && fits(mn[1], false)) { unit[0] = true; unit[1] = false; }
    else if (fits(mn[0], false) && fits(mn[1], true)) { unit[0] = false; unit[1] = true; }
    else throw AsmError(lineNo, "a co-issued pair needs one vector and one scalar instruction");
    for (size_t k = 0; k < sides.size(); k++) parseVSide(sides[k], lineNo, v, ctx, unit[k]);
    // the input and constant register fields are shared by all operands of the instruction
    if (ctx.input) v.input = *ctx.input;
    if (ctx.konst) v.konst = *ctx.konst;
    return v;
}

// ------------------------------------------------------------------ program walking
struct Line { int no; std::string text; };
std::vector<Line> codeLines(const std::string &src) {
    std::vector<Line> out;
    int no = 0;
    size_t b = 0;
    while (b <= src.size()) {
        size_t e = src.find('\n', b);
        std::string l = src.substr(b, e == std::string::npos ? std::string::npos : e - b);
        no++;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        l = stripComment(l);
        if (l.find_first_not_of(" \t;") != std::string::npos) out.push_back({no, l});
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return out;
}

std::string paramComment(const Program &p) {
    std::string s;
    auto tn = [](int t) -> std::string {
        static const std::map<int, const char *> m{{1045, "float"}, {1046, "float2"}, {1047, "float3"}, {1048, "float4"},
            {1091, "float1"}, {1031, "half"}, {1032, "half2"}, {1033, "half3"}, {1034, "half4"}, {1059, "float3x3"},
            {1063, "float4x3"}, {1064, "float4x4"}, {1065, "sampler1D"}, {1066, "sampler2D"}, {1067, "sampler3D"},
            {1068, "samplerRECT"}, {1069, "samplerCUBE"}, {1092, "bool"}, {1144, "int"}};
        auto it = m.find(t);
        return it != m.end() ? it->second : "type" + std::to_string(t);
    };
    for (const Param &q : p.params) {
        if (q.variability == kConst) continue;
        std::string where;
        if (isSampler(q.type)) where = q.resource >= kTexUnit0 && q.resource < kTexUnit0 + 16 ? "TEXUNIT" + std::to_string(q.resource - kTexUnit0) : "no unit";
        else if (q.resource == kCgC) where = "c[" + std::to_string(q.resIndex) + "]";
        else if (q.variability == kUniform) where = p.vertex ? "no register" : "embedded";
        else where = q.semantic;
        if (q.name.find('[') != std::string::npos) continue;   // matrix rows are listed with the matrix
        char buf[200];
        std::snprintf(buf, sizeof buf, "//   %-26s %-12s %-14s %s%s\n", q.name.c_str(), tn(q.type).c_str(), where.c_str(),
                      q.direction == 4098 ? "out" : q.variability == 4101 ? "in" : "",
                      q.referenced ? "" : (q.variability == kUniform && !isSampler(q.type) ? "  (unused: name it to use it)" : "  (unused)"));
        s += buf;
    }
    return s;
}

}  // namespace

// ------------------------------------------------------------------ public: listing
std::string listing(const Program &prog, const std::string &name) {
    if (prog.profile != 7003 && prog.profile != 7004) throw FormatError("not an RSX vertex or fragment program");
    std::string out = "// " + name + (prog.vertex ? "  (sce_vp_rsx vertex program)\n" : "  (sce_fp_rsx fragment program)\n");
    out += "// parameters:\n" + paramComment(prog) + "\n";
    const auto &uc = prog.ucode;
    if (prog.vertex) {
        const VpNames names = vpNames(prog);
        const size_t n = uc.size() / 16;
        for (size_t i = 0; i < n; i++) {
            std::array<uint32_t, 4> w{};
            for (int k = 0; k < 4; k++) w[size_t(k)] = rd32(uc, 16 * i + 4 * size_t(k));
            const bool end = bits(w[3], 0, 1);
            const VInst v = decodeV(w);
            const std::string text = printV(v, names);
            std::map<std::array<uint32_t, 4>, int> nl;
            std::vector<std::array<uint32_t, 4>> want;
            VParseCtx ctx{&names, std::nullopt, std::nullopt, &nl, &want};
            const VInst back = parseV(text, 0, ctx);
            const auto enc = encodeV(back, end);
            std::array<uint32_t, 4> x{};
            for (int k = 0; k < 4; k++) x[size_t(k)] = w[size_t(k)] ^ enc[size_t(k)];
            out += text;
            if (x != std::array<uint32_t, 4>{}) out += "  !xor(" + hex8(x[0]) + ", " + hex8(x[1]) + ", " + hex8(x[2]) + ", " + hex8(x[3]) + ")";
            out += "\n";
            if (end) break;
        }
    } else {
        const FpNames names = fpNames(prog);
        std::map<uint32_t, std::string> emb;
        for (const Param &q : prog.params) for (uint32_t o : q.embedded) emb[o] = q.name;
        size_t off = 0;
        while (off + 16 <= uc.size()) {
            std::array<uint32_t, 4> w{};
            for (int k = 0; k < 4; k++) w[size_t(k)] = swapHalves(rd32(uc, off + 4 * size_t(k)));
            const bool end = bits(w[0], 0, 1);
            FInst f = decodeF(w);
            f.hasConst = !fpIsFlow(f.op) && (bits(w[1], 0, 2) == 2 || bits(w[2], 0, 2) == 2 || bits(w[3], 0, 2) == 2);
            if (f.hasConst) {
                for (int k = 0; k < 4; k++) f.k[size_t(k)] = swapHalves(rd32(uc, off + 16 + 4 * size_t(k)));
                if (auto it = emb.find(uint32_t(off + 16)); it != emb.end()) f.kname = it->second;
            }
            const std::string text = printF(f, names);
            const FInst back = parseF(text, 0, names);
            const auto enc = encodeF(back, end);
            std::array<uint32_t, 4> x{};
            for (int k = 0; k < 4; k++) x[size_t(k)] = w[size_t(k)] ^ enc[size_t(k)];
            out += text;
            if (x != std::array<uint32_t, 4>{}) out += "  !xor(" + hex8(x[0]) + ", " + hex8(x[1]) + ", " + hex8(x[2]) + ", " + hex8(x[3]) + ")";
            out += "\n";
            off += f.hasConst ? 32 : 16;
            if (end) break;
        }
    }
    return out;
}

// ------------------------------------------------------------------ public: assemble
Program assemble(const std::string &source, const Program &original) {
    Program p = original;
    const auto lines = codeLines(source);
    if (lines.empty()) throw AsmError(0, "the program has no instructions");
    if (original.vertex && lines.size() > 512) throw AsmError(lines[512].no, "too many instructions (vertex programs allow 512)");
    if (original.vertex) {
        VpNames names = vpNames(original);
        // literal constants: reuse matching internal constants, allocate the rest in free c[] registers
        std::map<std::array<uint32_t, 4>, int> newLit;
        std::vector<std::array<uint32_t, 4>> want;
        for (const Line &l : lines) {
            VParseCtx ctx{&names, std::nullopt, std::nullopt, &newLit, &want};
            parseV(l.text, l.no, ctx);
        }
        std::set<int> used;
        for (const Param &q : p.params)
            if (q.resource == kCgC && q.resIndex >= 0 && (q.referenced || q.variability == kUniform))
                for (int r = 0; r < typeRows(q.type); r++) used.insert(q.resIndex + r);
        int nextInternal = 0;
        for (const Param &q : p.params)
            if (q.name.rfind("internal-constant-", 0) == 0) nextInternal = std::max(nextInternal, std::stoi(q.name.substr(18)) + 1);
        for (const auto &k : want) {
            if (newLit.contains(k)) continue;
            int idx = 467;
            while (idx >= 0 && used.contains(idx)) idx--;
            if (idx < 0) throw AsmError(0, "no free constant register for a new literal");
            used.insert(idx);
            newLit[k] = idx;
            Param q;
            q.name = "internal-constant-" + std::to_string(nextInternal++);
            q.type = 1048;
            q.resource = kCgC;
            q.resIndex = idx;
            q.variability = kConst;
            q.direction = kIn;
            q.referenced = true;
            for (uint32_t b : k) q.defaults.push_back(std::bit_cast<float>(b));
            q.paramno = int(p.params.size());
            p.params.push_back(q);
        }
        std::vector<uint8_t> uc;
        std::set<int> constsUsed;
        std::set<std::string> indexedArrays;   // arrays read through A0: every element counts as referenced
        uint32_t inMask = 0, outMask = 0;
        int maxTmp = -1;
        for (size_t i = 0; i < lines.size(); i++) {
            want.clear();
            VParseCtx ctx{&names, std::nullopt, std::nullopt, &newLit, &want};
            VInst v = parseV(lines[i].text, lines[i].no, ctx);
            auto d = encodeV(v, i + 1 == lines.size());
            for (int k = 0; k < 4; k++) wr32(uc, uc.size(), d[size_t(k)] ^ v.x[size_t(k)]);
            auto reads = [&](int slot) {
                const VSrc &s = v.src[size_t(slot)];
                if (s.type == 2) inMask |= 1u << s.reg;
                if (s.type == 3) constsUsed.insert(s.reg);
                if (s.type == 1) maxTmp = std::max(maxTmp, s.reg);
            };
            if (v.indexConst) {
                for (const Param &q : p.params)
                    if (q.resource == kCgC && q.resIndex == v.konst) indexedArrays.insert(q.name.substr(0, q.name.find('[')));
            }
            if (v.vop) for (int slot : vpVecSlots(v.vop)) reads(slot);
            if (v.sop && vpScaReads(v.sop)) reads(2);
            if (v.dstTmp != 63 && v.vop != 13) maxTmp = std::max(maxTmp, v.dstTmp);
            if (v.scaTmp != 63) maxTmp = std::max(maxTmp, v.scaTmp);
            if (v.dstOut != 31) {
                static const int kOutBit[16] = {-1, 0, 1, 2, 3, 4, 5, 14, 15, 16, 17, 18, 19, 20, 21, 12};
                const int b = v.dstOut == 6 && names.tex9 ? 13 : kOutBit[v.dstOut];
                if (b >= 0) outMask |= 1u << b;
            }
        }
        p.ucode = std::move(uc);
        p.vp.instructionCount = uint32_t(lines.size());
        p.vp.registerCount = uint32_t(std::max(maxTmp + 1, 1));
        p.vp.inputMask = inMask;
        p.vp.outputMask = (original.vp.outputMask & ~0x3ff03fu) | outMask;   // COL/FOG/PSIZE bits 0..5, TEX 12..21
        for (Param &q : p.params) {
            if (q.resource != kCgC || q.resIndex < 0) continue;
            bool any = false;
            for (int r = 0; r < typeRows(q.type); r++) any |= constsUsed.contains(q.resIndex + r);
            // the compiler may keep registers for values no instruction reads (dead paths): flags only ever gain
            if (q.variability == kConst || q.variability == kUniform) q.referenced = q.referenced || any;
            if (indexedArrays.contains(q.name.substr(0, q.name.find('[')))) q.referenced = true;
        }
        // a matrix counts as referenced when any of its rows is
        for (Param &q : p.params) {
            if (typeRows(q.type) > 1 && q.resource == kCgC) {
                for (const Param &r : p.params)
                    if (r.name.rfind(q.name + "[", 0) == 0 && r.referenced) q.referenced = true;
            }
        }
        return p;
    }
    // fragment
    const FpNames names = fpNames(original);
    std::vector<FInst> insts;
    for (const Line &l : lines) insts.push_back(parseF(l.text, l.no, names));
    std::vector<uint8_t> uc;
    std::map<std::string, std::vector<uint32_t>> emb;
    std::set<int> units;
    uint32_t inMask = 0;
    uint16_t tcMask = 0;
    std::array<bool, 16> tcZw{};
    int maxReg = -1;
    bool kil = false;
    for (size_t i = 0; i < insts.size(); i++) {
        const FInst &f = insts[i];
        auto w = encodeF(f, i + 1 == insts.size());
        for (int k = 0; k < 4; k++) w[size_t(k)] ^= f.x[size_t(k)];
        const bool hasConst = !fpIsFlow(int(bits(w[0], 24, 6) | bits(w[2], 31, 1) << 6)) &&
                              (bits(w[1], 0, 2) == 2 || bits(w[2], 0, 2) == 2 || bits(w[3], 0, 2) == 2);
        if (hasConst != f.hasConst && !f.hasConst) throw AsmError(f.line, "!xor turns an operand into a constant that the line does not give");
        const uint32_t off = uint32_t(uc.size());
        for (int k = 0; k < 4; k++) wr32(uc, uc.size(), swapHalves(w[size_t(k)]));
        if (hasConst) {
            for (int k = 0; k < 4; k++) wr32(uc, uc.size(), swapHalves(f.k[size_t(k)]));
            if (!f.kname.empty()) emb[f.kname].push_back(off + 16);
        }
        if (fpIsTex(f.op)) units.insert(f.tex);
        if (f.op == 18) kil = true;
        if (!f.noDest) maxReg = std::max(maxReg, f.dstH ? f.dstReg / 2 : f.dstReg);
        const int arity = fpArity(f.op) < 0 ? 3 : fpArity(f.op);
        for (int k = 0; k < arity; k++) {
            const FSrc &s = f.src[size_t(k)];
            if (s.type == 0) maxReg = std::max(maxReg, s.fp16 ? s.reg / 2 : s.reg);
            if (s.type == 1) {
                if (f.attr >= 4 && f.attr <= 11) inMask |= 1u << (f.attr + 10);   // TEX0..7 -> bits 14..21
                else if (f.attr == 12 || f.attr == 13) inMask |= 1u << f.attr;    // TEX8 / TEX9 -> bits 12 / 13
                if (f.attr >= 4 && f.attr <= 13) {
                    const int t = f.attr - 4;
                    tcMask |= uint16_t(1u << t);
                    // components read: per written component for component-wise ops, fixed counts for dot products
                    std::set<int> comps;
                    if (f.op == 5) for (int c = 0; c < 3; c++) comps.insert(s.swz[size_t(c)]);
                    else if (f.op == 6 || fpIsTex(f.op) || f.op == 54) for (int c = 0; c < 4; c++) comps.insert(s.swz[size_t(c)]);
                    else if (f.op == 56 || f.op == 46) for (int c = 0; c < 2; c++) comps.insert(s.swz[size_t(c)]);
                    else for (int c = 0; c < 4; c++) if (f.mask >> c & 1) comps.insert(s.swz[size_t(c)]);
                    if (comps.contains(2) || comps.contains(3)) tcZw[size_t(t)] = true;
                }
            }
        }
    }
    p.ucode = std::move(uc);
    p.fp.instructionCount = uint32_t(p.ucode.size() / 16);
    p.fp.registerCount = uint8_t(std::max(maxReg + 1, 2));
    p.fp.inputMask = (original.fp.inputMask & ~0x003ff000u) | inMask;   // texcoord bits; the rest keeps the compiler's
    p.fp.texcoordMask = tcMask;
    // a texcoord is 2D when its varying is declared with two or fewer components (unused ones read as 2D)
    uint16_t tc2d = 0;
    for (int t = 0; t < 16; t++) {
        const bool used = tcMask >> t & 1;
        int width = -1;
        for (const Param &q : original.params)
            if (q.variability == 4101 && q.direction == kIn && q.resource == 3220 + t) width = typeWidth(q.type);
        // a texcoord the original already read keeps the compiler's flag (older compilers never set it)
        const bool origUsed = original.fp.texcoordMask >> t & 1;
        const bool is2d = !used || (origUsed ? (original.fp.texcoord2d >> t & 1) != 0
                                             : (width >= 0 ? width <= 2 : !tcZw[size_t(t)]));
        if (is2d) tc2d |= uint16_t(1u << t);
    }
    p.fp.texcoord2d = tc2d;
    p.fp.pixelKill = uint8_t(original.fp.pixelKill || kil);
    std::set<int> origUnits;   // texture units the original program sampled
    for (size_t off = 0; off + 16 <= original.ucode.size();) {
        std::array<uint32_t, 4> w{};
        for (int k = 0; k < 4; k++) w[size_t(k)] = swapHalves(rd32(original.ucode, off + 4 * size_t(k)));
        const FInst f = decodeF(w);
        if (fpIsTex(f.op)) origUnits.insert(f.tex);
        const bool hc = !fpIsFlow(f.op) && (bits(w[1], 0, 2) == 2 || bits(w[2], 0, 2) == 2 || bits(w[3], 0, 2) == 2);
        off += hc ? 32 : 16;
        if (bits(w[0], 0, 1)) break;
    }
    for (Param &q : p.params) {
        if (isSampler(q.type)) {
            if (q.resource >= kTexUnit0 && q.resource < kTexUnit0 + 16) {
                const int unit = q.resource - kTexUnit0;
                const bool nowUsed = units.contains(unit), wasUsed = origUnits.contains(unit);
                if (nowUsed != wasUsed) {   // usage changed: the samplers on that unit follow it
                    bool other = false;
                    for (const Param &o : original.params)
                        if (&o != &q && o.resource == q.resource && o.referenced && o.name != q.name) other = true;
                    q.referenced = nowUsed && (!other || q.referenced);
                    if (nowUsed && !other) q.referenced = true;
                }
            }
        } else if (q.variability == kUniform && q.direction == kIn) {
            auto it = emb.find(q.name);
            q.embedded = it != emb.end() ? it->second : std::vector<uint32_t>{};
            std::reverse(q.embedded.begin(), q.embedded.end());   // sce-cgc lists the last use first
            // array elements can stay referenced without a patch slot (the compiler keeps the whole array)
            q.referenced = !q.embedded.empty() || (q.referenced && q.name.find('[') != std::string::npos);
        }
    }
    // an array uniform counts as referenced when any of its elements is
    for (Param &q : p.params)
        if (q.name.find('[') == std::string::npos && !isSampler(q.type))
            for (const Param &e : p.params)
                if (e.referenced && e.name.rfind(q.name + "[", 0) == 0) q.referenced = true;
    return p;
}

// ------------------------------------------------------------------ public: write
std::vector<uint8_t> write(const Program &prog, std::span<const uint8_t> orig) {
    const uint32_t pc0 = rd32(orig, 12), parr0 = rd32(orig, 16), prog0 = rd32(orig, 20);
    if (parr0 != 32) throw FormatError("CgBinary: unexpected parameter table position");
    const Program o = parse(orig);
    const uint32_t pc = uint32_t(prog.params.size());
    if (pc < pc0) throw FormatError("CgBinary: parameters cannot be removed");
    const uint32_t delta = 48 * (pc - pc0);
    const size_t blobStart = 32 + 48ull * pc0;
    if (prog0 < blobStart || prog0 + 32ull > orig.size()) throw FormatError("CgBinary: program header out of range");
    std::vector<uint8_t> out(orig.begin(), orig.begin() + ptrdiff_t(32));
    out.resize(32 + 48ull * pc, 0);
    out.insert(out.end(), orig.begin() + ptrdiff_t(blobStart), orig.begin() + ptrdiff_t(prog0));
    auto align = [&](size_t a) { while (out.size() % a) out.push_back(0); };
    auto fix = [&](uint32_t ptr) { return ptr ? ptr + delta : 0u; };
    for (uint32_t i = 0; i < pc; i++) {
        const Param &q = prog.params[i];
        const size_t po = 32 + 48ull * i;
        uint32_t nameOff = 0, defOff = 0, embOff = 0, semOff = 0;
        const bool old = i < pc0;
        if (old) {
            const size_t oo = 32 + 48ull * i;
            std::copy(orig.begin() + ptrdiff_t(oo), orig.begin() + ptrdiff_t(oo + 48), out.begin() + ptrdiff_t(po));
            nameOff = fix(rd32(orig, oo + 16)); defOff = fix(rd32(orig, oo + 20)); embOff = fix(rd32(orig, oo + 24)); semOff = fix(rd32(orig, oo + 28));
        }
        const Param *oq = old ? &o.params[i] : nullptr;
        auto str = [&](const std::string &s) { const uint32_t at = uint32_t(out.size()); out.insert(out.end(), s.begin(), s.end()); out.push_back(0); return at; };
        if (!oq || oq->name != q.name) nameOff = q.name.empty() ? 0 : str(q.name);
        if (!oq || oq->semantic != q.semantic) semOff = q.semantic.empty() ? 0 : str(q.semantic);
        if (!oq || oq->defaults != q.defaults) {
            if (q.defaults.empty()) defOff = 0;
            else { align(4); defOff = uint32_t(out.size()); for (float f : q.defaults) wr32(out, out.size(), std::bit_cast<uint32_t>(f)); }
        }
        if (!oq || oq->embedded != q.embedded) {
            if (q.embedded.empty()) embOff = 0;
            else {
                align(4);
                embOff = uint32_t(out.size());
                wr32(out, out.size(), uint32_t(q.embedded.size()));
                for (uint32_t e : q.embedded) wr32(out, out.size(), e);
            }
        }
        wr32(out, po, uint32_t(q.type));
        wr32(out, po + 4, uint32_t(q.resource));
        wr32(out, po + 8, uint32_t(q.variability));
        wr32(out, po + 12, uint32_t(q.resIndex));
        wr32(out, po + 16, nameOff);
        wr32(out, po + 20, defOff);
        wr32(out, po + 24, embOff);
        wr32(out, po + 28, semOff);
        wr32(out, po + 32, uint32_t(q.direction));
        wr32(out, po + 36, uint32_t(q.paramno));
        wr32(out, po + 40, q.referenced ? 1u : 0u);
    }
    align(16);
    const uint32_t progOff = uint32_t(out.size());
    out.insert(out.end(), orig.begin() + ptrdiff_t(prog0), orig.begin() + ptrdiff_t(prog0 + 32));
    if (prog.vertex) {
        wr32(out, progOff, prog.vp.instructionCount);
        wr32(out, progOff + 4, prog.vp.instructionSlot);
        wr32(out, progOff + 8, prog.vp.registerCount);
        wr32(out, progOff + 12, prog.vp.inputMask);
        wr32(out, progOff + 16, prog.vp.outputMask);
    } else {
        wr32(out, progOff, prog.fp.instructionCount);
        wr32(out, progOff + 4, prog.fp.inputMask);
        out[progOff + 12] = uint8_t(prog.fp.texcoordMask >> 8); out[progOff + 13] = uint8_t(prog.fp.texcoordMask);
        out[progOff + 14] = uint8_t(prog.fp.texcoord2d >> 8); out[progOff + 15] = uint8_t(prog.fp.texcoord2d);
        out[progOff + 18] = prog.fp.registerCount;
        out[progOff + 19] = prog.fp.outputFromH0;
        out[progOff + 20] = prog.fp.depthReplace;
        out[progOff + 21] = prog.fp.pixelKill;
    }
    const uint32_t ucOff = uint32_t(out.size());
    out.insert(out.end(), prog.ucode.begin(), prog.ucode.end());
    wr32(out, 8, uint32_t(out.size()));
    wr32(out, 12, pc);
    wr32(out, 16, 32);
    wr32(out, 20, progOff);
    wr32(out, 24, uint32_t(prog.ucode.size()));
    wr32(out, 28, ucOff);
    return out;
}

}  // namespace moyou::rsx
