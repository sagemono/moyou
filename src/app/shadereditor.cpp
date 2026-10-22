#include "app/shadereditor.h"

#include "rsx/asm.h"
#include "rsx/rsx.h"

#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSplitter>
#include <QStyle>
#include <QStandardItemModel>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QToolButton>
#include <QVBoxLayout>

#include <set>

namespace moyou::app {

namespace {
const std::set<std::string> kDrawn493{
    "lib/moyou/back_colours0.fpo", "lib/moyou/back_colours1.fpo", "lib/moyou/back_colours2.fpo", "lib/glutils/Copy.fpo",
    "lib/moyou/lines1.vpo", "lib/moyou/lines1.fpo", "lib/moyou/LinesController.vpo", "lib/moyou/LinesController.fpo",
    "lib/glutils/GlareSourcePre.fpo", "lib/glutils/Gaussian.vpo", "lib/glutils/Gaussian.fpo", "lib/glutils/AccGlare.fpo",
    "lib/glutils/ToneApplyDisplay.fpo", "lib/particles/particles_quads.vpo", "lib/particles/particles_quads.fpo",
    "lib/particles/particles_second.vpo", "lib/particles/particles_second.fpo"};
const std::set<std::string> kDrawn100{
    "lib/moyou/back_colours0.fpo", "lib/moyou/back_colours1.fpo", "lib/moyou/back_colours2.fpo", "lib/glutils/Copy.fpo",
    "lib/moyou/lines1.vpo", "lib/moyou/lines1.fpo", "lib/moyou/LinesController.vpo", "lib/moyou/LinesController.fpo",
    "lib/glutils/GlareSourcePre.fpo", "lib/glutils/GlareSource.fpo", "lib/glutils/Gaussian.fpo", "lib/glutils/AccGlare.fpo",
    "lib/glutils/ToneApplyDisplay.fpo"};

class AsmHighlighter : public QSyntaxHighlighter {
public:
    explicit AsmHighlighter(QTextDocument *doc, const QPalette &pal) : QSyntaxHighlighter(doc) {
        comment_.setForeground(pal.color(QPalette::PlaceholderText));
        mnemonic_.setFontWeight(QFont::Bold);
        number_.setForeground(pal.color(QPalette::Link));
        name_.setFontItalic(true);
    }
protected:
    void highlightBlock(const QString &text) override {
        static const QRegularExpression mnem(R"(^\s*([A-Z][A-Z0-9]*(?:_[A-Za-z0-9]+)*)|&\s*([A-Z][A-Z0-9]*(?:_[A-Za-z0-9]+)*))");
        static const QRegularExpression num(R"(\{[^}]*\}|\b0x[0-9a-fA-F]+\b|!xor\([^)]*\))");
        static const QRegularExpression name(R"(\b_[A-Za-z0-9_]+(\[\d+\])?)");
        const int c = int(text.indexOf("//"));
        const QString code = c >= 0 ? text.left(c) : text;
        for (auto it = mnem.globalMatch(code); it.hasNext();) {
            const auto m = it.next();
            const int g = m.capturedStart(1) >= 0 ? 1 : 2;
            setFormat(int(m.capturedStart(g)), int(m.capturedLength(g)), mnemonic_);
        }
        for (auto it = num.globalMatch(code); it.hasNext();) { const auto m = it.next(); setFormat(int(m.capturedStart()), int(m.capturedLength()), number_); }
        for (auto it = name.globalMatch(code); it.hasNext();) { const auto m = it.next(); setFormat(int(m.capturedStart()), int(m.capturedLength()), name_); }
        if (c >= 0) setFormat(c, int(text.size()) - c, comment_);
    }
private:
    QTextCharFormat comment_, mnemonic_, number_, name_;
};

class Gutter : public QWidget {
public:
    explicit Gutter(CodeEdit *e) : QWidget(e), e_(e) {}
    QSize sizeHint() const override { return {e_->gutterWidth(), 0}; }
protected:
    void paintEvent(QPaintEvent *ev) override { e_->paintGutter(ev); }
private:
    CodeEdit *e_;
};
}  // namespace

CodeEdit::CodeEdit(QWidget *parent) : QPlainTextEdit(parent), gutter_(new Gutter(this)) {
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(gutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &r, int dy) {
        if (dy) gutter_->scroll(0, dy);
        else gutter_->update(0, r.y(), gutter_->width(), r.height());
    });
    setViewportMargins(gutterWidth(), 0, 0, 0);
}

int CodeEdit::gutterWidth() const {
    int digits = 1;
    for (int n = std::max(1, blockCount()); n >= 10; n /= 10) digits++;
    return 8 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(3, digits);
}

void CodeEdit::resizeEvent(QResizeEvent *e) {
    QPlainTextEdit::resizeEvent(e);
    const QRect cr = contentsRect();
    gutter_->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
}

void CodeEdit::paintGutter(QPaintEvent *ev) {
    QPainter p(gutter_);
    p.fillRect(ev->rect(), palette().color(QPalette::AlternateBase));
    QTextBlock b = firstVisibleBlock();
    int top = int(blockBoundingGeometry(b).translated(contentOffset()).top());
    p.setPen(palette().color(QPalette::PlaceholderText));
    while (b.isValid() && top <= ev->rect().bottom()) {
        const int h = int(blockBoundingRect(b).height());
        if (b.isVisible())
            p.drawText(0, top, gutter_->width() - 4, h, Qt::AlignRight | Qt::AlignVCenter, QString::number(b.blockNumber() + 1));
        b = b.next();
        top += h;
    }
}

void CodeEdit::markLine(int line) {
    QList<QTextEdit::ExtraSelection> sel;
    if (line > 0) {
        QTextEdit::ExtraSelection s;
        s.format.setBackground(palette().color(QPalette::Highlight).lighter(170));
        s.format.setProperty(QTextFormat::FullWidthSelection, true);
        s.cursor = QTextCursor(document()->findBlockByNumber(line - 1));
        sel << s;
        setTextCursor(s.cursor);
        centerCursor();
    }
    setExtraSelections(sel);
}

ShaderEditor::ShaderEditor(QWidget *parent) : QWidget(parent) {
    programs_ = new QComboBox;
    programs_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    apply_btn_ = new QPushButton(tr("Apply"));
    apply_btn_->setToolTip(tr("Assemble this program and use it in the live view (Ctrl+Return)"));
    revert_btn_ = new QPushButton(tr("Revert"));
    revert_btn_->setToolTip(tr("Go back to the program as it is in the saved lines.qrc"));
    glslToggle_ = new QToolButton;
    glslToggle_->setText(tr("GLSL"));
    glslToggle_->setCheckable(true);
    glslToggle_->setToolTip(tr("Show the program as moyou runs it (transpiled GLSL)"));
    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Program:")));
    top->addWidget(programs_);
    top->addStretch();
    top->addWidget(glslToggle_);
    top->addWidget(revert_btn_);
    top->addWidget(apply_btn_);

    edit_ = new CodeEdit;
    new AsmHighlighter(edit_->document(), palette());
    glsl_ = new QPlainTextEdit;
    glsl_->setReadOnly(true);
    glsl_->setLineWrapMode(QPlainTextEdit::NoWrap);
    glsl_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    glsl_->setVisible(false);
    split_ = new QSplitter(Qt::Horizontal);
    split_->addWidget(edit_);
    split_->addWidget(glsl_);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusIcon_ = new QLabel;
    const int is = style()->pixelMetric(QStyle::PM_SmallIconSize);
    statusIcon_->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(is, is));
    statusIcon_->setVisible(false);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(statusIcon_, 0, Qt::AlignTop);
    statusRow->addWidget(status_, 1);

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(top);
    lay->addWidget(split_, 1);
    lay->addLayout(statusRow);

    connect(programs_, &QComboBox::currentIndexChanged, this, &ShaderEditor::selectProgram);
    connect(apply_btn_, &QPushButton::clicked, this, &ShaderEditor::applyCurrent);
    connect(revert_btn_, &QPushButton::clicked, this, &ShaderEditor::revertCurrent);
    connect(glslToggle_, &QToolButton::toggled, this, [this](bool on) { glsl_->setVisible(on); if (on) refreshGlsl(); });
    auto *sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    sc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(sc, &QShortcut::activated, this, &ShaderEditor::applyCurrent);
    connect(edit_, &QPlainTextEdit::textChanged, this, [this] {
        if (switching_) return;
        edit_->markLine(0);
        updateStatus();
    });
    setEnabled(false);
}

bool ShaderEditor::isDrawn(const std::string &id) const { return (firmware_ == 100 ? kDrawn100 : kDrawn493).contains(id); }

std::string ShaderEditor::currentId() const { return programs_->currentData().toString().toStdString(); }

std::string ShaderEditor::listingFor(const std::string &id) const {
    try {
        return rsx::listing(rsx::parse(current_->file(id)), id.substr(id.rfind('/') + 1));
    } catch (const std::exception &e) {
        return std::string("// cannot list this program: ") + e.what() + "\n";
    }
}

bool ShaderEditor::hasModifiedPrograms() const {
    if (!current_ || !saved_) return false;
    for (const auto &id : current_->ids())
        if ((id.ends_with(".vpo") || id.ends_with(".fpo")) && saved_->contains(id) && current_->file(id) != saved_->file(id)) return true;
    return false;
}

void ShaderEditor::setArchive(const qrc::Archive *current, const qrc::Archive *saved, int firmware) {
    current_ = current;
    saved_ = saved;
    firmware_ = firmware;
    buffers_.clear();
    shownId_.clear();
    setEnabled(current_ != nullptr);
    rebuildList();
}

void ShaderEditor::rebuildList() {
    const std::string keep = currentId();
    switching_ = true;
    programs_->clear();
    if (!current_) { edit_->clear(); glsl_->clear(); status_->clear(); switching_ = false; return; }
    std::vector<std::string> drawn, other;
    for (const auto &id : current_->ids())
        if (id.ends_with(".vpo") || id.ends_with(".fpo")) (isDrawn(id) ? drawn : other).push_back(id);
    auto *model = qobject_cast<QStandardItemModel *>(programs_->model());
    auto header = [&](const QString &t) {
        programs_->addItem(t);
        if (model) model->item(programs_->count() - 1)->setEnabled(false);
    };
    auto add = [&](const std::string &id) {
        const bool mod = saved_ && saved_->contains(id) && saved_->file(id) != current_->file(id);
        QString label = QString::fromStdString(id.substr(4));   // drop "lib/"
        if (mod) label += tr("  (edited)");
        programs_->addItem(label, QString::fromStdString(id));
    };
    header(firmware_ == 100 ? tr("Drawn by the 1.00 XMB") : tr("Drawn by the XMB"));
    for (const auto &id : drawn) add(id);
    header(tr("Not drawn (editing changes nothing)"));
    for (const auto &id : other) add(id);
    int sel = programs_->findData(QString::fromStdString(keep));
    if (sel < 0) sel = programs_->findData(QStringLiteral("lib/moyou/lines1.fpo"));
    switching_ = false;
    programs_->setCurrentIndex(std::max(1, sel));
    selectProgram(programs_->currentIndex());
}

void ShaderEditor::selectProgram(int) {
    if (switching_ || !current_) return;
    const std::string id = currentId();
    if (id.empty()) return;
    if (!shownId_.empty()) buffers_[shownId_] = edit_->toPlainText().toStdString();
    switching_ = true;
    auto it = buffers_.find(id);
    edit_->setPlainText(QString::fromStdString(it != buffers_.end() ? it->second : listingFor(id)));
    edit_->markLine(0);
    switching_ = false;
    shownId_ = id;
    if (glsl_->isVisible()) refreshGlsl();
    updateStatus();
}

void ShaderEditor::refreshProgram(const std::string &id) {
    if (!current_) return;
    buffers_.erase(id);
    const int idx = programs_->findData(QString::fromStdString(id));
    if (idx >= 0) {
        const bool mod = saved_ && saved_->contains(id) && saved_->file(id) != current_->file(id);
        switching_ = true;
        programs_->setItemText(idx, QString::fromStdString(id.substr(4)) + (mod ? tr("  (edited)") : QString()));
        switching_ = false;
    }
    if (currentId() == id) {
        switching_ = true;
        edit_->setPlainText(QString::fromStdString(listingFor(id)));
        switching_ = false;
        if (glsl_->isVisible()) refreshGlsl();
        updateStatus();
    }
}

void ShaderEditor::showText(const std::string &id, const std::string &text) {
    const int idx = programs_->findData(QString::fromStdString(id));
    if (idx < 0) return;
    programs_->setCurrentIndex(idx);   // stores the previous program's buffer and shows this one
    edit_->setPlainText(QString::fromStdString(text));
    updateStatus();
}

void ShaderEditor::refreshGlsl() {
    const std::string id = currentId();
    if (!current_ || id.empty()) return;
    try {
        glsl_->setPlainText(QString::fromStdString(rsx::transpile(rsx::parse(current_->file(id)), {true, false}, id.substr(id.rfind('/') + 1))));
    } catch (const std::exception &e) {
        glsl_->setPlainText(QString("// ") + e.what());
    }
}

void ShaderEditor::updateStatus(const QString &error, int line) {
    statusIcon_->setVisible(!error.isEmpty());
    if (!error.isEmpty()) {
        status_->setText((line > 0 ? tr("Line %1: ").arg(line) : QString()) + error);
        return;
    }
    const std::string id = currentId();
    if (id.empty() || !current_) { status_->clear(); return; }
    const bool pending = edit_->toPlainText().toStdString() != listingFor(id);
    const bool edited = saved_ && saved_->contains(id) && saved_->file(id) != current_->file(id);
    QString s = isDrawn(id) ? tr("Drawn by the XMB.") : tr("Not drawn by the XMB: changes here have no visible effect.");
    if (edited) s += tr(" Edited (not saved to lines.qrc yet).");
    if (pending) s += tr(" Unapplied changes: Ctrl+Return to apply.");
    status_->setText(s);
}

void ShaderEditor::applyCurrent() {
    const std::string id = currentId();
    if (!current_ || id.empty() || !apply_) return;
    const std::string text = edit_->toPlainText().toStdString();
    std::vector<uint8_t> bytes;
    try {
        const auto &orig = current_->file(id);
        const rsx::Program prog = rsx::assemble(text, rsx::parse(orig));
        bytes = rsx::write(prog, orig);
    } catch (const rsx::AsmError &e) {
        edit_->markLine(e.line());
        updateStatus(QString::fromStdString(e.what()), e.line());
        return;
    } catch (const std::exception &e) {
        updateStatus(QString::fromStdString(e.what()));
        return;
    }
    if (bytes == current_->file(id)) { buffers_.erase(id); updateStatus(); return; }
    const QString err = apply_(id, bytes);
    if (!err.isEmpty()) {
        updateStatus(tr("the renderer rejected the program, nothing was changed:\n%1").arg(err));
        return;
    }
    buffers_[id] = text;
    switching_ = true;
    const int idx = programs_->currentIndex();
    programs_->setItemText(idx, QString::fromStdString(id.substr(4)) + (saved_ && saved_->contains(id) && saved_->file(id) != current_->file(id) ? tr("  (edited)") : QString()));
    switching_ = false;
    if (glsl_->isVisible()) refreshGlsl();
    updateStatus();
    status_->setText(tr("Applied %1 (%2 bytes). ").arg(QString::fromStdString(id.substr(id.rfind('/') + 1))).arg(bytes.size()) + status_->text());
}

void ShaderEditor::revertCurrent() {
    const std::string id = currentId();
    if (!current_ || !saved_ || id.empty() || !saved_->contains(id)) return;
    if (saved_->file(id) != current_->file(id)) {
        if (const QString err = apply_ ? apply_(id, saved_->file(id)) : QString(); !err.isEmpty()) {
            updateStatus(err);
            return;
        }
    }
    buffers_.erase(id);
    switching_ = true;
    edit_->setPlainText(QString::fromStdString(listingFor(id)));
    programs_->setItemText(programs_->currentIndex(), QString::fromStdString(id.substr(4)));
    switching_ = false;
    if (glsl_->isVisible()) refreshGlsl();
    updateStatus();
}

}  // namespace moyou::app
