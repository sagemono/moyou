#include "sim/wave.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace moyou::sim {

namespace {
constexpr double PI = 3.14159265358979323846;

void bsp(double t, double out[4]) {
    const double s = 1 - t, t2 = t * t, t3 = t2 * t;
    out[0] = s * s * s / 6;
    out[1] = (3 * t3 - 6 * t2 + 4) / 6;
    out[2] = (-3 * t3 + 3 * t2 + 3 * t + 1) / 6;
    out[3] = t3 / 6;
}
}  // namespace

// ------------------------------------------------------------------ matrices
Mat4 mul(const Mat4 &a, const Mat4 &b) {
    Mat4 o{};
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    return o;
}

Mat4 inverse(const Mat4 &a) {
    const double b00 = a[0] * a[5] - a[1] * a[4], b01 = a[0] * a[6] - a[2] * a[4], b02 = a[0] * a[7] - a[3] * a[4];
    const double b03 = a[1] * a[6] - a[2] * a[5], b04 = a[1] * a[7] - a[3] * a[5], b05 = a[2] * a[7] - a[3] * a[6];
    const double b06 = a[8] * a[13] - a[9] * a[12], b07 = a[8] * a[14] - a[10] * a[12], b08 = a[8] * a[15] - a[11] * a[12];
    const double b09 = a[9] * a[14] - a[10] * a[13], b10 = a[9] * a[15] - a[11] * a[13], b11 = a[10] * a[15] - a[11] * a[14];
    const double d = 1 / (b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06);
    return {(a[5] * b11 - a[6] * b10 + a[7] * b09) * d, (a[2] * b10 - a[1] * b11 - a[3] * b09) * d,
            (a[13] * b05 - a[14] * b04 + a[15] * b03) * d, (a[10] * b04 - a[9] * b05 - a[11] * b03) * d,
            (a[6] * b08 - a[4] * b11 - a[7] * b07) * d, (a[0] * b11 - a[2] * b08 + a[3] * b07) * d,
            (a[14] * b02 - a[12] * b05 - a[15] * b01) * d, (a[8] * b05 - a[10] * b02 + a[11] * b01) * d,
            (a[4] * b10 - a[5] * b08 + a[7] * b06) * d, (a[1] * b08 - a[0] * b10 - a[3] * b06) * d,
            (a[12] * b04 - a[13] * b02 + a[15] * b00) * d, (a[9] * b02 - a[8] * b04 - a[11] * b00) * d,
            (a[5] * b07 - a[4] * b09 - a[6] * b06) * d, (a[0] * b09 - a[1] * b07 + a[2] * b06) * d,
            (a[13] * b01 - a[12] * b03 - a[14] * b00) * d, (a[8] * b03 - a[9] * b01 + a[10] * b00) * d};
}

// validated against the capture: perspective(53, 16/9) * T(0,0,-2) * Model. Firmware 1.00 (0xff6ac) uses
// near = 9.99999975e-05; lines1 shades from the clip-space position, so the near plane changes the look.
const Mat4 &projection(int firmware) {
    auto make = [](double n) {
        const double f = 1 / std::tan(53 * PI / 360), fa = 1000, aspect = 16.0 / 9.0;
        return Mat4{f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (fa + n) / (n - fa), -1, 0, 0, 2 * fa * n / (n - fa), 0};
    };
    static const Mat4 m493 = make(0.1), m100 = make(double(9.99999975e-05f));
    return firmware == 100 ? m100 : m493;
}

const Mat4 &view() {
    static const Mat4 m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -2, 1};
    return m;
}

// ------------------------------------------------------------------ rope start states from the plugin
static RopeStates readPrx(const std::filesystem::path &path, std::string *error);

RopeStates RopeStates::fromPrx(const std::filesystem::path &path, std::string *error) {
    try {
        return readPrx(path, error);
    } catch (const std::exception &) {
        if (error) *error = "truncated or malformed ELF/PRX";
        return {};
    }
}

static RopeStates readPrx(const std::filesystem::path &path, std::string *error) {
    auto fail = [&](const std::string &m) { if (error) *error = m; return RopeStates{}; };
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open " + path.string());
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 64 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0)
        return fail("not a decrypted ELF/PRX (an encrypted SPRX has to be decrypted first, e.g. with RPCS3's "
                    "Utilities > Decrypt PS3 Binaries or scetool)");
    auto be = [&](size_t o, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | d.at(o + i); return v; };
    const uint64_t phoff = be(32, 8);
    const int phent = int(be(54, 2)), phnum = int(be(56, 2));
    auto read = [&](uint64_t vaddr, size_t bytes, std::vector<uint8_t> &out) {
        for (int i = 0; i < phnum; i++) {
            const size_t ph = size_t(phoff) + size_t(i) * phent;
            if (be(ph, 4) != 1 && be(ph, 4) != 0x700000a4) continue;   // PT_LOAD (PRX segments are PT_LOAD too)
            const uint64_t off = be(ph + 8, 8), va = be(ph + 16, 8), filesz = be(ph + 32, 8);
            if (vaddr >= va && vaddr + bytes <= va + filesz && off + (vaddr - va) + bytes <= d.size()) {
                out.assign(d.begin() + ptrdiff_t(off + (vaddr - va)), d.begin() + ptrdiff_t(off + (vaddr - va) + bytes));
                return true;
            }
        }
        return false;
    };
    // custom_render_plugin 4.93: {positions, velocities} for states 0..2 (0x4f814's state table)
    const uint64_t tables[3][2] = {{0x95a40, 0x96b2c}, {0x97c18, 0x98d04}, {0x99df0, 0x9aedc}};
    RopeStates s;
    s.data.reserve(3 * NN * 6);
    for (const auto &t : tables)
        for (uint64_t va : t) {
            std::vector<uint8_t> raw;
            if (!read(va, NN * 3 * 4, raw)) return fail("state table not found: is this custom_render_plugin from firmware 4.93?");
            for (int i = 0; i < NN * 3; i++) {
                const uint32_t u = uint32_t(raw[i * 4]) << 24 | uint32_t(raw[i * 4 + 1]) << 16 | uint32_t(raw[i * 4 + 2]) << 8 | raw[i * 4 + 3];
                float v;
                std::memcpy(&v, &u, 4);
                if (!std::isfinite(v) || std::fabs(v) > 1000) return fail("state table looks wrong (not firmware 4.93?)");
                s.data.push_back(v);
            }
        }
    // sanity: in state 1 the driven end (j = 0, i = 18) sits at x = 0 and the free end (i = 0) about 5 units out
    const float *s1 = &s.data[size_t(NN) * 6];
    if (std::fabs(s1[18 * 3]) > 1e-3 || s1[0] < 1 || s1[0] > 20) return fail("state table looks wrong (not firmware 4.93?)");
    return s;
}

// ------------------------------------------------------------------ rope simulation
Wave::Prm Wave::prm(const Params &p) const {
    return {p.P("LINE1.TIMESTEP"), p.P("LINE1.TENSION"), p.P("LINE1.LENGTH"), p.P("LINE1.PERTURBATION"),
            p.P("LINE1.DAMPING"), p.P("LINE1.END Y"), p.P("LINE1.END Z")};
}

void Wave::loadState(int n, const Params &p) {
    if (states_.valid()) {
        const size_t base = size_t(n) * NN * 6;
        for (int i = 0; i < NN; i++) {
            for (int c = 0; c < 3; c++) {
                cur_[i * 4 + c] = states_.data[base + i * 3 + c];
                vel_[i * 4 + c] = states_.data[base + NN * 3 + i * 3 + c];
            }
            cur_[i * 4 + 3] = 1;
            vel_[i * 4 + 3] = 0;
        }
    } else {
        // No plugin tables: a straight rope (free end at +x, driven end at x = 0) run until its motion looks like the
        // PS3 states (free end near x = 5, velocities ~10). The strand springs start far from rest, so the first
        // ~20000 steps oscillate wildly; 36000+ steps match the tables statistically (checked in numpy).
        const double L = p.P("LINE1.LENGTH");
        for (int j = 0; j < NX; j++)
            for (int i = 0; i < NX; i++) {
                const int o = (j * NX + i) * 4;
                cur_[o] = (NX - 1 - i) * L; cur_[o + 1] = 0.2; cur_[o + 2] = 0.2 + 0.01 * j; cur_[o + 3] = 1;
                vel_[o] = vel_[o + 1] = vel_[o + 2] = vel_[o + 3] = 0;
            }
        rngN_ = 0; lineTime_ = 0; tsm_ = 0;
        const Prm q = prm(p);
        for (int k = 0; k < 36000 + 3000 * n; k++) step(q);
    }
    prev_ = cur_;
    pos_ = cur_;
    lineTime_ = 0; tsm_ = 0; acc_ = 0; steps_ = 0; rngN_ = 0; ffdTime_ = 0;
    // reset 0x56fe0: the FFD time wrap (0x47af0), then the loader clears the fade (0x4e624)
    ffdWrap();
    ffdW_ = 0;
    ffdFlag_ = false;
}

double Wave::rnd() {   // integer noise on the counter at Lines+0x2d4
    rngN_ = int(uint32_t(rngN_) + 1u);
    uint32_t n = uint32_t(rngN_);
    n = (n << 13) ^ n;
    const uint32_t v = (n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
    return 1 - double(v) / 1073741824.0;
}

void Wave::spring(int a, int b, double rest, double k) {
    const int ax = a * 4, bx = b * 4;
    const double dx = cur_[bx] - cur_[ax], dy = cur_[bx + 1] - cur_[ax + 1], dz = cur_[bx + 2] - cur_[ax + 2];
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-12) return;
    const double f = (len - rest) * k / len;
    vel_[ax] += dx * f; vel_[ax + 1] += dy * f; vel_[ax + 2] += dz * f;
    vel_[bx] -= dx * f; vel_[bx + 1] -= dy * f; vel_[bx + 2] -= dz * f;
}

void Wave::step(const Prm &q) {
    prev_ = cur_;
    const double dt = q.timestep * 1e-4;
    lineTime_ += dt;
    if (lineTime_ > 10) { lineTime_ = 0; ffdWrap(); }
    const double k = q.tension, L = q.length, kb = 10 * k;
    for (int j = 0; j < NX; j++)
        for (int i = 0; i < NX; i++) {
            const int idx = j * NX + i;
            if (i > 0) spring(idx, idx - 1, L, k);
            if (i > 1) spring(idx, idx - 2, 2 * L, kb);
            if (j > 0) spring(idx, idx - NX, L, k);
            if (j > 1) spring(idx, idx - 2 * NX, 2 * L, kb);
            const int o = idx * 4;
            vel_[o] += rnd() * q.perturbation;
            vel_[o + 1] += rnd() * q.perturbation;
            vel_[o + 2] += rnd() * q.perturbation;
        }
    const double damp = q.damping * q.timestep;
    for (int i = 0; i < NN * 4; i += 4)
        for (int c = 0; c < 3; c++) {
            cur_[i + c] += vel_[i + c] * dt;
            vel_[i + c] -= vel_[i + c] * damp;
        }
    tsm_ += (lineTime_ - tsm_) * 0.1;   // constraints (0x4bce0)
    for (int j = 0; j < NX; j++) {
        vel_[(j * NX) * 4] += q.timestep * 0.02;
        const double s = double(j) / NX + tsm_;
        const int o = (j * NX + 18) * 4;
        cur_[o] = 0;
        cur_[o + 1] = (std::sin(11 * s) * 0.5 + 0.5) * q.endY;
        cur_[o + 2] = (std::cos(15 * s) + 1) * 0.5 * q.endZ;
        cur_[o + 3] = 1;
        vel_[o] = vel_[o + 1] = vel_[o + 2] = vel_[o + 3] = 0;
    }
    steps_++;
}

// ------------------------------------------------------------------ FFD
namespace {
// ffd_shader0..3.fpo per sample; u, v, w = _UVW, T = _Time
void ffdEval(int n, double u, double v, double w, double T, double &dx, double &dy) {
    auto f = [](double x) { return double(float(x)); };   // fp32 intermediates the ucode rounds
    switch (n) {
    case 0: {
        const double A = f(T + 2400), h = f(A * 0.5), t1 = f(A * 0.1), t13 = f(A * 0.129999995);
        dx = 0.2 * u + 1.30999994 * u * u;
        dy = (0.5 - v) * (1 - std::exp(-2 * u)) * std::sin(h * (1 - 0.800000012 * u) - 6.28000021 * u) +
             std::exp(-50 * std::pow(u - 0.833333313 * (0.2 + std::sin(t1)), 2)) -
             0.15 * std::exp(-10 * std::pow(u - 0.833333313 * (0.2 + std::sin(t1 + 20)), 2)) +
             std::sin(15.7000008 * u - t13 - 2) / 4;
        break;
    }
    case 2:
        dx = 0.2 * u + 1.3 * u * u;
        dy = 0.24 * (1 + std::tanh(u - 0.5)) * (3 + std::sin(2 * T)) * std::exp(-T / 10000) * std::sin(7.85 * u - 2.5 * T - 1.25);
        break;
    case 3:
        dx = 1.20000005 * u * u;
        dy = (1 - 2 * v) * (1 - std::exp(-2 * u)) * std::sin((T / 2) * (1 - 0.8 * u) - 6.28318548 * u) +
             std::exp(-50 * std::pow(u - std::sin(T / 10), 2)) - 0.15 * std::exp(-10 * std::pow(u - std::sin(T / 10 + 20), 2));
        break;
    default:   // 1, the shipping shader (SPEC section 4)
        dx = 0.2 * u + 1.3 * u * u - 0.15;
        dy = 0.24 * (1 + std::tanh(u - 0.5)) * (3 + std::sin(2 * T)) * std::exp(-T / 10000) * std::sin(7.85 * u - 2.5 * T - 1.25) +
             std::exp(-50 * std::pow(u - (5.0 / 6.0) * (0.2 + std::sin(T / 10)), 2)) + std::sin(T / 4) / 2 + std::sin(T - 6.28 * w) / 8;
    }
}

void ffdPass(int n, double T, std::array<double, 256> &out) {   // the 128x1 FFD render target
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 8; i++) {
                const int s = (k * 4 + j) * 8 + i;
                ffdEval(n, i / 8.0, j / 4.0, k / 4.0, T, out[size_t(s) * 2], out[size_t(s) * 2 + 1]);
            }
}
}  // namespace

// 0x47af0, on a line_time wrap and on reset: the same shader keeps running at T ~ 100 while it fades out
void Wave::ffdWrap() {
    ffdTold_ = ffdTcur_;
    ffdFlag_ = false;
    ffdTcur_ = 0;
    ffdW_ = 1;
    ffdPrev_ = ffdCur_;
}

// 0x480ec, when FFD SHADER changes: fade from the old shader (or a frozen snapshot if a fade is running)
void Wave::ffdSelect(int n) {
    if (ffdW_ >= 0.01) { std::swap(ffdOut_, ffdFrozen_); ffdFlag_ = true; }
    else ffdPrev_ = ffdCur_;
    ffdCur_ = (n < 0 || n > 3) ? 3 : n;   // min((unsigned)n, 3)
    ffdTold_ = ffdTcur_;
    ffdW_ = 1;
}

// 0x4a220 per frame: ffd[cur] at Tcur blended over ffd[prev] at Told (or the frozen image), 200-frame smoothstep
void Wave::ffdSamples(double T, const double S1[3], const double OFF[3]) {
    ffdTold_ += T - ffdTcur_;
    ffdTcur_ = T;
    std::array<double, 256> A;
    ffdPass(ffdCur_, ffdTcur_, A);
    const double a = ffdW_ * ffdW_ * (3 - 2 * ffdW_);
    if (ffdW_ > 0) {
        if (!ffdFlag_) ffdPass(ffdPrev_, ffdTold_, ffdB_);
        ffdW_ -= 0.005;
        if (ffdW_ <= 0) { ffdW_ = 0; ffdFlag_ = false; }
    }
    const auto &old = ffdFlag_ ? ffdFrozen_ : ffdB_;
    for (size_t q = 0; q < 256; q++) ffdOut_[q] = A[q] + (old[q] - A[q]) * a;   // ffd_alpha_blend.fpo
    for (int s = 0; s < 128; s++) {
        samp_[size_t(s) * 4] = ffdOut_[size_t(s) * 2] * S1[0] + OFF[0];
        samp_[size_t(s) * 4 + 1] = ffdOut_[size_t(s) * 2 + 1] * S1[1] + OFF[1];
        samp_[size_t(s) * 4 + 2] = OFF[2];
        samp_[size_t(s) * 4 + 3] = 1;
    }
    auto cl = [](int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; };
    for (int k = -1; k < 6; k++)
        for (int j = -1; j < 6; j++)
            for (int i = -1; i < 10; i++) {
                const int d = ((k + 1) * 77 + (j + 1) * 11 + (i + 1)) * 4, s = ((cl(k, 3) * 4 + cl(j, 3)) * 8 + cl(i, 7)) * 4;
                for (int c = 0; c < 4; c++) lat_[d + c] = samp_[s + c];
            }
}

void Wave::ffdPoint(double x, double y, double z, const double S1[3], const double S2[3], const double OFF[3], double out[3]) const {
    const double lx = (x - OFF[0]) / S1[0], ly = (y - OFF[1]) / S1[1], lz = (z - OFF[2]) / S1[2];
    auto c = [](double v) { return v < 0 ? 0 : v > 0.999 ? 0.999 : v; };
    const double cx = c(lx) * 8, cy = c(ly) * 4, cz = c(lz) * 4;
    const int ix = int(std::floor(cx)), iy = int(std::floor(cy)), iz = int(std::floor(cz));
    double bx[4], by[4], bz[4];
    bsp(cx - ix, bx); bsp(cy - iy, by); bsp(cz - iz, bz);
    double sx = 0, sy = 0, sz = 0;
    for (int kz = 0; kz < 4; kz++)
        for (int ky = 0; ky < 4; ky++) {
            const double wyz = by[ky] * bz[kz];
            const int rowBase = ((iz + kz) * 77 + (iy + ky) * 11 + ix) * 4;
            for (int kx = 0; kx < 4; kx++) {
                const double w = bx[kx] * wyz;
                const int li = rowBase + kx * 4;
                sx += w * lat_[li]; sy += w * lat_[li + 1]; sz += w * lat_[li + 2];
            }
        }
    out[0] = (lx + sx) * S2[0];
    out[1] = (ly + sy) * S2[1];
    out[2] = (lz + sz) * S2[2];
}

// ------------------------------------------------------------------ per frame
void Wave::simulate(double dtFrame, const Params &p) {
    const Prm q = prm(p);
    if (!p.S.paused) {
        acc_ += dtFrame * 60 * p.S.simSpeed;
        int guard = 0;
        while (acc_ > 1 && guard++ < 2400) { step(q); acc_ -= 1; }
        if (acc_ > 1) acc_ = 1;
    }
    buildCtrl(p);
}

void Wave::stepOnce(const Params &p) {
    const Prm q = prm(p);
    acc_ += 60;
    int guard = 0;
    while (acc_ > 1 && guard++ < 2400) { step(q); acc_ -= 1; }
    buildCtrl(p);
}

void Wave::buildCtrl(const Params &p) {
    const double x0 = p.P("LINE1.FFD PARAM 1");
    for (int i = 0; i < NN; i++) {
        const int o = i * 4;
        const double px = prev_[o] + (cur_[o] - prev_[o]) * acc_, py = prev_[o + 1] + (cur_[o + 1] - prev_[o + 1]) * acc_,
                     pz = prev_[o + 2] + (cur_[o + 2] - prev_[o + 2]) * acc_;
        const double d = px - x0;
        double s = 0.3;
        if (d >= 0) {
            const double qq = std::min(d / 5, 1.0);
            s = 1.3 - std::cos(d * PI / 2) * (1 - qq * qq * (3 - 2 * qq));
        }
        pos_[o] = px; pos_[o + 1] = py * s; pos_[o + 2] = pz * s; pos_[o + 3] = 1;
    }
    const double S1[3] = {p.P("LINE1.FFD SCALE1 X"), p.P("LINE1.FFD SCALE1 Y"), p.P("LINE1.FFD SCALE1 Z")};
    const double S2[3] = {p.P("LINE1.FFD SCALE2 X"), p.P("LINE1.FFD SCALE2 Y"), p.P("LINE1.FFD SCALE2 Z")};
    const double OFF[3] = {p.P("LINE1.FFD OFFSET X"), p.P("LINE1.FFD OFFSET Y"), p.P("LINE1.FFD OFFSET Z")};
    // FFD SHADER: the LINE1 apply calls 0x480ec only when the value changes (the menu clamps it to 0..100)
    const int sel = int(std::lround(std::clamp(p.P("LINE1.FFD SHADER"), 0.0, 100.0)));
    if (ffdSel_ < 0) { ffdCur_ = ffdPrev_ = std::min(sel, 3); ffdSel_ = sel; }
    else if (sel != ffdSel_) { ffdSel_ = sel; ffdSelect(sel); }
    // The FFD pass runs on the GPU and is read back a frame later: this frame deforms with last frame's _Time.
    ffdSamples(ffdTime_, S1, OFF);
    ffdTime_ = lineTime_ * 10;

    double ax = p.P("LINE1.ANG X"), ay = p.P("LINE1.ANG Y"), az = p.P("LINE1.ANG Z");
    double al = std::sqrt(ax * ax + ay * ay + az * az);
    if (al == 0) al = 1;
    ax /= al; ay /= al; az /= al;
    const double t = p.P("LINE1.ANG ROT") * PI / 180, cs = std::cos(t), sn = std::sin(t), ic = 1 - cs;
    const Mat4 model{ax * ax * ic + cs, ay * ax * ic + az * sn, az * ax * ic - ay * sn, 0,
                     ax * ay * ic - az * sn, ay * ay * ic + cs, az * ay * ic + ax * sn, 0,
                     ax * az * ic + ay * sn, ay * az * ic - ax * sn, az * az * ic + cs, 0,
                     p.P("LINE1.POS X"), p.P("LINE1.POS Y"), p.P("LINE1.POS Z"), 1};
    mvp_ = mul(projection(p.firmware), mul(view(), model));
    const Mat4 &M = mvp_;
    auto xform = [&](double x, double y, double z, float *out) {
        out[0] = float(M[0] * x + M[4] * y + M[8] * z + M[12]);
        out[1] = float(M[1] * x + M[5] * y + M[9] * z + M[13]);
        out[2] = float(M[2] * x + M[6] * y + M[10] * z + M[14]);
        out[3] = float(M[3] * x + M[7] * y + M[11] * z + M[15]);
    };
    double v[3];
    for (int i = 0; i < NN; i++) {
        ffdPoint(pos_[i * 4], pos_[i * 4 + 1], pos_[i * 4 + 2], S1, S2, OFF, v);
        xform(v[0], v[1], v[2], &ctrl[size_t(i) * 4]);
    }
    if (p.S.showFfd)
        for (int n = 0; n < 128; n++) {
            const int i = n & 7, j = (n >> 3) & 3, k = n >> 5, o = n * 4;
            xform((i / 8.0 + samp_[o]) * S2[0], (j / 4.0 + samp_[o + 1]) * S2[1], (k / 4.0 + samp_[o + 2]) * S2[2], &ffdLattice[size_t(o)]);
        }
}

std::array<double, 3> Wave::surfaceWorld(int i, int j) const {
    static const Mat4 invPV = inverse(mul(projection(), view()));
    const int pi = i >> 3, pj = j >> 3;
    double bu[4], bv[4];
    bsp((i & 7) / 8.0, bu);
    bsp((j & 7) / 8.0, bv);
    double x = 0, y = 0, z = 0, w = 0;
    for (int r = 0; r < 4; r++)
        for (int q = 0; q < 4; q++) {
            const double k = bv[r] * bu[q];
            const int o = ((pj + r) * NX + (pi + q)) * 4;
            x += k * ctrl[o]; y += k * ctrl[o + 1]; z += k * ctrl[o + 2]; w += k * ctrl[o + 3];
        }
    const Mat4 &M = invPV;
    return {M[0] * x + M[4] * y + M[8] * z + M[12] * w, M[1] * x + M[5] * y + M[9] * z + M[13] * w,
            M[2] * x + M[6] * y + M[10] * z + M[14] * w};
}

}  // namespace moyou::sim
