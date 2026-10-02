#include "db/AuditLog.h"

#include "db/Database.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace db {

QString actionToString(Action action)
{
    switch (action) {
    case Action::Login:      return QStringLiteral("login");
    case Action::LoginFail:  return QStringLiteral("login_fail");
    case Action::Logout:     return QStringLiteral("logout");
    case Action::List:       return QStringLiteral("list");
    case Action::Download:   return QStringLiteral("download");
    case Action::UserCreate: return QStringLiteral("user_create");
    case Action::Upload:     return QStringLiteral("upload");
    case Action::Delete:     return QStringLiteral("delete");
    case Action::Mkdir:      return QStringLiteral("mkdir");
    }
    return QStringLiteral("unknown");
}

AuditLog::AuditLog(Database* database)
    : m_database(database)
{
}

void AuditLog::setClock(std::function<qint64()> secondsSinceEpoch)
{
    m_clock = std::move(secondsSinceEpoch);
}

qint64 AuditLog::now() const
{
    return m_clock ? m_clock() : QDateTime::currentSecsSinceEpoch();
}

bool AuditLog::record(const AuditEntry& entry)
{
    if (!m_database || !m_database->isOpen())
        return false;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral(
        "INSERT INTO audit (ts, user_id, action, path, bytes, status, ip) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)"));

    query.addBindValue(now());

    // NULL rather than 0 for an unidentified actor. The column is a foreign key with
    // ON DELETE SET NULL, so 0 would not merely read oddly — the insert would be
    // rejected outright, since no user has that id.
    query.addBindValue(entry.userId > 0 ? QVariant(entry.userId) : QVariant());

    query.addBindValue(actionToString(entry.action));
    query.addBindValue(entry.path.isEmpty() ? QVariant() : QVariant(entry.path));
    query.addBindValue(entry.bytes >= 0 ? QVariant(entry.bytes) : QVariant());
    query.addBindValue(entry.status > 0 ? QVariant(entry.status) : QVariant());
    query.addBindValue(entry.address.isEmpty() ? QVariant() : QVariant(entry.address));

    if (query.exec())
        return true;

    // Reported rather than returned upward. Every caller is on a path where the
    // useful work has already succeeded, and failing a download because its log row
    // did not land would be the wrong trade.
    qWarning() << "[Audit] Could not record"
               << actionToString(entry.action) << ":" << query.lastError().text();
    return false;
}

bool AuditLog::recordLogin(int userId, const QString& address)
{
    return record({ Action::Login, userId, {}, -1, 200, address });
}

bool AuditLog::recordLoginFailure(int userId, const QString& address, int status)
{
    return record({ Action::LoginFail, userId, {}, -1, status, address });
}

bool AuditLog::recordLogout(int userId, const QString& address)
{
    return record({ Action::Logout, userId, {}, -1, 200, address });
}

bool AuditLog::recordList(int userId, const QString& path, const QString& address,
                          int status)
{
    return record({ Action::List, userId, path, -1, status, address });
}

bool AuditLog::recordDownload(int userId, const QString& path, qint64 bytes,
                              const QString& address, int status)
{
    return record({ Action::Download, userId, path, bytes, status, address });
}

bool AuditLog::recordUserCreate(int createdUserId, const QString& address)
{
    return record({ Action::UserCreate, createdUserId, {}, -1, 200, address });
}

int AuditLog::sweepOlderThan(int days)
{
    if (!m_database || !m_database->isOpen() || days <= 0)
        return 0;

    const qint64 cutoff = now() - qint64(days) * 24 * 60 * 60;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral("DELETE FROM audit WHERE ts < ?"));
    query.addBindValue(cutoff);

    return query.exec() ? query.numRowsAffected() : 0;
}

int AuditLog::count() const
{
    if (!m_database || !m_database->isOpen())
        return 0;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM audit")) || !query.next())
        return 0;

    return query.value(0).toInt();
}

} // namespace db
