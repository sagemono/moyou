#include "app/mainwindow.h"

#include "app/controlpanel.h"
#include "app/icon.h"
#include "app/effectspanel.h"
#include "app/shadereditor.h"
#include "app/waveview.h"
#include "formats/mnu.h"
#include "rsx/asm.h"
#include "render/renderer.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStyle>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <set>

namespace moyou::app {

namespace {
const char *const kQrcRel = "dev_flash/vsh/resource/qgl/lines.qrc";
std::filesystem::path fsPath(const QString &s) { return std::filesystem::path(s.toStdWString()); }
}  // namespace

MainWindow::MainWindow() {
    resize(1500, 860);
    setAcceptDrops(true);

    stack_ = new QStackedWidget;
    auto *welcome = new QWidget;
    auto *wl = new QVBoxLayout(welcome);
    wl->addStretch();
    auto *icon = new QLabel;
    icon->setPixmap(appIcon().pixmap(64, 64));
    icon->setAlignment(Qt::AlignCenter);
    auto *title = new QLabel(tr("No lines.qrc open"));
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.6);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);
    auto *text = new QLabel(tr("Open the XMB wave's resource file to start, or drop it on this window. It is in dev_flash/vsh/resource/qgl/ of a PS3 firmware or an RPCS3 installation."));
    text->setAlignment(Qt::AlignCenter);
    text->setWordWrap(true);
    text->setMaximumWidth(460);
    auto *bOpen = new QPushButton(style()->standardIcon(QStyle::SP_DialogOpenButton), tr("Open lines.qrc..."));
    auto *bRpcs3 = new QPushButton(style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Open from RPCS3..."));
    bOpen->setDefault(true);
    auto *btns = new QHBoxLayout;
    btns->addStretch();
    btns->addWidget(bOpen);
    btns->addWidget(bRpcs3);
    btns->addStretch();
    wl->addWidget(icon);
    wl->addWidget(title);
    auto *textRow = new QHBoxLayout;   // centred with stretches: an alignment flag would cost the label its wrapped height
    textRow->addStretch();
    textRow->addWidget(text, 1);
    textRow->addStretch();
    wl->addLayout(textRow);
    wl->addSpacing(8);
    wl->addLayout(btns);
    wl->addStretch();
    connect(bOpen, &QPushButton::clicked, this, &MainWindow::openDialog);
    connect(bRpcs3, &QPushButton::clicked, this, &MainWindow::openFromRpcs3);

    view_ = new WaveView(params_);
    stack_->addWidget(welcome);
    stack_->addWidget(view_);
    setCentralWidget(stack_);
    connect(view_, &WaveView::failed, this, [this](const QString &m) {
        QMessageBox::critical(this, "Renderer error", "The wave could not be set up on this GPU.\n\n" + m);
    });
    connect(view_, &WaveView::doubleClicked, this, &MainWindow::toggleFullscreen);

    panel_ = new ControlPanel(params_, view_);
    dock_ = new QDockWidget(tr("Parameters"));
    dock_->setObjectName("parametersDock");
    dock_->setWidget(panel_);
    dock_->setMinimumWidth(420);
    addDockWidget(Qt::RightDockWidgetArea, dock_);
    shaders_ = new ShaderEditor;
    shaders_->setApplier([this](const std::string &id, const std::vector<uint8_t> &bytes) { return replaceProgram(id, bytes); });
    effects_ = new EffectsPanel;
    effects_->setApplier([this](const std::string &id, const std::vector<uint8_t> &bytes) { return replaceProgram(id, bytes); });
    auto *shaderTabs = new QTabWidget;
    shaderTabs->setDocumentMode(true);
    shaderTabs->addTab(effects_, tr("Effects"));
    shaderTabs->addTab(shaders_, tr("Assembly"));
    connect(effects_, &EffectsPanel::applied, this, [this](const QString &id) { shaders_->refreshProgram(id.toStdString()); });
    connect(effects_, &EffectsPanel::showSource, this, [this, shaderTabs] {
        shaderTabs->setCurrentWidget(shaders_);
        shaders_->showText("lib/moyou/LinesController.fpo", effects_->currentSource());
    });
    shaderDock_ = new QDockWidget(tr("Shader Editor"));
    shaderDock_->setObjectName("shaderDock");
    shaderDock_->setWidget(shaderTabs);
    addDockWidget(Qt::BottomDockWidgetArea, shaderDock_);
    shaderDock_->hide();
    connect(panel_, &ControlPanel::edited, this, &MainWindow::markDirty);
    connect(panel_, &ControlPanel::settingsChanged, this, &MainWindow::saveSettings);
    connect(panel_, &ControlPanel::ropeStateRequested, this, [this] { view_->resetRope(); });

    status_ = new QLabel;
    statusBar()->addPermanentWidget(status_);
    ui_ = new QTimer(this);
    ui_->setInterval(200);
    connect(ui_, &QTimer::timeout, this, [this] {
        if (!archive_) { status_->clear(); return; }
        panel_->syncAll();
        const int n = int(params_.edits.size());
        QString t = tr("%1 \u2014 %2 fps").arg(QString::fromStdString(params_.segmentLabel())).arg(view_->fps(), 0, 'f', 0);
        if (n) t += tr(" \u2014 %n edited value(s)", nullptr, n);
        status_->setText(t);
    });
    ui_->start();

    buildMenus();
    loadSettings();
    if (!archive_) updateDock(true);   // nothing opened at start: no parameter tabs yet
    updateTitle();
    if (!QApplication::arguments().contains("--selftest")) QTimer::singleShot(0, this, &MainWindow::showNotice);
}

// Shown at every start, as in the author's other PS3 tools: what can go wrong on a console and how to undo it.
void MainWindow::showNotice() {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Before you start"));
    box.setText(tr("<b>moyou edits lines.qrc, a file in your PS3's system flash.</b>"));
    box.setInformativeText(tr(
        "A broken or wrong lines.qrc can stop the XMB from loading on a console. moyou is new software and can "
        "have bugs that do not show up in its own preview.<br><br>"
        "<b>Before you put a theme on a console:</b><br>"
        "&nbsp;&nbsp;- Try it in RPCS3 first (File &gt; Install to RPCS3).<br>"
        "&nbsp;&nbsp;- Keep the original lines.qrc. moyou saves it as lines.qrc.orig the first time it writes over "
        "a file; keep another copy off the console too.<br>"
        "&nbsp;&nbsp;- Only use the lines.qrc of the firmware your console runs.<br>"
        "&nbsp;&nbsp;- Know how to reinstall the firmware from Recovery Mode, which puts the original file back, "
        "before you need it.<br><br>"
        "There is <b>no warranty of any kind</b>. You use moyou entirely at your own risk, and the developers "
        "accept no responsibility for broken themes, lost data or consoles that no longer boot to the XMB.<br><br>"
        "moyou is not affiliated with Sony Interactive Entertainment."));
    QPushButton *go = box.addButton(tr("I understand"), QMessageBox::AcceptRole);
    QPushButton *out = box.addButton(tr("Exit"), QMessageBox::RejectRole);
    box.setDefaultButton(out);
    box.setEscapeButton(out);
    box.exec();
    if (box.clickedButton() != go) {
        close();
        QCoreApplication::quit();
    }
}

void MainWindow::buildMenus() {
    auto act = [this](QMenu *m, const QString &text, QKeySequence key, auto fn, QStyle::StandardPixmap icon = QStyle::SP_CustomBase) {
        QAction *a = m->addAction(text);
        if (icon != QStyle::SP_CustomBase) a->setIcon(style()->standardIcon(icon));
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);   // shortcuts keep working with the menu bar hidden
        return a;
    };
    QMenu *file = menuBar()->addMenu(tr("&File"));
    act(file, tr("&Open lines.qrc..."), QKeySequence::Open, [this] { openDialog(); }, QStyle::SP_DialogOpenButton);
    act(file, tr("Open from &RPCS3..."), {}, [this] { openFromRpcs3(); }, QStyle::SP_DirOpenIcon);
    file->addSeparator();
    act(file, tr("&Save"), QKeySequence::Save, [this] { save(); }, QStyle::SP_DialogSaveButton);
    act(file, tr("Save &As..."), QKeySequence::SaveAs, [this] { saveAs(); });
    act(file, tr("&Install to RPCS3..."), {}, [this] { installToRpcs3(); });
    file->addSeparator();
    act(file, tr("Load Rope &Tables from custom_render_plugin..."), {}, [this] { loadPluginDialog(); });
    act(file, tr("Save Scree&nshot..."), QKeySequence(Qt::Key_F12), [this] { screenshot(); });
    file->addSeparator();
    act(file, tr("&Quit"), QKeySequence::Quit, [this] { close(); });

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    act(edit, tr("&Copy Edits as Text"), {}, [this] { copyEditsAsText(); });
    act(edit, tr("&Revert All Edits"), {}, [this] {
        params_.edits.clear();
        panel_->syncAll();
        markDirty();
    }, QStyle::SP_DialogResetButton);

    QMenu *viewM = menuBar()->addMenu(tr("&View"));
    QAction *dockToggle = dock_->toggleViewAction();
    dockToggle->setShortcut(QKeySequence(Qt::Key_M));
    viewM->addAction(dockToggle);
    addAction(dockToggle);
    QAction *shaderToggle = shaderDock_->toggleViewAction();
    shaderToggle->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    viewM->addAction(shaderToggle);
    addAction(shaderToggle);
    act(viewM, tr("&Hide All UI"), QKeySequence(Qt::Key_H), [this] { setUiHidden(!uiHidden_); });
    act(viewM, tr("&Full Screen"), QKeySequence::FullScreen, [this] { toggleFullscreen(); });
    viewM->addSeparator();
    act(viewM, tr("&Reset All"), {}, [this] { resetAll(); });

    QMenu *simM = menuBar()->addMenu(tr("&Simulation"));
    act(simM, tr("&Pause / Resume"), QKeySequence(Qt::Key_Space), [this] { params_.S.paused = !params_.S.paused; panel_->syncAll(); },
        QStyle::SP_MediaPause);
    act(simM, tr("&Step 60 Frames"), QKeySequence(Qt::Key_Period), [this] { view_->stepRope(); }, QStyle::SP_MediaSkipForward);
    simM->addSeparator();
    act(simM, tr("Reset &Rope"), {}, [this] { view_->resetRope(); });
    act(simM, tr("Restart P&articles"), {}, [this] { view_->restartParticles(); });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    act(help, tr("&About moyou"), {}, [this] {
        QMessageBox::about(this, tr("About moyou"),
                           tr("<b>moyou</b> %1<p>A theming tool for the PS3 XMB wave. Change the colours, the wave, the "
                              "background and the effects in a firmware 4.93 lines.qrc, see the result live, and save it for "
                              "RPCS3 or a console.</p><p>No Sony data ships with this program; it reads your own files.</p>"
                              "<p>moyou is an independent project. It is not affiliated with, endorsed by, or derived from "
                              "Sony Interactive Entertainment. &quot;PlayStation&quot;, &quot;PS3&quot; and &quot;XMB&quot; are "
                              "trademarks of their respective owners, used here only to identify the hardware and software "
                              "moyou works with.</p><p>Developed by <a href=\"https://github.com/sagemono\">sagemono</a>.</p>")
                               .arg(QApplication::applicationVersion()));
    });
    act(help, tr("About &Qt"), {}, [] { QApplication::aboutQt(); });
}

QString MainWindow::droppedFile(const QMimeData *mime) const {
    if (!mime->hasUrls() || mime->urls().size() != 1 || !mime->urls().first().isLocalFile()) return {};
    const QString path = mime->urls().first().toLocalFile();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "qrc" || (archive_ && (suffix == "prx" || suffix == "elf"))) return path;
    return {};
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
    if (!droppedFile(e->mimeData()).isEmpty()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e) {
    const QString path = droppedFile(e->mimeData());
    if (path.isEmpty()) return;
    e->acceptProposedAction();
    if (QFileInfo(path).suffix().compare("qrc", Qt::CaseInsensitive) != 0) { loadPlugin(path, false); return; }
    activateWindow();
    if (confirmDiscard()) openQrc(path);
}

void MainWindow::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        if (isFullScreen()) toggleFullscreen();
        else if (uiHidden_) setUiHidden(false);
        return;
    }
    QMainWindow::keyPressEvent(e);
}

bool MainWindow::openQrc(const QString &path, bool quiet) {
    try {
        qrc::Archive a = qrc::Archive::load(fsPath(path));
        sim::Params test;
        test.load(a);                                  // validates the parameter files
        render::Assets assets = render::loadAssets(a); // decodes textures, transpiles the RSX programs
        const bool same = QFileInfo(path) == QFileInfo(qrcPath_);
        const bool wasOpen = archive_.has_value();
        archive_ = std::move(a);
        saved_ = *archive_;
        params_.load(*archive_);
        if (!same && !qrcPath_.isEmpty()) params_.edits.clear();
        qrcPath_ = path;
        view_->setAssets(std::move(assets));
        panel_->setArchive(&*archive_);
        shaders_->setArchive(&*archive_, &*saved_, params_.firmware);
        effects_->setArchive(&*archive_, &*saved_, params_.firmware);
        stack_->setCurrentWidget(view_);
        updateDock(wasOpen);
        params_.forceTick();
        updateTitle();
        saveSettings();
        const QString madeWith = archive_->contains(qrc::kNoteId)
                                     ? "  (" + QString::fromUtf8(QByteArray(reinterpret_cast<const char *>(archive_->file(qrc::kNoteId).data()),
                                                                         qsizetype(archive_->file(qrc::kNoteId).size())))
                                                   .section('\n', 0, 0).trimmed() + ")"
                                     : QString();
        statusBar()->showMessage("Loaded " + QDir::toNativeSeparators(path) + madeWith, 4000);
        return true;
    } catch (const std::exception &e) {
        if (!quiet) QMessageBox::warning(this, "Cannot open lines.qrc", QDir::toNativeSeparators(path) + "\n\n" + e.what());
        return false;
    }
}

void MainWindow::openDialog() {
    if (!confirmDiscard()) return;
    const QString start = qrcPath_.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation) : QFileInfo(qrcPath_).path();
    const QString p = QFileDialog::getOpenFileName(this, "Open lines.qrc", start, "XMB resources (*.qrc);;All files (*)");
    if (!p.isEmpty()) openQrc(p);
}

QString MainWindow::rpcs3Dir(bool ask) {
    QSettings s;
    QString dir = s.value("rpcs3Dir").toString();
    if ((dir.isEmpty() || !QFileInfo::exists(dir + "/" + kQrcRel)) && ask) {
        dir = QFileDialog::getExistingDirectory(this, "Select your RPCS3 folder (the one with the RPCS3 binary and dev_flash)", dir);
        if (dir.isEmpty()) return {};
        if (!QFileInfo::exists(dir + "/" + kQrcRel)) {
            QMessageBox::warning(this, "Not an RPCS3 folder", "No " + QString(kQrcRel) + " in\n" + QDir::toNativeSeparators(dir) +
                                                                   "\n\nInstall the PS3 firmware in RPCS3 first (File > Install Firmware).");
            return {};
        }
        s.setValue("rpcs3Dir", dir);
    }
    return dir;
}

void MainWindow::openFromRpcs3() {
    if (!confirmDiscard()) return;
    const QString dir = rpcs3Dir(true);
    if (dir.isEmpty()) return;
    if (!openQrc(dir + "/" + kQrcRel)) return;
    if (!view_->exactRopeStates())   // RPCS3's "Decrypt PS3 Binaries" writes the PRX next to the SPRX
        for (const char *n : {"custom_render_plugin.prx", "custom_render_plugin.elf", "custom_render_plugin.sprx.elf"})
            if (QFileInfo::exists(dir + "/dev_flash/vsh/module/" + n) && loadPlugin(dir + "/dev_flash/vsh/module/" + n, true)) break;
}

void MainWindow::applyEditsTo(qrc::Archive &a) const {
    const auto missing = sim::applyEdits(a, params_.edits);
    if (!missing.empty()) {
        QString list;
        for (const auto &k : missing) list += "\n  " + QString::fromStdString(k);
        statusBar()->showMessage(QString("%1 edited values are not in lines.qrc and were not saved").arg(missing.size()), 8000);
        qWarning("not in lines.qrc:%s", qPrintable(list));
    }
}

bool MainWindow::saveTo(const QString &path) {
    if (!archive_) return false;
    try {
        qrc::Archive out = *archive_;
        applyEditsTo(out);
        qrc::addNote(out);
        const QString orig = path + ".orig";            // first save over a file keeps the original next to it
        if (QFileInfo::exists(path) && !QFileInfo::exists(orig)) QFile::copy(path, orig);
        out.saveTo(fsPath(path));
        archive_ = std::move(out);
        saved_ = *archive_;
        params_.load(*archive_);
        params_.edits.clear();                          // the values are in the file now
        qrcPath_ = path;
        panel_->setArchive(&*archive_);
        shaders_->setArchive(&*archive_, &*saved_, params_.firmware);
        effects_->setArchive(&*archive_, &*saved_, params_.firmware);
        updateTitle();
        saveSettings();
        statusBar()->showMessage("Saved " + QDir::toNativeSeparators(path), 5000);
        return true;
    } catch (const std::exception &e) {
        QMessageBox::warning(this, "Save failed", QDir::toNativeSeparators(path) + "\n\n" + e.what());
        return false;
    }
}

bool MainWindow::save() {
    if (!archive_) return false;
    return saveTo(qrcPath_);
}

bool MainWindow::saveAs() {
    if (!archive_) return false;
    const QString p = QFileDialog::getSaveFileName(this, "Save lines.qrc as", qrcPath_, "XMB resources (*.qrc)");
    return !p.isEmpty() && saveTo(p);
}

void MainWindow::installToRpcs3() {
    if (!archive_) return;
    const QString dir = rpcs3Dir(true);
    if (dir.isEmpty()) return;
    const QString target = dir + "/" + kQrcRel;
    if (QMessageBox::question(this, "Install to RPCS3",
                              "Write this lines.qrc (with your edits) to\n" + QDir::toNativeSeparators(target) +
                                  "\n\nThe current file is kept as lines.qrc.orig the first time. Restart RPCS3's XMB to see it.") !=
        QMessageBox::Yes)
        return;
    saveTo(target);
}

bool MainWindow::loadPlugin(const QString &path, bool quiet) {
    std::string err;
    sim::RopeStates st = sim::RopeStates::fromPrx(fsPath(path), &err);
    if (!st.valid()) {
        if (!quiet) QMessageBox::warning(this, "Cannot read rope tables", QString::fromStdString(err));
        return false;
    }
    view_->setRopeStates(std::move(st));
    QSettings().setValue("prxPath", path);
    statusBar()->showMessage("Rope start states loaded from " + QFileInfo(path).fileName(), 5000);
    return true;
}

void MainWindow::loadPluginDialog() {
    QMessageBox::information(this, "Rope start states",
                             "The wave's three start states are stored in custom_render_plugin, not in lines.qrc. Without them the "
                             "rope starts straight and settles for 100 simulated seconds, which looks right but is not the PS3's "
                             "exact start.\n\nSelect a decrypted custom_render_plugin (.prx or .elf). RPCS3 can make one: "
                             "Utilities > Decrypt PS3 Binaries, then pick dev_flash/vsh/module/custom_render_plugin.sprx.");
    const QString p = QFileDialog::getOpenFileName(this, "Decrypted custom_render_plugin", rpcs3Dir(false), "PRX / ELF (*.prx *.elf);;All files (*)");
    if (!p.isEmpty()) loadPlugin(p, false);
}

void MainWindow::copyEditsAsText() {
    QString t = "# moyou wave edits: moyou-cli patch lines.qrc edits.txt lines_new.qrc\n";
    for (const auto &[k, v] : params_.edits) t += QString::fromStdString(k) + " = " + QString::number(double(float(v)), 'g', 7) + "\n";
    QApplication::clipboard()->setText(t);
    statusBar()->showMessage(QString("Copied %1 edits").arg(params_.edits.size()), 3000);
}

void MainWindow::screenshot() {
    const QImage img = view_->grabDisplay();
    if (img.isNull()) return;
    const QString dir = QSettings().value("shotDir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    const QString name = dir + "/moyou-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".png";
    const QString p = QFileDialog::getSaveFileName(this, "Save screenshot (1920×1080)", name, "PNG (*.png)");
    if (p.isEmpty()) return;
    if (!img.save(p)) { QMessageBox::warning(this, "Screenshot", "Could not write " + QDir::toNativeSeparators(p)); return; }
    QSettings().setValue("shotDir", QFileInfo(p).path());
    statusBar()->showMessage("Saved " + QDir::toNativeSeparators(p), 4000);
}

void MainWindow::resetAll() {
    params_.edits.clear();
    params_.S = sim::Settings{};
    params_.syncClockFromDate();
    params_.forceTick();
    view_->resetRope();
    view_->restartParticles();
    panel_->syncSettings();
    panel_->syncAll();
    updateTitle();
    saveSettings();
}

void MainWindow::setUiHidden(bool hidden) {
    if (hidden && !uiHidden_) shaderDockWasVisible_ = shaderDock_->isVisible();
    uiHidden_ = hidden;
    dock_->setVisible(!hidden && archive_);
    shaderDock_->setVisible(!hidden && archive_ && shaderDockWasVisible_);
    menuBar()->setVisible(!hidden);
    statusBar()->setVisible(!hidden);
}

void MainWindow::updateDock(bool wasOpen) {
    dock_->toggleViewAction()->setEnabled(archive_.has_value());
    shaderDock_->toggleViewAction()->setEnabled(archive_.has_value());
    if (!archive_) { dock_->hide(); shaderDock_->hide(); }
    else if (!wasOpen && !uiHidden_) dock_->show();   // first file: bring the tabs in (later M toggles stick)
}

void MainWindow::toggleFullscreen() {
    if (isFullScreen()) { showNormal(); setUiHidden(false); }
    else { setUiHidden(true); showFullScreen(); }
}

bool MainWindow::unsaved() const { return archive_ && (!params_.edits.empty() || shaders_->hasModifiedPrograms()); }

QString MainWindow::replaceProgram(const std::string &id, const std::vector<uint8_t> &bytes) {
    if (!archive_) return tr("no lines.qrc is open");
    const std::vector<uint8_t> old = archive_->file(id);
    archive_->replace(id, bytes);
    try {
        const QString err = view_->replaceAssets(render::loadAssets(*archive_));
        if (!err.isEmpty()) { archive_->replace(id, old); return err; }
    } catch (const std::exception &e) {
        archive_->replace(id, old);
        return QString::fromStdString(e.what());
    }
    updateTitle();
    return {};
}

QString MainWindow::applyShaderListing(const std::string &id, const std::string &text) {
    if (!archive_ || !archive_->contains(id)) return tr("no such program");
    try {
        const auto &orig = archive_->file(id);
        return replaceProgram(id, rsx::write(rsx::assemble(text, rsx::parse(orig)), orig));
    } catch (const std::exception &e) {
        return QString::fromStdString(e.what());
    }
}

bool MainWindow::confirmDiscard() {
    if (!unsaved()) return true;
    QString what;
    if (!params_.edits.empty()) what = tr("%n edited value(s)", nullptr, int(params_.edits.size()));
    if (shaders_->hasModifiedPrograms()) what += (what.isEmpty() ? "" : tr(" and ")) + tr("edited shaders");
    const auto r = QMessageBox::question(this, "Unsaved edits", tr("You have %1 not saved to lines.qrc. Save them first?").arg(what),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Save) return save();
    return r == QMessageBox::Discard;
}

void MainWindow::markDirty() { updateTitle(); saveSettings(); }

void MainWindow::updateTitle() {
    // Just the file name: the platform adds the application name itself where that is the convention (KWin and
    // the Qt Wayland backend append it), and Qt turns the [*] placeholder into the modified marker.
    const QString f = qrcPath_.isEmpty() ? QString("no file") : QFileInfo(qrcPath_).fileName();
    setWindowTitle(f + "[*]");
    setWindowModified(unsaved());
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (!confirmDiscard()) { e->ignore(); return; }
    saveSettings();
    QSettings s;
    s.setValue("geometry", saveGeometry());
    s.setValue("windowState", saveState());
    e->accept();
}

void MainWindow::saveSettings() {
    QSettings s;
    const sim::Settings &S = params_.S;
    s.setValue("qrcPath", qrcPath_);
    s.beginGroup("view");
    s.setValue("follow", S.follow); s.setValue("dayFrac", S.dayFrac); s.setValue("month", S.month); s.setValue("lapse", S.lapse);
    s.setValue("preset", QString::fromStdString(S.preset)); s.setValue("view", S.view); s.setValue("showFfd", S.showFfd);
    s.setValue("showGrid", S.showGrid); s.setValue("state", S.state); s.setValue("simSpeed", S.simSpeed);
    s.setValue("glareOn", S.glareOn); s.setValue("particlesOn", S.particlesOn);
    s.setValue("brightLevel", S.brightLevel);
    s.endGroup();
    QVariantMap m;   // unsaved edits survive a restart (for the file they were made on)
    for (const auto &[k, v] : params_.edits) m[QString::fromStdString(k)] = v;
    s.setValue("edits", m);
    s.setValue("editsFor", qrcPath_);
}

void MainWindow::loadSettings() {
    QSettings s;
    restoreGeometry(s.value("geometry").toByteArray());
    restoreState(s.value("windowState").toByteArray());
    sim::Settings &S = params_.S;
    s.beginGroup("view");
    S.follow = s.value("follow", S.follow).toBool(); S.dayFrac = s.value("dayFrac", S.dayFrac).toDouble();
    S.month = s.value("month", S.month).toDouble(); S.lapse = s.value("lapse", S.lapse).toDouble();
    S.preset = s.value("preset", "schedule").toString().toStdString(); S.view = s.value("view", 0).toInt();
    S.showFfd = s.value("showFfd", false).toBool(); S.showGrid = s.value("showGrid", false).toBool();
    S.state = s.value("state", 1).toInt(); S.simSpeed = s.value("simSpeed", 1.0).toDouble();
    S.glareOn = s.value("glareOn", true).toBool(); S.particlesOn = s.value("particlesOn", true).toBool();
    S.brightLevel = s.value("brightLevel", 0).toInt();
    S.paused = false;
    s.endGroup();
    if (S.follow) params_.syncClockFromDate();
    panel_->syncSettings();

    const QString last = s.value("qrcPath").toString();
    // read before opening: openQrc saves the settings, which would overwrite them with the (still empty) edits
    const QVariantMap savedEdits = s.value("editsFor").toString() == last ? s.value("edits").toMap() : QVariantMap();
    if (!last.isEmpty() && QFileInfo::exists(last) && openQrc(last, true)) {
        for (auto it = savedEdits.begin(); it != savedEdits.end(); ++it) params_.edits[it.key().toStdString()] = it.value().toDouble();
        if (!savedEdits.isEmpty()) {
            params_.forceTick();
            markDirty();   // title shows the unsaved edits; settings keep them
        }
        const QString prx = s.value("prxPath").toString();
        if (!prx.isEmpty() && QFileInfo::exists(prx)) loadPlugin(prx, true);
    } else {
        panel_->setArchive(nullptr);
        shaders_->setArchive(nullptr, nullptr, 493);
        effects_->setArchive(nullptr, nullptr, 493);
    }
    updateTitle();
}

}  // namespace moyou::app
