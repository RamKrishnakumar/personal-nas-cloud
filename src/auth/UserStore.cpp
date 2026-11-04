#include "auth/UserStore.h"

#include "auth/PasswordHash.h"
#include "db/Database.h"

#include <QDateTime>
#include <QSqlQuery>

namespace {

/** Builds a record from a row selecting id, username, pw_hash, role, enabled. */
std::optional<auth::UserRecord> recordFromRow(const QSqlQuery& query)
{
    const auto role = auth::roleFromString(query.value(3).toString());
    if (!role) {
        // The CHECK constraint makes this unreachable through this code, but a
        // hand-edited database should not become an account with undefined rights.
        return std::nullopt;
    }

    auth::UserRecord record;
    record.id           = query.value(0).toInt();
    record.username     = query.value(1).toString();
    record.passwordHash = query.value(2).toString();
    record.role         = *role;
    record.enabled      = query.value(4).toInt() != 0;
    return record;
}

constexpr auto SelectColumns = "id, username, pw_hash, role, enabled";

} // namespace

namespace auth {

QString roleToString(Role role)
{
    switch (role) {
    case Role::Admin:  return QStringLiteral("admin");
    case Role::Family: return QStringLiteral("family");
    case Role::Guest:  return QStringLiteral("guest");
    }
    return QStringLiteral("guest");
}

std::optional<Role> roleFromString(const QString& text)
{
    if (text == QLatin1String("admin"))  return Role::Admin;
    if (text == QLatin1String("family")) return Role::Family;
    if (text == QLatin1String("guest"))  return Role::Guest;
    return std::nullopt;
}

UserStore::UserStore(db::Database* database)
    : m_database(database)
{
}

QString UserStore::normaliseUsername(const QString& username)
{
    return username.trimmed().toCaseFolded();
}

std::optional<UserRecord> UserStore::findByUsername(const QString& username) const
{
    if (!m_database || !m_database->isOpen())
        return std::nullopt;

    const QString key = normaliseUsername(username);
    if (key.isEmpty())
        return std::nullopt;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral("SELECT %1 FROM users WHERE username = ?")
                      .arg(QLatin1String(SelectColumns)));
    query.addBindValue(key);

    if (!query.exec() || !query.next())
        return std::nullopt;

    return recordFromRow(query);
}

std::optional<UserRecord> UserStore::findById(int userId) const
{
    if (!m_database || !m_database->isOpen() || userId <= 0)
        return std::nullopt;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral("SELECT %1 FROM users WHERE id = ?")
                      .arg(QLatin1String(SelectColumns)));
    query.addBindValue(userId);

    if (!query.exec() || !query.next())
        return std::nullopt;

    return recordFromRow(query);
}

int UserStore::create(const QString& username,
                      const QString& password,
                      Role           role,
                      QString*       error)
{
    const auto fail = [error](const QString& reason) {
        if (error)
            *error = reason;
        return 0;
    };

    if (!m_database || !m_database->isOpen())
        return fail(QStringLiteral("The database is not open."));

    const QString key = normaliseUsername(username);
    if (key.isEmpty())
        return fail(QStringLiteral("A username is required."));

    if (password.length() < MinPasswordLength) {
        return fail(QStringLiteral("The password must be at least %1 characters.")
                        .arg(MinPasswordLength));
    }

    // Hashed before the lock is taken. Holding the database mutex for the second
    // and a half this costs would stall every other request on the server.
    const QString encoded = m_iterations > 0 ? hashPassword(password, m_iterations)
                                            : hashPassword(password);
    if (encoded.isEmpty())
        return fail(QStringLiteral("The password could not be hashed."));

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO users (username, pw_hash, role, enabled, created_at) "
        "VALUES (?, ?, ?, 1, ?)"));
    query.addBindValue(key);
    query.addBindValue(encoded);
    query.addBindValue(roleToString(role));
    query.addBindValue(QDateTime::currentSecsSinceEpoch());

    if (!query.exec()) {
        // Almost always the UNIQUE constraint. Reported as a taken name rather
        // than as raw driver text, which would leak the schema.
        return fail(QStringLiteral("That username is already taken."));
    }

    if (error)
        error->clear();

    return query.lastInsertId().toInt();
}

bool UserStore::updatePasswordHash(int userId, const QString& encodedHash)
{
    if (!m_database || !m_database->isOpen() || encodedHash.isEmpty())
        return false;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral("UPDATE users SET pw_hash = ? WHERE id = ?"));
    query.addBindValue(encodedHash);
    query.addBindValue(userId);
    return query.exec();
}

void UserStore::setEnabled(int userId, bool enabled)
{
    if (!m_database || !m_database->isOpen())
        return;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral("UPDATE users SET enabled = ? WHERE id = ?"));
    query.addBindValue(enabled ? 1 : 0);
    query.addBindValue(userId);
    query.exec();
}

void UserStore::recordLogin(int userId)
{
    if (!m_database || !m_database->isOpen())
        return;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral("UPDATE users SET last_login_at = ? WHERE id = ?"));
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.addBindValue(userId);
    query.exec();
}

int UserStore::count() const
{
    if (!m_database || !m_database->isOpen())
        return 0;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM users")) || !query.next())
        return 0;

    return query.value(0).toInt();
}

} // namespace auth
