#include "app/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace {

/** Returns @p path, creating it if needed. Empty string when creation fails. */
QString ensureDirectory(const QString& path)
{
    if (path.isEmpty())
        return QString();

    if (!QDir().mkpath(path))
        return QString();

    return path;
}

} // namespace

namespace app {

void configureApplicationMetadata()
{
    // QStandardPaths builds its locations from these, so they have to be set
    // before any of them is queried.
    QCoreApplication::setApplicationName(QStringLiteral("NestDrive"));

    // organizationName is deliberately left unset. With it set, AppDataLocation
    // becomes "<AppData>/<Organization>/<Application>", and since both names
    // would be "NestDrive" that yields a pointless nested
    // "NestDrive/NestDrive/" directory. Leaving it empty gives a plain
    // "<AppData>/NestDrive". Nothing here relies on it: config.ini is opened by
    // explicit path rather than through QSettings' native location.
}

namespace paths {

QString dataDirectory()
{
    return ensureDirectory(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
}

QString cacheDirectory()
{
    return ensureDirectory(
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
}

QString databaseFile()
{
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty())
        return QString();

    // Not created here: Database::open() makes the parent directory itself, and
    // duplicating that would mean two places to keep in step.
    return QDir(directory).absoluteFilePath(QStringLiteral("nestdrive.sqlite"));
}

QString thumbnailDirectory()
{
    const QString directory = cacheDirectory();
    if (directory.isEmpty())
        return QString();

    return QDir(directory).absoluteFilePath(QStringLiteral("thumbs"));
}

} // namespace paths
} // namespace app
