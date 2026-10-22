#include "app/effectspanel.h"

#include "rsx/asm.h"
#include "rsx/rsx.h"

#include <QCryptographicHash>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace moyou::app {

namespace {
const char *kProgram = "lib/moyou/LinesController.fpo";
const char *kMemoryKey = "effects/saved";   // "sha1 spec" entries, newest first
constexpr int kMemorySize = 64;
int decimalsFor(double step) { int d = 0; while (step < 0.999 && d < 6) { step *= 10; d++; } return d; }
}  // namespace

EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent) {
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(120);
    connect(debounce_, &QTimer::timeout, this, &EffectsPanel::applyNow);

    note_ = new QLabel(tr("Effects rebuild LinesController, the pass that composites the wave over the background, from the stock 4.93 program. Changes apply live; File > Save writes them into lines.qrc."));
    note_->setWordWrap(true);
    auto *reset = new QPushButton(tr("Reset"));
    reset->setToolTip(tr("Turn every effect off and go back to the LinesController in the saved file (the stock one, if the file was saved with effects)"));
    auto *show = new QPushButton(tr("Open in Assembly"));
    show->setToolTip(tr("Show the generated program in the Assembly tab to read or edit it by hand"));
    connect(reset, &QPushButton::clicked, this, &EffectsPanel::resetAll);
    connect(show, &QPushButton::clicked, this, &EffectsPanel::showSource);
    auto *top = new QHBoxLayout;
    top->addWidget(note_, 1);
    top->addWidget(reset, 0, Qt::AlignTop);
    top->addWidget(show, 0, Qt::AlignTop);

    auto *inner = new QWidget;
    auto *grid = new QHBoxLayout(inner);   // effects in columns so the bottom dock stays short
    std::vector<QVBoxLayout *> cols;
    for (int c = 0; c < 3; c++) { cols.push_back(new QVBoxLayout); grid->addLayout(cols.back(), 1); }
    int i = 0;
    for (const auto &fx : render::linesControllerEffects()) {
        auto *g = new QGroupBox(tr(fx.name));
        g->setCheckable(true);
        g->setChecked(false);
        g->setToolTip(tr(fx.description));
        g->setProperty("fx", QString::fromLatin1(fx.id));
        auto *form = new QFormLayout(g);
        for (const auto &p : fx.params) {
            auto *slider = new QSlider(Qt::Horizontal);
            const int steps = int(std::lround((p.max - p.min) / p.step));
            slider->setRange(0, steps);
            auto *box = new QDoubleSpinBox;
            box->setRange(p.min, p.max);
            box->setSingleStep(p.step);
            box->setDecimals(decimalsFor(p.step));
            box->setValue(p.def);
            box->setKeyboardTracking(false);
            slider->setValue(int(std::lround((p.def - p.min) / p.step)));
            auto *row = new QHBoxLayout;
            row->addWidget(slider, 1);
            row->addWidget(box);
            form->addRow(tr(p.label), row);
            rows_.push_back({fx.id, &p, slider, box});
            connect(slider, &QSlider::valueChanged, this, [this, slider, box, &p](int v) {
                const QSignalBlocker b(box);
                box->setValue(p.min + v * p.step);
                if (!loading_) scheduleApply();
                (void)slider;
            });
            connect(box, &QDoubleSpinBox::valueChanged, this, [this, slider, &p](double v) {
                const QSignalBlocker b(slider);
                slider->setValue(int(std::lround((v - p.min) / p.step)));
                if (!loading_) scheduleApply();
            });
        }
        if (fx.params.empty()) form->addRow(new QLabel(tr("No settings.")));
        connect(g, &QGroupBox::toggled, this, [this] { if (!loading_) scheduleApply(); });
        groups_.push_back(g);
        cols[size_t(i++ % 3)]->addWidget(g);
    }
    for (auto *c : cols) c->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidget(inner);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    status_ = new QLabel;
    status_->setWordWrap(true);
    auto *lay = new QVBoxLayout(this);
    lay->addLayout(top);
    lay->addWidget(scroll, 1);
    lay->addWidget(status_);
    setEnabled(false);
}

render::EffectSettings EffectsPanel::settings() const {
    render::EffectSettings s;
    for (auto *g : groups_) s.on[g->property("fx").toString().toStdString()] = g->isChecked();
    for (const auto &r : rows_) s.values[r.id + "." + r.param->key] = r.box->value();
    return s;
}

const std::vector<uint8_t> *EffectsPanel::fileProgram() const {
    if (saved_ && saved_->contains(kProgram)) return &saved_->file(kProgram);
    if (current_ && current_->contains(kProgram)) return &current_->file(kProgram);
    return nullptr;
}

std::string EffectsPanel::currentSource() const {
    const auto *prog = fileProgram();
    if (!prog) return {};
    try {
        return render::linesControllerSource(settings(), *prog);
    } catch (const std::exception &e) {
        return std::string("// ") + e.what() + "\n";
    }
}

QString EffectsPanel::programHash(const std::vector<uint8_t> &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(QByteArrayView(bytes.data(), qsizetype(bytes.size())), QCryptographicHash::Sha1).toHex());
}

void EffectsPanel::remember(const std::vector<uint8_t> &bytes, const std::string &spec) {
    QSettings st;
    QStringList list = st.value(kMemoryKey).toStringList();
    const QString hash = programHash(bytes);
    list.removeIf([&](const QString &e) { return e.startsWith(hash + ' '); });
    list.prepend(hash + ' ' + QString::fromStdString(spec));
    while (list.size() > kMemorySize) list.removeLast();
    st.setValue(kMemoryKey, list);
}

std::optional<render::EffectSettings> EffectsPanel::recall(const std::vector<uint8_t> &bytes) {
    const QString hash = programHash(bytes);
    for (const QString &e : QSettings().value(kMemoryKey).toStringList())
        if (e.startsWith(hash + ' ')) {
            try {
                return render::parseEffectSpec(e.mid(hash.size() + 1).toStdString());
            } catch (const std::exception &) {
                return std::nullopt;   // an effect that no longer exists
            }
        }
    return std::nullopt;
}

void EffectsPanel::setArchive(const qrc::Archive *current, const qrc::Archive *saved, int firmware) {
    if (current && current == current_ && saved && current->contains(kProgram) && lastSettings_.any() &&
        saved->file(kProgram) == lastApplied_)
        remember(lastApplied_, render::effectSpec(lastSettings_));
    current_ = current;
    saved_ = saved;
    const bool v493 = current && current->contains(kProgram) && firmware == 493;
    bool ok = false;
    if (v493) {
        try {
            render::linesControllerSource({}, *fileProgram());
            ok = true;
        } catch (const std::exception &) {
        }
    }
    setEnabled(ok);
    const auto restored = ok && saved && saved->contains(kProgram) ? recall(saved->file(kProgram)) : std::nullopt;
    baked_ = restored.has_value();
    loading_ = true;
    for (auto *g : groups_) {
        auto it = restored ? restored->on.find(g->property("fx").toString().toStdString()) : render::EffectSettings{}.on.end();
        g->setChecked(restored && it != restored->on.end() && it->second);
    }
    for (const auto &r : rows_) r.box->setValue(restored ? restored->value(r.id, *r.param) : r.param->def);
    loading_ = false;
    debounce_->stop();
    lastSettings_ = restored.value_or(render::EffectSettings{});
    lastApplied_ = restored ? saved->file(kProgram) : std::vector<uint8_t>{};
    status_->setText(!current ? QString()
                     : !v493  ? tr("Effects need a firmware 4.93 lines.qrc (1.00 composites differently).")
                     : !ok    ? tr("This file's LinesController is not the stock 4.93 one (a mod?), so the effects cannot rebuild it.")
                     : restored ? tr("Saved with these effects: %1").arg(QString::fromStdString(render::effectSpec(*restored))) : tr("No effects: the LinesController from the file."));
}

void EffectsPanel::setSettings(const render::EffectSettings &s) {
    loading_ = true;
    for (auto *g : groups_) {
        auto it = s.on.find(g->property("fx").toString().toStdString());
        g->setChecked(it != s.on.end() && it->second);
    }
    for (const auto &r : rows_) r.box->setValue(s.value(r.id, *r.param));
    loading_ = false;
    applyNow();
}

void EffectsPanel::scheduleApply() { debounce_->start(); }

void EffectsPanel::resetAll() {
    loading_ = true;
    for (auto *g : groups_) g->setChecked(false);
    for (const auto &r : rows_) r.box->setValue(r.param->def);
    loading_ = false;
    applyNow();
}

void EffectsPanel::applyNow() {
    if (!current_ || !apply_ || !current_->contains(kProgram)) return;
    const render::EffectSettings s = settings();
    std::vector<uint8_t> bytes;
    if (!s.any() && !baked_ && saved_ && saved_->contains(kProgram)) {
        bytes = saved_->file(kProgram);   // nothing on: the program as it is in the file (unless the file has effects)
    } else {
        try {
            const auto &orig = current_->file(kProgram);
            bytes = rsx::write(rsx::assemble(render::linesControllerSource(s, *fileProgram()), rsx::parse(orig)), orig);
        } catch (const std::exception &e) {
            status_->setText(tr("Could not build the program: %1").arg(QString::fromStdString(e.what())));
            return;
        }
    }
    lastSettings_ = s;
    lastApplied_ = bytes;
    if (bytes == current_->file(kProgram)) {
        status_->setText(s.any() ? tr("Applied: %1").arg(QString::fromStdString(render::effectSpec(s))) : tr("No effects: the LinesController from the file."));
        return;
    }
    const QString err = apply_(kProgram, bytes);
    if (!err.isEmpty()) { status_->setText(tr("The renderer rejected the program: %1").arg(err)); return; }
    status_->setText(s.any() ? tr("Applied: %1  (%2 slots)").arg(QString::fromStdString(render::effectSpec(s))).arg(bytes.size() > 0 ? rsx::parse(bytes).fp.instructionCount : 0) : tr("No effects: the LinesController from the file."));
    emit applied(QString::fromLatin1(kProgram));
}

}  // namespace moyou::app
