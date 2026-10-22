#pragma once

#include "render/gl.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace moyou::particles {

// Parameter lookup by PARTICLES.mnu name ("emit vel min", "size near", ...), already time-of-day blended / edited.
using ParamFn = std::function<double(const std::string &)>;

struct Camera {
    std::array<float, 16> mvp{};        // projection * view, column-major (as WebGL Float32Array)
    std::array<float, 16> modelview{};  // view, column-major
    double fovy = 53.0 * 3.14159265358979323846 / 180.0;
    std::array<float, 3> color{1, 1, 1};   // ParticlesLayer+0xd0 (_Color, Theme Brightness fade)
    float gamma = 1.0f;                 // ParticlesLayer+0x120 (_Gamma)
};

struct Assets {
    std::vector<uint8_t> textureRGBA;   // particles/proc_iridescent.tga as RGBA = TGA (R,G,B,A), file row 0 = t 0
    int textureW = 128, textureH = 128;
    // World-space position of wave vertex (i, j) of the 128x128 sheet (emission source).
    std::function<std::array<double, 3>(int i, int j)> surfaceSampler;
    bool exactFloat = true;             // SPU round-toward-zero arithmetic
    uint32_t seed = 0;
    std::vector<float> initialQuats;    // optional 2048 x 4 slot orientations (empty: identity)
};

// Transpiled + post-processed programs (rsx::transpile + rsx::postParticleVP/FP), GLSL ES 3.00.
struct Shaders {
    std::string quadsVP, quadsFP, secondVP, secondFP;
};

class ParticleSystem {
public:
    explicit ParticleSystem(Assets assets);
    ~ParticleSystem();

    void reset(uint32_t seed);                                         // empty system, all 2048 slots free
    int update(double dtSeconds, const ParamFn &P, const Camera &cam); // runs whole 60 Hz steps; returns steps run
    void stepOnce(const ParamFn &P, const Camera &cam, bool doEmit = true);  // (not "emit": Qt macro)
    void prewarm(int steps, const ParamFn &P, const Camera &cam);
    int count() const;                                                 // live particles in the vertex buffer
    static constexpr int capacity = 2048;

    // Vertex buffer as the SPU writes it: pos.xyz + fade (float4) and the quaternion as 4 truncated halves.
    struct VertexData {
        int count = 0;
        float *pos = nullptr;      // count * 4
        uint16_t *rotHalf = nullptr;  // count * 4
    };
    VertexData vertexData();
    void markVertexDataDirty();

    // Raw per-slot state for validation: 2048 x 12 floats (pos.xyz age | vel.xyz rate | quat xyzw), free list.
    std::vector<float> &slotState();
    std::vector<int32_t> &freeList();
    int &freeCount();
    std::array<uint32_t, 3> lcgState() const;  // brownian LCG streams after the last SPU step (SPU LS 0xbb20)

    // GL side. initGL compiles the programs and uploads the texture; draw issues draws 35 and 36 into the
    // currently bound framebuffer with the RSX state of the capture.
    void initGL(gl::Fn &f, const Shaders &shaders);
    void draw(gl::Fn &f, const ParamFn &P, const Camera &cam, float flipY);
    void releaseGL(gl::Fn &f);

private:
    struct Impl;
    Impl *d;
};

}  // namespace moyou::particles
