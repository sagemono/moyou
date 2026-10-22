#pragma once
// Private: decoded RSX microcode shared by the disassembler (cgbinary.cpp) and the GLSL emitter (glsl.cpp).
// Field layout per RPCS3's RSX program decoder.

#include "rsx/rsx.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace moyou::rsx::detail {

inline constexpr int kCgC = 2178;              // CG_C: constant register bank

// ---- vertex program ----
struct VpSrc {
    enum Kind { Bad, Temp, In, Const } kind = Bad;
    int reg = 0;
    std::array<int, 4> swz{};
    bool neg = false, abs = false, indexed = false;
    int addrReg = 0, addrSwz = 0;
};

struct VpDst {
    enum Kind { Out, Temp, Addr, Cc } kind = Temp;
    int value = 0;                              // Out: index into vpOutName()
};

struct VpInst {
    int index = 0;
    std::string vecOp, scaOp;
    std::array<VpSrc, 3> srcs;
    std::vector<VpDst> vecDst, scaDst;
    std::string vmask, smask;
    bool sat = false;
    int tex = 0;
    int cond = 7, ccSel = 0;                    // cond 7: always
    std::array<int, 4> ccSwz{};
    int addr = 0;
    bool end = false;
};

std::vector<VpInst> decodeVp(const Program &prog);
const char *vpInName(int reg);                  // ATTRn
const char *vpOutName(int reg);                 // HPOS, COL0, ..., TEXCOORDn
bool isVpFlow(const std::string &op);

struct ConstEntry {                             // vp_const_map value
    bool literal = false;
    std::vector<double> values;                 // literal
    std::string name;                           // uniform (with "[row]" for matrix rows)
};
std::map<int, ConstEntry> vpConstMap(const Program &prog);

// ---- fragment program ----
struct FpSrc {
    enum Kind { Temp, In, Const, Bad } kind = Temp;
    bool fp16 = false;
    int reg = 0;
    std::string inReg;                          // In: WPOS, COL0, TEXn ...
    std::array<int, 4> swz{};
    bool neg = false, abs = false;
    int pmod = 0;                               // operand precision modifier (src1 bits 19 + 3k)
};

struct FpInst {
    uint32_t offset = 0;
    std::string op;
    std::array<FpSrc, 3> srcs;
    bool hasConst = false;
    std::array<double, 4> konst{};
    std::optional<std::string> constName;       // embedded-constant parameter patched at offset + 16
    bool hasDst = false;
    char dstFile = 'R';
    int dstReg = 0;
    std::string mask;
    bool setCc = false;
    int ccReg = 0;
    bool sat = false;
    int prec = 0;
    double scale = 1.0;
    int tex = 0;
    int cond = 7, condReg = 0;
    std::array<int, 4> condSwz{};
    std::array<uint32_t, 4> raw{};
    bool end = false;
};

std::vector<FpInst> decodeFp(const Program &prog);
bool isFpFlow(const std::string &op);
const char *fpInRes(const std::string &in);     // FP_IN_RES lookup, nullptr if absent

// ---- shared helpers ----
std::string cgResName(uint32_t res, int idx);
std::map<std::string, std::string> varyingByRes(const Program &prog, int direction);  // 4097 in / 4098 out
int typeWidth(int type);
int typeRows(int type);

std::string fmtf(double v);                     // a constant in the disassembly listing
std::string fmtG(double v, int precision);      // "%.<p>g"
std::string flit(double v);                     // a float literal in the generated GLSL
std::string ident(const std::string &name);     // rsx_to_glsl.ident

}  // namespace moyou::rsx::detail
