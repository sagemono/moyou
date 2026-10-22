// CgBinary container parsing, RSX VP/FP microcode decoding and the disassembly listing.
#include "rsx/decode.h"
#include "rsx/rsx.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>

namespace moyou::rsx {
namespace detail {
namespace {

constexpr uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1u); }

const char *const kVpVec[] = {"NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN", "MAX", "SLT",
                              "SGE", "ARL", "FRC", "FLR", "SEQ", "SFL", "SGT", "SLE", "SNE", "STR", "SSG", "?17",
                              "?18", "TXL"};
const char *const kVpSca[] = {"NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", "BRA", "BRI", "CAL",
                              "CLI", "RET", "LG2", "EX2", "SIN", "COS", "BRB", "CLB", "PSH", "POP"};
const char *const kVpIn[] = {"ATTR0", "ATTR1", "ATTR2",  "ATTR3",  "ATTR4",  "ATTR5",  "ATTR6",  "ATTR7",
                             "ATTR8", "ATTR9", "ATTR10", "ATTR11", "ATTR12", "ATTR13", "ATTR14", "ATTR15"};
const char *const kVpOut[] = {"HPOS",      "COL0",      "COL1",      "BFC0",      "BFC1",      "FOGC",
                              "PSIZE",     "TEXCOORD0", "TEXCOORD1", "TEXCOORD2", "TEXCOORD3", "TEXCOORD4",
                              "TEXCOORD5", "TEXCOORD6", "TEXCOORD7", "TEXCOORD8"};

const char *const kFpOps[] = {
    "NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DP4", "DST", "MIN", "MAX", "SLT", "SGE", "SLE", "SGT", "SNE",
    "SEQ", "FRC", "FLR", "KIL", "PK4", "UP4", "DDX", "DDY", "TEX", "TXP", "TXD", "RCP", "RSQ", "EX2", "LG2",
    "LIT", "LRP", "STR", "SFL", "COS", "SIN", "PK2", "UP2", "POW", "PKB", "UPB", "PK16", "UP16", "BEM", "PKG",
    "UPG", "DP2A", "TXL", "?48", "TXB", "?50", "TEXBEM", "TXPBEM", "BEMLUM", "REFL", "TIMESWTEX", "DP2", "NRM",
    "DIV", "DIVSQ", "LIF", "FENCT", "FENCB", "?63", "BRK", "CAL", "IFE", "LOOP", "REP", "RET"};
const char *const kFpIn[] = {"WPOS", "COL0", "COL1", "FOGC", "TEX0", "TEX1", "TEX2", "TEX3",
                             "TEX4", "TEX5", "TEX6", "TEX7", "TEX8", "TEX9", "SSA"};

uint32_t be32(std::span<const uint8_t> d, uint64_t off) {
    if (off + 4 > d.size())
        throw FormatError("CgBinary: read past end of data");
    return uint32_t(d[off]) << 24 | uint32_t(d[off + 1]) << 16 | uint32_t(d[off + 2]) << 8 | d[off + 3];
}

uint16_t be16(std::span<const uint8_t> d, uint64_t off) {
    if (off + 2 > d.size())
        throw FormatError("CgBinary: read past end of data");
    return uint16_t(d[off] << 8 | d[off + 1]);
}

// FP microcode words are stored with their 16-bit halves swapped
uint32_t fpword(const std::vector<uint8_t> &uc, size_t off) {
    uint32_t v = be32(uc, off);
    return (v << 16) | (v >> 16);
}

std::string cstr(std::span<const uint8_t> d, uint32_t off) {
    if (off == 0 || off >= d.size())
        return {};
    auto *p = static_cast<const uint8_t *>(std::memchr(d.data() + off, 0, d.size() - off));
    if (!p)
        throw FormatError("CgBinary: unterminated string");
    return std::string(reinterpret_cast<const char *>(d.data() + off), reinterpret_cast<const char *>(p));
}

std::string padRight(std::string s, size_t n) {
    if (s.size() < n)
        s.append(n - s.size(), ' ');
    return s;
}

std::string padLeft(const std::string &s, size_t n) {
    return s.size() < n ? std::string(n - s.size(), ' ') + s : s;
}

std::string hex(uint32_t v, size_t width = 0) {
    char buf[16];
    auto r = std::to_chars(buf, buf + sizeof buf, v, 16);
    std::string s(buf, r.ptr);
    return s.size() < width ? std::string(width - s.size(), '0') + s : s;
}

std::string swzStr(const std::array<int, 4> &swz) {
    std::string s;
    for (int c : swz)
        s += "xyzw"[c];
    if (s == "xyzw")
        return {};
    if (s[0] == s[1] && s[0] == s[2] && s[0] == s[3])
        return std::string(".") + s[0];
    return "." + s;
}

const char *const kCmp[] = {"FL", "<", "==", "<=", ">", "!=", ">=", "TR"};

std::string typeName(int type) {
    switch (type) {
    case 1045: return "float";
    case 1046: return "float2";
    case 1047: return "float3";
    case 1048: return "float4";
    case 1057: return "float3x1";
    case 1058: return "float3x2";
    case 1059: return "float3x3";
    case 1060: return "float3x4";
    case 1061: return "float4x1";
    case 1062: return "float4x2";
    case 1063: return "float4x3";
    case 1064: return "float4x4";
    case 1065: return "sampler1D";
    case 1066: return "sampler2D";
    case 1067: return "sampler3D";
    case 1068: return "samplerRECT";
    case 1069: return "samplerCUBE";
    case 1091: return "float1";
    case 1092: return "bool";
    case 1031: return "half";
    case 1032: return "half2";
    case 1033: return "half3";
    case 1034: return "half4";
    case 1144: return "int";
    default: return "type" + std::to_string(uint32_t(type));
    }
}

std::string varName(int var) {
    switch (var) {
    case 4101: return "varying";
    case 4102: return "uniform";
    case 4103: return "const";
    case 4096: return "unknown";
    case 4104: return "literal";
    default: return std::to_string(uint32_t(var));
    }
}

const char *dirName(int dir) {
    switch (dir) {
    case 4097: return "in";
    case 4098: return "out";
    case 4099: return "inout";
    default: return "";
    }
}

}  // namespace

const char *vpInName(int reg) { return kVpIn[reg & 15]; }
const char *vpOutName(int reg) { return kVpOut[reg & 15]; }

bool isVpFlow(const std::string &op) {
    for (const char *f : {"BRA", "BRI", "CAL", "CLI", "RET", "BRB", "CLB", "PSH", "POP"})
        if (op == f)
            return true;
    return false;
}

bool isFpFlow(const std::string &op) {
    for (const char *f : {"IFE", "LOOP", "REP", "BRK", "CAL", "RET"})
        if (op == f)
            return true;
    return false;
}

const char *fpInRes(const std::string &in) {
    static const std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> r{{"WPOS", "WPOS"}, {"COL0", "COLOR0"}, {"COL1", "COLOR1"},
                                             {"FOGC", "FOGCOORD"}};
        for (int i = 0; i < 10; ++i)
            r["TEX" + std::to_string(i)] = "TEXCOORD" + std::to_string(i);
        return r;
    }();
    auto it = m.find(in);
    return it == m.end() ? nullptr : it->second.c_str();
}

int typeWidth(int type) {
    switch (type) {
    case 1045: case 1091: case 1031: return 1;
    case 1046: case 1032: return 2;
    case 1047: case 1033: return 3;
    default: return 4;
    }
}

int typeRows(int type) {
    switch (type) {
    case 1061: case 1062: case 1063: case 1064: return 4;
    case 1057: case 1058: case 1059: case 1060: return 3;
    default: return 1;
    }
}

std::string cgResName(uint32_t res, int idx) {
    if (res == kCgC)
        return "c[" + std::to_string(idx) + "]";
    if (res >= 2048 && res <= 2063)
        return "TEXUNIT" + std::to_string(res - 2048);
    if (res >= 2113 && res <= 2128)
        return "ATTR" + std::to_string(res - 2113);
    if (res >= 3220 && res <= 3227)
        return "TEXCOORD" + std::to_string(res - 3220);
    switch (res) {
    case 2243: return "HPOS";
    case 2245: return "COL0";
    case 2757: return "COLOR0";
    case 2758: return "COLOR1";
    case 2369: return "PSIZE";
    case 2373: return "POSITION0";
    case 2241: return "FOGCOORD";
    case 3239: return "WPOS";
    case 3256: return "(embedded)";
    default: return "res" + std::to_string(res) + "[" + std::to_string(idx) + "]";
    }
}

std::map<std::string, std::string> varyingByRes(const Program &prog, int direction) {
    std::map<std::string, std::string> m;
    for (const Param &p : prog.params)
        if (p.variability == 4101 && p.direction == direction)
            m[cgResName(uint32_t(p.resource), p.resIndex)] = p.name;
    return m;
}

std::string fmtG(double v, int precision) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::general, precision);
    return std::string(buf, r.ptr);
}

std::string fmtf(double v) {
    if (std::isfinite(v) && v == std::trunc(v) && std::fabs(v) < 1e6) {
        char buf[64];
        auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 1);
        return std::string(buf, r.ptr);
    }
    return fmtG(v, 9);
}

std::string flit(double v) {
    if (std::isnan(v))
        return "0.0";
    std::string s = fmtG(v, 9);
    if (s.find("inf") != std::string::npos)
        return v > 0 ? "3.4e38" : "-3.4e38";
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
        s += ".0";
    return s;
}

std::string ident(const std::string &name) {
    std::string s = name;
    for (char &c : s) {
        auto u = static_cast<unsigned char>(c);
        if (!((u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_'))
            c = '_';
    }
    size_t b = s.find_first_not_of('_');
    if (b == std::string::npos)
        return "unnamed";
    return s.substr(b, s.find_last_not_of('_') - b + 1);
}

std::map<int, ConstEntry> vpConstMap(const Program &prog) {
    std::map<int, ConstEntry> cmap;
    for (const Param &p : prog.params) {
        if (uint32_t(p.resource) != kCgC || p.resIndex < 0 || !p.referenced)
            continue;
        int rows = typeRows(p.type);
        for (int r = 0; r < rows; ++r) {
            int idx = p.resIndex + r;
            if (p.variability == 4103 && !p.defaults.empty()) {
                int w = typeWidth(p.type);
                ConstEntry e;
                e.literal = true;
                for (size_t k = size_t(w * r); k < size_t(w * r + w) && k < p.defaults.size(); ++k)
                    e.values.push_back(p.defaults[k]);
                for (int k = w; k < 4; ++k)
                    e.values.push_back(0.0);
                cmap[idx] = std::move(e);
            } else if (!cmap.contains(idx)) {
                ConstEntry e;
                e.name = rows == 1 ? p.name : p.name + "[" + std::to_string(r) + "]";
                cmap[idx] = std::move(e);
            }
        }
    }
    return cmap;
}

std::vector<VpInst> decodeVp(const Program &prog) {
    const auto &uc = prog.ucode;
    if (uc.size() % 4)
        throw FormatError("VP ucode size is not a multiple of 4");
    std::vector<VpInst> insts;
    for (size_t i = 0; i < uc.size() / 16; ++i) {
        uint32_t d0 = be32(uc, 16 * i), d1 = be32(uc, 16 * i + 4), d2 = be32(uc, 16 * i + 8),
                 d3 = be32(uc, 16 * i + 12);
        uint32_t srcRaw[3] = {bits(d2, 23, 9) | (bits(d1, 0, 8) << 9), bits(d2, 6, 17),
                              bits(d3, 21, 11) | (bits(d2, 0, 6) << 11)};
        bool absf[3] = {bits(d0, 21, 1) != 0, bits(d0, 22, 1) != 0, bits(d0, 23, 1) != 0};
        VpInst ins;
        ins.index = int(i);
        for (int k = 0; k < 3; ++k) {
            uint32_t s = srcRaw[k];
            uint32_t rt = bits(s, 0, 2);
            VpSrc &src = ins.srcs[size_t(k)];
            src.kind = rt == 1 ? VpSrc::Temp : rt == 2 ? VpSrc::In : rt == 3 ? VpSrc::Const : VpSrc::Bad;
            src.reg = int(rt == 1 ? bits(s, 2, 6) : rt == 2 ? bits(d1, 8, 4) : bits(d1, 12, 10));
            src.swz = {int(bits(s, 14, 2)), int(bits(s, 12, 2)), int(bits(s, 10, 2)), int(bits(s, 8, 2))};
            src.neg = bits(s, 16, 1) != 0;
            src.abs = absf[k];
            src.indexed = bits(d3, 1, 1) != 0;
            src.addrReg = int(bits(d0, 24, 1));
            src.addrSwz = int(bits(d0, 0, 2));
        }
        uint32_t vo = bits(d1, 22, 5), so = bits(d1, 27, 5);
        ins.vecOp = vo < std::size(kVpVec) ? kVpVec[vo] : "?";
        ins.scaOp = so < std::size(kVpSca) ? kVpSca[so] : "?";
        for (int c = 0; c < 4; ++c) {
            if (bits(d3, 16 - c, 1))
                ins.vmask += "xyzw"[c];
            if (bits(d3, 20 - c, 1))
                ins.smask += "xyzw"[c];
        }
        bool vecResult = bits(d0, 30, 1) != 0;
        uint32_t dstOut = bits(d3, 2, 5);
        bool ccUpd = bits(d0, 14, 1) || bits(d0, 29, 1);
        int ccSel = int(bits(d0, 25, 1));
        auto dsts = [&](bool isSca) {
            uint32_t tmp = isSca ? bits(d3, 7, 6) : bits(d0, 15, 6);
            bool isRes = isSca ? !vecResult : vecResult;
            std::vector<VpDst> out;
            if (isRes && dstOut != 0x1f) {
                if (dstOut >= std::size(kVpOut))
                    throw FormatError("VP: invalid output register");
                out.push_back({VpDst::Out, int(dstOut)});
            }
            if (tmp != 0x3f)
                out.push_back({(!isSca && ins.vecOp == "ARL") ? VpDst::Addr : VpDst::Temp, int(tmp)});
            if (ccUpd)
                out.push_back({VpDst::Cc, ccSel});
            return out;
        };
        ins.vecDst = dsts(false);
        ins.scaDst = dsts(true);
        ins.sat = bits(d0, 26, 1) != 0;
        ins.tex = int(bits(d2, 8, 2));
        ins.cond = bits(d0, 13, 1) ? int(bits(d0, 10, 3)) : 7;
        ins.ccSel = ccSel;
        ins.ccSwz = {int(bits(d0, 8, 2)), int(bits(d0, 6, 2)), int(bits(d0, 4, 2)), int(bits(d0, 2, 2))};
        ins.addr = int((bits(d0, 23, 1) << 9) | (bits(d2, 0, 6) << 3) | bits(d3, 29, 3));
        ins.end = bits(d3, 0, 1) != 0;
        insts.push_back(std::move(ins));
        if (bits(d3, 0, 1))
            break;
    }
    return insts;
}

std::vector<FpInst> decodeFp(const Program &prog) {
    std::map<uint32_t, std::string> emb;
    for (const Param &p : prog.params)
        for (uint32_t o : p.embedded)
            emb[o] = p.name;
    static const double kScale[8] = {1.0, 2.0, 4.0, 8.0, 1.0, 0.5, 0.25, 0.125};
    const auto &uc = prog.ucode;
    std::vector<FpInst> insts;
    size_t off = 0;
    while (off + 16 <= uc.size()) {
        uint32_t dst = fpword(uc, off);
        uint32_t s[3] = {fpword(uc, off + 4), fpword(uc, off + 8), fpword(uc, off + 12)};
        uint32_t opcode = bits(dst, 24, 6) | (bits(s[1], 31, 1) << 6);
        FpInst ins;
        ins.offset = uint32_t(off);
        ins.op = opcode < std::size(kFpOps) ? std::string(kFpOps[opcode]) : "?" + std::to_string(opcode);
        ins.hasConst = !isFpFlow(ins.op) && (bits(s[0], 0, 2) == 2 || bits(s[1], 0, 2) == 2 || bits(s[2], 0, 2) == 2);
        if (ins.hasConst) {
            for (int k = 0; k < 4; ++k)
                ins.konst[size_t(k)] = std::bit_cast<float>(fpword(uc, off + 16 + 4 * size_t(k)));
            if (auto it = emb.find(uint32_t(off + 16)); it != emb.end())
                ins.constName = it->second;
        }
        uint32_t attr = bits(dst, 13, 4);
        for (int k = 0; k < 3; ++k) {
            uint32_t x = s[k];
            uint32_t rt = bits(x, 0, 2);
            FpSrc &src = ins.srcs[size_t(k)];
            src.kind = FpSrc::Kind(rt);
            src.fp16 = bits(x, 8, 1) != 0;
            src.reg = int(bits(x, 2, 6));
            src.swz = {int(bits(x, 9, 2)), int(bits(x, 11, 2)), int(bits(x, 13, 2)), int(bits(x, 15, 2))};
            src.neg = bits(x, 17, 1) != 0;
            src.abs = (k == 0 ? bits(x, 29, 1) : bits(x, 18, 1)) != 0;
            src.pmod = int(bits(s[1], 19 + 3 * k, 3));
            if (rt == 1)
                src.inReg = attr < std::size(kFpIn) ? std::string(kFpIn[attr]) : "IN" + std::to_string(attr);
        }
        for (int c = 0; c < 4; ++c)
            if (bits(dst, 9 + c, 1))
                ins.mask += "xyzw"[c];
        ins.hasDst = !bits(dst, 30, 1);
        ins.dstFile = bits(dst, 7, 1) ? 'H' : 'R';
        ins.dstReg = int(bits(dst, 1, 6));
        ins.setCc = bits(dst, 8, 1) != 0;
        ins.ccReg = int(bits(s[0], 30, 1));
        ins.sat = bits(dst, 31, 1) != 0;
        ins.prec = int(bits(dst, 22, 2));
        ins.scale = kScale[bits(s[1], 28, 3)];
        ins.tex = int(bits(dst, 17, 4));
        ins.cond = int(bits(s[0], 18, 1) | (bits(s[0], 19, 1) << 1) | (bits(s[0], 20, 1) << 2));
        ins.condReg = int(bits(s[0], 31, 1));
        ins.condSwz = {int(bits(s[0], 21, 2)), int(bits(s[0], 23, 2)), int(bits(s[0], 25, 2)), int(bits(s[0], 27, 2))};
        ins.raw = {dst, s[0], s[1], s[2]};
        ins.end = bits(dst, 0, 1) != 0;
        off += ins.hasConst ? 32 : 16;
        insts.push_back(std::move(ins));
        if (bits(dst, 0, 1))
            break;
    }
    return insts;
}

}  // namespace detail

using namespace detail;

Program parse(std::span<const uint8_t> d) {
    if (d.size() < 32)
        throw FormatError("CgBinary: truncated header");
    Program prog;
    prog.profile = be32(d, 0);
    uint32_t pcount = be32(d, 12), parr = be32(d, 16), progOff = be32(d, 20);
    uint32_t ucodeSize = be32(d, 24), ucodeOff = be32(d, 28);
    prog.vertex = prog.profile == 7003;
    for (uint32_t i = 0; i < pcount; ++i) {
        uint64_t o = parr + 48ull * i;
        if (o + 48 > d.size())
            throw FormatError("CgBinary: parameter table past end of data");
        Param p;
        p.type = int(be32(d, o));
        p.resource = int(be32(d, o + 4));
        p.variability = int(be32(d, o + 8));
        p.resIndex = int(be32(d, o + 12));
        uint32_t nameOff = be32(d, o + 16), defv = be32(d, o + 20), emb = be32(d, o + 24),
                 sem = be32(d, o + 28);
        p.direction = int(be32(d, o + 32));
        p.paramno = int(be32(d, o + 36));
        p.referenced = be32(d, o + 40) != 0;
        p.name = cstr(d, nameOff);
        p.semantic = cstr(d, sem);
        if (defv) {
            int n = typeWidth(p.type) * typeRows(p.type);
            for (int k = 0; k < n; ++k)
                p.defaults.push_back(std::bit_cast<float>(be32(d, defv + 4ull * uint64_t(k))));
        }
        if (emb) {
            uint32_t cnt = be32(d, emb);
            if (emb + 4ull + 4ull * cnt > d.size())
                throw FormatError("CgBinary: embedded constant list past end of data");
            for (uint32_t k = 0; k < cnt; ++k)
                p.embedded.push_back(be32(d, emb + 4ull + 4ull * k));
        }
        prog.params.push_back(std::move(p));
    }
    // a ucode range past the end of the file is clamped to it, not rejected
    size_t ub = std::min<size_t>(ucodeOff, d.size()), ue = std::min<size_t>(size_t(ucodeOff) + ucodeSize, d.size());
    prog.ucode.assign(d.begin() + ptrdiff_t(ub), d.begin() + ptrdiff_t(ue));
    if ((prog.profile == 7003 || prog.profile == 7004) && progOff + 24ull > d.size())
        throw FormatError("CgBinary: program header past end of data");
    if (prog.profile == 7003) {
        prog.vp = {be32(d, progOff), be32(d, progOff + 4ull), be32(d, progOff + 8ull), be32(d, progOff + 12ull),
                   be32(d, progOff + 16ull)};
    } else if (prog.profile == 7004) {
        prog.fp.instructionCount = be32(d, progOff);
        prog.fp.inputMask = be32(d, progOff + 4ull);
        prog.fp.texcoordMask = be16(d, progOff + 12ull);
        prog.fp.texcoord2d = be16(d, progOff + 14ull);
        prog.fp.registerCount = d[progOff + 18ull];
        prog.fp.outputFromH0 = d[progOff + 19ull];
        prog.fp.depthReplace = d[progOff + 20ull];
        prog.fp.pixelKill = d[progOff + 21ull];
    }
    return prog;
}

namespace {

std::vector<std::string> paramLines(const Program &prog) {
    std::vector<std::string> out{"// parameters:"};
    for (size_t i = 0; i < prog.params.size(); ++i) {
        const Param &p = prog.params[i];
        if (!p.referenced)
            continue;
        std::string s = "//  #" + padRight(std::to_string(i), 2) + " " + padRight(varName(p.variability), 7) + " " +
                        padRight(typeName(p.type), 11) + " " + padRight(p.name, 28) + " " +
                        padRight(cgResName(uint32_t(p.resource), p.resIndex), 12);
        if (!p.semantic.empty())
            s += " :" + p.semantic;
        s += std::string(" ") + dirName(p.direction);
        if (!p.defaults.empty()) {
            s += " = {";
            for (size_t k = 0; k < p.defaults.size(); ++k)
                s += (k ? ", " : "") + fmtf(p.defaults[k]);
            s += "}";
        }
        if (!p.embedded.empty()) {
            s += " patch@";
            for (size_t k = 0; k < p.embedded.size(); ++k)
                s += (k ? ",0x" : "0x") + hex(p.embedded[k]);
        }
        out.push_back(std::move(s));
    }
    return out;
}

void disasmVp(const Program &prog, std::vector<std::string> &out) {
    const VertexHeader &h = prog.vp;
    out.push_back("// sce_vp_rsx  instructions=" + std::to_string(h.instructionCount) +
                  " slot=" + std::to_string(h.instructionSlot) + " regs=" + std::to_string(h.registerCount) +
                  " in_mask=0x" + hex(h.inputMask) + " out_mask=0x" + hex(h.outputMask));
    for (auto &l : paramLines(prog))
        out.push_back(std::move(l));
    auto cmap = vpConstMap(prog);
    auto vin = varyingByRes(prog, 4097);
    auto vout = varyingByRes(prog, 4098);
    out.emplace_back();
    for (const VpInst &ins : decodeVp(prog)) {
        auto src = [&](int k) {
            const VpSrc &s = ins.srcs[size_t(k)];
            std::string r;
            if (s.kind == VpSrc::Temp) {
                r = "R" + std::to_string(s.reg);
            } else if (s.kind == VpSrc::In) {
                auto it = vin.find(vpInName(s.reg));
                r = it != vin.end() ? it->second : "v" + std::to_string(s.reg);
            } else if (s.kind == VpSrc::Const) {
                if (s.indexed) {
                    r = "c[A" + std::to_string(s.addrReg) + "." + "xyzw"[s.addrSwz] + "+" + std::to_string(s.reg) + "]";
                } else if (auto it = cmap.find(s.reg); it == cmap.end()) {
                    r = "c[" + std::to_string(s.reg) + "]";
                } else if (it->second.literal) {
                    r = "{";
                    for (size_t j = 0; j < it->second.values.size(); ++j)
                        r += (j ? "," : "") + fmtf(it->second.values[j]);
                    r += "}";
                } else {
                    r = it->second.name;
                }
            } else {
                r = "<bad>";
            }
            r += swzStr(s.swz);
            if (s.abs)
                r = "abs(" + r + ")";
            if (s.neg)
                r = "-" + r;
            return r;
        };
        auto dst = [&](const std::vector<VpDst> &lst, const std::string &mask) {
            std::string m = (mask == "xyzw" || mask.empty()) ? "" : "." + mask;
            std::string res;
            for (const VpDst &d : lst) {
                std::string n;
                if (d.kind == VpDst::Out) {
                    auto it = vout.find(vpOutName(d.value));
                    n = it != vout.end() ? it->second : std::string("o_") + vpOutName(d.value);
                } else if (d.kind == VpDst::Temp) {
                    n = "R" + std::to_string(d.value);
                } else if (d.kind == VpDst::Addr) {
                    n = "A0";
                } else {
                    n = "CC" + std::to_string(d.value);
                }
                res += (res.empty() ? "" : " = ") + n + m;
            }
            return res.empty() ? std::string("_") : res;
        };
        std::string condstr = ins.cond == 7 ? "" : "if (CC" + std::to_string(ins.ccSel) + swzStr(ins.ccSwz) + " " +
                                                     kCmp[ins.cond] + " 0) ";
        std::vector<std::string> line;
        if (ins.vecOp != "NOP") {
            std::string a = src(0), b = src(1), c = src(2);
            const std::string &op = ins.vecOp;
            std::string expr;
            if (op == "MOV") expr = a;
            else if (op == "MUL") expr = a + " * " + b;
            else if (op == "ADD") expr = a + " + " + c;
            else if (op == "MAD") expr = a + " * " + b + " + " + c;
            else if (op == "DP3") expr = "dot3(" + a + ", " + b + ")";
            else if (op == "DPH") expr = "dph(" + a + ", " + b + ")";
            else if (op == "DP4") expr = "dot4(" + a + ", " + b + ")";
            else if (op == "DST") expr = "dst(" + a + ", " + b + ")";
            else if (op == "MIN") expr = "min(" + a + ", " + b + ")";
            else if (op == "MAX") expr = "max(" + a + ", " + b + ")";
            else if (op == "SLT") expr = "(" + a + " < " + b + ")";
            else if (op == "SGE") expr = "(" + a + " >= " + b + ")";
            else if (op == "ARL") expr = "int(floor(" + a + "))";
            else if (op == "FRC") expr = "fract(" + a + ")";
            else if (op == "FLR") expr = "floor(" + a + ")";
            else if (op == "SEQ") expr = "(" + a + " == " + b + ")";
            else if (op == "SFL") expr = "0";
            else if (op == "SGT") expr = "(" + a + " > " + b + ")";
            else if (op == "SLE") expr = "(" + a + " <= " + b + ")";
            else if (op == "SNE") expr = "(" + a + " != " + b + ")";
            else if (op == "STR") expr = "1";
            else if (op == "SSG") expr = "sign(" + a + ")";
            else if (op == "TXL") expr = "textureLod(vtex" + std::to_string(ins.tex) + ", " + a + ")";
            else expr = op + "(" + a + ", " + b + ", " + c + ")";
            if (ins.sat)
                expr = "sat(" + expr + ")";
            line.push_back(condstr + dst(ins.vecDst, ins.vmask) + " = " + expr + ";");
        }
        if (ins.scaOp != "NOP") {
            std::string s = src(2);
            const std::string &op = ins.scaOp;
            if (isVpFlow(op)) {
                line.push_back(condstr + op + " " + std::to_string(ins.addr) + ";");
            } else {
                std::string expr;
                if (op == "MOV") expr = s;
                else if (op == "RCP") expr = "1/(" + s + ")";
                else if (op == "RCC") expr = "rcc(" + s + ")";
                else if (op == "RSQ") expr = "rsqrt(" + s + ")";
                else if (op == "EXP") expr = "exp_lp(" + s + ")";
                else if (op == "LOG") expr = "log_lp(" + s + ")";
                else if (op == "LIT") expr = "lit(" + s + ")";
                else if (op == "LG2") expr = "log2(" + s + ")";
                else if (op == "EX2") expr = "exp2(" + s + ")";
                else if (op == "SIN") expr = "sin(" + s + ")";
                else if (op == "COS") expr = "cos(" + s + ")";
                else expr = op + "(" + s + ")";
                if (ins.sat)
                    expr = "sat(" + expr + ")";
                line.push_back(condstr + dst(ins.scaDst, ins.smask) + " = " + expr + ";");
            }
        }
        if (line.empty())
            line.emplace_back("NOP;");
        std::string joined;
        for (size_t j = 0; j < line.size(); ++j)
            joined += (j ? "  ||  " : "") + line[j];
        out.push_back(padLeft(std::to_string(ins.index), 3) + ": " + joined + (ins.end ? "   // END" : ""));
    }
}

std::string fpExpr(const std::string &op, const std::string &a, const std::string &b, const std::string &c,
                   const std::string &t) {
    static const std::map<std::string, std::string> unary = {
        {"FRC", "fract"}, {"FLR", "floor"}, {"RSQ", "rsqrt"}, {"EX2", "exp2"}, {"LG2", "log2"}, {"LIT", "lit"},
        {"COS", "cos"}, {"SIN", "sin"}, {"DDX", "dFdx"}, {"DDY", "dFdy"}, {"NRM", "normalize3"},
        {"PK2", "pack_half2"}, {"UP2", "unpack_half2"}, {"PK4", "pack_snorm4"}, {"UP4", "unpack_snorm4"},
        {"PKB", "pack_unorm4"}, {"UPB", "unpack_unorm4"}, {"PK16", "pack_unorm2x16"},
        {"UP16", "unpack_unorm2x16"}};
    static const std::map<std::string, std::string> binary = {
        {"DP3", "dot3"}, {"DP4", "dot4"}, {"DP2", "dot2"}, {"DST", "dst"}, {"MIN", "min"},
        {"MAX", "max"}, {"POW", "pow"}, {"REFL", "reflect"}};
    static const std::map<std::string, std::string> cmp = {{"SLT", " < "}, {"SGE", " >= "}, {"SLE", " <= "},
                                                           {"SGT", " > "}, {"SNE", " != "}, {"SEQ", " == "}};
    static const std::map<std::string, std::string> tex = {
        {"TEX", "texture"}, {"TXP", "textureProj"}, {"TXB", "textureBias"}, {"TXL", "textureLod"}};
    if (auto it = unary.find(op); it != unary.end())
        return it->second + "(" + a + ")";
    if (auto it = binary.find(op); it != binary.end())
        return it->second + "(" + a + ", " + b + ")";
    if (auto it = cmp.find(op); it != cmp.end())
        return "(" + a + it->second + b + ")";
    if (auto it = tex.find(op); it != tex.end())
        return it->second + "(" + t + ", " + a + ")";
    if (op == "MOV") return a;
    if (op == "MUL") return a + " * " + b;
    if (op == "ADD") return a + " + " + b;
    if (op == "MAD") return a + " * " + b + " + " + c;
    if (op == "DP2A") return "dot2(" + a + ", " + b + ") + " + c;
    if (op == "SFL") return "0";
    if (op == "STR") return "1";
    if (op == "RCP") return "1/(" + a + ")";
    if (op == "LRP") return "mix(" + c + ", " + b + ", " + a + ")";
    if (op == "DIV") return a + " / " + b;
    if (op == "DIVSQ") return a + " / sqrt(" + b + ")";
    if (op == "TXD") return "textureGrad(" + t + ", " + a + ", " + b + ", " + c + ")";
    return op + "(" + a + ", " + b + ", " + c + ")";
}

void disasmFp(const Program &prog, std::vector<std::string> &out) {
    const FragmentHeader &h = prog.fp;
    out.push_back("// sce_fp_rsx  slots=" + std::to_string(h.instructionCount) + " in_mask=0x" + hex(h.inputMask) +
                  " texcoords=0x" + hex(h.texcoordMask) + " tc2d=0x" + hex(h.texcoord2d) +
                  " regs=" + std::to_string(h.registerCount) + " outputFromH0=" + std::to_string(h.outputFromH0) +
                  " depthReplace=" + std::to_string(h.depthReplace) + " kill=" + std::to_string(h.pixelKill));
    for (auto &l : paramLines(prog))
        out.push_back(std::move(l));
    std::map<int, std::string> samplers;
    for (const Param &p : prog.params)
        if (uint32_t(p.resource) >= 2048 && uint32_t(p.resource) <= 2063)
            samplers[p.resource - 2048] = p.name;
    auto vin = varyingByRes(prog, 4097);
    out.emplace_back();
    auto insts = decodeFp(prog);
    for (size_t idx = 0; idx < insts.size(); ++idx) {
        const FpInst &ins = insts[idx];
        auto src = [&](int k) {
            const FpSrc &s = ins.srcs[size_t(k)];
            std::string r;
            if (s.kind == FpSrc::Temp) {
                r = (s.fp16 ? "H" : "R") + std::to_string(s.reg);
            } else if (s.kind == FpSrc::In) {
                const char *res = fpInRes(s.inReg);
                auto it = vin.find(res ? res : "");
                r = it != vin.end() ? it->second : s.inReg;
            } else if (s.kind == FpSrc::Const) {
                if (ins.constName && !ins.constName->empty()) {
                    r = *ins.constName;
                } else {
                    // a flow-control op has no inline constant (malformed input): print {} rather than fail
                    r = "{";
                    if (ins.hasConst)
                        for (size_t j = 0; j < 4; ++j)
                            r += (j ? ", " : "") + fmtf(ins.konst[j]);
                    r += "}";
                }
            } else {
                r = "<src3>";
            }
            r += swzStr(s.swz);
            if (s.abs)
                r = "abs(" + r + ")";
            if (s.neg)
                r = "-" + r;
            return r;
        };
        std::string a = src(0), b = src(1), c = src(2);
        auto st = samplers.find(ins.tex);
        std::string tname = st != samplers.end() ? st->second : "tex" + std::to_string(ins.tex);
        std::string m = ins.mask == "xyzw" ? "" : "." + ins.mask;
        std::string dreg = ins.hasDst ? ins.dstFile + std::to_string(ins.dstReg) : "_";
        if (ins.setCc)
            dreg = (dreg == "_" ? "" : dreg + m + " = ") + "CC" + std::to_string(ins.ccReg);
        const std::string &op = ins.op;
        std::string condstr = ins.cond == 7 ? "" : "if (CC" + std::to_string(ins.condReg) + swzStr(ins.condSwz) +
                                                     " " + kCmp[ins.cond] + " 0) ";
        std::string line;
        if (op == "KIL") {
            line = condstr + "discard;";
        } else if (isFpFlow(op) || op == "FENCT" || op == "FENCB") {
            line = op + " " + condstr + "(raw=";
            for (size_t j = 0; j < 4; ++j)
                line += (j ? " " : "") + hex(ins.raw[j], 8);
            line += ")";
        } else if (op == "NOP") {
            line = "NOP;";
        } else {
            std::string e = fpExpr(op, a, b, c, tname);
            if (ins.scale != 1.0)
                e += " * " + fmtG(ins.scale, 6);
            if (ins.sat)
                e = "sat(" + e + ")";
            static const char *const kPrec[] = {"", "_h", "_x12", "_x9"};
            line = condstr + dreg + m + " = " + e + ";   // " + op + kPrec[ins.prec];
        }
        out.push_back(padLeft(std::to_string(idx), 3) + ": " + line + (ins.end ? "   // END" : ""));
    }
}

}  // namespace

std::string disassemble(const Program &prog, const std::string &name) {
    std::vector<std::string> out{"// ==== " + name + " ===="};
    if (prog.profile == 7003)
        disasmVp(prog, out);
    else if (prog.profile == 7004)
        disasmFp(prog, out);
    else
        out.push_back("// unknown profile " + std::to_string(prog.profile));
    std::string s;
    for (const auto &l : out)
        s += l + "\n";
    return s;
}

}  // namespace moyou::rsx
