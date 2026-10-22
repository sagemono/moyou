#pragma once
// Wave geometry on the CPU: the 19x19 rope simulation (0x4f814 / 0x4bed0), the FFD deformation
// (ffd_shader1.fpo + 0x47b1c + spline.elf func_3c68) and the camera, producing the 19x19 control points in
// clip space that the SPU spline job turns into the 128x128 wave sheet.

#include "sim/params.h"

#include <array>
#include <filesystem>
#include <vector>

namespace moyou::sim {

constexpr int NX = 19, NN = NX * NX;

using Mat4 = std::array<double, 16>;   // column-major

Mat4 mul(const Mat4 &a, const Mat4 &b);
Mat4 inverse(const Mat4 &a);
const Mat4 &projection(int firmware = 493);   // perspective(53 deg, 16:9, 0.1, 1000); 1.00: near 1e-4
const Mat4 &view();                    // translate(0, 0, -2)

// Rope start states from custom_render_plugin (3 x {361 float3 positions, 361 float3 velocities}).
struct RopeStates {
    std::vector<float> data;           // 3 * 361 * 6 floats, or empty
    bool valid() const { return data.size() == 3 * NN * 6; }
    // Reads the tables from a decrypted custom_render_plugin PRX/ELF (vaddr 0x95a40.. in firmware 4.93).
    static RopeStates fromPrx(const std::filesystem::path &path, std::string *error = nullptr);
};

class Wave {
public:
    void setStates(RopeStates s) { states_ = std::move(s); }
    bool exactStates() const { return states_.valid(); }
    void loadState(int n, const Params &p);          // without the plugin tables: a straight rope run ~36000 steps
    void simulate(double dtFrame, const Params &p);  // steps the rope at 60 Hz * simSpeed and rebuilds ctrl
    void stepOnce(const Params &p);                  // one extra frame (Debug > Step)

    std::array<float, NN * 4> ctrl{};                // clip-space control points, row j major
    std::array<float, 128 * 4> ffdLattice{};         // FFD sample lattice in clip space (debug overlay)
    Mat4 mvp() const { return mvp_; }
    int steps() const { return steps_; }
    double lineTime() const { return lineTime_; }
    int rngCounter() const { return rngN_; }

    // World position of wave vertex (i, j) of the 128x128 sheet: the SPU surface point back through inv(P*V).
    std::array<double, 3> surfaceWorld(int i, int j) const;

private:
    struct Prm { double timestep, tension, length, perturbation, damping, endY, endZ; };
    Prm prm(const Params &p) const;
    double rnd();
    void spring(int a, int b, double rest, double k);
    void step(const Prm &prm);
    void ffdWrap();
    void ffdSelect(int n);
    void ffdSamples(double T, const double S1[3], const double OFF[3]);
    void ffdPoint(double x, double y, double z, const double S1[3], const double S2[3], const double OFF[3], double out[3]) const;
    void buildCtrl(const Params &p);

    RopeStates states_;
    std::array<double, NN * 4> prev_{}, cur_{}, vel_{}, pos_{};
    std::array<double, 128 * 4> samp_{};
    std::array<double, 539 * 4> lat_{};
    // FFD object: raw (dx, dy) render targets and the cross-fade state
    std::array<double, 128 * 2> ffdOut_{}, ffdFrozen_{}, ffdB_{};
    int ffdCur_ = 1, ffdPrev_ = 1, ffdSel_ = -1;
    double ffdW_ = 0, ffdTcur_ = 0, ffdTold_ = 0;
    bool ffdFlag_ = false;
    int rngN_ = 0, steps_ = 0;
    double lineTime_ = 0, tsm_ = 0, acc_ = 0, ffdTime_ = 0;
    Mat4 mvp_{};
};

}  // namespace moyou::sim
