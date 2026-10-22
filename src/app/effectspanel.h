#pragma once

#include "formats/qrc.h"
#include "render/effects.h"

#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

class QCheckBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QSlider;
class QTimer;

namespace moyou::app {

class EffectsPanel : public QWidget {
    Q_OBJECT
public:
    using Applier = std::function<QString(const std::string &id, const std::vector<uint8_t> &bytes)>;

    explicit EffectsPanel(QWidget *parent = nullptr);
    void setApplier(Applier a) { apply_ = std::move(a); }
    void setArchive(const qrc::Archive *current, const qrc::Archive *saved, int firmware);   // resets the effects
    std::string currentSource() const;      // the generated assembly (for the Assembly tab)
    void setSettings(const render::EffectSettings &s);   // check boxes and values, then apply
    std::string spec() const { return render::effectSpec(settings()); }   // the check boxes, as an effect spec

signals:
    void applied(const QString &programId); // LinesController changed in the document
    void showSource();                      // "Open in Assembly"

private:
    struct Row { std::string id; const render::EffectParam *param; QSlider *slider; QDoubleSpinBox *box; };
    void scheduleApply();
    void applyNow();
    void resetAll();
    render::EffectSettings settings() const;
    const std::vector<uint8_t> *fileProgram() const;   // the file's LinesController (the effects' stock source)
    static QString programHash(const std::vector<uint8_t> &bytes);
    static void remember(const std::vector<uint8_t> &bytes, const std::string &spec);
    static std::optional<render::EffectSettings> recall(const std::vector<uint8_t> &bytes);

    const qrc::Archive *current_ = nullptr, *saved_ = nullptr;
    Applier apply_;
    std::vector<QGroupBox *> groups_;
    std::vector<Row> rows_;
    QTimer *debounce_;
    QLabel *status_, *note_;
    bool loading_ = false;
    bool baked_ = false;                      // the saved file's LinesController already has effects
    render::EffectSettings lastSettings_;     // what lastApplied_ was built from
    std::vector<uint8_t> lastApplied_;
};

}  // namespace moyou::app
