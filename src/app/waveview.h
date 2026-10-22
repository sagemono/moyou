#pragma once

#include "particles/particles.h"
#include "render/renderer.h"
#include "sim/params.h"
#include "sim/wave.h"

#include <QElapsedTimer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>

#include <memory>
#include <optional>

namespace moyou::app {

class WaveView : public QOpenGLWidget, protected QOpenGLExtraFunctions {
    Q_OBJECT
public:
    explicit WaveView(sim::Params &params, QWidget *parent = nullptr);
    ~WaveView() override;

    void setAssets(render::Assets assets);          // new lines.qrc: (re)creates GL resources on the next frame
    QString replaceAssets(render::Assets assets);
    void setRopeStates(sim::RopeStates states);
    bool exactRopeStates() const { return wave_.exactStates(); }
    void resetRope();                                // load Params::S.state
    void stepRope();
    void restartParticles();
    QImage grabDisplay();                            // 1920x1080 display buffer
    const sim::Wave &wave() const { return wave_; }
    int particleCount() const { return ps_ ? ps_->count() : 0; }
    double fps() const { return fps_; }

signals:
    void failed(const QString &message);             // shader / GL errors
    void doubleClicked();

protected:
    void initializeGL() override;
    void paintGL() override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

private:
    void rebuild();
    QString initRenderer(render::Assets assets, bool keepRope);
    void releaseAll();

    sim::Params &p_;
    sim::Wave wave_;
    render::Renderer renderer_;
    std::unique_ptr<particles::ParticleSystem> ps_;
    std::optional<render::Assets> pending_;
    std::optional<render::Assets> current_;        // what the renderer was built from (restored after a failed edit)
    QElapsedTimer clock_;
    double fps_ = 0, fpsAcc_ = 0;
    int fpsN_ = 0;
    bool broken_ = false;
};

}  // namespace moyou::app
