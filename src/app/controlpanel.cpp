#include "app/controlpanel.h"

#include "app/waveview.h"
#include "formats/mnu.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>

namespace moyou::app {

namespace {

struct Def { const char *key; double min, max, step; const char *group; };

const Def DEFS[] = {
    {"LINE1.STATE", 0, 3, 1, "Drawing"}, {"LINE1.SHADER", 0, 2, 1, nullptr},
    {"LINE1.TIMESTEP", 0, 20, 0.1, "Simulation"}, {"LINE1.DAMPING", 0, 0.005, 0.00001, nullptr}, {"LINE1.LENGTH", 0.05, 1, 0.001, nullptr},
    {"LINE1.TENSION", 0, 1, 0.001, nullptr}, {"LINE1.PERTURBATION", 0, 1, 0.001, nullptr}, {"LINE1.END Y", 0, 2, 0.001, nullptr},
    {"LINE1.END Z", 0, 2, 0.001, nullptr},
    {"LINE1.SPACING", 50, 1000, 0.001, "Shading"}, {"LINE1.THINNESS", 0, 1, 0.001, nullptr}, {"LINE1.BRIGHTNESS", 0, 3, 0.001, nullptr},
    {"LINE1.FRESNEL", 0, 3, 0.001, nullptr}, {"LINE1.MIPMAP BIAS", 0, 5, 0.001, nullptr},
    {"LINE1.POS X", -16, 4, 0.01, "Placement"}, {"LINE1.POS Y", -6, 4, 0.01, nullptr}, {"LINE1.POS Z", -16, 4, 0.01, nullptr},
    {"LINE1.ANG X", -1, 1, 0.001, nullptr}, {"LINE1.ANG Y", -1, 1, 0.001, nullptr}, {"LINE1.ANG Z", -1, 1, 0.001, nullptr},
    {"LINE1.ANG ROT", -180, 180, 0.01, nullptr},
    {"LINE1.FFD SCALE1 X", 0.5, 12, 0.001, "Deformation (FFD shader 1)"}, {"LINE1.FFD SCALE1 Y", 0.1, 4, 0.001, nullptr},
    {"LINE1.FFD SCALE1 Z", 0.1, 4, 0.001, nullptr}, {"LINE1.FFD SCALE2 X", 0.5, 8, 0.001, nullptr},
    {"LINE1.FFD SCALE2 Y", 0.1, 4, 0.001, nullptr}, {"LINE1.FFD SCALE2 Z", 0.1, 8, 0.001, nullptr},
    {"LINE1.FFD OFFSET X", -3, 3, 0.001, nullptr}, {"LINE1.FFD OFFSET Y", -3, 3, 0.001, nullptr}, {"LINE1.FFD OFFSET Z", -3, 3, 0.001, nullptr},
    {"LINE1.FFD PARAM 1", -6, 4, 0.001, nullptr}, {"LINE1.FFD SHADER", 0, 3, 1, nullptr},
    {"BACKGROUND.COLOUR SHADER", 0, 2, 1, "Map shader"},
    {"BACKGROUND.NIGHT BLEND", 0, 1, 0.001, "Day and night"}, {"BACKGROUND.NIGHT2DAY BEGIN", 0, 24, 0.01, nullptr},
    {"BACKGROUND.NIGHT2DAY END", 0, 24, 0.01, nullptr}, {"BACKGROUND.DAY2NIGHT BEGIN", 0, 24, 0.01, nullptr},
    {"BACKGROUND.DAY2NIGHT END", 0, 24, 0.01, nullptr}, {"BACKGROUND.DAYSPREAD", -6, 6, 0.001, nullptr},
    {"BACKGROUND.NIGHT WHIT BIAS", 0, 2, 0.001, nullptr},
    {"HDR.ENABLED", 0, 1, 1, "Passes"}, {"HDR.GLARE", 0, 1, 1, nullptr}, {"HDR.GLARE_ONLY", 0, 1, 1, nullptr},
    {"HDR.EXPOSURE", 0, 4, 0.001, "Tone mapping"}, {"HDR.WHITE LEVEL", 0.2, 4, 0.001, nullptr},
    {"HDR.GLARE LEVEL", 0, 4, 0.001, "Glare"}, {"HDR.GLARE THRESH", 0, 2, 0.001, nullptr}, {"HDR.GAUSSIAN RAD R", 0.7, 2.66, 0.001, nullptr},
    {"HDR.GAUSSIAN RAD G", 0.7, 2.66, 0.001, nullptr}, {"HDR.GAUSSIAN RAD B", 0.7, 2.66, 0.001, nullptr},
    {"HDR.GLARE SUM POW", 0, 1.5, 0.001, nullptr},
    {"PARTICLES.emit prob", 0, 1, 0.001, "Emission"}, {"PARTICLES.emit per frame", 0, 64, 1, nullptr},
    {"PARTICLES.emit vel min", 0, 1, 0.001, nullptr}, {"PARTICLES.emit vel mul", 0, 1, 0.001, nullptr},
    {"PARTICLES.emit vel var", 0, 1, 0.001, nullptr}, {"PARTICLES.emit cone angle", 0, 180, 0.1, nullptr},
    {"PARTICLES.emit neg prob", 0, 1, 0.001, nullptr}, {"PARTICLES.aging speed", 0, 0.02, 0.00001, nullptr},
    {"PARTICLES.aging variance", 0, 1, 0.001, nullptr},
    {"PARTICLES.friction", 0, 0.2, 0.0001, "Motion"}, {"PARTICLES.gravity", -0.001, 0.001, 0.000001, nullptr},
    {"PARTICLES.brownian scale", 0, 1, 0.001, nullptr}, {"PARTICLES.spin time scale", 0, 10, 0.01, nullptr},
    {"PARTICLES.delta time", 0, 0.05, 0.0001, nullptr}, {"PARTICLES.wind scale", 0, 0.05, 0.0001, nullptr},
    {"PARTICLES.global alpha", 0, 2, 0.001, "Look"}, {"PARTICLES.exposure", 0, 0.2, 0.0001, nullptr},
    {"PARTICLES.size middle", 0, 0.2, 0.0001, nullptr}, {"PARTICLES.size near", 0, 0.3, 0.0001, nullptr},
    {"PARTICLES.size far", 0, 3, 0.001, nullptr}, {"PARTICLES.near focus", 0, 20, 0.01, nullptr},
    {"PARTICLES.far focus", 0, 30, 0.01, nullptr}, {"PARTICLES.specular power", 0, 100, 0.01, nullptr},
    {"PARTICLES.specular coeff", 0, 200, 0.01, nullptr}, {"PARTICLES.lambert coeff", 0, 20, 0.01, nullptr},
    {"PARTICLES.fresnel", 0, 3, 0.001, nullptr}, {"PARTICLES.glare", 0, 1, 0.001, nullptr},
    {"PARTICLES.glare scale", 0, 20, 0.01, nullptr},
};

const std::map<std::string, const char *> NO_EFFECT{
    {"LINE1.FALLOFF", "Only lines2 reads it, and lines2 writes channels the 1080p frame never uses."},
    {"LINE1.END X", "Never read: the rope constraint uses only END Y and END Z."},
    {"LINE1.SHOW FFD", "Stored but never read: the debug lattice was compiled out."},
    {"BACKGROUND.FOVY", "Its setter is an empty function: the camera stays at 53 degrees."},
    {"HDR.TONEBEFORE", "Written but never read: the shipped shaders are the tone-before versions."},
    {"HDR.BLUR", "Only sets a ToneApplyDisplay uniform the shader compiles out."},
    {"HDR.TEX SIZE", "Ignored: the glare chain is fixed at size 7, 6 levels."},
    {"HDR.TEX MAX MIP", "Ignored: the glare chain is fixed at size 7, 6 levels."},
};

constexpr int KeyRole = Qt::UserRole, IntRole = Qt::UserRole + 1;

int decimalsFor(double step) { return step >= 1 ? 0 : std::clamp(int(std::ceil(-std::log10(step) - 1e-9)), 1, 6); }

QString title(const std::string &key) { return QString::fromStdString(key.substr(key.find('.') + 1)); }

QString hhmm(double f) {
    const int s = int(std::floor(f * 86400)) % 86400;
    return QString("%1:%2:%3").arg(s / 3600, 2, 10, QChar('0')).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}
QString monthStr(double mo) {
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int m = int(std::floor(mo)) % 12;
    return QLocale().monthName(m + 1) + " " + QString::number(int(std::floor((mo - std::floor(mo)) * dim[m])) + 1);
}

class ValueDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &index) const override {
        auto *e = new QDoubleSpinBox(parent);
        const bool isInt = index.siblingAtColumn(0).data(IntRole).toBool();
        e->setRange(-1e9, 1e9);
        e->setDecimals(isInt ? 0 : 6);
        e->setSingleStep(isInt ? 1 : 0.01);
        e->setFrame(false);
        return e;
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override {
        static_cast<QDoubleSpinBox *>(editor)->setValue(index.data(Qt::EditRole).toDouble());
    }
    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override {
        auto *e = static_cast<QDoubleSpinBox *>(editor);
        e->interpretText();
        model->setData(index, e->value(), Qt::EditRole);
    }
    QString displayText(const QVariant &v, const QLocale &locale) const override {
        return v.typeId() == QMetaType::Double ? locale.toString(v.toDouble(), 'g', 7) : QStyledItemDelegate::displayText(v, locale);
    }
};

}  // namespace

ParamRow::ParamRow(sim::Params &p, std::string key, const QString &label, double min, double max, double step, QWidget *parent)
    : QWidget(parent), p_(p), key_(std::move(key)), min_(min), step_(step), isInt_(step >= 1) {
    label_ = new QLabel(label);
    label_->setToolTip(QString::fromStdString(key_));
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setRange(0, std::max(1, int(std::min(100000.0, std::round((max - min) / step)))));
    step_ = (max - min) / slider_->maximum();
    box_ = new QDoubleSpinBox;
    box_->setRange(-1e9, 1e9);   // typing may go past the slider range
    box_->setDecimals(isInt_ ? 0 : std::max(decimalsFor(step), 3));
    box_->setSingleStep(step);
    box_->setKeyboardTracking(false);
    box_->setMinimumWidth(box_->fontMetrics().horizontalAdvance(QStringLiteral("-0000.000000")));
    reset_ = new QToolButton;
    reset_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    reset_->setToolTip(tr("Return to the time-of-day schedule"));
    reset_->setAutoRaise(true);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    if (min == 0 && max == 1 && step == 1) {   // an on/off .mnu int: a check box instead of a 0..1 slider
        check_ = new QCheckBox;
        h->addWidget(check_);
        h->addStretch();
        slider_->hide();
        box_->hide();
        label_->setBuddy(check_);
        connect(check_, &QCheckBox::toggled, this, [this](bool on) { if (!syncing_) set(on ? 1 : 0); });
    } else {
        h->addWidget(slider_, 1);
        h->addWidget(box_);
        label_->setBuddy(box_);
    }
    h->addWidget(reset_);
    connect(box_, &QDoubleSpinBox::valueChanged, this, [this](double v) { if (!syncing_) set(v); });
    connect(slider_, &QSlider::valueChanged, this, [this](int i) { if (!syncing_) set(min_ + i * step_); });
    connect(reset_, &QToolButton::clicked, this, [this] { p_.edits.erase(key_); sync(); emit edited(); });
    sync();
}

double ParamRow::value() const {
    auto it = p_.edits.find(key_);
    if (it != p_.edits.end()) return it->second;
    const double v = p_.scheduled(key_);
    return std::isfinite(v) ? v : 0;
}

void ParamRow::set(double v) {
    if (!std::isfinite(v)) return;
    p_.edits[key_] = isInt_ ? std::round(v) : v;
    sync();
    emit edited();
}

void ParamRow::sync() {
    syncing_ = true;
    const bool edited = p_.edits.count(key_) != 0;
    if (label_->font().bold() != edited) {
        QFont f = label_->font();
        f.setBold(edited);
        label_->setFont(f);
    }
    reset_->setEnabled(edited);
    const double v = value();
    if (check_) check_->setChecked(v != 0);
    if (!box_->hasFocus()) box_->setValue(v);
    if (!slider_->isSliderDown()) slider_->setValue(int(std::lround((v - min_) / step_)));
    syncing_ = false;
}

QWidget *ControlPanel::page(const QString &prefix, QFormLayout **form, const QString &formTitle) {
    auto *content = new QWidget;
    auto *v = new QVBoxLayout(content);
    if (form) {
        auto *box = new QGroupBox(formTitle);
        *form = new QFormLayout(box);
        (*form)->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        v->addWidget(box);
    }
    if (!prefix.isEmpty()) {
        addRows(content, prefix);
        v->addStretch();
    }
    auto *scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return scroll;
}

void ControlPanel::finish(QWidget *page) {
    static_cast<QVBoxLayout *>(static_cast<QScrollArea *>(page)->widget()->layout())->addStretch();
}

void ControlPanel::addRows(QWidget *content, const QString &prefix) {
    auto *v = static_cast<QVBoxLayout *>(content->layout());
    QFormLayout *form = nullptr;
    for (const Def &d : DEFS) {
        if (!QString(d.key).startsWith(prefix)) continue;
        if (d.group || !form) {
            auto *box = new QGroupBox(d.group ? tr(d.group) : QString());
            form = new QFormLayout(box);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            v->addWidget(box);
        }
        auto *row = new ParamRow(p_, d.key, title(d.key), d.min, d.max, d.step);
        connect(row, &ParamRow::edited, this, &ControlPanel::edited);
        rows_.push_back(row);
        form->addRow(row->label(), row);
    }
}

QWidget *ControlPanel::buildClock() {
    QFormLayout *f;
    QWidget *w = page({}, &f, tr("Time"));
    segLabel_ = new QLabel;
    f->addRow(tr("Schedule:"), segLabel_);
    follow_ = new QCheckBox(tr("Follow the local clock"));
    f->addRow(QString(), follow_);
    runClock100_ = new QCheckBox(tr("Run the 1.00 day cycle"));
    runClock100_->setToolTip(tr("A real firmware 1.00 console never moves the wave's clock off 00:00, so it shows a static black background and the night preset. Tick this to run the day cycle that is in the 1.00 plugin and lines.qrc anyway."));
    runClock100_->setVisible(false);
    f->addRow(QString(), runClock100_);
    time_ = new QSlider(Qt::Horizontal);
    time_->setRange(0, 24 * 60 - 1);
    timeOut_ = new QLabel;
    auto *tr1 = new QWidget;
    auto *h1 = new QHBoxLayout(tr1);
    h1->setContentsMargins(0, 0, 0, 0);
    h1->addWidget(time_, 1);
    h1->addWidget(timeOut_);
    f->addRow(tr("Time of day:"), tr1);
    month_ = new QSlider(Qt::Horizontal);
    month_->setRange(0, 11999);
    monthOut_ = new QLabel;
    auto *tr2 = new QWidget;
    auto *h2 = new QHBoxLayout(tr2);
    h2->setContentsMargins(0, 0, 0, 0);
    h2->addWidget(month_, 1);
    h2->addWidget(monthOut_);
    f->addRow(tr("Month:"), tr2);
    lapse_ = new QComboBox;
    for (auto [t, v] : {std::pair{"Stopped", 0}, {"Real time", 1}, {"60×", 60}, {"600×", 600}, {"3600×", 3600}})
        lapse_->addItem(tr(t), v);
    f->addRow(tr("Time-lapse:"), lapse_);
    preset_ = new QComboBox;
    preset_->addItem(tr("Time-of-day schedule"), "schedule");
    for (auto [n, t] : {std::pair{"night", "Night"}, {"yoake", "Dawn (yoake)"}, {"day", "Day"}, {"higure", "Dusk (higure)"}, {"base", "Base files (no override)"}})
        preset_->addItem(tr(t), n);
    f->addRow(tr("Parameters:"), preset_);
    clockInfo_ = new QLabel;
    clockInfo_->setWordWrap(true);
    clockInfo_->setForegroundRole(QPalette::PlaceholderText);
    f->addRow(clockInfo_);
    finish(w);
    return w;
}

QWidget *ControlPanel::buildDebug() {
    QFormLayout *f;
    QWidget *w = page({}, &f, tr("Inspection"));
    view3_ = new QComboBox;
    for (const char *t : {"Display buffer", "Wave buffer (lines1, decoded)", "Background map (back_colours0)", "Glare accumulation"})
        view3_->addItem(tr(t));
    f->addRow(tr("View:"), view3_);
    showGrid_ = new QCheckBox(tr("Rope control grid (19×19)"));
    showFfd_ = new QCheckBox(tr("FFD lattice"));
    f->addRow(tr("Overlays:"), showGrid_);
    f->addRow(QString(), showFfd_);
    state_ = new QComboBox;
    for (const char *t : {"0 (t = 76.41)", "1 (t = 98.62)", "2 (t = 90.54)"}) state_->addItem(t);
    f->addRow(tr("Rope start state:"), state_);
    simSpeed_ = new QSlider(Qt::Horizontal);
    simSpeed_->setRange(0, 80);
    simSpeedOut_ = new QLabel;
    auto *sr = new QWidget;
    auto *h = new QHBoxLayout(sr);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(simSpeed_, 1);
    h->addWidget(simSpeedOut_);
    f->addRow(tr("Simulation speed:"), sr);
    auto *btns = new QWidget;
    auto *bl = new QHBoxLayout(btns);
    bl->setContentsMargins(0, 0, 0, 0);
    pause_ = new QPushButton(tr("Pause"));
    auto *stepB = new QPushButton(tr("Step 60"));
    auto *resetB = new QPushButton(tr("Reset rope"));
    for (QPushButton *b : {pause_, stepB, resetB}) bl->addWidget(b);
    bl->addStretch();
    f->addRow(QString(), btns);
    simInfo_ = new QLabel;
    simInfo_->setWordWrap(true);
    simInfo_->setForegroundRole(QPalette::PlaceholderText);
    f->addRow(simInfo_);
    connect(stepB, &QPushButton::clicked, this, [this] { view_->stepRope(); });
    connect(resetB, &QPushButton::clicked, this, [this] { emit ropeStateRequested(); });
    finish(w);
    return w;
}

QWidget *ControlPanel::buildQrc() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *bar = new QHBoxLayout;
    qrcFilter_ = new QLineEdit;
    qrcFilter_->setPlaceholderText(tr("Filter"));
    qrcFilter_->setClearButtonEnabled(true);
    qrcRevert_ = new QToolButton;
    qrcRevert_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    qrcRevert_->setToolTip(tr("Return the selected values to the schedule"));
    qrcRevert_->setEnabled(false);
    bar->addWidget(qrcFilter_, 1);
    bar->addWidget(qrcRevert_);
    v->addLayout(bar);
    qrcTree_ = new QTreeWidget;
    qrcTree_->setColumnCount(3);
    qrcTree_->setHeaderLabels({tr("Parameter"), tr("Value"), tr("Shipped")});
    qrcTree_->setItemDelegateForColumn(1, new ValueDelegate(qrcTree_));
    qrcTree_->setItemDelegateForColumn(2, new ValueDelegate(qrcTree_));
    qrcTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    qrcTree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    qrcTree_->setUniformRowHeights(true);
    qrcTree_->setAlternatingRowColors(true);
    qrcTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    qrcTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    qrcTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    qrcTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    qrcTree_->header()->setStretchLastSection(false);
    v->addWidget(qrcTree_, 1);
    qrcInfo_ = new QLabel;
    qrcInfo_->setWordWrap(true);
    qrcInfo_->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(qrcInfo_);

    auto revertSelected = [this] {
        for (QTreeWidgetItem *it : qrcTree_->selectedItems())
            if (const auto key = it->data(0, KeyRole).toString().toStdString(); !key.empty()) p_.edits.erase(key);
        syncAll();
        emit edited();
    };
    connect(qrcRevert_, &QToolButton::clicked, this, revertSelected);
    connect(qrcFilter_, &QLineEdit::textChanged, this, &ControlPanel::filterQrc);
    connect(qrcTree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        bool any = false;
        for (QTreeWidgetItem *it : qrcTree_->selectedItems())
            any = any || p_.edits.count(it->data(0, KeyRole).toString().toStdString());
        qrcRevert_->setEnabled(any);
    });
    connect(qrcTree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *it, int col) {
        if (qrcSyncing_ || col != 1) return;
        const std::string key = it->data(0, KeyRole).toString().toStdString();
        if (key.empty()) return;
        const double v = it->data(1, Qt::EditRole).toDouble();
        p_.edits[key] = it->data(0, IntRole).toBool() ? std::round(v) : v;
        syncAll();
        emit edited();
    });
    connect(qrcTree_, &QTreeWidget::customContextMenuRequested, this, [this, revertSelected](const QPoint &pos) {
        QMenu m;
        m.addAction(style()->standardIcon(QStyle::SP_DialogResetButton), tr("Return to Schedule"), revertSelected)
            ->setEnabled(qrcRevert_->isEnabled());
        m.exec(qrcTree_->viewport()->mapToGlobal(pos));
    });
    return w;
}

ControlPanel::ControlPanel(sim::Params &p, WaveView *view, QWidget *parent) : QWidget(parent), p_(p), view_(view) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(true);
    tabs_->setUsesScrollButtons(true);
    root->addWidget(tabs_);

    tabs_->addTab(buildClock(), tr("Clock"));
    tabs_->addTab(page("LINE1."), tr("Wave"));

    // Background: theme brightness and the four colours, then the day / night timing sliders
    {
        QFormLayout *f;
        QWidget *w = page({}, &f, tr("Theme"));
        bright_ = new QComboBox;
        for (int l = 0; l < 6; l++)
            bright_->addItem(l ? tr("Level %1 (×%2)").arg(l).arg(1 - 0.15 * l, 0, 'f', 2) : tr("Level 0 (default)"), l);
        bright_->setToolTip(tr("PS3 Theme Settings > Brightness. Dims the wave, background and particles with a 1 second fade."));
        f->addRow(tr("Theme brightness:"), bright_);
        auto *cols = new QWidget;
        auto *ch = new QHBoxLayout(cols);
        ch->setContentsMargins(0, 0, 0, 0);
        for (int c = 1; c <= 4; c++) {
            auto *b = new QPushButton(QString::number(c));
            b->setToolTip(tr("Background colour %1 (BACKGROUND.%1 RED/GREEN/BLUE)").arg(c));
            connect(b, &QPushButton::clicked, this, [this, c] {
                const std::string n = "BACKGROUND." + std::to_string(c) + " ";
                auto cl = [](double v) { return float(std::clamp(v, 0.0, 1.0)); };
                const QColor cur = QColor::fromRgbF(cl(p_.P(n + "RED")), cl(p_.P(n + "GREEN")), cl(p_.P(n + "BLUE")));
                const QColor col = QColorDialog::getColor(cur, this, tr("Background colour %1").arg(c));
                if (!col.isValid()) return;
                p_.edits[n + "RED"] = col.redF();
                p_.edits[n + "GREEN"] = col.greenF();
                p_.edits[n + "BLUE"] = col.blueF();
                emit edited();
                syncAll();
            });
            colourButtons_.push_back(b);
            ch->addWidget(b);
        }
        ch->addStretch();
        f->addRow(tr("Colours:"), cols);
        addRows(static_cast<QScrollArea *>(w)->widget(), "BACKGROUND.");
        finish(w);
        tabs_->addTab(w, tr("Background"));
    }
    tabs_->addTab(page("HDR."), tr("HDR"));
    {
        QFormLayout *f;
        QWidget *w = page({}, &f, tr("Drawing"));
        particlesOn_ = new QCheckBox(tr("Draw particles"));
        auto *restart = new QPushButton(tr("Restart"));
        connect(restart, &QPushButton::clicked, this, [this] { view_->restartParticles(); });
        particleInfo_ = new QLabel;
        particleInfo_->setForegroundRole(QPalette::PlaceholderText);
        f->addRow(QString(), particlesOn_);
        f->addRow(restart, particleInfo_);
        addRows(static_cast<QScrollArea *>(w)->widget(), "PARTICLES.");
        finish(w);
        particlesTab_ = tabs_->addTab(w, tr("Particles"));
    }
    tabs_->addTab(buildQrc(), tr("lines.qrc"));
    tabs_->addTab(buildDebug(), tr("Debug"));

    // settings wiring
    auto changed = [this] { emit settingsChanged(); };
    connect(runClock100_, &QCheckBox::toggled, this, [this, changed](bool on) {
        p_.S.runClock100 = on;
        p_.forceTick();
        changed();
    });
    connect(follow_, &QCheckBox::toggled, this, [this, changed](bool on) {
        p_.S.follow = on;
        if (on) { p_.S.lapse = 1; p_.forceTick(); }
        syncSettings(); changed();
    });
    connect(time_, &QSlider::sliderMoved, this, [this, changed](int v) {
        p_.S.follow = false; p_.S.dayFrac = v / 1440.0; p_.forceTick(); syncSettings(); changed();
    });
    connect(month_, &QSlider::sliderMoved, this, [this, changed](int v) {
        p_.S.follow = false; p_.S.month = std::min(11.999, v / 1000.0); p_.forceTick(); syncSettings(); changed();
    });
    connect(lapse_, &QComboBox::activated, this, [this, changed](int i) {
        p_.S.lapse = lapse_->itemData(i).toDouble();
        if (p_.S.lapse != 1) p_.S.follow = false;
        syncSettings(); changed();
    });
    connect(preset_, &QComboBox::activated, this, [this, changed](int i) { p_.S.preset = preset_->itemData(i).toString().toStdString(); changed(); });
    connect(bright_, &QComboBox::activated, this, [this, changed](int i) { p_.S.brightLevel = i; changed(); });
    connect(particlesOn_, &QCheckBox::toggled, this, [this, changed](bool on) { p_.S.particlesOn = on; changed(); });
    connect(view3_, &QComboBox::activated, this, [this, changed](int i) { p_.S.view = i; changed(); });
    connect(showFfd_, &QCheckBox::toggled, this, [this, changed](bool on) { p_.S.showFfd = on; changed(); });
    connect(showGrid_, &QCheckBox::toggled, this, [this, changed](bool on) { p_.S.showGrid = on; changed(); });
    connect(state_, &QComboBox::activated, this, [this, changed](int i) { p_.S.state = i; emit ropeStateRequested(); changed(); });
    connect(simSpeed_, &QSlider::valueChanged, this, [this, changed](int v) { p_.S.simSpeed = v / 2.0; syncAll(); changed(); });
    connect(pause_, &QPushButton::clicked, this, [this, changed] { p_.S.paused = !p_.S.paused; syncAll(); changed(); });
    syncSettings();
    syncAll();
}

void ControlPanel::setArchive(const qrc::Archive *archive) {
    qrcSyncing_ = true;
    qrcTree_->clear();
    const bool particles = !archive || archive->contains("PARTICLES.mnu");   // firmware 1.00 has none
    if (!particles && tabs_->currentIndex() == particlesTab_) tabs_->setCurrentIndex(0);
    tabs_->setTabVisible(particlesTab_, particles);
    for (ParamRow *r : rows_) {   // keys the file does not have (1.00: night blend, DAYSPREAD, ...) are not shown
        const bool has = !archive || p_.has(r->key());
        r->setVisible(has);
        r->label()->setVisible(has);
    }
    const bool v100 = archive && p_.firmware == 100;
    runClock100_->setVisible(v100);
    bright_->setEnabled(!v100);
    bright_->setToolTip(v100 ? tr("Firmware 1.00 has no theme brightness.") : tr("PS3 Theme Settings > Brightness. Dims the wave, background and particles with a 1 second fade."));
    if (!archive) {
        qrcInfo_->setText(tr("No lines.qrc loaded."));
        qrcSyncing_ = false;
        return;
    }
    int n = 0;
    for (const char *file : {"LINE1", "LINE2", "BACKGROUND", "HDR", "PARTICLES"}) {
        const std::string id = std::string(file) + ".mnu";
        if (!archive->contains(id)) continue;
        // firmware 1.00 ships LINE2.mnu but never loads it
        const bool unused = std::string(file) == "LINE2";
        auto *top = new QTreeWidgetItem(qrcTree_, {unused ? tr("%1 (not used by the firmware)").arg(QString::fromStdString(id)) : QString::fromStdString(id)});
        top->setFlags(Qt::ItemIsEnabled);
        top->setFirstColumnSpanned(true);
        for (const auto &e : mnu::parse(mnu::fromBytes(archive->file(id)))) {
            const std::string key = std::string(file) + "." + e.name;
            const auto noEffect = NO_EFFECT.find(key);
            const bool fileOnly = unused || noEffect != NO_EFFECT.end();
            auto *it = new QTreeWidgetItem(top);
            it->setText(0, QString::fromStdString(e.name));
            it->setData(0, KeyRole, QString::fromStdString(key));
            it->setData(0, IntRole, e.type == "int");
            it->setData(2, Qt::EditRole, e.value);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            QString tip = QString("%1 (%2)").arg(QString::fromStdString(key), QString::fromStdString(e.type));
            if (fileOnly) {
                tip += "\n" + tr("No effect on the PS3 either: %1").arg(unused ? tr("the firmware never loads LINE2.mnu.") : tr(noEffect->second));
                for (int c = 0; c < 3; c++) it->setForeground(c, palette().brush(QPalette::Disabled, QPalette::Text));
            }
            for (int c = 0; c < 3; c++) it->setToolTip(c, tip);
            n++;
        }
        top->setExpanded(true);
    }
    qrcSyncing_ = false;
    qrcInfo_->setText(tr("%1 values in 4 files. Double-click a value to edit it; edited values are bold. Greyed entries have no effect on the PS3 either (see their tooltips).").arg(n));
    filterQrc(qrcFilter_->text());
    syncQrc();
}

void ControlPanel::filterQrc(const QString &text) {
    const QString f = text.trimmed();
    for (int i = 0; i < qrcTree_->topLevelItemCount(); i++) {
        QTreeWidgetItem *top = qrcTree_->topLevelItem(i);
        bool any = false;
        for (int k = 0; k < top->childCount(); k++) {
            QTreeWidgetItem *it = top->child(k);
            const bool show = f.isEmpty() || it->data(0, KeyRole).toString().contains(f, Qt::CaseInsensitive);
            it->setHidden(!show);
            any = any || show;
        }
        top->setHidden(!any);
    }
}

void ControlPanel::syncQrc() {
    if (!qrcTree_->isVisible() && qrcTree_->topLevelItemCount()) return;
    qrcSyncing_ = true;
    // an open value editor is a spin box on the tree's viewport: leave that row alone while the user types
    QWidget *fw = QApplication::focusWidget();
    const bool editorOpen = fw && fw->parentWidget() == qrcTree_->viewport();
    const QModelIndex editing = editorOpen ? qrcTree_->currentIndex() : QModelIndex();
    for (int i = 0; i < qrcTree_->topLevelItemCount(); i++) {
        QTreeWidgetItem *top = qrcTree_->topLevelItem(i);
        for (int k = 0; k < top->childCount(); k++) {
            QTreeWidgetItem *it = top->child(k);
            if (it->isHidden()) continue;
            const std::string key = it->data(0, KeyRole).toString().toStdString();
            const bool edited = p_.edits.count(key) != 0;
            if (it->font(0).bold() != edited) {
                QFont f = it->font(0);
                f.setBold(edited);
                for (int c = 0; c < 2; c++) it->setFont(c, f);
            }
            if (editing.isValid() && qrcTree_->itemFromIndex(editing) == it) continue;
            double v = edited ? p_.edits.at(key) : p_.scheduled(key);
            if (!std::isfinite(v)) v = it->data(2, Qt::EditRole).toDouble();
            if (it->data(1, Qt::EditRole).toDouble() != v || !it->data(1, Qt::EditRole).isValid()) it->setData(1, Qt::EditRole, v);
        }
    }
    qrcSyncing_ = false;
}

void ControlPanel::syncSettings() {
    const QSignalBlocker b0(runClock100_), b1(follow_), b2(lapse_), b3(preset_), b4(bright_), b6(particlesOn_), b8(view3_),
        b9(showFfd_), b10(showGrid_), b11(state_), b12(simSpeed_);
    follow_->setChecked(p_.S.follow);
    runClock100_->setChecked(p_.S.runClock100);
    lapse_->setCurrentIndex(std::max(0, lapse_->findData(p_.S.lapse)));
    preset_->setCurrentIndex(std::max(0, preset_->findData(QString::fromStdString(p_.S.preset))));
    bright_->setCurrentIndex(std::clamp(p_.S.brightLevel, 0, 5));
    particlesOn_->setChecked(p_.S.particlesOn);
    view3_->setCurrentIndex(std::clamp(p_.S.view, 0, 3));
    showFfd_->setChecked(p_.S.showFfd);
    showGrid_->setChecked(p_.S.showGrid);
    state_->setCurrentIndex(std::clamp(p_.S.state, 0, 2));
    simSpeed_->setValue(int(std::lround(p_.S.simSpeed * 2)));
}

void ControlPanel::syncAll() {
    for (ParamRow *r : rows_) if (r->isVisible()) r->sync();
    syncQrc();
    if (!time_->isSliderDown()) time_->setValue(int(p_.S.dayFrac * 1440));
    if (!month_->isSliderDown()) month_->setValue(int(p_.S.month * 1000));
    timeOut_->setText(hhmm(p_.S.dayFrac));
    monthOut_->setText(monthStr(p_.S.month));
    segLabel_->setText(QString::fromStdString(p_.segmentLabel()));
    const int m = int(std::floor(p_.clk.month)) % 12;
    clockInfo_->setText((p_.firmware == 100 ? tr("Last clock update %1. Background textures: %2 to %3.")
                                            : tr("Last clock update %1. Background textures: %2 to %3 (day and night)."))
                            .arg(hhmm(p_.clk.dayFrac), QLocale().monthName(m + 1), QLocale().monthName((m + 1) % 12 + 1)));
    for (int c = 0; c < 4; c++) {
        const std::string n = "BACKGROUND." + std::to_string(c + 1) + " ";
        auto cl = [](double v) { return float(std::clamp(v, 0.0, 1.0)); };
        QPixmap px(16, 16);
        px.fill(QColor::fromRgbF(cl(p_.P(n + "RED")), cl(p_.P(n + "GREEN")), cl(p_.P(n + "BLUE"))));
        colourButtons_[size_t(c)]->setIcon(px);
    }
    const auto &w = view_->wave();
    simInfo_->setText(tr("%1 steps, line time %2, _Time %3, RNG counter %4. Rope start: %5. %6 fps.")
                          .arg(w.steps()).arg(w.lineTime(), 0, 'f', 4).arg(w.lineTime() * 10, 0, 'f', 3).arg(w.rngCounter())
                          .arg(view_->exactRopeStates() ? tr("PS3 tables") : tr("settled (no plugin tables)"))
                          .arg(view_->fps(), 0, 'f', 0));
    particleInfo_->setText(tr("%1 of 2048 live").arg(view_->particleCount()));
    simSpeedOut_->setText(QString::number(p_.S.simSpeed, 'f', 1) + QString::fromUtf8("×"));
    pause_->setText(p_.S.paused ? tr("Resume") : tr("Pause"));
}

}  // namespace moyou::app
