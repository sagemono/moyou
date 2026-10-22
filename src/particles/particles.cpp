#include "particles/particles.h"

#include <bit>
#include <cmath>
#include <utility>

namespace moyou::particles {

namespace {

constexpr int CAP = ParticleSystem::capacity;  // 0x2df88: 0x60b14(..., 0x800, ...)
constexpr int GRID_W = 128, GRID_H = 128;      // emitter grid = wave vertex buffer (Lines+0x2b0/0x2b4)
constexpr double DEAD = -666;                  // pos.w marker of a free particle (0x92fd0)
constexpr double STEP = 1.0 / 60;              // one simulation step per PS3 frame
constexpr size_t CMD_CAP = 0x333;              // command buffer capacity (ps+0x132c)

inline double fr(double x) { return static_cast<float>(x); }   // Math.fround
inline double hexf(uint32_t h) { return std::bit_cast<float>(h); }
constexpr double FLT_MIN_D = 1.1754943508222875e-38;
constexpr double TOWARD_ZERO = 0.999999940395355224609375;     // 1 - 2^-24: fr(y * this) = next float toward zero
// SPU single precision: round toward zero, denormals flushed (validated bit-exact on pos/vel/age/rate)
double rz(double x) {
    double y = fr(x);
    if (y != x && std::abs(y) > std::abs(x)) y = fr(y * TOWARD_ZERO);
    return (y < FLT_MIN_D && y > -FLT_MIN_D) ? 0.0 : y;
}
double truncd(double x) { return x < 0 ? std::ceil(x) : std::floor(x); }   // fctiwz / cflts (value of the JS trunc)
// JS ToInt32 (for `k & 3` on a double)
int32_t toInt32(double x) {
    if (!std::isfinite(x)) return 0;
    double m = std::fmod(truncd(x), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return static_cast<int32_t>(static_cast<uint32_t>(m));
}

// SPU constants (particles.elf .text immediates)
const double C_TWO_PI = hexf(0x40c90fda), C_037 = hexf(0x3ebd70a3), C_017 = hexf(0x3e2e147a), C_031 = hexf(0x3e9eb851);
const double C_SIXTIETH = hexf(0x3c888888), C_AGE_MAX = hexf(0x3f7fff58), C_094 = hexf(0x3f70a3d7), C_002 = hexf(0x3ca3d70a);
const double C_FADE_K = hexf(0xc1855554), C_ONE_ULP = hexf(0x3f800001);
const double SC_TWO_OVER_PI = hexf(0x3f22f983), SC_PIO2HI = hexf(0x3fc90fd9), SC_PIO2LO = hexf(0x33a22168);
const double SC_C0 = hexf(0xbab24992), SC_C1 = hexf(0x3d2aa035), SC_C2 = hexf(0xbeffffdf);
const double SC_S0 = hexf(0xb94c8c6d), SC_S1 = hexf(0x3c088342), SC_S2 = hexf(0xbe2aaaa1);

// float -> half, truncating (particles.elf func_6e08); denormals -> 0, overflow -> inf
uint16_t halfTrunc(double x) {
    const uint32_t b = std::bit_cast<uint32_t>(static_cast<float>(x));
    const uint32_t s = (b >> 16) & 0x8000, e = (b >> 23) & 0xff, m = b & 0x7fffff;
    if (e > 142) return uint16_t(s | 0x7c00);
    if (e > 112) return uint16_t(s | ((e - 112) << 10) | (m >> 13));
    return uint16_t(s);
}

// PARTICLES.mnu names, in menu order (readParams reads them all through ParamFn)
const char *const MENU[] = {
    "emit vel min", "emit vel mul", "emit vel var", "emit cone angle", "emit neg prob", "emit vel zscale",
    "emit per frame", "emit prob", "aging speed", "aging variance", "friction", "spin time scale", "delta time",
    "gravity", "wind dir x", "wind dir y", "wind dir z", "wind scale", "wind scale 10", "brownian scale",
    "spot pos x", "spot pos y", "spot pos z", "spot attn x", "spot attn y", "spot attn z", "specular power",
    "specular coeff", "lambert coeff", "exposure", "fresnel", "color_control", "iridescent exp", "global alpha",
    "size middle", "size near", "size far", "near focus", "near focus_dist", "near focus_pow", "near darkness",
    "near fuzziness", "far focus", "far focus_dist", "far focus_pow", "far darkness", "near align", "size align",
    "glare", "glare scale", "glare p1", "glare p2"};
enum Prm {
    EmitVelMin, EmitVelMul, EmitVelVar, EmitConeAngle, EmitNegProb, EmitVelZscale,
    EmitPerFrame, EmitProb, AgingSpeed, AgingVariance, Friction, SpinTimeScale, DeltaTime,
    Gravity, WindDirX, WindDirY, WindDirZ, WindScale, WindScale10, BrownianScale,
    SpotPosX, SpotPosY, SpotPosZ, SpotAttnX, SpotAttnY, SpotAttnZ, SpecularPower,
    SpecularCoeff, LambertCoeff, Exposure, Fresnel, ColorControl, IridescentExp, GlobalAlpha,
    SizeMiddle, SizeNear, SizeFar, NearFocus, NearFocusDist, NearFocusPow, NearDarkness,
    NearFuzziness, FarFocus, FarFocusDist, FarFocusPow, FarDarkness, NearAlign, SizeAlign,
    Glare, GlareScale, GlareP1, GlareP2, PRM_COUNT};
static_assert(std::size(MENU) == size_t(PRM_COUNT));

struct Params {
    double v[PRM_COUNT];
    double perFrame;
    double W[3];
    double operator[](int k) const { return v[k]; }
};

// A missing ParamFn or NaN value reads as 0 (the app always supplies every name; the firmware's preset tables
// are not part of moyou).
Params readParams(const ParamFn &P) {
    Params p{};
    for (int k = 0; k < PRM_COUNT; k++) {
        double v = P ? P(MENU[k]) : NAN;
        if (std::isnan(v)) v = 0;
        p.v[k] = fr(v);
    }
    p.perFrame = truncd(p[EmitPerFrame]);   // fctiwz (0x32a40)
    // wind (0x31494 @0x31e9c): W = normalize(dir) * (scale + 10*scale10) if |dir| >= 1e-4 else 0
    const double wx = p[WindDirX], wy = p[WindDirY], wz = p[WindDirZ];
    const double wl = std::sqrt(wx * wx + wy * wy + wz * wz), ws = fr(fr(p[WindScale10] * 10) + p[WindScale]);
    if (wl >= 1e-4) {
        const double inv = 1 / wl;
        p.W[0] = fr(fr(wx * inv) * ws); p.W[1] = fr(fr(wy * inv) * ws); p.W[2] = fr(fr(wz * inv) * ws);
    } else {
        p.W[0] = p.W[1] = p.W[2] = 0;
    }
    return p;
}

// ------------------------------------------------------------------ camera helpers
struct CamInfo {
    double eye[3], axis[3], ffq[4], fovy;
};
using M4 = std::array<double, 16>;

bool invert4(const M4 &a, M4 &out) {   // column-major 4x4, full inverse via cofactors
    const double a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3], a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7],
                 a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11], a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];
    const double b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10,
                 b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12,
                 b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30,
                 b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    double det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (det == 0 || std::isnan(det)) return false;   // JS: if (!det) return null
    det = 1 / det;
    out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det; out[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
    out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det; out[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
    out[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det; out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
    out[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det; out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
    out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det; out[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
    out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det; out[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
    out[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det; out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
    out[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det; out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;
    return true;
}

void quatFromRot(const M4 &m, double q[4]) {   // rotation part (column-major 4x4) -> quaternion (x,y,z,w)
    const double m00 = m[0], m11 = m[5], m22 = m[10], tr = m00 + m11 + m22;
    double x, y, z, w, s;
    if (tr > 0) { s = std::sqrt(tr + 1) * 2; w = 0.25 * s; x = (m[6] - m[9]) / s; y = (m[8] - m[2]) / s; z = (m[1] - m[4]) / s; }
    else if (m00 > m11 && m00 > m22) { s = std::sqrt(1 + m00 - m11 - m22) * 2; w = (m[6] - m[9]) / s; x = 0.25 * s; y = (m[4] + m[1]) / s; z = (m[8] + m[2]) / s; }
    else if (m11 > m22) { s = std::sqrt(1 + m11 - m00 - m22) * 2; w = (m[8] - m[2]) / s; x = (m[4] + m[1]) / s; y = 0.25 * s; z = (m[9] + m[6]) / s; }
    else { s = std::sqrt(1 + m22 - m00 - m11) * 2; w = (m[1] - m[4]) / s; x = (m[8] + m[2]) / s; y = (m[9] + m[6]) / s; z = 0.25 * s; }
    q[0] = x; q[1] = y; q[2] = z; q[3] = w;
}

// translate(0,0,-2). An all-zero Camera::modelview stands for "no modelview supplied".
constexpr std::array<float, 16> DEFAULT_MV = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -2, 1};
const std::array<float, 16> &modelviewOf(const Camera &cam) {
    for (float v : cam.modelview)
        if (v != 0) return cam.modelview;
    return DEFAULT_MV;
}

CamInfo camInfo(const Camera &camera) {
    const auto &mv = modelviewOf(camera);
    M4 a, inv;
    for (int i = 0; i < 16; i++) a[i] = mv[i];
    if (!invert4(a, inv)) inv = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 2, 1};
    CamInfo c;
    c.eye[0] = inv[12] / inv[15]; c.eye[1] = inv[13] / inv[15]; c.eye[2] = inv[14] / inv[15];
    const double ax[3] = {mv[2], mv[6], mv[10]};   // view z axis in world
    double al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (al == 0 || std::isnan(al)) al = 1;         // JS: || 1
    for (int k = 0; k < 3; k++) c.axis[k] = ax[k] / al;
    quatFromRot(inv, c.ffq);
    c.fovy = camera.fovy;
    return c;
}

// ------------------------------------------------------------------ uniforms (0x5c254)
struct Uniforms {
    float ParticleSize[3], Transparency[2], Focus[4], FocusCurves[3], NearControl[4], Darkness[2], Glare[4];
    float LifeBoundsMin[3], LifeBoundsMax[3], FrontFacingQuaternion[4], LightPack[4][4], Color[3], Gamma, IridescentExp;
};
const double LB_MIN[3] = {-10, -10, -12}, LB_MAX[3] = {10, 10, 7};   // 0x940b0 / 0x940c0

template <size_t N>
void put(float (&dst)[N], std::initializer_list<double> v) {   // uniform*fv: JS numbers -> float32 (nearest)
    size_t i = 0;
    for (double x : v) dst[i++] = static_cast<float>(x);
}

Uniforms computeUniforms(const ParamFn &P, const Camera &camera) {
    const Params p = readParams(P);
    const CamInfo c = camInfo(camera);
    Uniforms u;
    put(u.ParticleSize, {p[SizeMiddle], p[SizeNear], p[SizeFar]});
    put(u.Transparency, {p[Fresnel], p[GlobalAlpha]});
    put(u.Focus, {p[NearFocus], fr(p[NearFocus] + p[NearFocusDist]), p[FarFocus], fr(p[FarFocus] + p[FarFocusDist])});
    put(u.FocusCurves, {p[NearFocusPow], p[FarFocusPow], fr(c.fovy)});
    put(u.NearControl, {p[SizeAlign], p[NearFuzziness], p[NearAlign], 0});
    put(u.Darkness, {p[NearDarkness], p[FarDarkness]});
    put(u.Glare, {p[Glare], p[GlareScale], p[GlareP1], p[GlareP2]});
    put(u.LifeBoundsMin, {LB_MIN[0], LB_MIN[1], LB_MIN[2]});
    put(u.LifeBoundsMax, {LB_MAX[0], LB_MAX[1], LB_MAX[2]});
    put(u.FrontFacingQuaternion, {c.ffq[0], c.ffq[1], c.ffq[2], c.ffq[3]});
    // LightPack rows (columns: eye, (spot pos, specular power), (spot attn, 0), (lambert, spec coeff, exposure, color_control))
    put(u.LightPack[0], {c.eye[0], p[SpotPosX], p[SpotAttnX], p[LambertCoeff]});
    put(u.LightPack[1], {c.eye[1], p[SpotPosY], p[SpotAttnY], p[SpecularCoeff]});
    put(u.LightPack[2], {c.eye[2], p[SpotPosZ], p[SpotAttnZ], p[Exposure]});
    put(u.LightPack[3], {1, p[SpecularPower], 0, p[ColorControl]});
    put(u.Color, {camera.color[0], camera.color[1], camera.color[2]});   // ParticlesLayer+0xd0 (theme fade)
    u.Gamma = camera.gamma;                                              // layer+0x120
    u.IridescentExp = static_cast<float>(p[IridescentExp]);
    return u;
}

}  // namespace

// ==================================================================== state
struct ParticleSystem::Impl {
    Assets assets;
    bool exact = true;

    std::vector<float> S = std::vector<float>(CAP * 12);   // per particle: pos.xyz age | vel.xyz rate | quat xyzw
    std::vector<int32_t> freeList = std::vector<int32_t>(CAP);
    int freeCount = 0;
    struct Req { int x, y; double px, py, pz; };
    std::vector<Req> reqPrev, reqCur;                       // emission requests
    struct { int count = 0, x = 0, y = 0; } streak;         // layer+0x1008/0x100c/0x1010
    uint32_t frame = 0;                                     // stands in for the qgl frame counter (qgl 0x4dfa0)
    double accum = 0, tSim = 0;
    struct Cmd { int t; double x, y, z; };
    std::vector<Cmd> cmds;                                  // PPU particle commands
    std::vector<float> vbPos = std::vector<float>(CAP * 4);
    std::vector<uint16_t> vbRot = std::vector<uint16_t>(CAP * 4);
    int vbCount = 0;
    bool vbDirty = true;
    uint32_t lcg[3] = {};
    int32_t qCounter = 0;

    // GL
    bool glReady = false;
    gl::Program programs[2];
    GLuint vao = 0, posBuf = 0, rotBuf = 0, cornerBuf = 0, tex = 0;

    // -------------------------------------------------------------- SPU float ops
    double R(double x) const { return exact ? rz(x) : fr(x); }
    double fM(double a, double b) const { return R(a * b); }
    double fA(double a, double b) const { return R(a + b); }
    double fS(double a, double b) const { return R(a - b); }
    double fMA(double a, double b, double c) const { return R(a * b + c); }    // spu fma  : a*b + c
    double fNMS(double a, double b, double c) const { return R(c - a * b); }   // spu fnms : c - a*b

    // SPU sinf/cosf (particles.elf func_45b0 / func_44d0)
    double spuSinCos(double x, bool isCos) const {
        double q = fM(x, SC_TWO_OVER_PI);
        q = fA(q, q >= 0 ? 0.5 : -0.5);
        const double k = truncd(q);                             // cflts: truncate
        const double kf = k;
        double r = fNMS(kf, SC_PIO2HI, x);
        r = fNMS(kf, SC_PIO2LO, r);
        const double z = fM(r, r);
        double res;
        const int off = (toInt32(k) & 3) + (isCos ? 1 : 0);
        if ((off & 1) == 0) res = fMA(fMA(fMA(z, SC_S0, SC_S1), z, SC_S2), fM(z, r), r);
        else res = fMA(fMA(fMA(z, SC_C0, SC_C1), z, SC_C2), z, 1);
        return (off & 2) ? -res : res;
    }

    // -------------------------------------------------------------- RNGs
    // (1) SPU brownian LCG: s *= 16807 (mod 2^32); r = asfloat(0x40000000 | s>>9) - 3  -> [-1,1)
    //     three independent streams, reset every frame (0x5cdc0 / func_4978)
    double lcgRand(int i) {
        lcg[i] *= 16807u;
        return fr(static_cast<double>(std::bit_cast<float>(0x40000000u | (lcg[i] >> 9))) - 3);
    }
    // (2) qglbase global hash RNG (qgl 0x5f578), used by the PPU emitter:
    //     n = ++counter; n ^= n<<13; r = (n*(n*n*15731+789221)+1376312589) & 0x7fffffff; return 1 - r/2^30
    double qrand() {
        qCounter = static_cast<int32_t>(static_cast<uint32_t>(qCounter) + 1u);
        uint32_t n = static_cast<uint32_t>(qCounter);
        n ^= n << 13;
        const uint32_t t = n * n * 15731u + 789221u;
        const uint32_t r = (n * t + 1376312589u) & 0x7fffffffu;
        return fr(fr(static_cast<double>(r)) * -9.313225746154785e-10 + 1);   // fmadds(f13, -2^-30, 1)
    }

    void reset(uint32_t seed) {
        for (int i = 0; i < CAP; i++) {   // 0x5b74c then Clear (0x597bc)
            float *o = &S[i * 12];
            o[0] = 0; o[1] = 0; o[2] = 0; o[3] = static_cast<float>(DEAD);
            o[4] = 0; o[5] = 0; o[6] = 0; o[7] = 0;
            if (assets.initialQuats.size() >= size_t(CAP) * 4) {   // slot orientations of a long-running PS3
                for (int k = 0; k < 4; k++) o[8 + k] = assets.initialQuats[i * 4 + k];
            } else {
                o[8] = 0; o[9] = 0; o[10] = 0; o[11] = 1;
            }
            freeList[i] = i;
        }
        freeCount = CAP;
        reqPrev.clear(); reqCur.clear(); cmds.clear();
        streak = {};
        qCounter = static_cast<int32_t>(seed);
        frame = 0; accum = 0; vbCount = 0; tSim = 0;
    }

    // -------------------------------------------------------------- wave-sheet emitter grid
    // Emission positions are vertices of the 128x128 wave sheet (Lines vertex buffer), in world space:
    // pos = inverse(Proj*View) * clipPos(vertex J*128 + I)   (0x2d640, matrix = layer+0x50).
    // Without a surfaceSampler a stand-in sheet is used (NOT the PS3 wave; for testing only).
    std::array<double, 3> fallbackSurface(int i, int j) const {
        const double u = i / 127.0, v = j / 127.0, t = tSim;
        return {9.5 - 20 * u + 2.0 * (v - 0.5), 0.6 + 1.2 * std::sin(3.1 * u + 2.3 * v + 0.45 * t) - 0.8 * (v - 0.5),
                -6.6 - 1.6 * (v - 0.5) + 0.9 * std::cos(2.0 * u - 1.7 * v + 0.31 * t)};
    }
    std::array<double, 3> cellPos(int i, int j) const {
        i = ((i % GRID_W) + GRID_W) % GRID_W; j = ((j % GRID_H) + GRID_H) % GRID_H;   // 0x2d6e8 wraps
        const auto p = assets.surfaceSampler ? assets.surfaceSampler(i, j) : fallbackSurface(i, j);
        return {fr(p[0]), fr(p[1]), fr(p[2])};
    }
    static int cellFromRand(double r, int n) {   // (int)(((r + 1) * 0.5) * (n - 1))
        return static_cast<int>(truncd(fr(fr(fr(r + 1) * 0.5) * (n - 1))));
    }

    // -------------------------------------------------------------- PPU: emission (0x30b80)
    void emission(const Params &p, const CamInfo &cam) {
        reqCur.clear();
        // 0x2dde4: with probability 'emit prob', 'emit per frame' requests on random grid cells
        const double r = qrand();
        if (fr(r * 0.5 + 0.5) < p[EmitProb] && p.perFrame != 0) {
            for (double k = 0; k < p.perFrame; k++) {
                const int x = cellFromRand(qrand(), GRID_W);
                const int y = cellFromRand(qrand(), GRID_H);
                const auto c = cellPos(x, y);
                reqCur.push_back({x, y, c[0], c[1], c[2]});
            }
        }
        // 0x2dbf4: "streak" emitter walking along +I, one request every other frame
        if (streak.count == 0) {
            if (fr(qrand() * 0.5 + 0.5) < fr(0.06)) {
                streak.x = cellFromRand(qrand(), GRID_W);
                streak.y = cellFromRand(qrand(), GRID_H);
                const double cf = fr(qrand() * 80);
                streak.count = static_cast<int>(truncd(cf)) + 100;
            }
        } else if ((frame & 1) == 0) {
            if (GRID_W > streak.x) {
                const auto cs = cellPos(streak.x, streak.y);
                reqCur.push_back({streak.x, streak.y, cs[0], cs[1], cs[2]});
                streak.x++; streak.count--;
            } else {
                streak.count = 0;
            }
        }
        // process last frame's requests -> commands
        const double invDt = fr(1 / p[DeltaTime]);                  // vrefp (exact on RPCS3 / exact for this value)
        const double ax = cam.axis[0], ay = cam.axis[1], az = cam.axis[2];   // normalize(eye - target)
        const double cone = fr(p[EmitConeAngle] * fr(0.0174532924));
        for (const Req &rq : reqPrev) {
            const auto now = cellPos(rq.x, rq.y);
            const double vx = fr(fr(now[0] - rq.px) * invDt), vy = fr(fr(now[1] - rq.py) * invDt),
                         vz = fr(fr(now[2] - rq.pz) * invDt);
            const double len2 = fr(fr(vx * vx + vy * vy) + vz * vz), len = fr(std::sqrt(len2));
            if (!(len >= fr(1e-4))) continue;                         // 1e-4 > |v| -> no particle
            const double il = fr(1 / std::sqrt(len2));
            double dx = fr(vx * il), dy = fr(vy * il), dz = fr(vz * il);
            if (fr(fr(qrand() + 1) * 0.5) < p[EmitNegProb]) { dx = -dx; dy = -dy; dz = -dz; }
            // perpendicular p1 = normalize(largest of (0,dz,-dy), (-dz,0,dx), (dy,-dx,0))
            const double n4 = fr(dy * dy + dz * dz), n6 = fr(dx * dx + dz * dz), n5 = fr(dx * dx + dy * dy);
            double p1[4];
            if (n4 > n6 && n4 > n5) { p1[0] = 0; p1[1] = dz; p1[2] = -dy; p1[3] = n4; }
            else if (!(n4 > n6) && n6 > n5) { p1[0] = -dz; p1[1] = 0; p1[2] = dx; p1[3] = n6; }
            else { p1[0] = dy; p1[1] = -dx; p1[2] = 0; p1[3] = n5; }
            const double ip = fr(1 / std::sqrt(p1[3]));
            const double p1x = fr(p1[0] * ip), p1y = fr(p1[1] * ip), p1z = fr(p1[2] * ip);
            const double p2x = fr(dy * p1z - dz * p1y), p2y = fr(dz * p1x - dx * p1z), p2z = fr(dx * p1y - dy * p1x); // d x p1
            const double th = fr(fr(fr(qrand() + 1) * 0.5) * cone);
            const double ph = fr(fr(fr(qrand() + 1) * 0.5) * fr(6.28318548));
            const double sth = fr(std::sin(th)), cph = fr(std::cos(ph)), sph = fr(std::sin(ph)), cth = fr(std::cos(th)); // _FSin
            const double sx = fr(sth * cph), sy = fr(sth * sph);
            const double ex = fr(dx * cth + fr(p1x * sx + fr(p2x * sy))), ey = fr(dy * cth + fr(p1y * sx + fr(p2y * sy))),
                         ez = fr(dz * cth + fr(p1z * sx + fr(p2z * sy)));
            // 0x2b560: scale the component along the camera axis by 'emit vel zscale'
            const double t = fr(fr(fr(ex * ax + ey * ay) + ez * az) * fr(1 / fr(fr(ax * ax + ay * ay) + az * az)));
            const double prx = fr(ax * t), pry = fr(ay * t), prz = fr(az * t), zs = p[EmitVelZscale];
            const double ox = fr(fr(prx * zs) + fr(ex - prx)), oy = fr(fr(pry * zs) + fr(ey - pry)),
                         oz = fr(fr(prz * zs) + fr(ez - prz));
            // speed = max(pow(|v| * mul * (1 + var*r), 0.5), vel_min). Math.pow(x, 0.5) is sqrt(x) in V8 (fdlibm
            // special case) for x >= +0; the only other inputs (-0, negative, NaN) end up as vel_min either way.
            const double mulv = fr(p[EmitVelMul] * fr(qrand() * p[EmitVelVar] + 1));
            double spd = fr(std::sqrt(fr(len * mulv)));
            if (spd < p[EmitVelMin]) spd = p[EmitVelMin];
            double rate = fr(p[AgingSpeed] * fr(qrand() * p[AgingVariance] + 1));
            if (fr(0.001) >= rate) rate = fr(0.001);
            if (cmds.size() < CMD_CAP) cmds.push_back({0, 0, 0, 0}); else continue;
            if (cmds.size() < CMD_CAP) cmds.push_back({1, now[0], now[1], now[2]}); else continue;
            if (cmds.size() < CMD_CAP) cmds.push_back({2, fr(ox * spd), fr(oy * spd), fr(oz * spd)}); else continue;
            if (cmds.size() < CMD_CAP) cmds.push_back({4, rate, 0, 0});
        }
        reqPrev = std::move(reqCur);
        reqCur.clear();
    }

    // -------------------------------------------------------------- PPU: commands (0x5cad0)
    void execCommands() {
        int cur = -1;
        for (const Cmd &c : cmds) {
            if (c.t == 0) {   // 0x5b6bc: pop free list (LIFO), pos = vel = 0 (quat kept)
                cur = -1;
                if (freeCount > 0) {
                    cur = freeList[--freeCount];
                    float *o = &S[cur * 12];
                    for (int k = 0; k < 8; k++) o[k] = 0;
                }
                continue;
            }
            if (cur < 0) continue;
            float *b = &S[cur * 12];
            if (c.t == 1) { b[0] = float(c.x); b[1] = float(c.y); b[2] = float(c.z); }
            else if (c.t == 2) { b[4] = float(c.x); b[5] = float(c.y); b[6] = float(c.z); }
            else if (c.t == 3) { b[4] = float(b[4] + c.x); b[5] = float(b[5] + c.y); b[6] = float(b[6] + c.z); }
            else if (c.t == 4) { b[7] = float(c.x); }
        }
        cmds.clear();
    }

    // -------------------------------------------------------------- SPU: update + vertex write (LS 0x6ed0)
    void spuUpdate(const Params &p) {
        lcg[0] = 0x98756161u; lcg[1] = 0x21324889u; lcg[2] = 0x82181158u;
        const double brown = p[BrownianScale], g = p[Gravity], frc = p[Friction], dt = p[DeltaTime];
        const double spinS = fM(p[SpinTimeScale], C_SIXTIETH);
        const double Wx = p.W[0], Wy = p.W[1], Wz = p.W[2];
        int n = 0;
        for (int i = 0; i < CAP; i++) {
            float *o = &S[i * 12];
            if (o[3] == DEAD) continue;
            const double r0 = lcgRand(0), r1 = lcgRand(1), r2 = lcgRand(2);
            // F = brownian*r + W (+ interaction field * fieldScale, zero when idle)
            const double Fx = fA(fM(r0, brown), Wx), Fy = fA(fM(r1, brown), Wy), Fz = fA(fM(r2, brown), Wz);
            // (d-pad rotation term vel += (C + R(p-C) - p)/dt is exactly 0 when idle)
            double vx = o[4], vy = o[5], vz = o[6];
            const double rate = o[7];
            const double axv = fNMS(frc, vx, fA(0, Fx)), ayv = fNMS(frc, vy, fA(g, Fy)), azv = fNMS(frc, vz, fA(0, Fz));
            vx = fA(vx, fM(axv, dt)); vy = fA(vy, fM(ayv, dt)); vz = fA(vz, fM(azv, dt));
            const double px = fA(o[0], fM(vx, dt)), py = fA(o[1], fM(vy, dt)), pz = fA(o[2], fM(vz, dt));
            const double age = fA(o[3], rate);
            // spin: q += 0.5 * (w (x) q) * spin/60, w = (sin(.37a), cos(.17a), cos(.31a), 0), a = 2*pi*age
            const double a = fM(age, C_TWO_PI);
            const double wx = spuSinCos(fM(a, C_037), false), wy = spuSinCos(fM(a, C_017), true),
                         wz = spuSinCos(fM(a, C_031), true);
            double qx = o[8], qy = o[9], qz = o[10], qw = o[11];
            double rx = fM(qx, 0), ry = fM(qy, 0), rzz = fM(qz, 0), rw = fM(qw, 0);   // q * w.w
            rx = fMA(fM(qw, wx), 1, rx); ry = fMA(fM(qz, wx), -1, ry); rzz = fMA(fM(qy, wx), 1, rzz); rw = fMA(fM(qx, wx), -1, rw);
            rx = fMA(fM(qz, wy), 1, rx); ry = fMA(fM(qw, wy), 1, ry); rzz = fMA(fM(qx, wy), -1, rzz); rw = fMA(fM(qy, wy), -1, rw);
            rx = fMA(fM(qy, wz), -1, rx); ry = fMA(fM(qx, wz), 1, ry); rzz = fMA(fM(qw, wz), 1, rzz); rw = fMA(fM(qz, wz), -1, rw);
            qx = fA(qx, fM(fM(rx, 0.5), spinS)); qy = fA(qy, fM(fM(ry, 0.5), spinS));
            qz = fA(qz, fM(fM(rzz, 0.5), spinS)); qw = fA(qw, fM(fM(rw, 0.5), spinS));
            const double dd = fA(fA(fA(fM(qx, qx), fM(qy, qy)), fM(qz, qz)), fM(qw, qw));
            const double y0 = fr(1 / std::sqrt(dd));                                         // frsqest+fi (approx.)
            const double nn = fMA(fNMS(y0, fM(y0, dd), C_ONE_ULP), fM(y0, 0.5), y0);
            qx = fM(qx, nn); qy = fM(qy, nn); qz = fM(qz, nn); qw = fM(qw, nn);
            o[4] = float(vx); o[5] = float(vy); o[6] = float(vz);
            o[0] = float(px); o[1] = float(py); o[2] = float(pz);
            o[8] = float(qx); o[9] = float(qy); o[10] = float(qz); o[11] = float(qw);
            const bool alive = age < C_AGE_MAX;
            const bool inb = !(px < LB_MIN[0] || px > LB_MAX[0] || py < LB_MIN[1] || py > LB_MAX[1] ||
                               pz < LB_MIN[2] || pz > LB_MAX[2]);
            if (!(alive && inb)) { o[3] = float(DEAD); freeList[freeCount++] = i; continue; }
            o[3] = float(age);
            // vertex: (pos, fade), quat as truncated halves, attr10 = 0 (unused by the shaders)
            const double fa = age > C_002 ? C_002 : age;
            double fb = fS(age, C_094);
            if (0 > fb) fb = 0;
            const double fade = fM(fM(fa, 50), fMA(fb, C_FADE_K, 1));
            float *v = &vbPos[n * 4];
            v[0] = float(px); v[1] = float(py); v[2] = float(pz); v[3] = float(fade);
            uint16_t *h = &vbRot[n * 4];
            h[0] = halfTrunc(qx); h[1] = halfTrunc(qy); h[2] = halfTrunc(qz); h[3] = halfTrunc(qw);
            n++;
        }
        vbCount = n;
    }

    // -------------------------------------------------------------- one PS3 frame
    void stepOnce(const ParamFn &P, const Camera &camera, bool doEmit) {
        const Params p = readParams(P);
        const CamInfo cam = camInfo(camera);
        if (doEmit) emission(p, cam);
        execCommands();
        spuUpdate(p);
        vbDirty = true;
        frame++; tSim += STEP;
    }

    // -------------------------------------------------------------- GL
    void flushVB(gl::Fn &f) {
        if (!vbDirty) return;
        f.glBindBuffer(GL_ARRAY_BUFFER, posBuf);
        f.glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(vbCount) * 16, vbPos.data());
        f.glBindBuffer(GL_ARRAY_BUFFER, rotBuf);
        f.glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(vbCount) * 8, vbRot.data());
        vbDirty = false;
    }

    void setU(gl::Fn &f, const gl::Program &pr, const Uniforms &u, const float *mvpRows, const float *mvRows,
              float flipY) {
        auto v = [&](const char *name, const float *a, int n) {
            const GLint l = pr.loc(name);
            if (l < 0) return;   // optimised out / not declared by this program
            if (n == 1) f.glUniform1f(l, a[0]);
            else if (n == 2) f.glUniform2fv(l, 1, a);
            else if (n == 3) f.glUniform3fv(l, 1, a);
            else f.glUniform4fv(l, 1, a);
        };
        GLint l;
        if ((l = pr.loc("ModelviewProjection")) >= 0) f.glUniform4fv(l, 4, mvpRows);   // matrix rows (RSX c[] layout)
        if ((l = pr.loc("Modelview")) >= 0) f.glUniform4fv(l, 4, mvRows);
        if ((l = pr.loc("LightPack")) >= 0) f.glUniform4fv(l, 4, &u.LightPack[0][0]);
        v("LightPack_0", u.LightPack[0], 4); v("LightPack_1", u.LightPack[1], 4);
        v("LightPack_2", u.LightPack[2], 4); v("LightPack_3", u.LightPack[3], 4);
        v("ParticleSize", u.ParticleSize, 3); v("Transparency", u.Transparency, 2); v("Focus", u.Focus, 4);
        v("FocusCurves", u.FocusCurves, 3); v("FrontFacingQuaternion", u.FrontFacingQuaternion, 4);
        v("NearControl", u.NearControl, 4); v("LifeBoundsMin", u.LifeBoundsMin, 3);
        v("LifeBoundsMax", u.LifeBoundsMax, 3); v("Darkness", u.Darkness, 2); v("Glare", u.Glare, 4);
        v("Color", u.Color, 3); v("Gamma", &u.Gamma, 1); v("IridescentExp", &u.IridescentExp, 1);
        v("u_flipY", &flipY, 1);
        if ((l = pr.loc("IridescentTex")) >= 0) f.glUniform1i(l, 0);
    }
};

// ==================================================================== public API
ParticleSystem::ParticleSystem(Assets assets) : d(new Impl) {
    d->assets = std::move(assets);
    d->exact = d->assets.exactFloat;
    d->reset(d->assets.seed);
}
ParticleSystem::~ParticleSystem() { delete d; }

void ParticleSystem::reset(uint32_t seed) { d->reset(seed); }

int ParticleSystem::update(double dtSeconds, const ParamFn &P, const Camera &cam) {
    d->accum += (dtSeconds > 0) ? dtSeconds : 0;   // Math.max(0, dtSeconds || 0)
    int steps = 0;
    while (d->accum >= STEP && steps < 8) { d->stepOnce(P, cam, true); d->accum -= STEP; steps++; }
    if (d->accum > STEP * 8) d->accum = 0;          // drop time after long stalls instead of spiralling
    if (steps) d->vbDirty = true;
    return steps;
}

void ParticleSystem::stepOnce(const ParamFn &P, const Camera &cam, bool doEmit) { d->stepOnce(P, cam, doEmit); }

void ParticleSystem::prewarm(int steps, const ParamFn &P, const Camera &cam) {
    for (int k = 0; k < steps; k++) d->stepOnce(P, cam, true);
    d->vbDirty = true;
}

int ParticleSystem::count() const { return d->vbCount; }

ParticleSystem::VertexData ParticleSystem::vertexData() { return {d->vbCount, d->vbPos.data(), d->vbRot.data()}; }
void ParticleSystem::markVertexDataDirty() { d->vbDirty = true; }

std::vector<float> &ParticleSystem::slotState() { return d->S; }
std::vector<int32_t> &ParticleSystem::freeList() { return d->freeList; }
int &ParticleSystem::freeCount() { return d->freeCount; }
std::array<uint32_t, 3> ParticleSystem::lcgState() const { return {d->lcg[0], d->lcg[1], d->lcg[2]}; }

void ParticleSystem::initGL(gl::Fn &f, const Shaders &shaders) {
    if (d->glReady) return;
    d->programs[0] = gl::link(f, shaders.quadsVP, shaders.quadsFP, "particles_quads");
    try {
        d->programs[1] = gl::link(f, shaders.secondVP, shaders.secondFP, "particles_second");
    } catch (...) {
        gl::destroy(f, d->programs[0]);
        throw;
    }

    GLint svVao = 0, svBuf = 0, svAt = 0, svTex = 0, svAl = 0;
    f.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &svVao);
    f.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &svBuf);
    f.glGetIntegerv(GL_ACTIVE_TEXTURE, &svAt);
    f.glActiveTexture(GL_TEXTURE0);
    f.glGetIntegerv(GL_TEXTURE_BINDING_2D, &svTex);
    f.glGetIntegerv(GL_UNPACK_ALIGNMENT, &svAl);

    f.glGenVertexArrays(1, &d->vao);
    f.glBindVertexArray(d->vao);
    static const float corners[8] = {0, 0, 1, 0, 1, 1, 0, 1};   // attr8: static quad corners (RSX 0x2c46e80, MODULO 4)
    f.glGenBuffers(1, &d->cornerBuf);
    f.glBindBuffer(GL_ARRAY_BUFFER, d->cornerBuf);
    f.glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    f.glEnableVertexAttribArray(8);
    f.glVertexAttribPointer(8, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
    f.glGenBuffers(1, &d->posBuf);                               // attr0: pos.xyz + fade (per particle: DIVIDE 4)
    f.glBindBuffer(GL_ARRAY_BUFFER, d->posBuf);
    f.glBufferData(GL_ARRAY_BUFFER, CAP * 16, nullptr, GL_DYNAMIC_DRAW);
    f.glEnableVertexAttribArray(0);
    f.glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, nullptr);
    f.glVertexAttribDivisor(0, 1);
    f.glGenBuffers(1, &d->rotBuf);                               // attr11: quaternion, 4 x half
    f.glBindBuffer(GL_ARRAY_BUFFER, d->rotBuf);
    f.glBufferData(GL_ARRAY_BUFFER, CAP * 8, nullptr, GL_DYNAMIC_DRAW);
    f.glEnableVertexAttribArray(11);
    f.glVertexAttribPointer(11, 4, GL_HALF_FLOAT, GL_FALSE, 8, nullptr);
    f.glVertexAttribDivisor(11, 1);

    const Assets &a = d->assets;
    f.glGenTextures(1, &d->tex);
    f.glBindTexture(GL_TEXTURE_2D, d->tex);
    f.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (a.textureW > 0 && a.textureH > 0 && a.textureRGBA.size() >= size_t(a.textureW) * a.textureH * 4) {
        f.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, a.textureW, a.textureH, 0, GL_RGBA, GL_UNSIGNED_BYTE, a.textureRGBA.data());
    } else {
        static const uint8_t grey[4] = {128, 128, 128, 255};
        f.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, grey);
    }
    // RSX tex0: filter 0x2064000 (min LINEAR_MIPMAP_LINEAR on a 1-level texture = LINEAR, mag LINEAR),
    //           address 0x30050202 (S,T = MIRROR)
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    f.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);

    f.glBindTexture(GL_TEXTURE_2D, GLuint(svTex));
    f.glActiveTexture(GLenum(svAt));
    f.glPixelStorei(GL_UNPACK_ALIGNMENT, svAl);
    f.glBindVertexArray(GLuint(svVao));
    f.glBindBuffer(GL_ARRAY_BUFFER, GLuint(svBuf));
    d->glReady = true;
    d->vbDirty = true;
}

// Issues draw 35 (particles_quads) and draw 36 (particles_second) into the bound framebuffer.
void ParticleSystem::draw(gl::Fn &f, const ParamFn &P, const Camera &cam, float flipY) {
    if (!d->glReady || !d->vbCount) return;
    GLint svBuf = 0;
    f.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &svBuf);
    d->flushVB(f);
    f.glBindBuffer(GL_ARRAY_BUFFER, GLuint(svBuf));
    const Uniforms u = computeUniforms(P, cam);
    const auto &mvp = cam.mvp;
    const auto &mv = modelviewOf(cam);
    float mvpRows[16], mvRows[16];   // column-major -> 4 RSX rows
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) { mvpRows[r * 4 + c] = mvp[c * 4 + r]; mvRows[r * 4 + c] = mv[c * 4 + r]; }

    // save state
    GLint prog = 0, vao = 0, at = 0, srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0, eqRGB = 0, eqA = 0, tex0 = 0;
    GLboolean mask[4] = {}, dmask = GL_TRUE;
    f.glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    f.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    f.glGetIntegerv(GL_ACTIVE_TEXTURE, &at);
    const bool blend = f.glIsEnabled(GL_BLEND), depth = f.glIsEnabled(GL_DEPTH_TEST),
               cull = f.glIsEnabled(GL_CULL_FACE), stencil = f.glIsEnabled(GL_STENCIL_TEST);
    f.glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB); f.glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
    f.glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA); f.glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
    f.glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRGB); f.glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqA);
    f.glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    f.glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    f.glActiveTexture(GL_TEXTURE0);
    f.glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);

    // RSX state of draws 35/36: blend ONE/ONE FUNC_ADD (rgb+alpha), alpha test GREATER 0 (discard in FP),
    // no depth test/write, no cull, no stencil, colour mask RGBA
    f.glEnable(GL_BLEND); f.glBlendFunc(GL_ONE, GL_ONE); f.glBlendEquation(GL_FUNC_ADD);
    f.glDisable(GL_DEPTH_TEST); f.glDisable(GL_CULL_FACE); f.glDisable(GL_STENCIL_TEST);
    f.glDepthMask(GL_FALSE); f.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    f.glBindTexture(GL_TEXTURE_2D, d->tex);
    f.glBindVertexArray(d->vao);
    for (auto &pr : d->programs) {
        f.glUseProgram(pr.id);
        d->setU(f, pr, u, mvpRows, mvRows, flipY);
        f.glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, d->vbCount);
    }

    // restore state
    f.glBindVertexArray(GLuint(vao)); f.glUseProgram(GLuint(prog));
    f.glBindTexture(GL_TEXTURE_2D, GLuint(tex0)); f.glActiveTexture(GLenum(at));
    auto setCap = [&](GLenum cap, bool on) { on ? f.glEnable(cap) : f.glDisable(cap); };
    setCap(GL_BLEND, blend); setCap(GL_DEPTH_TEST, depth); setCap(GL_CULL_FACE, cull); setCap(GL_STENCIL_TEST, stencil);
    f.glBlendFuncSeparate(GLenum(srcRGB), GLenum(dstRGB), GLenum(srcA), GLenum(dstA));
    f.glBlendEquationSeparate(GLenum(eqRGB), GLenum(eqA));
    f.glColorMask(mask[0], mask[1], mask[2], mask[3]); f.glDepthMask(dmask);
}

void ParticleSystem::releaseGL(gl::Fn &f) {
    if (!d->glReady) return;
    for (auto &pr : d->programs) gl::destroy(f, pr);
    f.glDeleteVertexArrays(1, &d->vao);
    const GLuint bufs[3] = {d->cornerBuf, d->posBuf, d->rotBuf};
    f.glDeleteBuffers(3, bufs);
    f.glDeleteTextures(1, &d->tex);
    d->vao = d->cornerBuf = d->posBuf = d->rotBuf = d->tex = 0;
    d->glReady = false;
}

}  // namespace moyou::particles
