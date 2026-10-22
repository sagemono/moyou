#pragma once

#include "formats/image.h"
#include "formats/qrc.h"
#include "particles/particles.h"
#include "render/gl.h"
#include "sim/params.h"
#include "sim/wave.h"

#include <QImage>

#include <array>
#include <map>
#include <memory>
#include <string>

namespace moyou::render {

constexpr int RES_W = 1920, RES_H = 1080;   // PS3 1080p display buffer

// Everything the renderer needs from lines.qrc, decoded and transpiled.
struct Assets {
    std::map<std::string, std::string> glsl;   // "lines1.vp", "Copy.fp", ... (rsx::transpile, hwprec)
    int firmware = 493;                        // 493, or 100 for the firmware 1.00 frame graph
    std::array<image::Rgba, 24> months;        // month_bg/rgb/01..12.dds then month_bg/night/01..12.dds (1.00: rgb only)
    image::Rgba fres;                          // textures/TGA/freslut1.tga (512 x 1)
    image::Rgba particleTexture;               // particles/proc_iridescent.tga
    particles::Shaders particleShaders;        // particles_quads / particles_second, post-processed
    bool hasParticles() const { return !particleShaders.quadsVP.empty(); }
};
Assets loadAssets(const qrc::Archive &archive);   // throws qrc/image/rsx FormatError

class Renderer {
public:
    void init(gl::Fn &f, const Assets &assets);   // throws gl::ShaderError
    void release(gl::Fn &f);
    bool ready() const { return ready_; }
    int firmware() const { return fw_; }

    // One frame: simulate-independent GPU passes into the 1920x1080 display buffer, then present it letterboxed
    // into the framebuffer `fbo` of size w x h (device pixels).
    void render(gl::Fn &f, double dt, sim::Params &p, sim::Wave &wave, particles::ParticleSystem *ps, GLuint fbo, int w, int h);
    QImage grab(gl::Fn &f);                       // the display buffer as shown (or the selected debug view)
    particles::Camera particleCamera() const { return pcam_; }

private:
    struct Surface { GLuint tex = 0, fbo = 0; int w = 0, h = 0; };
    Surface surface(gl::Fn &f, int w, int h, bool f16, bool linear = false);
    GLuint texture(gl::Fn &f, int w, int h, GLenum ifmt, GLenum fmt, GLenum type, const void *data, GLenum filter = GL_NEAREST);
    void target(gl::Fn &f, const Surface &s);
    void bind(gl::Fn &f, int unit, GLuint tex);
    void quad(gl::Fn &f, GLuint vao = 0);
    void copy(gl::Fn &f, const Surface &src, const Surface &dst);
    void buildHdrTables(gl::Fn &f, double E, double W, double T);
    void buildNoise(gl::Fn &f);
    void brightTick(double dt, const sim::Params &p);
    void passBackground(gl::Fn &f, double dt, sim::Params &p);
    void passWave(gl::Fn &f, const sim::Params &p, const sim::Wave &wave);
    void passLinesController(gl::Fn &f, const sim::Params &p);
    void passGlare(gl::Fn &f, const sim::Params &p);
    void glareChain(gl::Fn &f, const sim::Params &p);
    void passTone100(gl::Fn &f, const sim::Params &p);
    void present(gl::Fn &f, const sim::Params &p, const sim::Wave &wave, GLuint fbo, int w, int h);
    void drawDebug(gl::Fn &f, const sim::Params &p, const sim::Wave &wave);

    bool ready_ = false;
    int fw_ = 493;
    gl::Program progCopy_, progBack_, progBack1_, progBack2_, progGlareOnly_, progWave_, progLC_, progGSP_, progCopyAniso_, progGauss_, progAcc_, progTone_,
        progPresent_, progDbg_;
    GLuint vaoQuad_ = 0, vaoLC_ = 0, vaoWave_ = 0, vaoDbg_ = 0, quadBuf_ = 0, waveIdx_ = 0, dbgBuf_ = 0;
    gl::Program progGS100_;                    // 1.00 GlareSource (decode, tone, threshold)
    Surface bgMap_, bgSnap_, wave_, main_, glsrc_, acc_, screen_;   // screen_: 1.00 HDR-encoded RGBA8
    std::array<Surface, 7> lvl_;
    std::array<Surface, 6> hb_, vb_;
    std::array<GLuint, 24> monthTex_{};
    GLuint fresTex_ = 0, stripesTex_ = 0, encodeTex_ = 0, ctrlTex_ = 0, exTex0_ = 0, exTex1_ = 0, noiseTex_ = 0, noise100Tex_ = 0;
    int32_t frandCounter_ = 0;                 // 1.00 frand counter (global 0x145964)
    float frand100();
    int32_t noiseCounter_ = 0;
    int bgShader_ = -1;                        // last COLOUR SHADER (a change starts the 2 s map cross-fade)
    std::string hdrKey_;
    struct Tween { double cur = 1, start = 1, target = 1, t = 0, dur = 1; };
    int brightLvl_ = -1;
    Tween brightCol_, brightPart_;
    particles::Camera pcam_;
};

}  // namespace moyou::render
