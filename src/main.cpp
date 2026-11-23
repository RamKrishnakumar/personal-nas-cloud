#include <QApplication>
#include <QDebug>
#include <QIcon>

#include "app/Paths.h"
#include "db/Database.h"
#include "ui/MainWindow.h"
#include "ui/TrayManager.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);   // Keep running in tray when window closed

    // Before anything touches QStandardPaths or QSettings — both derive their
    // locations from the application and organization names.
    app::configureApplicationMetadata();

    // Set on the application rather than per-window, so every window, dialog and
    // message box inherits it without having to remember to.
    //
    // This is the icon the running application shows — title bar, Alt-Tab, taskbar
    // button, Dock. It is NOT the icon the operating system shows for the installed
    // executable in Explorer or Finder: those are read from the file itself, which
    // needs a .ico compiled into the binary on Windows and a .icns inside the bundle
    // on macOS. Both are generated during packaging; see docs/CI-CD.md.
    //
    // SVG rather than a set of PNG sizes because QIcon renders it at whatever size is
    // asked for, so one file covers a 16px title bar and a 256px Alt-Tab preview with
    // no resampling. It needs Qt's SVG icon plugin, which is deployed on all three
    // platforms — and if it were ever missing, the icon is simply absent rather than
    // the application failing to start.
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/web/icon.svg")));

    // ── Persistence ───────────────────────────────────────────────────────
    // Opened here so it outlives every window, and so a schema problem is
    // reported at startup rather than on the first query.
    //
    // A failure is not fatal in v0.6: nothing yet depends on the database, and
    // refusing to start would take away LAN file access for a problem the user
    // cannot see. Once authentication lands in step 13 this becomes fatal,
    // because serving files without being able to check a session is worse than
    // not serving them at all.
    db::Database database;
    const QString databasePath = app::paths::databaseFile();

    if (databasePath.isEmpty()) {
        qWarning() << "[DB] ❌ No writable application data location. "
                      "Database features are disabled.";
    } else if (!database.open(databasePath)) {
        qWarning() << "[DB] ❌" << database.lastError();
        qWarning() << "[DB] ⚠️ Continuing without the database. "
                      "Authentication will require this to be fixed.";
    } else {
        qInfo() << "[DB] ✅ Opened" << database.filePath()
                << "| schema version" << database.schemaVersion();
    }

    MainWindow window(&database);
    window.show();

    TrayManager tray(&window);

    return app.exec();
}
