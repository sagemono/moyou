#pragma once

#include "formats/qrc.h"
#include "sim/params.h"

#include <QMainWindow>

#include <optional>

class QDockWidget;
class QLabel;
class QMimeData;
class QStackedWidget;
class QTimer;

namespace moyou::app {

class ControlPanel;
class ShaderEditor;
class EffectsPanel;
class WaveView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    void openFile(const QString &path) { openQrc(path); }
    QString applyShaderListing(const std::string &id, const std::string &listing);   // --selftest --apply id file
    bool saveFileAs(const QString &path) { return saveTo(path); }                    // --selftest --save path

protected:
    void closeEvent(QCloseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void buildMenus();
    bool openQrc(const QString &path, bool quiet = false);
    void openDialog();
    void openFromRpcs3();
    bool save();
    bool saveAs();
    bool saveTo(const QString &path);
    void installToRpcs3();
    void loadPluginDialog();
    bool loadPlugin(const QString &path, bool quiet);
    void copyEditsAsText();
    void screenshot();
    void resetAll();
    void setUiHidden(bool hidden);
    void updateDock(bool wasOpen);   // the parameter dock only exists once a file is open
    QString replaceProgram(const std::string &id, const std::vector<uint8_t> &bytes);   // Shader Editor apply
    bool unsaved() const;
    QString droppedFile(const QMimeData *mime) const;
    void toggleFullscreen();
    bool confirmDiscard();
    void applyEditsTo(qrc::Archive &a) const;
    void markDirty();
    void updateTitle();
    void saveSettings();
    void loadSettings();
    void showNotice();   // the start-up warning; Exit quits
    QString rpcs3Dir(bool ask);

    sim::Params params_;
    std::optional<qrc::Archive> archive_;
    QString qrcPath_;
    WaveView *view_;
    ControlPanel *panel_;
    QDockWidget *dock_;
    ShaderEditor *shaders_;
    EffectsPanel *effects_;
    QDockWidget *shaderDock_;
    bool shaderDockWasVisible_ = false;
    std::optional<qrc::Archive> saved_;   // the file as last opened or saved (Shader Editor revert / modified marks)
    QStackedWidget *stack_;
    QLabel *status_;
    QTimer *ui_;
    bool uiHidden_ = false;
};

}  // namespace moyou::app
