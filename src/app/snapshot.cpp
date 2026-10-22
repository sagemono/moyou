#include <memory>
#include "app/snapshot.h"

#include "particles/particles.h"
#include "render/renderer.h"
#include "sim/params.h"
#include "sim/wave.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QStringList>

#include <cstdio>

namespace moyou::app {

int snapshot(const QStringList &args) {
    auto opt = [&](const QString &name, const QString &def = {}) {
        const int i = int(args.indexOf(name));
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : def;
    };
    const int at = int(args.indexOf("--snapshot"));
    if (at < 0 || at + 2 >= args.size()) {
        std::fputs("usage: moyou --snapshot lines.qrc out.png [--time HH:MM] [--month M] [--frames N] [--prx file] [--edit KEY=V]\n", stderr);
        return 2;
    }
    const QString qrcPath = args[at + 1], out = args[at + 2];
    try {
        const qrc::Archive archive = qrc::Archive::load(std::filesystem::path(qrcPath.toStdWString()));
        sim::Params p;
        p.load(archive);
        p.S.follow = false;
        p.S.lapse = 0;
        const QStringList hm = opt("--time", "12:00").split(':');
        p.S.dayFrac = (hm.value(0).toDouble() * 60 + hm.value(1).toDouble()) / 1440.0;
        p.S.month = opt("--month", "9.5").toDouble();
        p.clk.bootFade = 0.05;
        p.S.runClock100 = args.contains("--time");   // 1.00 holds 00:00 unless a time is asked for
        for (int i = 0; i + 1 < args.size(); i++)
            if (args[i] == "--edit") {
                const QStringList kv = args[i + 1].split('=');
                if (kv.size() == 2) p.edits[kv[0].trimmed().toStdString()] = kv[1].toDouble();
            }
        const render::Assets assets = render::loadAssets(archive);

        QOpenGLContext ctx;
        ctx.setFormat(QSurfaceFormat::defaultFormat());
        if (!ctx.create()) throw std::runtime_error("no OpenGL 4.3 core context");
        QOffscreenSurface surf;
        surf.setFormat(ctx.format());
        surf.create();
        if (!ctx.makeCurrent(&surf)) throw std::runtime_error("cannot make the context current");
        QOpenGLExtraFunctions &f = *ctx.extraFunctions();
        GLuint fbo = 0, tex = 0;
        f.glGenTextures(1, &tex);
        f.glBindTexture(GL_TEXTURE_2D, tex);
        f.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 960, 540, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        f.glGenFramebuffers(1, &fbo);
        f.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        f.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

        render::Renderer r;
        r.init(f, assets);
        sim::Wave wave;
        if (const QString prx = opt("--prx"); !prx.isEmpty()) {
            std::string err;
            sim::RopeStates st = sim::RopeStates::fromPrx(std::filesystem::path(prx.toStdWString()), &err);
            if (!st.valid()) throw std::runtime_error(err);
            wave.setStates(std::move(st));
        }
        wave.loadState(1, p);
        std::unique_ptr<particles::ParticleSystem> ps;
        if (assets.hasParticles()) {   // firmware 1.00 has no particle system
            particles::Assets pa;
            pa.textureRGBA = assets.particleTexture.pixels;
            pa.textureW = assets.particleTexture.width;
            pa.textureH = assets.particleTexture.height;
            pa.surfaceSampler = [&wave](int i, int j) { return wave.surfaceWorld(i, j); };
            ps = std::make_unique<particles::ParticleSystem>(std::move(pa));
            ps->initGL(f, assets.particleShaders);
        }

        const int frames = opt("--frames", "300").toInt();
        const double dt = 1.0 / 60;
        p.S.particlesOn = !args.contains("--no-particles");
        p.forceTick();
        for (int i = 0; i < frames; i++) {
            p.advance(dt);
            wave.simulate(dt, p);
            r.render(f, dt, p, wave, ps.get(), fbo, 960, 540);
        }
        f.glFinish();
        const QImage img = r.grab(f);
        if (ps) ps->releaseGL(f);
        r.release(f);
        if (!img.save(out)) throw std::runtime_error("cannot write " + out.toStdString());
        std::printf("wrote %s (%d frames, %d particles, rope %s)\nrope: steps %d lineTime %.6f ctrl[0] %.6f ctrl[100] %.6f\n",
                    qPrintable(out), frames, ps ? ps->count() : 0, wave.exactStates() ? "PS3 tables" : "settled", wave.steps(),
                    wave.lineTime(), double(wave.ctrl[0]), double(wave.ctrl[100]));
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "snapshot failed: %s\n", e.what());
        return 1;
    }
}

}  // namespace moyou::app
