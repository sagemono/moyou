#include "app/waveview.h"

#include <QMouseEvent>

#include <algorithm>

namespace moyou::app {

WaveView::WaveView(sim::Params &params, QWidget *parent) : QOpenGLWidget(parent), p_(params) {
    setMinimumSize(320, 180);
    // vsync-paced: schedule the next frame as soon as this one is on screen
    connect(this, &QOpenGLWidget::frameSwapped, this, qOverload<>(&QWidget::update));
}

WaveView::~WaveView() {
    // QOpenGLWidget destroys the context after this body and the members have gone; the aboutToBeDestroyed handler
    // from initializeGL must not run then, or releaseAll() writes into destroyed members.
    if (context()) disconnect(context(), &QOpenGLContext::aboutToBeDestroyed, this, nullptr);
    makeCurrent();
    releaseAll();
    doneCurrent();
}

void WaveView::initializeGL() {
    initializeOpenGLFunctions();
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] { makeCurrent(); releaseAll(); doneCurrent(); });
    clock_.start();
}

void WaveView::setAssets(render::Assets assets) {
    pending_ = std::move(assets);
    broken_ = false;
    update();
}

void WaveView::setRopeStates(sim::RopeStates states) {
    wave_.setStates(std::move(states));
    resetRope();
}

void WaveView::resetRope() { wave_.loadState(std::clamp(p_.S.state, 0, 2), p_); }
void WaveView::stepRope() { wave_.stepOnce(p_); }
void WaveView::restartParticles() { if (ps_) ps_->reset(0); }

void WaveView::releaseAll() {
    if (!isValid()) return;
    if (ps_) ps_->releaseGL(*this);
    ps_.reset();
    renderer_.release(*this);   // also after an init that threw halfway: whatever it created so far
}

void WaveView::rebuild() {
    render::Assets as = std::move(*pending_);
    pending_.reset();
    if (const QString e = initRenderer(std::move(as), false); !e.isEmpty()) {
        broken_ = true;
        emit failed(e);
    }
}

QString WaveView::replaceAssets(render::Assets as) {
    if (!isValid() || !current_) { setAssets(std::move(as)); return {}; }
    makeCurrent();
    std::optional<render::Assets> prev = current_;
    QString e = initRenderer(std::move(as), true);
    if (!e.isEmpty() && prev) initRenderer(std::move(*prev), true);
    doneCurrent();
    update();
    return e;
}

QString WaveView::initRenderer(render::Assets as, bool keepRope) {
    releaseAll();
    try {
        renderer_.init(*this, as);
        if (as.hasParticles()) {   // firmware 1.00 has no particle system
            particles::Assets pa;
            pa.textureRGBA = as.particleTexture.pixels;
            pa.textureW = as.particleTexture.width;
            pa.textureH = as.particleTexture.height;
            pa.surfaceSampler = [this](int i, int j) { return wave_.surfaceWorld(i, j); };
            ps_ = std::make_unique<particles::ParticleSystem>(std::move(pa));
            ps_->initGL(*this, as.particleShaders);
        }
        if (!keepRope) resetRope();
        current_ = std::move(as);
        broken_ = false;
        return {};
    } catch (const std::exception &e) {
        releaseAll();
        return QString::fromStdString(e.what());
    }
}

void WaveView::paintGL() {
    if (pending_) rebuild();
    const double dt = std::min(0.25, std::max(0.0, clock_.nsecsElapsed() / 1e9));
    clock_.restart();
    const int w = int(width() * devicePixelRatioF()), h = int(height() * devicePixelRatioF());
    if (!renderer_.ready() || broken_) {
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        glViewport(0, 0, w, h);
        glClearColor(0.02f, 0.024f, 0.028f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    p_.advance(dt);
    wave_.simulate(dt, p_);
    renderer_.render(*this, dt, p_, wave_, ps_.get(), defaultFramebufferObject(), w, h);
    fpsAcc_ += dt;
    if (++fpsN_ >= 30) { fps_ = fpsN_ / std::max(1e-6, fpsAcc_); fpsAcc_ = 0; fpsN_ = 0; }
}

QImage WaveView::grabDisplay() {
    if (!renderer_.ready()) return {};
    makeCurrent();
    QImage img = renderer_.grab(*this);
    doneCurrent();
    return img;
}

void WaveView::mouseDoubleClickEvent(QMouseEvent *e) {
    emit doubleClicked();
    e->accept();
}

}  // namespace moyou::app
