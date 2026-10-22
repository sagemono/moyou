#include "render/gl.h"

#include <vector>

namespace moyou::gl {

std::string toDesktop(const std::string &src) {
    std::string s = src;
    const std::string es = "#version 300 es";
    auto p = s.find(es);
    if (p != std::string::npos) s.replace(p, es.size(), "#version 430 core");
    else if (s.rfind("#version", 0) != 0) s = "#version 430 core\n" + s;
    return s;
}

static GLuint compile(Fn &f, GLenum type, const std::string &src, const std::string &label) {
    GLuint s = f.glCreateShader(type);
    const std::string d = toDesktop(src);
    const char *p = d.c_str();
    f.glShaderSource(s, 1, &p, nullptr);
    f.glCompileShader(s);
    GLint ok = 0;
    f.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        f.glGetShaderiv(s, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(std::max(n, 1));
        f.glGetShaderInfoLog(s, n, nullptr, log.data());
        f.glDeleteShader(s);
        throw ShaderError("shader compile failed (" + label + (type == GL_VERTEX_SHADER ? " vs" : " fs") + "):\n" +
                          std::string(log.data()));
    }
    return s;
}

Program link(Fn &f, const std::string &vs, const std::string &fs, const std::string &label) {
    GLuint v = compile(f, GL_VERTEX_SHADER, vs, label);
    GLuint fr = 0;
    try {
        fr = compile(f, GL_FRAGMENT_SHADER, fs, label);
    } catch (...) {
        f.glDeleteShader(v);
        throw;
    }
    Program pr;
    pr.id = f.glCreateProgram();
    f.glAttachShader(pr.id, v);
    f.glAttachShader(pr.id, fr);
    f.glLinkProgram(pr.id);
    f.glDeleteShader(v);
    f.glDeleteShader(fr);
    GLint ok = 0;
    f.glGetProgramiv(pr.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        f.glGetProgramiv(pr.id, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(std::max(n, 1));
        f.glGetProgramInfoLog(pr.id, n, nullptr, log.data());
        f.glDeleteProgram(pr.id);
        throw ShaderError("program link failed (" + label + "):\n" + std::string(log.data()));
    }
    GLint count = 0;
    f.glGetProgramiv(pr.id, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; i++) {
        char name[256];
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        f.glGetActiveUniform(pr.id, GLuint(i), sizeof(name), &len, &size, &type, name);
        std::string n(name, len);
        GLint loc = f.glGetUniformLocation(pr.id, n.c_str());
        if (n.size() > 3 && n.compare(n.size() - 3, 3, "[0]") == 0) n.resize(n.size() - 3);
        pr.uniforms[n] = loc;
    }
    return pr;
}

void destroy(Fn &f, Program &p) {
    if (p.id) f.glDeleteProgram(p.id);
    p = {};
}

}  // namespace moyou::gl
