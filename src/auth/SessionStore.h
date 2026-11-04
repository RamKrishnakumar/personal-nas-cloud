#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QMutex>
#include <QString>

#include <functional>
#include <optional>

namespace db { class Database; }

namespace auth {

struct Session
{
    int        userId      = 0;
    qint64     expiresAt   = 0;      ///< Seconds since epoch.
    qint64     idleSeconds = 0;      ///< Window this session is refreshed by.
    QByteArray tokenHash;

    bool isValid() const { return userId > 0; }
};

/**
 * @class SessionStore
 * @brief Issues, validates and retires login sessions.
 *
 * ### Only the hash is stored
 *
 * The cookie carries 32 random bytes; the `sessions` table holds SHA-256 of them
 * and never the value itself. Someone who reads the database file therefore gets
 * no usable session, which is the same reasoning that applies to passwords — with
 * the difference that a session token is already high-entropy, so a plain hash is
 * enough and no work factor is needed.
 *
 * ### Expiry is idle-based
 *
 * Twelve hours by default, fourteen days when the user asked to be remembered. The
 * window is stored per session, so a refresh extends by the right amount. The
 * refresh write itself is throttled to once a minute per session: a single folder
 * view issues dozens of requests, and a database write on each would turn browsing
 * into a burst of writes for no benefit.
 *
 * Thread-safe. One instance is shared by every worker.
 */
class SessionStore
{
public:
    static constexpr qint64 DefaultIdleSeconds    = 12 * 60 * 60;
    static constexpr qint64 RememberMeIdleSeconds = 14 * 24 * 60 * 60;

    /** Token entropy. 256 bits is well past anything brute-forceable. */
    static constexpr int TokenBytes = 32;

    /** Shortest gap between two `last_seen_at` writes for one session. */
    static constexpr qint64 RefreshIntervalSeconds = 60;

    explicit SessionStore(db::Database* database);

    /**
     * @brief Creates a session and returns its token in plain text.
     *
     * This is the only moment the token exists outside the client; it is not
     * recoverable afterwards. Returns an empty array on failure.
     */
    QByteArray create(int            userId,
                      qint64         idleSeconds,
                      const QString& address,
                      const QString& userAgent);

    /**
     * @brief Looks up @p token, refreshing its expiry when it is due.
     *
     * An expired session is deleted rather than merely rejected, so the table does
     * not accumulate rows for sessions nobody will present again.
     */
    std::optional<Session> validate(QByteArrayView token);

    /** Deletes one session. Used by logout. */
    bool destroy(QByteArrayView token);

    /**
     * @brief Deletes every session belonging to @p userId.
     *
     * For a password change or a disabled account: changing a password that leaves
     * old sessions alive does not actually lock anyone out.
     */
    int destroyAllForUser(int userId);

    /** Deletes expired rows. Called on construction and on a timer. */
    int sweepExpired();

    int count() const;

    /** SHA-256 of @p token. The form stored in the table. */
    static QByteArray hashToken(QByteArrayView token);

    /** A fresh base64url token from the system CSPRNG. */
    static QByteArray generateToken();

    /** Replaces the time source, for tests that need to reach an expiry. */
    void setClock(std::function<qint64()> secondsSinceEpoch);

private:
    qint64 now() const;

    db::Database* m_database = nullptr;

    mutable QMutex m_mutex;

    /// Last `last_seen_at` write per session, so refreshes can be throttled
    /// without reading the stored value back first.
    QHash<QByteArray, qint64> m_lastRefreshed;

    std::function<qint64()> m_clock;
};

} // namespace auth
