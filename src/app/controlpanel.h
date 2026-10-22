#pragma once

#include "formats/qrc.h"
#include "sim/params.h"

#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace moyou::app {

class WaveView;

class ParamRow : public QWidget {
    Q_OBJECT
public:
    ParamRow(sim::Params &p, std::string key, const QString &label, double min, double max, double step, QWidget *parent = nullptr);
    QLabel *label() const { return label_; }
    const std::string &key() const { return key_; }
    void sync();
signals:
    void edited();
private:
    void set(double v);
    double value() const;
    sim::Params &p_;
    std::string key_;
    double min_, step_;
    bool isInt_;
    QLabel *label_;
    QSlider *slider_;
    QDoubleSpinBox *box_;
    QToolButton *reset_;
    QCheckBox *check_ = nullptr;
    bool syncing_ = false;
};

class ControlPanel : public QWidget {
    Q_OBJECT
public:
    ControlPanel(sim::Params &p, WaveView *view, QWidget *parent = nullptr);
    void setArchive(const qrc::Archive *archive);   // rebuilds the lines.qrc tab
    void syncAll();                                 // values and readouts (called ~5 times a second)
    void syncSettings();                            // check boxes / combos from Params::S

signals:
    void edited();                                  // a parameter edit (marks the document dirty)
    void settingsChanged();                         // view / clock settings (persisted, not a document change)
    void ropeStateRequested();                      // STATE changed or Reset rope

private:
    QWidget *page(const QString &prefix, QFormLayout **form = nullptr, const QString &formTitle = {});
    void addRows(QWidget *page, const QString &prefix);
    static void finish(QWidget *page);   // trailing stretch
    QWidget *buildClock();
    QWidget *buildDebug();
    QWidget *buildQrc();
    void syncQrc();
    void filterQrc(const QString &text);

    sim::Params &p_;
    WaveView *view_;
    QTabWidget *tabs_;
    int particlesTab_ = -1;
    std::vector<ParamRow *> rows_;
    std::vector<QPushButton *> colourButtons_;

    QTreeWidget *qrcTree_ = nullptr;
    QLineEdit *qrcFilter_ = nullptr;
    QLabel *qrcInfo_ = nullptr;
    QToolButton *qrcRevert_ = nullptr;
    bool qrcSyncing_ = false;

    QCheckBox *runClock100_ = nullptr;
    QCheckBox *follow_, *particlesOn_, *showFfd_, *showGrid_;
    QComboBox *lapse_, *preset_, *bright_, *view3_, *state_;
    QSlider *time_, *month_, *simSpeed_;
    QLabel *timeOut_, *monthOut_, *segLabel_, *clockInfo_, *simInfo_, *particleInfo_, *simSpeedOut_;
    QPushButton *pause_;
};

}  // namespace moyou::app
