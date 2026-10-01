#include "auth/SessionStore.h"

#include "db/Database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QSqlQuery>

namespace {

constexpr QByteArray::Base64Options Base64Format =
    QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

} // namespace

namespace auth {

SessionStore::SessionStore(db::Database* database)
    : m_database(database)
{
    // Startup is the natural moment: rows for sessions that expired while the
    // server was off would otherwise sit there until something else swept them.
    sweepExpired();
}

qint64 SessionStore::now() const
{
    std::function<qint64()> clock;
    {
        QMutexLocker locker(&m_mutex);
        clock = m_clock;
    }

    return clock ? clock() : QDateTime::currentSecsSinceEpoch();
}

void SessionStore::setClock(std::function<qint64()> secondsSinceEpoch)
{
    QMutexLocker locker(&m_mutex);
    m_clock = std::move(secondsSinceEpoch);
}

QByteArray SessionStore::hashToken(QByteArrayView token)
{
    return QCryptographicHash::hash(token, QCryptographicHash::Sha256);
}

QByteArray SessionStore::generateToken()
{
    static_assert(TokenBytes % 4 == 0, "Token length must be a whole number of words");

    QByteArray raw(TokenBytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32*>(raw.data()), TokenBytes / 4);

    // base64url so the value is safe in a cookie without escaping.
    return raw.toBase64(Base64Format);
}

QByteArray SessionStore::create(int            userId,
                                qint64         idleSeconds,
                                const QString& address,
                                const QString& userAgent)
{
    if (!m_database || !m_database->isOpen() || userId <= 0 || idleSeconds <= 0)
        return {};

    const QByteArray token     = generateToken();
    const QByteArray tokenHash = hashToken(token);
    const qint64     moment    = now();

    {
        auto handle = m_database->lock();
        QSqlQuery query(handle.connection());
        query.prepare(QStringLiteral(
            "INSERT INTO sessions "
            "(token_hash, user_id, created_at, last_seen_at, expires_at, "
            " idle_seconds, ip, user_agent) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
        query.addBindValue(tokenHash);
        query.addBindValue(userId);
        query.addBindValue(moment);
        query.addBindValue(moment);
        query.addBindValue(moment + idleSeconds);
        query.addBindValue(idleSeconds);
        query.addBindValue(address);
        // Truncated: a client controls this string, and there is no reason to store
        // a megabyte of it per session.
        query.addBindValue(userAgent.left(512));

        if (!query.exec())
            return {};
    }

    {
        QMutexLocker locker(&m_mutex);
        m_lastRefreshed.insert(tokenHash, moment);
    }

    return token;
}

std::optional<Session> SessionStore::validate(QByteArrayView token)
{
    if (!m_database || !m_database->isOpen() || token.isEmpty())
        return std::nullopt;

    const QByteArray tokenHash = hashToken(token);
    const qint64     moment    = now();

    Session session;
    session.tokenHash = tokenHash;

    {
        auto handle = m_database->lock();
        QSqlQuery query(handle.connection());
        query.prepare(QStringLiteral(
            "SELECT user_id, expires_at, idle_seconds FROM sessions "
            "WHERE token_hash = ?"));
        query.addBindValue(tokenHash);

        if (!query.exec() || !query.next())
            return std::nullopt;

        session.userId      = query.value(0).toInt();
        session.expiresAt   = query.value(1).toLongLong();
        session.idleSeconds = query.value(2).toLongLong();
    }

    if (session.expiresAt <= moment) {
        destroy(token);
        return std::nullopt;
    }

    // Refreshed at most once a minute. The expiry is an idle window, so extending
    // it on every single request would be a database write per thumbnail.
    bool shouldRefresh = false;
    {
        QMutexLocker locker(&m_mutex);
        const qint64 lastWrite = m_lastRefreshed.value(tokenHash, 0);
        if (moment - lastWrite >= RefreshIntervalSeconds) {
            m_lastRefreshed.insert(tokenHash, moment);
            shouldRefresh = true;
        }
    }

    if (shouldRefresh) {
        const qint64 extendedTo = moment + session.idleSeconds;

        auto handle = m_database->lock();
        QSqlQuery query(handle.connection());
        query.prepare(QStringLiteral(
            "UPDATE sessions SET last_seen_at = ?, expires_at = ? "
            "WHERE token_hash = ?"));
        query.addBindValue(moment);
        query.addBindValue(extendedTo);
        query.addBindValue(tokenHash);
        if (query.exec())
            session.expiresAt = extendedTo;
    }

    return session;
}

bool SessionStore::destroy(QByteArrayView token)
{
    if (!m_database || !m_database->isOpen() || token.isEmpty())
        return false;

    const QByteArray tokenHash = hashToken(token);

    {
        QMutexLocker locker(&m_mutex);
        m_lastRefreshed.remove(tokenHash);
    }

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral("DELETE FROM sessions WHERE token_hash = ?"));
    query.addBindValue(tokenHash);

    return query.exec() && query.numRowsAffected() > 0;
}

int SessionStore::destroyAllForUser(int userId)
{
    if (!m_database || !m_database->isOpen() || userId <= 0)
        return 0;

    // The whole throttle map is dropped rather than filtered by user: it is a
    // write-throttling cache, so losing entries costs one extra write each and
    // keeping a stale one would skip a refresh that should have happened.
    {
        QMutexLocker locker(&m_mutex);
        m_lastRefreshed.clear();
    }

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral("DELETE FROM sessions WHERE user_id = ?"));
    query.addBindValue(userId);

    if (!query.exec())
        return 0;

    return query.numRowsAffected();
}

int SessionStore::sweepExpired()
{
    if (!m_database || !m_database->isOpen())
        return 0;

    const qint64 moment = now();

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    query.prepare(QStringLiteral("DELETE FROM sessions WHERE expires_at <= ?"));
    query.addBindValue(moment);

    if (!query.exec())
        return 0;

    return query.numRowsAffected();
}

int SessionStore::count() const
{
    if (!m_database || !m_database->isOpen())
        return 0;

    auto handle = m_database->lock();
    QSqlQuery query(handle.connection());
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM sessions")) || !query.next())
        return 0;

    return query.value(0).toInt();
}

} // namespace auth
