#pragma once
// RSX Cg binaries (.vpo / .fpo from lines.qrc) -> instruction-exact GLSL: container and microcode decoding,
// the disassembly listing and GLSL emission.

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace moyou::rsx {

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Param {                    // one entry of the Cg parameter table
    std::string name;             // e.g. "_Colour1", "IN.uv0", "_LightPack[2]"
    int type = 0;                 // Cg type enum (1045 float, 1048 float4, 1064 float4x4, 1066 sampler2D, ...)
    int resource = 0;             // Cg resource enum (2178 CG_C, 2048+n TEXUNITn, 2113+n ATTRn, 3220+n TEXCOORDn ...)
    int resIndex = 0;             // constant register / texture unit
    int variability = 0;          // 4101 varying, 4102 uniform, 4103 const, 4104 literal
    int direction = 0;            // 4097 in, 4098 out, 4099 inout
    bool referenced = false;
    std::vector<float> defaults;  // literal values for internal constants (empty: no default)
    std::vector<uint32_t> embedded;  // FP only: ucode byte offsets of the inline constants patched with this uniform
    std::string semantic;         // e.g. "POSITION0", "TEXCOORD0"
    int paramno = 0;
};

struct VertexHeader {             // CgBinaryVertexProgram
    uint32_t instructionCount = 0, instructionSlot = 0, registerCount = 0, inputMask = 0, outputMask = 0;
};

struct FragmentHeader {           // CgBinaryFragmentProgram
    uint32_t instructionCount = 0, inputMask = 0;
    uint16_t texcoordMask = 0, texcoord2d = 0;
    uint8_t registerCount = 0, outputFromH0 = 0, depthReplace = 0, pixelKill = 0;
};

struct Program {
    bool vertex = false;          // profile 7003 (sce_vp_rsx) or 7004 (sce_fp_rsx)
    std::vector<uint8_t> ucode;   // big-endian microcode as stored in the binary
    std::vector<Param> params;
    uint32_t profile = 0;
    VertexHeader vp;              // valid when vertex
    FragmentHeader fp;            // valid when !vertex
};

// Parses a CgBinary container (throws FormatError).
Program parse(std::span<const uint8_t> cgBinary);

struct Options {
    bool hwprec = true;           // model fp16 H registers and _h/_x12/_x9 precision (the port always uses this)
    bool gl330 = false;           // desktop GLSL 330 header instead of GLSL ES 3.00
};

// Transpiles to GLSL; `name` (a file name) goes in the header comment. The result ends with a single '\n'.
std::string transpile(const Program &prog, const Options &opt, const std::string &name);

// Disassembly listing (`moyou-cli disasm`). Every line ends with '\n'.
std::string disassemble(const Program &prog, const std::string &name);

// Post-processing applied to the particle programs (throws FormatError on unexpected input):
//  VP: gl_Position.y *= u_flipY (adds `uniform float u_flipY;`)
//  FP: unused MRT outputs o_COLOR1/o_COLOR2 become plain globals, RSX alpha test GREATER 0 appended as a discard.
std::string postParticleVP(const std::string &glsl);
std::string postParticleFP(const std::string &glsl);

}  // namespace moyou::rsx
