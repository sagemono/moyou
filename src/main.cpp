#include "app/icon.h"
#include "app/effectspanel.h"
#include "app/mainwindow.h"
#include "app/snapshot.h"

#include <QApplication>
#include <QDockWidget>
#include <QFile>
#include <QSettings>

#include <clocale>
#include <cstdio>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>

int main(int argc, char **argv) {
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(4, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    // On Unix, QApplication applies the user's locale to the C library. Numbers in .mnu files, effect specs and
    // assembler literals are always written with a '.', and std::stod / snprintf have to keep reading and writing it.
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setOrganizationName("moyou");
    // --selftest keeps its own settings so it never touches the user's last file or view options
    QApplication::setApplicationName(QApplication::arguments().contains("--selftest") ? "moyou-selftest" : "moyou");
    QApplication::setApplicationDisplayName("moyou");
    QApplication::setApplicationVersion(MOYOU_VERSION);
    QApplication::setWindowIcon(moyou::app::appIcon());

    if (QApplication::arguments().contains("--snapshot")) return moyou::app::snapshot(QApplication::arguments());

    const QStringList args = QApplication::arguments();
    const bool selftest = args.contains("--selftest");
    // --selftest --time HH:MM: hold the clock there (written to the selftest's own settings before the window reads them)
    if (const int t = int(args.indexOf("--time")); selftest && t >= 0 && t + 1 < args.size()) {
        const QStringList hm = args[t + 1].split(':');
        QSettings s;
        s.setValue("view/follow", false);
        s.setValue("view/dayFrac", (hm.value(0).toDouble() + hm.value(1).toDouble() / 60) / 24);
    }

    // --selftest lines.qrc --edit KEY=V ...: start with these edits, restored the way unsaved edits are after a restart
    if (selftest && args.contains("--edit")) {
        QString qrc;
        for (const QString &a : args.mid(1))
            if (a.endsWith(".qrc", Qt::CaseInsensitive)) { qrc = a; break; }
        QVariantMap edits;
        for (int k = 0; k + 1 < args.size(); k++)
            if (args[k] == "--edit") {
                const QStringList kv = args[k + 1].split('=');
                edits[kv.value(0).trimmed()] = kv.value(1).toDouble();
            }
        QSettings s;
        s.setValue("qrcPath", qrc);
        s.setValue("editsFor", qrc);
        s.setValue("edits", edits);
    }

    moyou::app::MainWindow w;
    // --selftest --size WxH: the window's client size, e.g. for README screenshots
    if (const int z = int(args.indexOf("--size")); selftest && z >= 0 && z + 1 < args.size()) {
        const QStringList wh = args[z + 1].split('x');
        w.resize(wh.value(0).toInt(), wh.value(1).toInt());
    }
    w.show();
    for (const QString &a : QApplication::arguments().mid(1))   // moyou path/to/lines.qrc
        if (a.endsWith(".qrc", Qt::CaseInsensitive)) { w.openFile(a); break; }
    // --selftest out.png: run the real window for a few seconds, save a grab of it and quit (smoke test / CI)
    if (const int i = int(QApplication::arguments().indexOf("--selftest")); i >= 0 && i + 1 < QApplication::arguments().size()) {
        const QString out = QApplication::arguments()[i + 1];
        if (const int t = int(QApplication::arguments().indexOf("--tab")); t >= 0 && t + 1 < QApplication::arguments().size())
            if (auto *tabs = w.findChild<QTabWidget *>()) tabs->setCurrentIndex(QApplication::arguments()[t + 1].toInt());
        if (const int a = int(QApplication::arguments().indexOf("--apply")); a >= 0 && a + 2 < QApplication::arguments().size()) {
            const std::string id = QApplication::arguments()[a + 1].toStdString();
            QFile f(QApplication::arguments()[a + 2]);
            if (!f.open(QIODevice::ReadOnly)) {
                std::fprintf(stderr, "cannot read %s\n", qPrintable(f.fileName()));
                return 2;
            }
            const std::string text = f.readAll().toStdString();
            QTimer::singleShot(1500, &w, [&w, id, text] {
                const QString e = w.applyShaderListing(id, text);
                std::fprintf(stderr, "apply %s: %s\n", id.c_str(), e.isEmpty() ? "ok" : qPrintable(e));
            });
        }
        if (const int e = int(QApplication::arguments().indexOf("--effects")); e >= 0 && e + 1 < QApplication::arguments().size()) {
            moyou::render::EffectSettings fx;
            try {
                fx = moyou::render::parseEffectSpec(QApplication::arguments()[e + 1].toStdString());
            } catch (const std::exception &ex) {
                std::fprintf(stderr, "--effects: %s\n", ex.what());
                return 2;
            }
            QTimer::singleShot(1500, &w, [&w, fx] {
                if (auto *p = w.findChild<moyou::app::EffectsPanel *>()) p->setSettings(fx);
            });
        }
        if (const int s = int(QApplication::arguments().indexOf("--save")); s >= 0 && s + 1 < QApplication::arguments().size()) {
            const QString path = QApplication::arguments()[s + 1];
            QTimer::singleShot(2500, &w, [&w, path] { std::fprintf(stderr, "save: %s\n", w.saveFileAs(path) ? "ok" : "failed"); });
        }
        if (auto *p = w.findChild<moyou::app::EffectsPanel *>())   // after loading: what the Effects tab shows
            QTimer::singleShot(1000, &w, [p] { std::fprintf(stderr, "effects on load: '%s'\n", p->spec().c_str()); });
        if (QApplication::arguments().contains("--shaders"))
            if (auto *d = w.findChild<QDockWidget *>("shaderDock")) d->show();
        // --delay MS: when to grab (default 4000; the "Loaded ..." status message shows for the first 4 s)
        const int d = int(args.indexOf("--delay"));
        const int delay = d >= 0 && d + 1 < args.size() ? args[d + 1].toInt() : 4000;
        QTimer::singleShot(delay, &w, [&w, out] { w.grab().save(out); QApplication::exit(0); });
    }
    return app.exec();
}
