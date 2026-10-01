#include <QApplication>
#include <QDebug>

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
