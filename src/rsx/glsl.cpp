// GLSL emission and the particle program post-processing.
// Every RSX register becomes a vec4 and every instruction a masked assignment; condition codes are honoured per
// component. Varyings are named by resource (v_TEXCOORD0, ...) so any VP/FP pair links like on the RSX.
#include "rsx/decode.h"
#include "rsx/rsx.h"

#include <algorithm>
#include <optional>
#include <set>
#include <tuple>

namespace moyou::rsx {
namespace {

using namespace detail;

constexpr const char *kC = "xyzw";

const char *const kLitFn = R"(vec4 rsx_lit(vec4 s) {
    float d = max(s.x, 0.0);
    float sp = s.x > 0.0 ? exp2(clamp(s.w, -128.0, 128.0) * log2(max(s.y, 1e-30))) : 0.0;
    return vec4(1.0, d, sp, 1.0);
})";

// --hwprec: H registers and _h ops round to fp16, _x12 / _x9 ops to the RSX fixed-point formats
const char *const kHwprecFns =
    R"(vec4 rsx_h(vec4 v) { return vec4(unpackHalf2x16(packHalf2x16(v.xy)), unpackHalf2x16(packHalf2x16(v.zw))); }
vec4 rsx_x12(vec4 v) { return clamp(floor(v * 1024.0 + 0.5) / 1024.0, -2.0, 2.0 - 1.0 / 1024.0); }
vec4 rsx_x9(vec4 v) { return clamp(floor(v * 256.0 + 0.5) / 256.0, -1.0, 1.0 - 1.0 / 256.0); })";

std::string glslType(int type) {
    switch (type) {
    case 1045: case 1091: case 1031: return "float";
    case 1046: case 1032: return "vec2";
    case 1047: case 1033: return "vec3";
    case 1048: case 1034: return "vec4";
    case 1064: return "mat4";
    default: return "vec4";
    }
}

std::string samplerType(int type) {
    switch (type) {
    case 1067: return "sampler3D";
    case 1069: return "samplerCube";
    default: return "sampler2D";
    }
}

std::string vec4lit(const auto &vals) {
    std::string s = "vec4(";
    bool first = true;
    for (double v : vals) {
        s += (first ? "" : ", ") + flit(v);
        first = false;
    }
    return s + ")";
}

std::string swz(const std::array<int, 4> &s) {
    std::string r;
    for (int c : s)
        r += kC[c];
    return r;
}

std::string join(const std::vector<std::string> &lines) {
    std::string s;
    for (size_t i = 0; i < lines.size(); ++i)
        s += (i ? "\n" : "") + lines[i];
    return s;
}

std::vector<std::string> header(bool gl330, bool fp) {
    if (gl330)
        return {"#version 330 core"};
    std::vector<std::string> h{"#version 300 es", "precision highp float;", "precision highp int;"};
    if (fp)
        h.emplace_back("precision highp sampler2D;");
    return h;
}

std::optional<std::string> scalarFn(const std::string &op, const std::string &a) {
    std::string x = "(" + a + ").x";
    if (op == "RCP") return "vec4(1.0 / " + x + ")";
    if (op == "RCC") return "vec4(clamp(1.0 / " + x + ", 5.42101e-20, 1.884467e19))";
    if (op == "RSQ") return "vec4(inversesqrt(abs(" + x + ")))";
    if (op == "EX2" || op == "EXP") return "vec4(exp2(" + x + "))";
    if (op == "LG2" || op == "LOG") return "vec4(log2(" + x + "))";
    if (op == "SIN") return "vec4(sin(" + x + "))";
    if (op == "COS") return "vec4(cos(" + x + "))";
    return std::nullopt;
}

std::optional<std::string> vecExpr(const std::string &op, const std::string &a, const std::string &b,
                                   const std::string &c) {
    if (auto sf = scalarFn(op, a))
        return sf;
    auto p = [](const std::string &s) { return "(" + s + ")"; };
    if (op == "MOV") return a;
    if (op == "MUL") return a + " * " + b;
    if (op == "ADD") return a + " + " + b;
    if (op == "MAD") return a + " * " + b + " + " + c;
    if (op == "DP2") return "vec4(dot(" + p(a) + ".xy, " + p(b) + ".xy))";
    if (op == "DP2A") return "vec4(dot(" + p(a) + ".xy, " + p(b) + ".xy) + " + p(c) + ".x)";
    if (op == "DP3") return "vec4(dot(" + p(a) + ".xyz, " + p(b) + ".xyz))";
    if (op == "DP4") return "vec4(dot(" + a + ", " + b + "))";
    if (op == "DPH") return "vec4(dot(" + p(a) + ".xyz, " + p(b) + ".xyz) + " + p(b) + ".w)";
    if (op == "DST") return "vec4(1.0, " + p(a) + ".y * " + p(b) + ".y, " + p(a) + ".z, " + p(b) + ".w)";
    if (op == "MIN") return "min(" + a + ", " + b + ")";
    if (op == "MAX") return "max(" + a + ", " + b + ")";
    if (op == "SLT") return "vec4(lessThan(" + a + ", " + b + "))";
    if (op == "SGE") return "vec4(greaterThanEqual(" + a + ", " + b + "))";
    if (op == "SLE") return "vec4(lessThanEqual(" + a + ", " + b + "))";
    if (op == "SGT") return "vec4(greaterThan(" + a + ", " + b + "))";
    if (op == "SNE") return "vec4(notEqual(" + a + ", " + b + "))";
    if (op == "SEQ") return "vec4(equal(" + a + ", " + b + "))";
    if (op == "SFL") return "vec4(0.0)";
    if (op == "STR") return "vec4(1.0)";
    if (op == "FRC") return "fract(" + a + ")";
    if (op == "FLR") return "floor(" + a + ")";
    if (op == "SSG") return "sign(" + a + ")";
    if (op == "LRP") return "mix(" + c + ", " + b + ", " + a + ")";
    if (op == "POW") return "vec4(pow(" + p(a) + ".x, " + p(b) + ".x))";
    if (op == "DIV") return a + " / " + p(b) + ".x";
    if (op == "DIVSQ") return a + " * inversesqrt(" + p(b) + ".x)";
    if (op == "NRM") return "vec4(normalize(" + p(a) + ".xyz), 0.0).xyzz";
    if (op == "DDX") return "dFdx(" + a + ")";
    if (op == "DDY") return "dFdy(" + a + ")";
    if (op == "REFL") return "vec4(reflect(" + p(b) + ".xyz, " + p(a) + ".xyz), 0.0)";
    if (op == "LIT") return "rsx_lit(" + a + ")";
    if (op == "PK2") return "vec4(uintBitsToFloat(packHalf2x16(" + p(a) + ".xy)))";
    if (op == "UP2") return "unpackHalf2x16(floatBitsToUint(" + p(a) + ".x)).xyxy";
    if (op == "PK4") return "vec4(uintBitsToFloat(packSnorm4x8(" + a + ")))";
    if (op == "UP4") return "unpackSnorm4x8(floatBitsToUint(" + p(a) + ".x))";
    if (op == "PKB") return "vec4(uintBitsToFloat(packUnorm4x8(" + a + ")))";
    if (op == "UPB") return "unpackUnorm4x8(floatBitsToUint(" + p(a) + ".x))";
    if (op == "PK16") return "vec4(uintBitsToFloat(packUnorm2x16(" + p(a) + ".xy)))";
    if (op == "UP16") return "unpackUnorm2x16(floatBitsToUint(" + p(a) + ".x)).xyxy";
    return std::nullopt;
}

std::string condTest(int code, const std::string &v) {
    static const char *const ops[] = {"", " < 0.0", " == 0.0", " <= 0.0", " > 0.0", " != 0.0", " >= 0.0", ""};
    if (code == 7) return "true";
    if (code == 0) return "false";
    return v + ops[code];
}

struct Cond {
    int code;
    std::string reg;
    std::array<int, 4> swz;
};

void emitWrite(std::vector<std::string> &lines, const std::vector<std::string> &dsts, const std::string &mask,
               std::string e, const std::optional<Cond> &cond, bool sat, double scale, int prec, bool hwprec) {
    if (mask.empty())
        return;
    if (scale != 1.0)
        e = "(" + e + ") * " + flit(scale);
    if (sat)
        e = "clamp(" + e + ", 0.0, 1.0)";
    if (hwprec) {
        if (prec == 2)
            e = "rsx_x12(" + e + ")";
        else if (prec == 3)
            e = "rsx_x9(" + e + ")";
        else if (prec == 1 || std::any_of(dsts.begin(), dsts.end(), [](auto &d) { return d.starts_with('H'); }))
            e = "rsx_h(" + e + ")";
    }
    if (!cond) {
        if (dsts.size() == 1 && mask == "xyzw") {
            lines.push_back("    " + dsts[0] + " = " + e + ";");
        } else {
            lines.push_back("    t = " + e + ";");
            for (auto &d : dsts)
                lines.push_back("    " + d + "." + mask + " = t." + mask + ";");
        }
    } else {
        lines.push_back("    t = " + e + ";");
        for (int i = 0; i < 4; ++i) {
            char ch = kC[i];
            if (mask.find(ch) == std::string::npos)
                continue;
            std::string test = condTest(cond->code, cond->reg + "." + kC[cond->swz[size_t(i)]]);
            for (auto &d : dsts)
                lines.push_back("    if (" + test + ") " + d + "." + ch + " = t." + ch + ";");
        }
    }
}

// re.match(r"(.*)\[(\d+)\]$", name): matrix-row / array-element uniform names
std::optional<std::pair<std::string, std::string>> splitIndexed(const std::string &name) {
    if (name.size() < 3 || name.back() != ']')
        return std::nullopt;
    size_t lb = name.rfind('[');
    if (lb == std::string::npos || lb + 2 > name.size() - 1)
        return std::nullopt;
    std::string digits = name.substr(lb + 1, name.size() - lb - 2);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return std::pair{name.substr(0, lb), digits};
}

std::string fpToGlsl(const Program &prog, const Options &opt, const std::string &name) {
    const FragmentHeader &h = prog.fp;
    auto insts = decodeFp(prog);
    auto lines = header(opt.gl330, true);
    lines.push_back("// transpiled from " + name + " (sce_fp_rsx, " + std::to_string(h.instructionCount) + " slots)");
    struct Sampler { std::string name, type; bool rect; };
    struct Uniform { std::string name, type, init; std::vector<uint32_t> offsets; };
    std::map<int, Sampler> samplers;
    std::vector<Uniform> uniforms;
    for (const Param &p : prog.params) {
        if (!p.referenced)
            continue;
        if (uint32_t(p.resource) >= 2048 && uint32_t(p.resource) <= 2063) {
            samplers[p.resource - 2048] = {ident(p.name), samplerType(p.type), p.type == 1068};
        } else if (!p.embedded.empty()) {
            std::string t = glslType(p.type), init;
            if (!p.defaults.empty()) {
                if (t != "float") {
                    init = " = " + t + "(";
                    for (size_t k = 0; k < p.defaults.size(); ++k)
                        init += (k ? ", " : "") + flit(p.defaults[k]);
                    init += ")";
                } else {
                    init = " = " + flit(p.defaults[0]);
                }
            }
            uniforms.push_back({ident(p.name), t, init, p.embedded});
        }
    }
    std::map<uint32_t, std::pair<std::string, std::string>> emb;
    for (auto &u : uniforms)
        for (uint32_t o : u.offsets)
            emb[o] = {u.name, u.type};
    std::set<std::string> usedIn;
    for (auto &ins : insts)
        for (auto &s : ins.srcs)
            if (s.kind == FpSrc::In)
                usedIn.insert(s.inReg);
    auto inRes = [](const std::string &r) {
        const char *res = fpInRes(r);
        return res ? std::string(res) : r;
    };
    for (auto &r : usedIn)
        if (r != "WPOS")
            lines.push_back("in vec4 v_" + inRes(r) + ";");
    for (auto &u : uniforms) {
        lines.push_back("uniform " + u.type + " " + u.name + (opt.gl330 ? u.init : "") + ";");
        if (!u.init.empty() && !opt.gl330)
            lines.back() += "   // default" + u.init;
    }
    for (auto &[unit, s] : samplers)
        lines.push_back("uniform " + s.type + " " + s.name + ";   // TEXUNIT" + std::to_string(unit) +
                        (s.rect ? " (samplerRECT: unnormalized coords)" : ""));
    // MRT outputs: R0/R2/R3/R4, or H0/H4/H6/H8 when the program outputs from the half registers
    const int mrt[4] = {0, h.outputFromH0 ? 4 : 2, h.outputFromH0 ? 6 : 3, h.outputFromH0 ? 8 : 4};
    const char base = h.outputFromH0 ? 'H' : 'R';
    std::set<std::string> regs;
    for (auto &ins : insts) {
        if (ins.hasDst)
            regs.insert(ins.dstFile + std::to_string(ins.dstReg));
        for (auto &s : ins.srcs)
            if (s.kind == FpSrc::Temp)
                regs.insert((s.fp16 ? "H" : "R") + std::to_string(s.reg));
    }
    std::vector<int> outs;
    for (int i = 0; i < 4; ++i)
        if (regs.contains(base + std::to_string(mrt[i])))
            outs.push_back(i);
    for (int i : outs)
        lines.push_back("layout(location = " + std::to_string(i) + ") out vec4 o_COLOR" + std::to_string(i) + ";");
    if (std::any_of(insts.begin(), insts.end(), [](auto &i) { return i.op == "LIT"; }))
        lines.emplace_back(kLitFn);
    if (opt.hwprec)
        lines.emplace_back(kHwprecFns);
    lines.emplace_back("void main() {");
    lines.emplace_back("    vec4 t;");
    lines.emplace_back("    vec4 CC0 = vec4(0.0), CC1 = vec4(0.0);");
    std::vector<std::string> sortedRegs(regs.begin(), regs.end());
    std::sort(sortedRegs.begin(), sortedRegs.end(), [](const std::string &a, const std::string &b) {
        return std::tuple(a[0], std::stoi(a.substr(1))) < std::tuple(b[0], std::stoi(b.substr(1)));
    });
    for (auto &r : sortedRegs)
        lines.push_back("    vec4 " + r + " = vec4(0.0);");
    for (const FpInst &ins : insts) {
        const std::string &op = ins.op;
        if (op == "NOP" || op == "FENCT" || op == "FENCB")
            continue;
        if (isFpFlow(op)) {
            lines.push_back("    // UNSUPPORTED flow control " + op);
            continue;
        }
        auto src = [&](int k) {
            const FpSrc &s = ins.srcs[size_t(k)];
            std::string r;
            if (s.kind == FpSrc::Temp) {
                r = (s.fp16 ? "H" : "R") + std::to_string(s.reg);
            } else if (s.kind == FpSrc::In) {
                if (s.inReg == "WPOS")
                    r = "gl_FragCoord";
                else if (s.inReg == "COL0" || s.inReg == "COL1")  // colour inputs are clamped to [0, 1]
                    r = "clamp(v_" + inRes(s.inReg) + ", 0.0, 1.0)";
                else
                    r = "v_" + inRes(s.inReg);
            } else if (s.kind == FpSrc::Const) {
                auto it = emb.find(ins.offset + 16);
                if (ins.constName && !ins.constName->empty() && it != emb.end()) {
                    const auto &[nm, t] = it->second;
                    if (t == "float") r = "vec4(" + nm + ")";
                    else if (t == "vec2") r = "vec4(" + nm + ", 0.0, 0.0)";
                    else if (t == "vec3") r = "vec4(" + nm + ", 0.0)";
                    else if (t == "vec4") r = nm;
                    else r = "vec4(" + nm + ")";
                } else {
                    r = vec4lit(ins.konst);
                }
            } else {
                r = "vec4(0.0)";
            }
            std::string sw = swz(s.swz);
            if (sw != "xyzw")
                r = "(" + r + ")." + sw;
            // operand precision modifier, applied as RPCS3 does: saturate before abs, the others after
            int pm = s.kind == FpSrc::Bad || (s.kind == FpSrc::In && s.inReg == "SSA") ? 0 : s.pmod;
            if (pm == 1 && s.kind == FpSrc::Temp && s.fp16)   // half of an H register: nothing to do
                pm = 0;
            if (pm == 4)
                r = "clamp(" + r + ", 0.0, 1.0)";
            if (s.abs)
                r = "abs(" + r + ")";
            if (pm >= 1 && pm <= 3 && opt.hwprec)
                r = std::string(pm == 1 ? "rsx_h(" : pm == 2 ? "rsx_x12(" : "rsx_x9(") + r + ")";
            if (s.neg)
                r = "-(" + r + ")";
            return r;
        };
        if (ins.cond == 0)
            continue;
        std::optional<Cond> cond;
        if (ins.cond != 7)
            cond = Cond{ins.cond, "CC" + std::to_string(ins.condReg), ins.condSwz};
        if (op == "KIL") {
            if (!cond) {
                lines.emplace_back("    discard;");
            } else {
                std::string tests;
                for (int i = 0; i < 4; ++i)
                    tests += (i ? " || " : "") +
                             condTest(ins.cond, "CC" + std::to_string(ins.condReg) + "." + kC[ins.condSwz[size_t(i)]]);
                lines.push_back("    if (" + tests + ") discard;");
            }
            continue;
        }
        std::string a = src(0), b = src(1), c = src(2);
        std::optional<std::string> expr;
        if (op == "TEX" || op == "TXP" || op == "TXB" || op == "TXL" || op == "TXD") {
            auto it = samplers.find(ins.tex);
            Sampler smp = it != samplers.end() ? it->second : Sampler{"tex" + std::to_string(ins.tex), "sampler2D", false};
            std::string crd = "(" + a + ").xy";
            if (smp.rect)
                crd = "(" + crd + ") / vec2(textureSize(" + smp.name + ", 0))";
            if (op == "TEX") expr = "texture(" + smp.name + ", " + crd + ")";
            else if (op == "TXP") expr = "textureProj(" + smp.name + ", (" + a + ").xyw)";
            else if (op == "TXB") expr = "texture(" + smp.name + ", " + crd + ", (" + a + ").w)";
            else if (op == "TXL") expr = "textureLod(" + smp.name + ", " + crd + ", (" + a + ").w)";
            else expr = "textureGrad(" + smp.name + ", " + crd + ", (" + b + ").xy, (" + c + ").xy)";
        } else {
            expr = vecExpr(op, a, b, c);
        }
        if (!expr) {
            lines.push_back("    // UNSUPPORTED op " + op);
            continue;
        }
        std::vector<std::string> dsts;
        if (ins.hasDst)
            dsts.push_back(ins.dstFile + std::to_string(ins.dstReg));
        if (ins.setCc)
            dsts.push_back("CC" + std::to_string(ins.ccReg));
        if (!dsts.empty())
            emitWrite(lines, dsts, ins.mask, *expr, cond, ins.sat, ins.scale, ins.prec, opt.hwprec);
    }
    for (int i : outs)
        lines.push_back("    o_COLOR" + std::to_string(i) + " = " + base + std::to_string(mrt[i]) + ";");
    lines.emplace_back("}");
    return join(lines) + "\n";
}

int vpOutIndex(const std::string &o) {
    for (int i = 0; i < 16; ++i)
        if (o == vpOutName(i))
            return i;
    return -1;
}

std::string varyingName(int out) {
    std::string o = vpOutName(out);
    if (o == "COL0") return "COLOR0";
    if (o == "COL1") return "COLOR1";
    return o;
}

std::string vpToGlsl(const Program &prog, const Options &opt, const std::string &name) {
    auto insts = decodeVp(prog);
    auto cmap = vpConstMap(prog);
    auto vin = varyingByRes(prog, 4097);
    auto lines = header(opt.gl330, false);
    lines.push_back("// transpiled from " + name + " (sce_vp_rsx, " + std::to_string(insts.size()) + " instructions)");
    std::set<int> usedIn, usedC, usedOut, regs;
    for (auto &ins : insts) {
        for (auto &s : ins.srcs) {
            if (s.kind == VpSrc::In) usedIn.insert(s.reg);
            else if (s.kind == VpSrc::Const) usedC.insert(s.reg);
            else if (s.kind == VpSrc::Temp) regs.insert(s.reg);
        }
        for (auto *lst : {&ins.vecDst, &ins.scaDst})
            for (auto &d : *lst) {
                if (d.kind == VpDst::Out) usedOut.insert(d.value);
                else if (d.kind == VpDst::Temp) regs.insert(d.value);
            }
    }
    auto attrName = [&](int r) {
        auto it = vin.find(vpInName(r));
        return it != vin.end() ? it->second : "attr" + std::to_string(r);
    };
    for (int r : usedIn)
        lines.push_back("layout(location = " + std::to_string(r) + ") in vec4 a_" + ident(attrName(r)) + ";   // " +
                        vpInName(r));
    auto paramType = [&](const std::string &n) {
        for (const Param &p : prog.params)
            if (p.name == n)
                return glslType(p.type);
        return std::string("vec4");
    };
    // uniforms: matrix rows are grouped back into arrays (dict keyed by name, first insertion fixes the order)
    std::vector<std::pair<std::string, std::string>> uniDecl;
    auto declare = [&](const std::string &key, std::string decl) {
        for (auto &[k, v] : uniDecl)
            if (k == key) {
                v = std::move(decl);
                return;
            }
        uniDecl.emplace_back(key, std::move(decl));
    };
    for (int idx : usedC) {
        auto it = cmap.find(idx);
        if (it == cmap.end()) {
            declare("c" + std::to_string(idx), "uniform vec4 c" + std::to_string(idx) + ";");
        } else if (!it->second.literal) {
            const std::string &n = it->second.name;
            if (auto m = splitIndexed(n))
                declare(m->first, "uniform vec4 " + ident(m->first) + "[4];   // matrix rows (RSX c[] layout)");
            else
                declare(n, "uniform " + paramType(n) + " " + ident(n) + ";");
        }
    }
    for (auto &[k, v] : uniDecl)
        lines.push_back(v);
    for (int o : usedOut)  // std::set<int> iterates in VP_OUT order
        if (o != 0)
            lines.push_back("out vec4 v_" + varyingName(o) + ";");
    lines.emplace_back("void main() {");
    lines.emplace_back("    vec4 t, ts;");
    lines.emplace_back("    vec4 CC0 = vec4(0.0), CC1 = vec4(0.0);");
    lines.emplace_back("    ivec4 A0 = ivec4(0);");
    for (int r : regs)
        lines.push_back("    vec4 R" + std::to_string(r) + " = vec4(0.0);");
    // in RSX output-register order, so the output is the same on every run
    for (int o : usedOut)
        lines.push_back(std::string("    vec4 o_") + vpOutName(o) + " = vec4(0.0, 0.0, 0.0, 1.0);");

    auto constRef = [&](int idx) -> std::string {
        auto it = cmap.find(idx);
        if (it == cmap.end())
            return "c" + std::to_string(idx);
        if (it->second.literal)
            return vec4lit(it->second.values);
        const std::string &n = it->second.name;
        if (auto m = splitIndexed(n))
            return ident(m->first) + "[" + m->second + "]";
        std::string t = paramType(n);
        if (t == "float") return "vec4(" + ident(n) + ")";
        if (t == "vec2") return "vec4(" + ident(n) + ", 0.0, 0.0)";
        if (t == "vec3") return "vec4(" + ident(n) + ", 0.0)";
        return ident(n);
    };

    for (const VpInst &ins : insts) {
        auto src = [&](int k) {
            const VpSrc &s = ins.srcs[size_t(k)];
            std::string r;
            if (s.kind == VpSrc::Temp)
                r = "R" + std::to_string(s.reg);
            else if (s.kind == VpSrc::In)
                r = "a_" + ident(attrName(s.reg));
            else if (s.kind == VpSrc::Const)
                r = s.indexed ? "/*indexed*/ c[A0." + std::string(1, kC[s.addrSwz]) + " + " + std::to_string(s.reg) + "]"
                              : constRef(s.reg);
            else
                r = "vec4(0.0)";
            std::string sw = swz(s.swz);
            if (sw != "xyzw")
                r = "(" + r + ")." + sw;
            if (s.abs)
                r = "abs(" + r + ")";
            if (s.neg)
                r = "-(" + r + ")";
            return r;
        };
        std::optional<Cond> cond;
        if (ins.cond != 7)
            cond = Cond{ins.cond, "CC" + std::to_string(ins.ccSel), ins.ccSwz};
        struct Write { const char *tv; const std::vector<VpDst> *dsts; const std::string *mask; };
        std::vector<Write> writes;
        if (ins.vecOp != "NOP") {
            const std::string &op = ins.vecOp;
            std::string a = src(0), b = src(1), c = src(2);
            if (op == "ADD")  // the VP adder takes its second operand from src2
                b = c;
            std::optional<std::string> expr;
            if (op == "TXL")
                expr = "textureLod(vtex" + std::to_string(ins.tex) + ", (" + a + ").xy, (" + a + ").w)";
            else if (op == "ARL")
                lines.push_back("    A0 = ivec4(floor(" + a + "));");
            else
                expr = vecExpr(op, a, b, c);
            if (expr && !expr->empty()) {
                lines.push_back("    t = " + *expr + ";");
                writes.push_back({"t", &ins.vecDst, &ins.vmask});
            }
        }
        if (ins.scaOp != "NOP") {
            const std::string &op = ins.scaOp;
            if (isVpFlow(op)) {
                lines.push_back("    // UNSUPPORTED flow control " + op);
            } else {
                std::string s = src(2);
                lines.push_back("    ts = " + vecExpr(op, s, s, s).value_or("None") + ";");
                writes.push_back({"ts", &ins.scaDst, &ins.smask});
            }
        }
        for (const Write &w : writes) {
            std::vector<std::string> names;
            for (const VpDst &d : *w.dsts) {
                if (d.kind == VpDst::Temp) names.push_back("R" + std::to_string(d.value));
                else if (d.kind == VpDst::Out) names.push_back(std::string("o_") + vpOutName(d.value));
                else if (d.kind == VpDst::Cc) names.push_back("CC" + std::to_string(d.value));
            }
            std::string val = ins.sat ? "clamp(" + std::string(w.tv) + ", 0.0, 1.0)" : std::string(w.tv);
            const std::string &mask = *w.mask;
            for (auto &n : names) {
                if (!cond) {
                    lines.push_back(mask != "xyzw" ? "    " + n + "." + mask + " = (" + val + ")." + mask + ";"
                                                   : "    " + n + " = " + val + ";");
                } else {
                    for (int i = 0; i < 4; ++i) {
                        char ch = kC[i];
                        if (mask.find(ch) == std::string::npos)
                            continue;
                        lines.push_back("    if (" + condTest(cond->code, cond->reg + "." + kC[ins.ccSwz[size_t(i)]]) +
                                        ") " + n + "." + ch + " = (" + val + ")." + ch + ";");
                    }
                }
            }
        }
    }
    for (int o : usedOut) {
        if (o == 0)
            lines.push_back("    gl_Position = o_HPOS;");
        else
            lines.push_back("    v_" + varyingName(o) + " = o_" + vpOutName(o) + ";");
    }
    lines.emplace_back("}");
    return join(lines) + "\n";
}

std::string replaceAll(std::string s, const std::string &from, const std::string &to) {
    for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
        s.replace(pos, from.size(), to);
    return s;
}

}  // namespace

std::string transpile(const Program &prog, const Options &opt, const std::string &name) {
    return prog.vertex ? vpToGlsl(prog, opt, name) : fpToGlsl(prog, opt, name);
}

std::string postParticleVP(const std::string &glsl) {
    const std::string pos = "gl_Position = o_HPOS;";
    if (glsl.find(pos) == std::string::npos)
        throw FormatError("postParticleVP: no gl_Position = o_HPOS;");
    std::string src = replaceAll(glsl, pos, "gl_Position = vec4(o_HPOS.x, o_HPOS.y * u_flipY, o_HPOS.z, o_HPOS.w);");
    // declare the flip uniform after the last uniform declaration
    std::vector<std::string> lines;
    for (size_t b = 0;;) {
        size_t e = src.find('\n', b);
        lines.push_back(src.substr(b, e == std::string::npos ? std::string::npos : e - b));
        if (e == std::string::npos)
            break;
        b = e + 1;
    }
    std::optional<size_t> last;
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].starts_with("uniform "))
            last = i;
    if (!last)
        throw FormatError("postParticleVP: no uniform declaration");
    lines.insert(lines.begin() + ptrdiff_t(*last + 1),
                 "uniform float u_flipY;   // +1/-1: framebuffer orientation (RSX viewport y-scale -540)");
    return join(lines);
}

std::string postParticleFP(const std::string &glsl) {
    std::string src = glsl;
    for (int k : {1, 2}) {
        std::string decl = "layout(location = " + std::to_string(k) + ") out vec4 o_COLOR" + std::to_string(k) + ";";
        if (src.find(decl) == std::string::npos)
            throw FormatError("postParticleFP: missing " + decl);
        src = replaceAll(src, decl,
                         "vec4 o_COLOR" + std::to_string(k) + ";   // unused MRT output on the RSX (single colour target)");
    }
    // the last '}' after trailing whitespace is stripped
    size_t end = src.size();
    while (end > 0 && (src[end - 1] == ' ' || (src[end - 1] >= '\t' && src[end - 1] <= '\r') ||
                       (src[end - 1] >= '\x1c' && src[end - 1] <= '\x1f')))
        --end;
    size_t idx = end == 0 ? std::string::npos : src.rfind('}', end - 1);
    std::string head = idx == std::string::npos ? src.substr(0, src.empty() ? 0 : src.size() - 1) : src.substr(0, idx);
    return head + "    if (!(o_COLOR0.w > 0.0)) discard;   // RSX alpha test: GREATER, ref 0\n}\n";
}

}  // namespace moyou::rsx
