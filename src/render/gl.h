#pragma once

#include <QOpenGLExtraFunctions>

#include <map>
#include <stdexcept>
#include <string>

namespace moyou::gl {

using Fn = QOpenGLExtraFunctions;

// GLSL ES 3.00 (what the RSX transpiler and the WebGL port emit) -> desktop GLSL 4.30 core.
// Precision qualifiers are legal no-ops in desktop GLSL, so only the version line changes.
std::string toDesktop(const std::string &src);

struct Program {
    GLuint id = 0;
    std::map<std::string, GLint> uniforms;   // active uniform name (array "[0]" suffix removed) -> location
    GLint loc(const std::string &name) const {
        auto it = uniforms.find(name);
        return it == uniforms.end() ? -1 : it->second;
    }
};

class ShaderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Compiles (after toDesktop) and links; throws ShaderError with the info log and a label.
Program link(Fn &f, const std::string &vs, const std::string &fs, const std::string &label);
void destroy(Fn &f, Program &p);

}  // namespace moyou::gl
