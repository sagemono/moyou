#include "render/renderer.h"

#include "formats/mnu.h"
#include "rsx/rsx.h"

#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <regex>
#include <stdexcept>

namespace moyou::render {

namespace {

inline float F(double v) { return float(v); }   // Math.fround

// GLSL sources compiled into the binary (render/*.glsl), read once.
const std::string &glslResource(const char *path) {
    static std::map<std::string, std::string> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    QFile f(QString(":/moyou/") + path);
    if (!f.open(QIODevice::ReadOnly)) throw gl::ShaderError(std::string(path) + " is not in the resources");
    const QByteArray b = f.readAll();
    return cache.emplace(path, std::string(b.constData(), size_t(b.size()))).first->second;
}

const std::string &RSX_LIB() { return glslResource("render/rsx_lib.glsl"); }

struct FpPatch {
    std::map<std::string, std::string> samplers;
    std::string helpers;
    bool wpos = false;
    std::vector<std::pair<std::string, std::string>> fix;
};

// Drop MRT outputs the frame does not bind, route texture fetches through the sampler models, give WPOS the RSX
// bottom-origin definition (libgcm: WPOS.y = height - 1 - row + 0.5 for CELL_GCM_WINDOW_ORIGIN_BOTTOM).
std::string patchFP(const std::string &src, const FpPatch &o, const std::string &label) {
    std::string s = std::regex_replace(src, std::regex(R"(layout\(location = ([1-3])\) out vec4 (o_COLOR\d);)"), "vec4 $2;");
    {
        static const std::regex tex(R"(texture\((\w+), (.*?)\);)");
        std::string out;
        auto last = s.cbegin();
        for (std::sregex_iterator it(s.begin(), s.end(), tex), end; it != end; ++it) {
            const auto &m = *it;
            out.append(last, m[0].first);
            const std::string name = m[1], coordFull = m[2];
            auto sit = o.samplers.find(name);
            if (sit == o.samplers.end()) out += m.str();
            else {
                std::string coord = coordFull;
                const std::string rect = " / vec2(textureSize(" + name + ", 0))";
                if (coord.size() >= rect.size() && coord.compare(coord.size() - rect.size(), rect.size(), rect) == 0)
                    coord.resize(coord.size() - rect.size());
                out += sit->second + "(" + coord + ");";
            }
            last = m[0].second;
        }
        out.append(last, s.cend());
        s = out;
    }
    for (const auto &[a, b] : o.fix) {
        auto p = s.find(a);
        if (p == std::string::npos) throw gl::ShaderError("shader patch did not apply (" + label + "): " + a);
        s.replace(p, a.size(), b);
    }
    std::string pre = std::string(RSX_LIB()) + o.helpers;
    const std::string mainSig = "void main() {";
    auto mp = s.find(mainSig);
    if (mp == std::string::npos) throw gl::ShaderError("no main() in " + label);
    if (o.wpos) {
        s = std::regex_replace(s, std::regex("gl_FragCoord"), "rsx_wpos");
        mp = s.find(mainSig);
        pre = "uniform float uRsxWinH;\nvec4 rsx_wpos;\n" + pre;
        s.replace(mp, mainSig.size(), pre + "\nvoid rsx_main() {");
        s += "\nvoid main() { rsx_wpos = vec4(gl_FragCoord.x, uRsxWinH - gl_FragCoord.y, gl_FragCoord.zw); rsx_main(); }\n";
    } else {
        s.replace(mp, mainSig.size(), pre + "\nvoid main() {");
    }
    return s;
}

// Turn a transpiled vertex program into a function so a wrapper can feed it (or post-transform HPOS).
std::string patchVP(const std::string &src, bool attrsAsGlobals, const std::string &pre, const std::string &main) {
    std::string s = src;
    if (attrsAsGlobals) s = std::regex_replace(s, std::regex(R"(layout\(location = \d+\) in vec4 (a_\w+);[^\n]*)"), "vec4 $1;");
    const std::string sig = "void main() {";
    auto p = s.find(sig);
    if (p == std::string::npos) throw gl::ShaderError("no main() in vertex program");
    s.replace(p, sig.size(), pre + "\nvoid rsx_vp_main() {");
    return s + "\n" + main + "\n";
}

const std::string &VS_QUAD() { return glslResource("render/quad_vs.glsl"); }

// lines1.vpo, fed by the spline.elf surface evaluation (16x16 bicubic B-spline patches x 8x8 samples).
const std::string &WAVE_PRE() { return glslResource("render/wave_pre.glsl"); }
const std::string &WAVE_MAIN() { return glslResource("render/wave_main.glsl"); }

const std::string &FS_PRESENT() { return glslResource("render/present_fs.glsl"); }
const char *VS_DBG = R"GLSL(#version 300 es
layout(location = 0) in vec4 aPos;
uniform float uSize;
void main() { gl_Position = aPos; gl_PointSize = uSize; })GLSL";
const char *FS_DBG = R"GLSL(#version 300 es
precision mediump float;
uniform vec4 uColor; out vec4 o;
void main() { o = uColor; })GLSL";

const float IDENT_ZFLIP[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};   // rows as uploaded to c[256..259]
const float MVM[16] = {2, 0, 0, -1, 0, 2, 0, -1, 0, 0, -1, 0, 0, 0, 0, 1};

uint16_t halfTrunc(float x) {   // the VSH's float -> half: truncating mantissa, no denormals
    uint32_t u;
    std::memcpy(&u, &x, 4);
    if (u == 0) return 0;
    int e = int((u >> 23) & 0xff) - 112;
    if (e < 0) return 0;
    if (e > 31) e = 31;
    return uint16_t(((e << 10) & 0x7c00) | ((u >> 16) & 0x8000) | ((u >> 13) & 0x3ff));
}

// 0x71260 / 0x70cec: normalized Gaussian over 2h+1 taps, h = floor(3 r) - 1
std::array<float, 8> gaussWeights(double rIn) {
    const float r = F(rIn);
    const int half = int(std::floor(F(double(r) * 3))) - 1;
    std::vector<float> w;
    float sum = 0;
    for (int i = 0; i <= 2 * half; i++) {
        const float x = F(i - half);
        const float twoR = F(double(r) + r);                       // f32(r + r)
        const float den = F(double(r) * twoR);                     // f32(r * f32(r + r))
        const float ex = F(double(F(-double(x) * x)) / den);       // f32(f32(-x*x) / den)
        const float num = F(std::exp(double(ex)));                 // f32(Math.exp(...))
        const float v = F(double(num) / double(F(double(r) * 2.50662827)));
        w.push_back(v);
        sum = F(double(sum) + v);
    }
    std::array<float, 8> out{};
    for (int k = 0; k <= half && k < 8; k++) out[size_t(k)] = F(double(w[size_t(half + k)]) / sum);
    return out;
}

// HDR::make_glare: w_k = GL * sp^k / sum(sp^j), and the AccGlare pass multiplies by GL once more
std::array<float, 6> accWeights(double GLin, double spIn) {
    const float GL = F(GLin), sp = F(spIn);
    float sum = 0;
    for (int k = 0; k < 6; k++) sum = F(double(sum) + F(std::pow(double(sp), k)));
    std::array<float, 6> out{};
    for (int k = 0; k < 6; k++)
        out[size_t(k)] = F(double(F(double(F(double(F(std::pow(double(sp), k))) / sum)) * GL)) * GL);
    return out;
}

double hermite(double t, double m) { const double t2 = t * t, t3 = t2 * t; return (t3 - 2 * t2 + t) * m + (-2 * t3 + 3 * t2) + (t3 - t2) * m; }
double ss(double a, double b, double x) {
    if (b <= a) return x >= b ? 1 : 0;
    const double t = std::min(1.0, std::max(0.0, (x - a) / (b - a)));
    return t * t * (3 - 2 * t);
}

std::array<float, 16> toF(const sim::Mat4 &m) { std::array<float, 16> o{}; for (int i = 0; i < 16; i++) o[size_t(i)] = float(m[size_t(i)]); return o; }

}  // namespace

// ------------------------------------------------------------------ assets from lines.qrc
Assets loadAssets(const qrc::Archive &a) {
    Assets as;
    // Firmware 1.00 lines.qrc: no Gaussian.vpo, no particles, no night month maps, LINE2.mnu, ToneApplyDisplay with
    // its own tone curve
    as.firmware = a.contains("lib/glutils/Gaussian.vpo") ? 493 : 100;
    std::map<std::string, std::string> progs{
        {"back_colours0.fp", "lib/moyou/back_colours0.fpo"}, {"back_colours1.fp", "lib/moyou/back_colours1.fpo"},
        {"back_colours2.fp", "lib/moyou/back_colours2.fpo"}, {"Copy.fp", "lib/glutils/Copy.fpo"},
        {"lines1.vp", "lib/moyou/lines1.vpo"}, {"lines1.fp", "lib/moyou/lines1.fpo"},
        {"LinesController.vp", "lib/moyou/LinesController.vpo"}, {"LinesController.fp", "lib/moyou/LinesController.fpo"},
        {"Gaussian.fp", "lib/glutils/Gaussian.fpo"}, {"AccGlare.fp", "lib/glutils/AccGlare.fpo"},
        {"ToneApplyDisplay.fp", "lib/glutils/ToneApplyDisplay.fpo"}};
    progs["GlareSourcePre.fp"] = "lib/glutils/GlareSourcePre.fpo";
    if (as.firmware == 100) progs["GlareSource.fp"] = "lib/glutils/GlareSource.fpo";
    else progs["Gaussian.vp"] = "lib/glutils/Gaussian.vpo";
    const rsx::Options opt{true, false};
    auto tr = [&](const std::string &id) {
        const auto &bin = a.file(id);
        return rsx::transpile(rsx::parse(bin), opt, id.substr(id.rfind('/') + 1));
    };
    for (const auto &[name, id] : progs) as.glsl[name] = tr(id);
    if (a.contains("lib/particles/particles_quads.vpo")) {
        as.particleShaders.quadsVP = rsx::postParticleVP(tr("lib/particles/particles_quads.vpo"));
        as.particleShaders.quadsFP = rsx::postParticleFP(tr("lib/particles/particles_quads.fpo"));
        as.particleShaders.secondVP = rsx::postParticleVP(tr("lib/particles/particles_second.vpo"));
        as.particleShaders.secondFP = rsx::postParticleFP(tr("lib/particles/particles_second.fpo"));
        as.particleTexture = image::decodeTga(a.file("particles/proc_iridescent.tga"));
    }
    for (int k = 0; k < (as.firmware == 100 ? 1 : 2); k++)
        for (int mo = 1; mo <= 12; mo++) {
            char id[64];
            std::snprintf(id, sizeof id, "textures/month_bg/%s/%02d.dds", k ? "night" : "rgb", mo);
            as.months[size_t(k * 12 + mo - 1)] = image::decodeDds(a.file(id));
        }
    as.fres = image::decodeTga(a.file("textures/TGA/freslut1.tga"));
    return as;
}

// ------------------------------------------------------------------ GL resources
GLuint Renderer::texture(gl::Fn &f, int w, int h, GLenum ifmt, GLenum fmt, GLenum type, const void *data, GLenum filter) {
    GLuint t = 0;
    f.glGenTextures(1, &t);
    f.glBindTexture(GL_TEXTURE_2D, t);
    f.glTexImage2D(GL_TEXTURE_2D, 0, GLint(ifmt), w, h, 0, fmt, type, data);
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GLint(filter));
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GLint(filter));
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return t;
}

Renderer::Surface Renderer::surface(gl::Fn &f, int w, int h, bool f16, bool linear) {
    Surface s;
    s.w = w; s.h = h;
    s.tex = texture(f, w, h, f16 ? GL_RGBA16F : GL_RGBA8, GL_RGBA, f16 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr,
                    linear ? GL_LINEAR : GL_NEAREST);
    f.glGenFramebuffers(1, &s.fbo);
    f.glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    f.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
    if (f.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw gl::ShaderError(std::string("a ") + (f16 ? "half-float" : "RGBA8") + " render target is not supported");
    f.glClearColor(0, 0, 0, 0);
    f.glClear(GL_COLOR_BUFFER_BIT);
    return s;
}

void Renderer::init(gl::Fn &f, const Assets &as) {
    auto G = [&](const char *n) -> const std::string & {
        auto it = as.glsl.find(n);
        if (it == as.glsl.end()) throw gl::ShaderError(std::string("missing program ") + n);
        return it->second;
    };
    progCopy_ = gl::link(f, VS_QUAD(), G("Copy.fp"), "Copy");
    // COLOUR SHADER 1 (procedural, _DayTime + _Alpha) and 2 (writes only alpha; rgb = the zeroed temp register)
    progBack1_ = gl::link(f, VS_QUAD(), G("back_colours1.fp"), "back_colours1");
    progBack2_ = gl::link(f, VS_QUAD(), G("back_colours2.fp"), "back_colours2");
    // GLARE_ONLY: tone_apply_display copies the accumulated glare over the display with Copy.fpo, blending off
    progGlareOnly_ = gl::link(f, VS_QUAD(), patchFP(G("Copy.fp"), {{{"CopyTex", "S_CopyGlare"}},
        "vec4 S_CopyGlare(vec2 uv) { return rsx_f16(rsx_bilerp(CopyTex, uv, ivec2(5))); }"}, "Copy (glare only)"), "Copy (glare only)");
    fw_ = as.firmware;
    const bool v100 = fw_ == 100;
    if (v100)   // 1.00 back_colours0: one month pair, no night maps
        progBack_ = gl::link(f, VS_QUAD(), patchFP(G("back_colours0.fp"), {{{"MonthlyTex1", "S_M1"}, {"MonthlyTex2", "S_M2"}}, R"GLSL(
vec4 S_M1(vec2 uv) { return rsx_q8(rsx_bilerp(MonthlyTex1, uv, ivec2(3))); }
vec4 S_M2(vec2 uv) { return rsx_q8(rsx_bilerp(MonthlyTex2, uv, ivec2(3))); })GLSL"}, "back_colours0"), "back_colours0");
    else
        progBack_ = gl::link(f, VS_QUAD(), patchFP(G("back_colours0.fp"), {{{"MonthlyTex1Day", "S_M1D"}, {"MonthlyTex2Day", "S_M2D"},
            {"MonthlyTex1Night", "S_M1N"}, {"MonthlyTex2Night", "S_M2N"}}, R"GLSL(
vec4 S_M1D(vec2 uv) { return rsx_q8(rsx_bilerp(MonthlyTex1Day, uv, ivec2(3))); }
vec4 S_M2D(vec2 uv) { return rsx_q8(rsx_bilerp(MonthlyTex2Day, uv, ivec2(3))); }
vec4 S_M1N(vec2 uv) { return rsx_q8(rsx_bilerp(MonthlyTex1Night, uv, ivec2(3))); }
vec4 S_M2N(vec2 uv) { return rsx_q8(rsx_bilerp(MonthlyTex2Night, uv, ivec2(3))); })GLSL"}, "back_colours0"), "back_colours0");
    // lines1: 4.93 encodes the intensity through the _Encode table, 1.00 inline (same bytes)
    std::string waveHelpers = R"GLSL(
vec4 S_Stripes(vec2 uv) { return rsx_q8(rsx_bilerp(Stripes, uv, ivec2(1, 3))); }
vec4 S_FresLUT(vec2 uv) { return rsx_q8(rsx_bilerp(FresLUT, uv, ivec2(3, 3))); })GLSL";
    if (!v100) waveHelpers += "\nvec4 S_Encode(vec2 uv) { return texelFetch(Encode, ivec2(clamp(int(floor(uv.x * 4096.0)), 0, 4095), 0), 0); }";
    progWave_ = gl::link(f, patchVP(G("lines1.vp"), true, WAVE_PRE(), WAVE_MAIN()),
        patchFP(G("lines1.fp"), {{{"Stripes", "S_Stripes"}, {"FresLUT", "S_FresLUT"}, {"Encode", "S_Encode"}}, waveHelpers, false,
            // THINNESS = 1 in every preset makes this 0/0; NaN texture coordinates resolve to 0 on the RSX.
            {{"t = rsx_h(H0 / ((H0).wwww).x);", "t = rsx_h((H0).w == 0.0 ? vec4(0.0) : H0 / ((H0).wwww).x);"}}}, "lines1"), "lines1");
    std::string lcHelpers = R"GLSL(
vec4 S_Tex0(vec2 uv) { return rsx_q8(rsx_gauss3(Tex0, uv, ivec2(5))); }
vec4 S_Back(vec2 uv) { return rsx_f16(rsx_bilerp(Back, uv, ivec2(3))); })GLSL";
    if (!v100) lcHelpers += R"GLSL(
vec4 S_Noise(vec2 c) { ivec2 p = ivec2(floor(c)); return texelFetch(preexpose_Noise, ivec2(p.x & 31, p.y & 31), 0); }
vec4 S_Ex0(vec2 c) { return rsx_f16(rsx_bilerp(preexpose_ExposeTex0, c / 128.0, ivec2(5))).xyxy; }
vec4 S_Ex1(vec2 c) { return rsx_f16(rsx_bilerp(preexpose_ExposeTex1, c / 128.0, ivec2(5))).xyxy; })GLSL";
    progLC_ = gl::link(f, patchVP(G("LinesController.vp"), false, "",
                                  "void main() { rsx_vp_main(); gl_Position.y = -gl_Position.y; }   // viewport scale.y = -540"),
        patchFP(G("LinesController.fp"), {{{"Tex0", "S_Tex0"}, {"Back", "S_Back"}, {"preexpose_Noise", "S_Noise"},
            {"preexpose_ExposeTex0", "S_Ex0"}, {"preexpose_ExposeTex1", "S_Ex1"}}, lcHelpers, true}, "LinesController"),
        "LinesController");
    // GlareSourcePre and the 128x540 -> 128x64 pass (4.93 Copy, 1.00 GlareSource: decode, tone, threshold) are
    // programmed with maxAniso 16 (texture control0 0x...70). 1.00's GlareSourcePre copies the still-encoded Screen.
    progGSP_ = gl::link(f, VS_QUAD(), patchFP(G("GlareSourcePre.fp"), {{{"TexSamp", "S_TexSamp"}},
        "vec4 S_TexSamp(vec2 uv) { return rsx_aniso(TexSamp, uv, 16.0, false, ivec2(3)); }"}, "GlareSourcePre"), "GlareSourcePre");
    if (v100)
        progGS100_ = gl::link(f, VS_QUAD(), patchFP(G("GlareSource.fp"), {{{"TexSamp", "S_TexSamp"}},
            "vec4 S_TexSamp(vec2 uv) { return rsx_aniso(TexSamp, uv, 16.0, true, ivec2(5)); }"}, "GlareSource"), "GlareSource");
    progCopyAniso_ = gl::link(f, VS_QUAD(), patchFP(G("Copy.fp"), {{{"CopyTex", "S_CopyTex"}},
        "vec4 S_CopyTex(vec2 uv) { return rsx_aniso(CopyTex, uv, 16.0, true, ivec2(5)); }"}, "Copy (aniso)"), "Copy (aniso)");
    const std::string gaussFP = patchFP(G("Gaussian.fp"), {{{"TexSamp", "S_TexSamp"}}, R"GLSL(
uniform int uGaussMode;   // 0: LINEAR at LOD0 with CLAMP (border), 1: NEAREST at the level's LOD (minification)
vec4 S_TexSamp(vec2 uv) {
  if (uGaussMode == 0) return rsx_f16(rsx_bilerp(TexSamp, uv, ivec2(5)));
  ivec2 n = textureSize(TexSamp, 0);
  return texelFetch(TexSamp, clamp(ivec2(floor(clamp(uv, 0.0, 1.0) * vec2(n))), ivec2(0), n - 1), 0);
})GLSL"}, "Gaussian");
    // 1.00 has no Gaussian.vpo: the fragment program offsets the taps itself
    progGauss_ = v100 ? gl::link(f, VS_QUAD(), gaussFP, "Gaussian")
                      : gl::link(f, patchVP(G("Gaussian.vp"), false, "", "void main() { rsx_vp_main(); }"), gaussFP, "Gaussian");
    progAcc_ = gl::link(f, VS_QUAD(), patchFP(G("AccGlare.fp"), {{{"TexSamp", "S_TexSamp"}},
        "vec4 S_TexSamp(vec2 uv) { return rsx_f16(rsx_bilerp(TexSamp, uv, ivec2(5))); }"}, "AccGlare"), "AccGlare");
    if (v100)   // decode the screen, Reinhard per channel, add glare, scale, dither from Noise.tga
        progTone_ = gl::link(f, VS_QUAD(), patchFP(G("ToneApplyDisplay.fp"), {{{"Screen", "S_Screen"}, {"Glare", "S_Glare"}, {"Noise", "S_Noise"}}, R"GLSL(
vec4 S_Screen(vec2 uv) { return rsx_q8(rsx_bilerp(Screen, uv, ivec2(3))); }
vec4 S_Glare(vec2 uv) { return rsx_f16(rsx_bilerp(Glare, uv, ivec2(5))); }
vec4 S_Noise(vec2 uv) { return texelFetch(Noise, ivec2(floor(uv * 32.0)) & 31, 0); })GLSL", true}, "ToneApplyDisplay"), "ToneApplyDisplay");
    else
        progTone_ = gl::link(f, VS_QUAD(), patchFP(G("ToneApplyDisplay.fp"), {{{"Glare", "S_Glare"}, {"preexpose_Noise", "S_Noise"}}, R"GLSL(
vec4 S_Glare(vec2 uv) { return rsx_f16(rsx_bilerp(Glare, uv, ivec2(5))); }
vec4 S_Noise(vec2 c) { ivec2 p = ivec2(floor(c)); return texelFetch(preexpose_Noise, ivec2(p.x & 31, p.y & 31), 0); })GLSL", true},
            "ToneApplyDisplay"), "ToneApplyDisplay");
    progPresent_ = gl::link(f, VS_QUAD(), FS_PRESENT(), "present");
    progDbg_ = gl::link(f, VS_DBG, FS_DBG, "debug");

    // geometry: quads in the RSX layout (pos at ATTR0, texcoord at ATTR8)
    const float quadData[] = {-1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, 1, 0, 1};
    f.glGenBuffers(1, &quadBuf_);
    f.glBindBuffer(GL_ARRAY_BUFFER, quadBuf_);
    f.glBufferData(GL_ARRAY_BUFFER, sizeof quadData, quadData, GL_STATIC_DRAW);
    for (int k = 0; k < 2; k++) {   // vaoLC: oldpos = oldcrd in [0,1], _MVM maps to clip space
        GLuint &vao = k ? vaoLC_ : vaoQuad_;
        f.glGenVertexArrays(1, &vao);
        f.glBindVertexArray(vao);
        f.glEnableVertexAttribArray(0);
        f.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void *>(uintptr_t(k ? 8 : 0)));
        f.glEnableVertexAttribArray(8);
        f.glVertexAttribPointer(8, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void *>(uintptr_t(8)));
    }
    f.glGenVertexArrays(1, &vaoWave_);
    f.glBindVertexArray(vaoWave_);
    {   // index order as the strips in the original IB: rows of quads between J and J+1
        std::vector<uint16_t> idx;
        idx.reserve(127 * 127 * 6);
        for (int J = 0; J < 127; J++)
            for (int I = 0; I < 127; I++) {
                const uint16_t a = uint16_t(J * 128 + I);
                for (uint16_t v : {a, uint16_t(a + 128), uint16_t(a + 1), uint16_t(a + 1), uint16_t(a + 128), uint16_t(a + 129)}) idx.push_back(v);
            }
        f.glGenBuffers(1, &waveIdx_);
        f.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, waveIdx_);
        f.glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 2), idx.data(), GL_STATIC_DRAW);
    }
    f.glGenVertexArrays(1, &vaoDbg_);
    f.glBindVertexArray(vaoDbg_);
    f.glGenBuffers(1, &dbgBuf_);
    f.glBindBuffer(GL_ARRAY_BUFFER, dbgBuf_);
    f.glEnableVertexAttribArray(0);
    f.glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    f.glBindVertexArray(0);

    // surfaces as allocated by the VSH (capture offsets in comments)
    f.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    bgMap_ = surface(f, 64, 32, true);                 // 0x2bfe580 background map
    bgSnap_ = surface(f, 64, 32, true);                // 0x2c02600 snapshot taken on each clock update
    wave_ = surface(f, RES_W, RES_H, false);           // 0x18c0000 lines1 encoded intensity
    main_ = surface(f, RES_W, RES_H, false, true);     // 0xe500000 display buffer
    if (v100) screen_ = surface(f, RES_W, RES_H, false, true);   // 1.00: LinesController's HDR-encoded output
    glsrc_ = surface(f, 128, 540, true);               // 0x27a0000 GlareSourcePre
    acc_ = surface(f, 128, 64, true);                  // 0x28b0000 accumulated glare
    for (int k = 0, w = 128, h = 64; k < 7; k++, w >>= 1, h = std::max(1, h >> 1)) {
        lvl_[size_t(k)] = surface(f, w, h, true, true);   // 0x2760000 mip chain
        if (k < 6) { hb_[size_t(k)] = surface(f, w, h, true); vb_[size_t(k)] = surface(f, w, h, true); }
    }

    for (int i = 0; i < 24; i++) {
        const auto &m = as.months[size_t(i)];
        if (m.pixels.empty()) continue;
        monthTex_[size_t(i)] = texture(f, m.width, m.height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, m.pixels.data());
    }
    {
        std::vector<uint8_t> fres(size_t(as.fres.width) * 4, 0);
        for (int i = 0; i < as.fres.width; i++) {   // freslut1: R, G used; B 0, A 255 (as the RSX sees the A8R8G8B8 upload)
            fres[size_t(i) * 4] = as.fres.pixels[size_t(i) * 4];
            fres[size_t(i) * 4 + 1] = as.fres.pixels[size_t(i) * 4 + 1];
            fres[size_t(i) * 4 + 3] = 255;
        }
        fresTex_ = texture(f, as.fres.width, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, fres.data());
    }
    {   // Stripes (0x4bad0): 16 x 4, row q = r/3, val = int(min(1, (1.01 - 4f^2)^(20 q^3) * (0.5 + q/2)) * 255)
        std::vector<uint8_t> st(16 * 4 * 4);
        for (int r = 0; r < 4; r++)
            for (int x = 0; x < 16; x++) {
                const double q = r / 3.0, fx = x / 15.0 - 0.5;
                const auto v = uint8_t(std::floor(std::min(1.0, std::pow(1.01 - 4 * fx * fx, 20 * q * q * q) * (0.5 + q / 2)) * 255));
                std::fill_n(st.begin() + (r * 16 + x) * 4, 4, v);
            }
        stripesTex_ = texture(f, 16, 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, st.data());
    }
    {   // _Encode (0x4b928): y = i/4095*128; bytes [frac(y)/4, int(y)/64, frac(y)/4, int(y)/64], truncated, clamped
        std::vector<uint8_t> enc(4096 * 4);
        for (int i = 0; i < 4096; i++) {
            const float y = F(double(F(double(F(i / 4095.0)) * 32)) * 4);
            const double ip = std::floor(y);
            const float fr = F(double(y) - ip);
            const auto b0 = uint8_t(std::trunc(std::min(255.0, std::max(0.0, double(F(double(F(double(fr) * 0.25)) * 255))))));
            const auto b1 = uint8_t(std::trunc(std::min(255.0, std::max(0.0, double(F(double(F(ip * 0.015625)) * 255))))));
            enc[size_t(i) * 4] = b0; enc[size_t(i) * 4 + 1] = b1; enc[size_t(i) * 4 + 2] = b0; enc[size_t(i) * 4 + 3] = b1;
        }
        encodeTex_ = texture(f, 4096, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, enc.data());
    }
    ctrlTex_ = texture(f, 19, 19, GL_RGBA32F, GL_RGBA, GL_FLOAT, nullptr);
    exTex0_ = texture(f, 128, 128, GL_RG16F, GL_RG, GL_HALF_FLOAT, nullptr);
    exTex1_ = texture(f, 128, 128, GL_RG16F, GL_RG, GL_HALF_FLOAT, nullptr);
    noiseTex_ = texture(f, 32, 32, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (v100) {   // 1.00 dither noise (0xd5b8): 32x32 B8 of frand bytes, built once at init (Noise.tga is unused)
        frandCounter_ = 0;
        std::vector<uint8_t> nz(32 * 32 * 4, 255);
        for (int i = 0; i < 32 * 32; i++)
            nz[size_t(i) * 4] = nz[size_t(i) * 4 + 1] = nz[size_t(i) * 4 + 2] = uint8_t(int(F(double(F(double(frand100()) + 1)) * 0.5) * 255.f));
        noise100Tex_ = texture(f, 32, 32, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nz.data());
    }
    hdrKey_.clear();
    brightLvl_ = -1;
    pcam_.mvp = toF(sim::mul(sim::projection(), sim::view()));
    pcam_.modelview = toF(sim::view());
    ready_ = true;
}

void Renderer::release(gl::Fn &f) {
    for (gl::Program *p : {&progGS100_, &progCopy_, &progBack_, &progBack1_, &progBack2_, &progGlareOnly_, &progWave_, &progLC_, &progGSP_, &progCopyAniso_, &progGauss_, &progAcc_,
                           &progTone_, &progPresent_, &progDbg_})
        gl::destroy(f, *p);
    auto delS = [&](Surface &s) { if (s.fbo) f.glDeleteFramebuffers(1, &s.fbo); if (s.tex) f.glDeleteTextures(1, &s.tex); s = {}; };
    for (Surface *s : {&bgMap_, &bgSnap_, &wave_, &main_, &glsrc_, &acc_, &screen_}) delS(*s);
    for (auto &s : lvl_) delS(s);
    for (auto &s : hb_) delS(s);
    for (auto &s : vb_) delS(s);
    f.glDeleteTextures(24, monthTex_.data());
    monthTex_.fill(0);
    for (GLuint *t : {&fresTex_, &stripesTex_, &encodeTex_, &ctrlTex_, &exTex0_, &exTex1_, &noiseTex_, &noise100Tex_}) { if (*t) f.glDeleteTextures(1, t); *t = 0; }
    for (GLuint *v : {&vaoQuad_, &vaoLC_, &vaoWave_, &vaoDbg_}) { f.glDeleteVertexArrays(1, v); *v = 0; }
    for (GLuint *b : {&quadBuf_, &waveIdx_, &dbgBuf_}) { f.glDeleteBuffers(1, b); *b = 0; }
    ready_ = false;
}

// ------------------------------------------------------------------ HDR tables (0x71794)
void Renderer::buildHdrTables(gl::Fn &f, double Ein, double Win, double Tin) {
    const float E = F(Ein), W = F(Win), T = F(Tin);
    const float wsr = F(1.0 / double(F(double(W) * W)));
    uint16_t fH[128], gH[128];
    for (int i = 0; i < 128; i++) {
        const float L = F(double(F(double(F(i / 127.0)) * 16)) * E);
        const float y = F(double(F(1.0 / double(F(double(L) + 1)))) * double(F(double(L) * double(F(double(wsr) * L + 1)))));
        fH[i] = halfTrunc(y);
        const float g = F(double(F(double(F(double(y) / std::min(1.0, std::max(0.0, double(y))))) * double(F(double(y) - T)))) * 0.125);
        // texel (0,0) is 0/0 = NaN on the PS3; every fetch touching it ends up clamped to 0 in the 8-bit alpha either way
        gH[i] = halfTrunc(std::isnan(g) ? F(-double(T) * 0.125) : g);
    }
    std::vector<uint16_t> t0(128 * 128 * 2), t1(128 * 128 * 2);
    for (int r = 0; r < 128; r++)
        for (int c = 0; c < 128; c++) {
            const size_t o = size_t(r * 128 + c) * 2;
            t0[o] = fH[c]; t0[o + 1] = fH[r];              // X = f(col), Y = f(row); sampled as XYXY
            t1[o] = fH[c]; t1[o + 1] = gH[std::max(r, c)]; // X = f(col), Y = g(max(row, col))
        }
    f.glBindTexture(GL_TEXTURE_2D, exTex0_);
    f.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RG, GL_HALF_FLOAT, t0.data());
    f.glBindTexture(GL_TEXTURE_2D, exTex1_);
    f.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RG, GL_HALF_FLOAT, t1.data());
    buildNoise(f);
}

void Renderer::buildNoise(gl::Fn &f) {
    // dither noise: integer hash on the HDR object's counter (advances 1024 per frame), jitter 1/255 (HDR+0x2dc)
    const float jitter = F(1.0 / 255);
    std::vector<uint8_t> nz(32 * 32 * 4);
    for (int i = 0; i < 32 * 32; i++) {
        noiseCounter_ = int32_t(uint32_t(noiseCounter_) + 1u);
        uint32_t n = uint32_t(noiseCounter_);
        n = n ^ (n << 13);
        const uint32_t v = (n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
        const float x = F(F(double(F(double(F(double(F(double(v) * -9.31322575e-10 + 1)) * 0.5 + 0.5)) * jitter)) * 8));
        const auto u = uint8_t(int(std::trunc(std::min(1.0, std::max(-1.0, double(x))) * 255)) & 0xff);
        nz[size_t(i) * 4] = nz[size_t(i) * 4 + 1] = nz[size_t(i) * 4 + 2] = u;
        nz[size_t(i) * 4 + 3] = 0;
    }
    f.glBindTexture(GL_TEXTURE_2D, noiseTex_);
    f.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, nz.data());
}

// ------------------------------------------------------------------ helpers
void Renderer::target(gl::Fn &f, const Surface &s) { f.glBindFramebuffer(GL_FRAMEBUFFER, s.fbo); f.glViewport(0, 0, s.w, s.h); }
void Renderer::bind(gl::Fn &f, int unit, GLuint tex) { f.glActiveTexture(GLenum(GL_TEXTURE0 + unit)); f.glBindTexture(GL_TEXTURE_2D, tex); }
void Renderer::quad(gl::Fn &f, GLuint vao) { f.glBindVertexArray(vao ? vao : vaoQuad_); f.glDrawArrays(GL_TRIANGLE_FAN, 0, 4); }
void Renderer::copy(gl::Fn &f, const Surface &src, const Surface &dst) {
    target(f, dst);
    f.glUseProgram(progCopy_.id);
    bind(f, 0, src.tex);
    f.glUniform1i(progCopy_.loc("CopyTex"), 0);
    quad(f);
}

// Theme Settings > Brightness (custom_render_plugin event 0/7, 0x15330): level L = 0..5 -> b = 1 - 0.15 L.
// b scales BACKGROUND colours 1..4 through three smoothstep tweens of 1 s and the particle _Color linearly over 1 s.
void Renderer::brightTick(double dt, const sim::Params &p) {
    const int lvl = fw_ == 100 ? 0 : std::clamp(p.S.brightLevel, 0, 5);   // 1.00 has no theme brightness
    const double b = F(double(F(lvl * double(F(-0.15)))) + 1);
    if (lvl != brightLvl_) {
        for (Tween *tw : {&brightCol_, &brightPart_}) {
            if (brightLvl_ < 0) { tw->cur = tw->start = tw->target = b; tw->t = tw->dur * 2; }
            else { tw->start = tw->cur; tw->target = b; tw->t = 0; }
        }
        brightLvl_ = lvl;
    }
    Tween &c = brightCol_;                 // 0x5334c: stops once t > 1.5 dur
    if (c.t <= c.dur * 1.5) {
        c.t += dt;
        if (c.t <= 0) c.cur = c.start;
        else if (c.t >= c.dur) c.cur = c.target;
        else { const double fr = c.t / c.dur; c.cur = (c.target - c.start) * (fr * -2 + 3) * fr * fr + c.start; }
    }
    Tween &q = brightPart_;                // 0x31494: linear, clamped
    q.t = std::min(q.dur, q.t + dt);
    q.cur = q.start + (q.target - q.start) * std::clamp(q.t / q.dur, 0.0, 1.0);
    pcam_.color = {float(q.cur), float(q.cur), float(q.cur)};
}

// ------------------------------------------------------------------ passes
void Renderer::passBackground(gl::Fn &f, double dt, sim::Params &p) {
    auto &clk = p.clk;
    // COLOUR SHADER (BackMenu apply 0x216cc -> 0x4fdf0): a change restarts the map fade with 2 s
    const int shader = int(std::clamp(std::lround(p.P("BACKGROUND.COLOUR SHADER")), 0L, 2L));
    if (bgShader_ >= 0 && shader != bgShader_) { clk.snap = true; clk.fadeT = 0; clk.fadeDur = 2; }
    bgShader_ = shader;
    // 0x52784 on the first frame of each clock update: snapshot the current map
    if (clk.snap) { copy(f, bgMap_, bgSnap_); clk.snap = false; }
    // 0x52ef0: copy the snapshot back, then blend back_colours0 over it with _Alpha = smoothstep(t / fade)
    copy(f, bgSnap_, bgMap_);
    const double t = clk.fadeT, tmax = clk.fadeDur;
    double alpha = 1;
    if (t != tmax) { const double s = std::clamp(t / tmax, 0.0, 1.0); alpha = s * s * (3 - 2 * s); }
    if (t < tmax * 1.5) clk.fadeT = F(t + dt);
    const double d = clk.dayFrac, mo = std::clamp(clk.month, 0.0, 12.0);
    const int m = int(std::floor(mo)) % 12;
    const double nb = p.P("BACKGROUND.NIGHT BLEND");
    const double ndb = (ss(p.P("BACKGROUND.NIGHT2DAY BEGIN") / 24, p.P("BACKGROUND.NIGHT2DAY END") / 24, d) *
                            (1 - ss(p.P("BACKGROUND.DAY2NIGHT BEGIN") / 24, p.P("BACKGROUND.DAY2NIGHT END") / 24, d)) - 1) * nb + 1;
    target(f, bgMap_);
    f.glEnable(GL_BLEND);
    f.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (shader != 0) {   // 0x52ad8: back_colours1 gets _DayTime = clamp(d,0,1)*41.6666679 (no DAYSPREAD)
        const gl::Program &pr = shader == 1 ? progBack1_ : progBack2_;
        f.glUseProgram(pr.id);
        f.glUniform1f(pr.loc("DayTime"), F(double(F(std::clamp(d, 0.0, 1.0))) * 41.6666679));
        f.glUniform1f(pr.loc("Alpha"), F(alpha));
        quad(f);
        f.glDisable(GL_BLEND);
        return;
    }
    f.glUseProgram(progBack_.id);
    const auto &u = progBack_;
    if (fw_ == 100) {   // 1.00 (0x382a4): _DayTime = d * 2400 (no hermite, no DAYSPREAD), _MonthTime, _Alpha, month pair
        f.glUniform1f(u.loc("DayTime"), F(std::clamp(d, 0.0, 1.0) * 2400));
        f.glUniform1f(u.loc("MonthTime"), F((mo - std::floor(mo)) * 30));
        f.glUniform1f(u.loc("Alpha"), F(alpha));
        bind(f, 0, monthTex_[size_t(m)]); f.glUniform1i(u.loc("MonthlyTex1"), 0);
        bind(f, 1, monthTex_[size_t((m + 1) % 12)]); f.glUniform1i(u.loc("MonthlyTex2"), 1);
        quad(f);
        f.glDisable(GL_BLEND);
        return;
    }
    f.glUniform1f(u.loc("DayTime"), F(hermite(d, p.P("BACKGROUND.DAYSPREAD")) * 2400));
    f.glUniform1f(u.loc("NightTime"), F((d - 0.5 < 0 ? d + 0.5 : d - 0.5) * 2400));
    f.glUniform1f(u.loc("MonthTime"), F((mo - std::floor(mo)) * 30));
    f.glUniform1f(u.loc("NightBrightness"), F(p.P("BACKGROUND.NIGHT WHIT BIAS")));
    f.glUniform1f(u.loc("NightDayBlend"), F(ndb));
    f.glUniform1f(u.loc("Alpha"), F(alpha));
    f.glUniform1f(u.loc("PI"), 3.14159274f);
    const std::pair<const char *, int> tex[] = {{"MonthlyTex1Day", m}, {"MonthlyTex2Day", (m + 1) % 12},
                                                {"MonthlyTex1Night", 12 + m}, {"MonthlyTex2Night", 12 + (m + 1) % 12}};
    for (int unit = 0; unit < 4; unit++) {
        bind(f, unit, monthTex_[size_t(tex[unit].second)]);
        f.glUniform1i(u.loc(tex[unit].first), unit);
    }
    quad(f);
    f.glDisable(GL_BLEND);
}

void Renderer::passWave(gl::Fn &f, const sim::Params &p, const sim::Wave &wave) {
    target(f, wave_);
    f.glClearColor(0, 0, 0, 0);
    f.glClear(GL_COLOR_BUFFER_BIT);
    // 0x4e458: STATE 0 skips the lines draw. SHADER 2 writes only B|A (masked off) and SHADER 0 lands off screen,
    // so at 1080p only SHADER 1 reaches the wave buffer.
    if (std::lround(p.P("LINE1.STATE")) == 0 || std::lround(p.P("LINE1.SHADER")) != 1) return;
    f.glEnable(GL_BLEND);
    f.glBlendFunc(GL_ONE, GL_ONE);
    f.glColorMask(GL_TRUE, GL_TRUE, GL_FALSE, GL_FALSE);
    f.glUseProgram(progWave_.id);
    const auto &u = progWave_;
    bind(f, 0, ctrlTex_);
    f.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 19, 19, GL_RGBA, GL_FLOAT, wave.ctrl.data());
    f.glUniform1i(u.loc("uCtrl"), 0);
    bind(f, 1, stripesTex_); f.glUniform1i(u.loc("Stripes"), 1);
    bind(f, 2, fresTex_); f.glUniform1i(u.loc("FresLUT"), 2);
    bind(f, 3, encodeTex_); f.glUniform1i(u.loc("Encode"), 3);
    f.glUniform1f(u.loc("MipmapBias"), F(p.P("LINE1.MIPMAP BIAS")));
    f.glUniform1f(u.loc("Brightness"), F(p.P("LINE1.BRIGHTNESS")));
    f.glUniform1f(u.loc("Fresnel"), F(p.P("LINE1.FRESNEL")));
    f.glUniform1f(u.loc("Spacing"), F(p.P("LINE1.SPACING")));
    f.glUniform1f(u.loc("Thinness"), F(p.P("LINE1.THINNESS")));
    f.glBindVertexArray(vaoWave_);
    f.glDrawElements(GL_TRIANGLES, 127 * 127 * 6, GL_UNSIGNED_SHORT, nullptr);
    f.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    f.glDisable(GL_BLEND);
}

void Renderer::passLinesController(gl::Fn &f, const sim::Params &p) {
    // 1.00 writes the RGBM-encoded Screen and ToneApplyDisplay resolves it; with ENABLED 0 no Screen FBO is bound and the
    // encoded bytes land in the display
    target(f, fw_ == 100 && std::lround(p.P("HDR.ENABLED")) != 0 ? screen_ : main_);
    f.glUseProgram(progLC_.id);
    const auto &u = progLC_;
    f.glUniform4fv(u.loc("MVM"), 4, MVM);
    const double k = brightCol_.cur;
    for (int c = 1; c <= 4; c++) {   // _Colour1 takes menu colour 2 and _Colour2 colour 1 (both firmwares)
        const std::string n = "BACKGROUND." + std::to_string(c <= 2 ? 3 - c : c) + " ";
        f.glUniform4f(u.loc("Colour" + std::to_string(c)), F(p.P(n + "RED") * k), F(p.P(n + "GREEN") * k), F(p.P(n + "BLUE") * k), 1);
    }
    f.glUniform2f(u.loc("ScreenSizeRecip"), F(1.0 / RES_W), F(1.0 / RES_H));
    f.glUniform2f(u.loc("ScreenOffset"), 0.25f, 0.25f);
    // The stock program does not read these, but the XMB sets them for this pass (RPCS3 probe): edited programs
    // that name them get two random values in [0, 1) per frame and 1/255. _HDRParams is set too (each of xyz >= 1),
    // values not measured yet, so it stays 0 here.
    if (const GLint nj = u.loc("NoiseJitter"); nj >= 0) {
        const float jx = F(double(F(double(frand100()) + 1)) * 0.5), jy = F(double(F(double(frand100()) + 1)) * 0.5);
        f.glUniform2f(nj, jx, jy);
    }
    f.glUniform1f(u.loc("Dither"), F(1.0 / 255));
    f.glUniform1f(u.loc("uRsxWinH"), float(RES_H));
    bind(f, 0, wave_.tex); f.glUniform1i(u.loc("Tex0"), 0);
    bind(f, 1, bgMap_.tex); f.glUniform1i(u.loc("Back"), 1);
    bind(f, 13, noiseTex_); f.glUniform1i(u.loc("preexpose_Noise"), 13);
    bind(f, 14, exTex1_); f.glUniform1i(u.loc("preexpose_ExposeTex1"), 14);
    bind(f, 15, exTex0_); f.glUniform1i(u.loc("preexpose_ExposeTex0"), 15);
    quad(f, vaoLC_);
}

// HDR::endFrame 0x7cf68: GLARE gates the glare chain; GLARE_ONLY copies the
// accumulated glare over the display instead of ToneApplyDisplay (with GLARE 0 it shows the last glare built).
void Renderer::passGlare(gl::Fn &f, const sim::Params &p) {
    if (std::lround(p.P("HDR.GLARE")) != 0) glareChain(f, p);
    if (std::lround(p.P("HDR.GLARE_ONLY")) != 0) {
        target(f, main_);
        f.glDisable(GL_BLEND);
        f.glUseProgram(progGlareOnly_.id);
        bind(f, 0, acc_.tex);
        f.glUniform1i(progGlareOnly_.loc("CopyTex"), 0);
        quad(f);
        return;
    }
    // GLARE 0: ToneApplyDisplay runs with the glare sampler disabled (reads 0): 0 - noise/8 clamps to 0, a no-op
    if (std::lround(p.P("HDR.GLARE")) == 0) return;
    // ToneApplyDisplay: display += GlareWeight * glare - noise/8 (8-bit target: source clamped before the add)
    target(f, main_);
    f.glEnable(GL_BLEND);
    f.glBlendFunc(GL_ONE, GL_ONE);
    f.glUseProgram(progTone_.id);
    f.glUniform1f(progTone_.loc("GlareWeight"), 1.f);
    f.glUniform1f(progTone_.loc("uRsxWinH"), float(RES_H));
    bind(f, 1, acc_.tex); f.glUniform1i(progTone_.loc("Glare"), 1);
    bind(f, 13, noiseTex_); f.glUniform1i(progTone_.loc("preexpose_Noise"), 13);
    quad(f);
    f.glDisable(GL_BLEND);
}

// 1.00 frand (0x97e10): the integer hash of the 4.93 dither and rope rnd(), mapped to [-1, 1)
float Renderer::frand100() {
    frandCounter_ = int32_t(uint32_t(frandCounter_) + 1u);
    const uint32_t n = uint32_t(frandCounter_), x = (n << 13) ^ n;
    const uint32_t h = (x * (x * x * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
    return F(double(h) * -9.31322575e-10 + 1);
}

// 1.00 ToneApplyDisplay (0x179d0), blend off: display = (Reinhard(E * decode(Screen)) +
// GlareWeight * glare) * Gamma + Dither * noise(WPOS/32 + NoiseJitter). GlareWeight and Gamma are 1, Dither 1/255.
// With GLARE 0 it still runs (the glare sampler reads 0); with ENABLED 0 nothing runs after LinesController.
void Renderer::passTone100(gl::Fn &f, const sim::Params &p) {
    if (std::lround(p.P("HDR.ENABLED")) == 0) return;
    const bool glare = std::lround(p.P("HDR.GLARE")) != 0;
    if (glare) glareChain(f, p);
    target(f, main_);
    f.glDisable(GL_BLEND);
    if (std::lround(p.P("HDR.GLARE_ONLY")) != 0) {   // shows the stale glare when GLARE is 0
        f.glUseProgram(progGlareOnly_.id);
        bind(f, 0, acc_.tex);
        f.glUniform1i(progGlareOnly_.loc("CopyTex"), 0);
        quad(f);
        return;
    }
    const float E = F(p.P("HDR.EXPOSURE")), W = F(p.P("HDR.WHITE LEVEL"));
    const auto &u = progTone_;
    f.glUseProgram(u.id);
    f.glUniform1f(u.loc("Exposure"), E);
    f.glUniform1f(u.loc("WhiteSqrRcp"), F(1.0 / double(F(double(W) * W))));
    f.glUniform1f(u.loc("GlareWeight"), glare ? 1.f : 0.f);
    f.glUniform1f(u.loc("Gamma"), 1.f);
    f.glUniform1f(u.loc("Dither"), F(p.has("HDR.DITHER") ? p.P("HDR.DITHER") : 1.0 / 255));
    const float jx = F(double(F(double(frand100()) + 1)) * 0.5), jy = F(double(F(double(frand100()) + 1)) * 0.5);
    f.glUniform2f(u.loc("NoiseJitter"), jx, jy);
    f.glUniform1f(u.loc("uRsxWinH"), float(RES_H));
    bind(f, 0, screen_.tex); f.glUniform1i(u.loc("Screen"), 0);
    bind(f, 1, acc_.tex); f.glUniform1i(u.loc("Glare"), 1);
    bind(f, 2, noise100Tex_); f.glUniform1i(u.loc("Noise"), 2);
    quad(f);
}

void Renderer::glareChain(gl::Fn &f, const sim::Params &p) {
    const float E = F(p.P("HDR.EXPOSURE")), W = F(p.P("HDR.WHITE LEVEL"));
    target(f, glsrc_);                                 // GlareSourcePre: display buffer (1.00: encoded Screen) -> 128x540
    f.glUseProgram(progGSP_.id);
    f.glUniform1f(progGSP_.loc("Exposure"), E);
    f.glUniform1f(progGSP_.loc("WhiteSqrRcp"), F(1.0 / double(F(double(W) * W))));
    bind(f, 0, fw_ == 100 ? screen_.tex : main_.tex);
    f.glUniform1i(progGSP_.loc("TexSamp"), 0);
    quad(f);
    target(f, lvl_[0]);                                // 128x540 -> 128x64 (1.00: GlareSource decodes, tones, thresholds)
    if (fw_ == 100) {
        f.glUseProgram(progGS100_.id);
        f.glUniform1f(progGS100_.loc("Exposure"), E);
        f.glUniform1f(progGS100_.loc("WhiteSqrRcp"), F(1.0 / double(F(double(W) * W))));
        f.glUniform1f(progGS100_.loc("Threshold"), F(p.P("HDR.GLARE THRESH")));
        bind(f, 0, glsrc_.tex);
        f.glUniform1i(progGS100_.loc("TexSamp"), 0);
    } else {
        f.glUseProgram(progCopyAniso_.id);
        bind(f, 0, glsrc_.tex);
        f.glUniform1i(progCopyAniso_.loc("CopyTex"), 0);
    }
    quad(f);
    for (int k = 1; k < 7; k++) copy(f, lvl_[size_t(k - 1)], lvl_[size_t(k)]);   // mip chain, 2x2 box per level
    const auto wr = gaussWeights(p.P("HDR.GAUSSIAN RAD R")), wg = gaussWeights(p.P("HDR.GAUSSIAN RAD G")),
               wb = gaussWeights(p.P("HDR.GAUSSIAN RAD B"));
    f.glUseProgram(progGauss_.id);
    const auto &u = progGauss_;
    f.glUniform4fv(u.loc("ModelviewProjection"), 4, IDENT_ZFLIP);
    for (int k = 0; k < 8; k++) f.glUniform4f(u.loc("Weights" + std::to_string(k)), wr[size_t(k)], wg[size_t(k)], wb[size_t(k)], 0);
    f.glUniform1i(u.loc("TexSamp"), 0);
    for (int k = 0; k < 6; k++) {                      // horizontal: level k of the mip chain
        const Surface &d = hb_[size_t(k)];
        target(f, d);
        bind(f, 0, lvl_[size_t(k)].tex);
        f.glUniform1i(u.loc("uGaussMode"), k == 0 ? 0 : 1);
        f.glUniform2f(u.loc("Offset"), F(1.0 / d.w), 0);
        quad(f);
    }
    for (int k = 0; k < 6; k++) {                      // vertical
        const Surface &d = vb_[size_t(k)];
        target(f, d);
        bind(f, 0, hb_[size_t(k)].tex);
        f.glUniform1i(u.loc("uGaussMode"), 0);
        f.glUniform2f(u.loc("Offset"), 0, F(1.0 / d.h));
        quad(f);
    }
    target(f, acc_);
    f.glClearColor(0, 0, 0, 0);
    f.glClear(GL_COLOR_BUFFER_BIT);
    f.glEnable(GL_BLEND);
    f.glBlendFunc(GL_ONE, GL_ONE);
    f.glUseProgram(progAcc_.id);
    f.glUniform1i(progAcc_.loc("TexSamp"), 0);
    const auto aw = accWeights(p.P("HDR.GLARE LEVEL"), p.P("HDR.GLARE SUM POW"));
    for (int k = 5; k >= 0; k--) {
        bind(f, 0, vb_[size_t(k)].tex);
        f.glUniform1f(progAcc_.loc("Weight"), aw[size_t(k)]);
        quad(f);
    }
    f.glDisable(GL_BLEND);
}

void Renderer::present(gl::Fn &f, const sim::Params &p, const sim::Wave &wave, GLuint fbo, int cw, int ch) {
    int vw = cw, vh = int(std::lround(double(cw) * RES_H / RES_W));
    if (vh > ch) { vh = ch; vw = int(std::lround(double(ch) * RES_W / RES_H)); }
    const int vx = (cw - vw) / 2, vy = (ch - vh) / 2;
    f.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    f.glViewport(0, 0, cw, ch);
    f.glClearColor(0, 0, 0, 1);
    f.glClear(GL_COLOR_BUFFER_BIT);
    f.glViewport(vx, vy, vw, vh);
    f.glUseProgram(progPresent_.id);
    const Surface *views[] = {&main_, &wave_, &bgMap_, &acc_};
    const int view = std::clamp(p.S.view, 0, 3);
    bind(f, 0, views[view]->tex);
    f.glUniform1i(progPresent_.loc("uTex"), 0);
    f.glUniform1i(progPresent_.loc("uMode"), view);
    quad(f);
    if (p.S.showGrid || p.S.showFfd) drawDebug(f, p, wave);
}

void Renderer::drawDebug(gl::Fn &f, const sim::Params &p, const sim::Wave &wave) {
    using sim::NX;
    f.glEnable(GL_BLEND);
    f.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    f.glEnable(GL_PROGRAM_POINT_SIZE);
    f.glUseProgram(progDbg_.id);
    f.glBindVertexArray(vaoDbg_);
    f.glBindBuffer(GL_ARRAY_BUFFER, dbgBuf_);
    const float pt = 5;
    auto draw = [&](const std::vector<float> &v, GLenum mode, float r, float g, float b, float a) {
        f.glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * 4), v.data(), GL_STREAM_DRAW);
        f.glUniform1f(progDbg_.loc("uSize"), pt);
        f.glUniform4f(progDbg_.loc("uColor"), r, g, b, a);
        f.glDrawArrays(mode, 0, GLsizei(v.size() / 4));
    };
    auto push = [](std::vector<float> &v, const float *src) { v.insert(v.end(), src, src + 4); };
    if (p.S.showGrid) {
        std::vector<float> lines;
        const float *c = wave.ctrl.data();
        for (int j = 0; j < NX; j++)
            for (int i = 0; i < NX; i++) {
                const int o = (j * NX + i) * 4;
                if (i < NX - 1) { push(lines, c + o); push(lines, c + o + 4); }
                if (j < NX - 1) { push(lines, c + o); push(lines, c + o + NX * 4); }
            }
        draw(lines, GL_LINES, 1.0f, 0.77f, 0.43f, 0.55f);
        draw(std::vector<float>(wave.ctrl.begin(), wave.ctrl.end()), GL_POINTS, 1.0f, 0.77f, 0.43f, 0.95f);
    }
    if (p.S.showFfd) {
        std::vector<float> lines;
        const float *L = wave.ffdLattice.data();
        for (int q = 0; q < 128; q++) {
            const int i = q & 7, j = (q >> 3) & 3, k = q >> 5, o = q * 4;
            if (i < 7) { push(lines, L + o); push(lines, L + (q + 1) * 4); }
            if (j < 3) { push(lines, L + o); push(lines, L + (q + 8) * 4); }
            if (k < 3) { push(lines, L + o); push(lines, L + (q + 32) * 4); }
        }
        draw(lines, GL_LINES, 0.56f, 0.85f, 1.0f, 0.5f);
        draw(std::vector<float>(wave.ffdLattice.begin(), wave.ffdLattice.end()), GL_POINTS, 0.56f, 0.85f, 1.0f, 1.0f);
    }
    f.glDisable(GL_BLEND);
    f.glBindVertexArray(0);
}

// ------------------------------------------------------------------ frame
void Renderer::render(gl::Fn &f, double dt, sim::Params &p, sim::Wave &wave, particles::ParticleSystem *ps, GLuint fbo, int w, int h) {
    if (!ready_) return;
    const std::string key = std::to_string(F(p.P("HDR.EXPOSURE"))) + "," + std::to_string(F(p.P("HDR.WHITE LEVEL"))) + "," +
                            std::to_string(F(p.P("HDR.GLARE THRESH")));
    // HDR::beginFrame rebuilds the exposure LUTs and the dither noise every frame; with ENABLED 0 both it and endFrame
    // are skipped, so LinesController keeps the last tables and a frozen noise pattern
    const bool hdrOn = std::lround(p.P("HDR.ENABLED")) != 0;
    if (fw_ != 100 && (hdrOn || hdrKey_.empty())) {   // the HDR ctor defaults ENABLED to 1: one build always happens first
        if (key != hdrKey_) { hdrKey_ = key; buildHdrTables(f, p.P("HDR.EXPOSURE"), p.P("HDR.WHITE LEVEL"), p.P("HDR.GLARE THRESH")); }
        else buildNoise(f);
    }
    f.glDisable(GL_BLEND);
    f.glDisable(GL_DEPTH_TEST);
    f.glDisable(GL_CULL_FACE);
    f.glDisable(GL_SCISSOR_TEST);
    brightTick(dt, p);
    passBackground(f, dt, p);
    passWave(f, p, wave);
    passLinesController(f, p);
    if (fw_ == 100) passTone100(f, p);
    else if (hdrOn) passGlare(f, p);
    if (ps) {
        auto PP = [&p](const std::string &n) { return p.P("PARTICLES." + n); };
        ps->update(dt, PP, pcam_);
        if (p.S.particlesOn) {
            target(f, main_);
            ps->draw(f, PP, pcam_, -1);
        }
    }
    present(f, p, wave, fbo, w, h);
}

QImage Renderer::grab(gl::Fn &f) {
    QImage img(RES_W, RES_H, QImage::Format_RGBA8888);
    f.glBindFramebuffer(GL_FRAMEBUFFER, main_.fbo);
    f.glPixelStorei(GL_PACK_ALIGNMENT, 4);
    f.glReadPixels(0, 0, RES_W, RES_H, GL_RGBA, GL_UNSIGNED_BYTE, img.bits());   // memory row 0 = top of the screen
    QImage out = img.convertToFormat(QImage::Format_RGB888);
    return out;
}

}  // namespace moyou::render
