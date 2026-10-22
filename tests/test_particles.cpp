// Particle system against the RPCS3 guest memory dump (C++ counterpart of the core checks of
// tools/validate_particles.py): one SPU step from particle buffer A must reproduce buffer B, the free list,
// the brownian LCG state and the vertex buffer the SPU wrote. Parameters are the time-of-day blended PARTICLES
// presets, parsed at test time from the research checkout (nothing firmware-derived is stored here).
//
// usage: test_particles <research_dir>      exit 0 = all pass, 1 = failure, 77 = dump not available (skipped)
#include "particles/particles.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using moyou::particles::ParticleSystem;

namespace {

// Dump addresses (docs/PARTICLES.md, tools/validate_particles.py)
constexpr uint64_t PS = 0x200ded00;                      // particle system object (0x60b14)
constexpr uint64_t BUF_A = 0x2046a900, BUF_B = 0x20482980;
constexpr uint64_t FREE_LIST = 0x2049aa00;
constexpr uint64_t VB_B = 0x204b1780;
constexpr int CAP = ParticleSystem::capacity;

int failures = 0;
void report(const std::string &name, bool ok, const std::string &detail) {
    std::printf("[%s] %s: %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    if (!ok) failures++;
}
template <class... A>
std::string fmt(const char *f, A... a) {
    char buf[1024];
    std::snprintf(buf, sizeof buf, f, a...);
    return buf;
}

// rpcs3-guest-memory-dump: guest_memory.bin, file offset == guest address, big-endian
struct Mem {
    std::ifstream f;
    explicit Mem(const fs::path &dir) : f(dir / "guest_memory.bin", std::ios::binary) {}
    std::vector<uint8_t> read(uint64_t a, size_t n) {
        std::vector<uint8_t> b(n);
        f.seekg(std::streamoff(a));
        f.read(reinterpret_cast<char *>(b.data()), std::streamsize(n));
        if (!f) throw std::runtime_error(fmt("dump read failed at %#llx", (unsigned long long)a));
        return b;
    }
    static uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    uint32_t u32(uint64_t a) { auto b = read(a, 4); return be32(b.data()); }
    float f32(uint64_t a) { return std::bit_cast<float>(u32(a)); }
    std::vector<float> floats(uint64_t a, size_t n) {
        auto b = read(a, n * 4);
        std::vector<float> v(n);
        for (size_t i = 0; i < n; i++) v[i] = std::bit_cast<float>(be32(&b[i * 4]));
        return v;
    }
};

using Preset = std::map<std::string, double>;
Preset readMnu(const fs::path &p) {   // "name:type:value" lines (validate_particles.read_mnu)
    std::ifstream in(p);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    Preset d;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        size_t s = 0;
        while (s < line.size() && std::isspace(static_cast<unsigned char>(line[s]))) s++;
        line = line.substr(s);
        std::vector<std::string> parts;
        std::stringstream ss(line);
        for (std::string t; std::getline(ss, t, ':');) parts.push_back(t);
        if (!line.empty() && line.back() == ':') parts.push_back("");
        if (parts.size() == 3) d[parts[0]] = std::stod(parts[2]);
    }
    return d;
}

// ps+0x1360 word index of each menu value (ParticlesMenu apply 0x329a4); -1 = not stored as such
const std::pair<const char *, int> PIDX[] = {
    {"emit vel min", 0}, {"emit vel mul", 1}, {"emit vel var", 2}, {"emit cone angle", 3}, {"emit neg prob", 4},
    {"emit vel zscale", 5}, {"emit per frame", 6}, {"emit prob", 7}, {"aging speed", 8}, {"aging variance", 9},
    {"friction", 0xc}, {"spin time scale", 0xd}, {"delta time", 0xe}, {"gravity", 0xf}, {"wind dir x", 0x10},
    {"wind dir y", 0x11}, {"wind dir z", 0x12}, {"wind scale", -1}, {"wind scale 10", -1}, {"brownian scale", 0x15},
    {"spot pos x", 0x18}, {"spot pos y", 0x19}, {"spot pos z", 0x1a}, {"spot attn x", 0x1c}, {"spot attn y", 0x1d},
    {"spot attn z", 0x1e}, {"specular power", 0x20}, {"specular coeff", 0x21}, {"lambert coeff", 0x22},
    {"exposure", 0x23}, {"fresnel", 0x24}, {"color_control", 0x25}, {"iridescent exp", 0x26}, {"global alpha", 0x27},
    {"size middle", 0x28}, {"size near", 0x29}, {"size far", 0x2a}, {"near focus", 0x2b}, {"near focus_dist", 0x2c},
    {"near focus_pow", 0x2d}, {"near darkness", 0x2e}, {"near fuzziness", 0x2f}, {"far focus", 0x30},
    {"far focus_dist", 0x31}, {"far focus_pow", 0x32}, {"far darkness", 0x33}, {"near align", 0x34},
    {"size align", 0x35}, {"glare", 0x36}, {"glare scale", 0x37}, {"glare p1", 0x38}, {"glare p2", 0x39}};

uint32_t bits(float x) { return std::bit_cast<uint32_t>(x); }

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_particles <research_dir>\n");
        return 2;
    }
    const fs::path root = argv[1];
    const fs::path dump = root / "work" / "dumps" / "RPCS3_guest_memory_20261018_134839_488";
    if (!fs::exists(dump / "guest_memory.bin")) {
        std::printf("skipped: %s not found\n", dump.string().c_str());
        return 77;
    }
    try {
        Mem m(dump);

        // ---- parameters: day -> higure blend, weight solved from 'far focus' in the dump block (as the Python)
        const fs::path lines = root / "work" / "lines";
        std::map<std::string, Preset> pre;
        pre["base"] = readMnu(lines / "PARTICLES.mnu");
        for (const char *k : {"night", "yoake", "day", "higure"}) pre[k] = readMnu(lines / "override" / k / "PARTICLES.mnu");
        std::map<std::string, double> blk;
        for (auto [nm, i] : PIDX)
            if (i >= 0) blk[nm] = (std::string(nm) == "emit per frame") ? double(m.u32(PS + 0x1360 + 4 * i))
                                                                         : double(m.f32(PS + 0x1360 + 4 * i));
        const Preset &day = pre["day"], &hig = pre["higure"];
        const double w = (blk["far focus"] - day.at("far focus")) / (hig.at("far focus") - day.at("far focus"));
        Preset bl;   // f32(a + (b - a) * w)
        for (auto &[k, a] : day) bl[k] = float(a + (hig.at(k) - a) * w);
        double worst = 0;
        for (auto &[k, v] : blk) {
            const double ref = (k == "emit per frame") ? std::trunc(bl.at(k)) : bl.at(k);
            worst = std::max(worst, std::abs(v - ref));
        }
        report("dump parameter block ps+0x1360", worst < 1e-5,
               fmt("== day->higure blend at w=%.5f (max diff %.2g; 'emit per frame' stored as int %d)", w, worst,
                   int(blk["emit per frame"])));

        // ---- SPU parameter block (ps+0x100..) vs the values the port derives from the blend
        auto eqf = [](float a, double b) { return bits(a) == bits(float(b)); };
        const float brownDump = m.f32(PS + 0x9a4);
        bool spuOk = m.f32(PS + 0x100) == 0 && eqf(m.f32(PS + 0x104), bl["gravity"]) && m.f32(PS + 0x108) == 0;
        for (int k = 0; k < 3; k++) {
            spuOk = spuOk && eqf(m.f32(PS + 0x110 + 4 * k), bl["friction"]) && eqf(m.f32(PS + 0x9b0 + 4 * k), bl["delta time"]);
            spuOk = spuOk && m.f32(PS + 0x990 + 4 * k) == 0;                       // wind scale = wind scale 10 = 0 -> W = 0
        }
        spuOk = spuOk && eqf(m.f32(PS + 0x9ac), bl["spin time scale"]) && bl["wind scale"] == 0 && bl["wind scale 10"] == 0;
        const float lbMin[3] = {-10, -10, -12}, lbMax[3] = {10, 10, 7};
        for (int k = 0; k < 3; k++) spuOk = spuOk && m.f32(PS + 0x900 + 4 * k) == lbMin[k] && m.f32(PS + 0x910 + 4 * k) == lbMax[k];
        report("SPU parameter block ps+0x100", spuOk,
               fmt("gravity, friction, dt, spin, W = 0, LifeBounds == blended presets bit-exact; brownian %.9g = "
                   "'brownian scale' %.9g + interaction residue %.3g (layer+0x1d4/0x604, not modelled: the step uses "
                   "the dump value)", brownDump, float(bl["brownian scale"]), double(brownDump) - float(bl["brownian scale"])));

        Preset stepP = bl;
        stepP["brownian scale"] = brownDump;
        const moyou::particles::ParamFn P = [&stepP](const std::string &nm) {
            auto it = stepP.find(nm);
            return it == stepP.end() ? NAN : it->second;
        };
        const moyou::particles::Camera cam;   // unused by an SPU step without emission

        // ---- one SPU step A -> B
        const std::vector<float> A = m.floats(BUF_A, CAP * 12), B = m.floats(BUF_B, CAP * 12);
        int deadA = 0, aliveA = 0;
        for (int i = 0; i < CAP; i++) (A[i * 12 + 3] == -666.0f ? deadA : aliveA)++;
        const uint32_t dumpFreeCount = m.u32(PS + 0x1314);
        const std::vector<uint8_t> flRaw = m.read(FREE_LIST, 4 * size_t(dumpFreeCount));
        std::vector<int32_t> dumpFree(dumpFreeCount);
        for (uint32_t i = 0; i < dumpFreeCount; i++) dumpFree[i] = int32_t(Mem::be32(&flRaw[i * 4]));

        auto runStep = [&](bool exact) {
            moyou::particles::Assets as;
            as.exactFloat = exact;
            auto ps = std::make_unique<ParticleSystem>(as);
            ps->slotState() = A;
            for (int i = 0; i < deadA; i++) ps->freeList()[i] = dumpFree[i];
            ps->freeCount() = deadA;
            ps->stepOnce(P, cam, false);
            return ps;
        };
        auto ps = runStep(true);
        const std::vector<float> &S = ps->slotState();
        std::vector<int> killed;
        for (int i = 0; i < CAP; i++)
            if (A[i * 12 + 3] != -666.0f && S[i * 12 + 3] == -666.0f) killed.push_back(i);
        int colExact[12] = {};
        double qErr = 0;
        for (int i = 0; i < CAP; i++)
            for (int c = 0; c < 12; c++) {
                const float s = S[i * 12 + c], b = B[i * 12 + c];
                colExact[c] += bits(s) == bits(b);
                if (c >= 8) qErr = std::max(qErr, std::abs(double(s) - double(b)));
            }
        bool pvExact = true;
        std::string cols;
        for (int c = 0; c < 8; c++) {
            pvExact = pvExact && colExact[c] == CAP;
            cols += fmt("%s%.4f", c ? ", " : "", colExact[c] / double(CAP));
        }
        std::string kl;
        for (int k : killed) kl += fmt("%s%d", kl.empty() ? "" : ", ", k);
        report("SPU update A->B: pos/age/vel/rate", pvExact,
               fmt("bit-exact fraction per column [%s]; killed [%s]", cols.c_str(), kl.c_str()));
        const double qExact = (colExact[8] + colExact[9] + colExact[10] + colExact[11]) / (4.0 * CAP);
        report("SPU update A->B: quaternion", qErr < 3e-7,
               fmt("max abs err %.3g, bit-exact %.3f (frsqest/fi estimate not modelled bit-exactly)", qErr, qExact));

        // ---- free list
        bool flOk = ps->freeCount() == int(dumpFreeCount) && int(dumpFreeCount) == deadA + int(killed.size());
        for (int i = 0; flOk && i < ps->freeCount(); i++) flOk = ps->freeList()[i] == dumpFree[i];
        report("free list", flOk,
               fmt("count %d (dump %u) = %d old + %zu killed this frame, appended in slot order", ps->freeCount(),
                   dumpFreeCount, deadA, killed.size()));

        // ---- brownian LCG: state after the frame == SPU LS 0xbb20 of the particles task
        {
            std::ifstream ls(dump / "spu_02000002_ls.bin", std::ios::binary);
            uint8_t raw[12] = {};
            ls.seekg(0xbb20);
            ls.read(reinterpret_cast<char *>(raw), 12);
            const auto st = ps->lcgState();
            bool ok = bool(ls);
            std::string a, b;
            for (int k = 0; k < 3; k++) {
                const uint32_t saved = Mem::be32(raw + 4 * k);
                ok = ok && st[k] == saved;
                a += fmt("%s%#x", k ? ", " : "", st[k]);
                b += fmt("%s%#x", k ? ", " : "", saved);
            }
            report("brownian LCG", ok,
                   fmt("seeds after the frame [%s] == SPU LS 0xbb20 [%s] (advanced %d times = live particles in A)",
                       a.c_str(), b.c_str(), aliveA));
        }

        // ---- vertex buffer written by the step (32-byte records: pos.xyz fade | 4 x half quat | 4 x half attr10)
        {
            const auto vd = ps->vertexData();
            int aliveB = 0;
            for (int i = 0; i < CAP; i++) aliveB += B[i * 12 + 3] != -666.0f;
            const auto raw = m.read(VB_B, size_t(vd.count) * 32);
            int posEq = 0, qEq = 0;
            bool z0 = true;
            for (int i = 0; i < vd.count; i++) {
                const uint8_t *r = &raw[size_t(i) * 32];
                for (int k = 0; k < 4; k++) {
                    posEq += Mem::be32(r + 4 * k) == bits(vd.pos[i * 4 + k]);
                    const uint16_t h = uint16_t(r[16 + 2 * k] << 8 | r[17 + 2 * k]);
                    qEq += h == vd.rotHalf[i * 4 + k];
                    z0 = z0 && r[24 + 2 * k] == 0 && r[25 + 2 * k] == 0;
                }
            }
            const int n = vd.count * 4;
            report("vertex buffer written by the step (VB B)",
                   vd.count == aliveB && vd.count == ps->count() && posEq == n && qEq == n && z0,
                   fmt("%d vertices @%#llx (live in B: %d): pos+fade bit-exact %d/%d, quat halves exact %d/%d, attr10 all "
                       "zero %s", vd.count, (unsigned long long)VB_B, aliveB, posEq, n, qEq, n, z0 ? "True" : "False"));
        }

        // ---- the same step with exactFloat = false (round to nearest): docs/PARTICLES.md quotes errors <= 1e-6
        {
            auto pn = runStep(false);
            double e = 0;
            for (int i = 0; i < CAP; i++)
                if (B[i * 12 + 3] != -666.0f)
                    for (int c = 0; c < 12; c++)
                        e = std::max(e, std::abs(double(pn->slotState()[i * 12 + c]) - double(B[i * 12 + c])));
            report("SPU update A->B, exactFloat=false", e <= 1e-6, fmt("max abs err over live slots %.3g", e));
        }

        // ---- emission smoke test (stand-in surface): determinism, bookkeeping, finite vertex data
        {
            moyou::particles::Camera c;
            auto run = [&](uint32_t seed) {
                moyou::particles::Assets as;
                as.seed = seed;
                auto s = std::make_unique<ParticleSystem>(as);
                s->prewarm(900, P, c);
                return s;
            };
            auto s1 = run(12345), s2 = run(12345);
            const bool same = s1->slotState() == s2->slotState() && s1->count() == s2->count();
            int live = 0;
            for (int i = 0; i < CAP; i++) live += s1->slotState()[i * 12 + 3] != -666.0f;
            bool finite = true;
            const auto vd = s1->vertexData();
            for (int i = 0; i < vd.count * 4; i++) finite = finite && std::isfinite(vd.pos[i]);
            report("emission (stand-in surface, 900 steps)",
                   same && live > 0 && live == s1->count() && live + s1->freeCount() == CAP && finite,
                   fmt("%d live, %d free, deterministic %s, finite %s", live, s1->freeCount(), same ? "yes" : "no",
                       finite ? "yes" : "no"));
        }
    } catch (const std::exception &e) {
        report("exception", false, e.what());
    }
    std::printf("\n%s\n", failures ? fmt("%d check(s) failed", failures).c_str() : "all checks passed");
    return failures ? 1 : 0;
}
