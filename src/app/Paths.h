#pragma once

#include <QString>

namespace app {

/**
 * @brief Per-user locations NestDrive writes to.
 *
 * Deliberately not next to the executable. config.ini lives there for historical
 * reasons and because a user edits it by hand, but a database and a thumbnail
 * cache must not: Program Files is read-only for a standard account, and on
 * Linux and macOS an installed binary's directory is not writable either.
 *
 * All of these depend on QCoreApplication's organization and application names
 * being set before first use. See app::configureApplicationMetadata().
 */
namespace paths {

/** Directory for data that must survive, created if missing. Empty on failure. */
QString dataDirectory();

/** Directory for data that can be regenerated, created if missing. */
QString cacheDirectory();

/** Full path to the SQLite database. Its parent may not exist yet. */
QString databaseFile();

/** Directory holding generated thumbnails. Used from v0.6 step 10 onward. */
QString thumbnailDirectory();

/**
 * @brief Directory holding transcoded video renditions.
 *
 * Separate from the thumbnails, because the two differ by three orders of magnitude
 * in size and want their own ceilings — and because deleting one to reclaim space
 * should not throw away the other, which costs minutes per file to rebuild.
 */
QString videoDirectory();

} // namespace paths

/**
 * @brief Sets the application and organization names.
 *
 * Must be called before any paths:: function and before QSettings is used, since
 * both derive their locations from these values. Calling it late produces paths
 * under a directory named after the executable, which then differ between a
 * development build and an installed one.
 */
void configureApplicationMetadata();

} // namespace app
