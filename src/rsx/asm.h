#pragma once
// RSX shader assembly: an editable listing of a CgBinary program and the assembler that turns it back into
// microcode. The syntax follows NVIDIA's vp40/fp40 assembly as sce-cgcdisasm prints it, with names: uniforms by
// name (embedded fragment constants and vertex c[] registers), samplers by name, literals inline as {x, y, z, w}.
//
// Lossless by construction: any encoding bit the syntax does not model is printed as a trailing
// `!xor(w0, w1, w2, w3)` on its line, so assemble(listing(p)) reproduces p's microcode and parameter table exactly
// (checked over every program in the 4.93 and 1.00 lines.qrc and the Lineschive archive).
//
// Fragment programs:  MADR_m8 R1.xyz, H2.w, f[TEX3], R2;      TEXR R0, f[TEX0], _TexSamp;
//                     MULH H2.x(NE.xxxx), R1, {0.03125, 0, 0, 0}.x;     MOVRC RC.x, -|R0|;
// Vertex programs:    MAD o[TEXCOORD2].xyz, -R2, {2, 0, 0, 0}.x, R1;    DP4 o[HPOS].x, v[0], _MVM[0];
//                     MOV R0.y, _Brightness.x  &  RSQ R1.x, R1.x;      (vector & scalar co-issue)

#include "rsx/rsx.h"

#include <span>
#include <string>
#include <vector>

namespace moyou::rsx {

class AsmError : public std::runtime_error {
public:
    AsmError(int line, const std::string &msg) : std::runtime_error(msg), line_(line) {}
    int line() const { return line_; }   // 1-based source line, 0 when not tied to a line
private:
    int line_;
};

// The editable listing (comments describe the parameters; instructions one per line).
std::string listing(const Program &prog, const std::string &name);

// Assembles `source` against `original` (its parameter table supplies the names, types and resources). Returns the
// new program: microcode, header counts, embedded-constant offsets, referenced flags and, for vertex programs, the
// internal literal constants (reused or allocated from free c[] registers). Throws AsmError.
Program assemble(const std::string &source, const Program &original);

// CgBinary container for `prog`, laid out like `original` (the file it was parsed from): the parameter table and
// string blob are kept, changed defaults / embedded-constant tables and new parameters are appended, then the
// program header and microcode. write(parse(f), f) == f.
std::vector<uint8_t> write(const Program &prog, std::span<const uint8_t> original);

}  // namespace moyou::rsx
