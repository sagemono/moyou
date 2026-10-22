#pragma once

#include "formats/qrc.h"

#include <QPlainTextEdit>
#include <QWidget>

#include <functional>
#include <map>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QSplitter;
class QToolButton;

namespace moyou::app {

class CodeEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEdit(QWidget *parent = nullptr);
    void markLine(int line);                   // 1-based; 0 clears
    int gutterWidth() const;
    void paintGutter(QPaintEvent *e);
protected:
    void resizeEvent(QResizeEvent *e) override;
private:
    QWidget *gutter_;
};

class ShaderEditor : public QWidget {
    Q_OBJECT
public:
    using Applier = std::function<QString(const std::string &id, const std::vector<uint8_t> &bytes)>;

    explicit ShaderEditor(QWidget *parent = nullptr);
    void setApplier(Applier a) { apply_ = std::move(a); }
    void setArchive(const qrc::Archive *current, const qrc::Archive *saved, int firmware);
    bool hasModifiedPrograms() const;
    void refreshProgram(const std::string &id);                          // the program changed elsewhere (Effects)
    void showText(const std::string &id, const std::string &text);       // select id and load text as an edit

private:
    void rebuildList();
    void selectProgram(int index);
    void applyCurrent();
    void revertCurrent();
    void refreshGlsl();
    void updateStatus(const QString &error = {}, int line = 0);
    std::string currentId() const;
    std::string listingFor(const std::string &id) const;
    bool isDrawn(const std::string &id) const;

    const qrc::Archive *current_ = nullptr, *saved_ = nullptr;
    int firmware_ = 493;
    Applier apply_;
    std::map<std::string, std::string> buffers_;   // unapplied text per program
    std::string shownId_;
    bool switching_ = false;

    QComboBox *programs_;
    QPushButton *apply_btn_, *revert_btn_;
    QToolButton *glslToggle_;
    QSplitter *split_;
    CodeEdit *edit_;
    QPlainTextEdit *glsl_;
    QLabel *status_, *statusIcon_;
};

}  // namespace moyou::app
