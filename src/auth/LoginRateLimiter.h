#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>

#include <functional>

namespace auth {

struct RateLimitDecision
{
    bool allowed            = true;
    int  retryAfterSeconds  = 0;   ///< Only meaningful when @c allowed is false.
};

/**
 * @class LoginRateLimiter
 * @brief Caps how often a password may be guessed.
 *
 * Entirely in memory and deliberately so: a counter per attempt would mean a
 * database write on the one path an attacker controls the rate of. Losing the
 * counters on restart is an acceptable trade — restarting NestDrive is not
 * something a remote attacker can do.
 *
 * ### Two independent buckets
 *
 * Per username, because that is the account being attacked. Per source address,
 * because otherwise an attacker sweeps a thousand usernames at four attempts each
 * and never trips the per-username limit.
 *
 * Usernames are case-folded for bucketing. Without that, `admin`, `Admin` and
 * `ADMIN` would be three separate budgets for one account.
 *
 * Thread-safe: every worker thread can reach it.
 */
class LoginRateLimiter
{
public:
    static constexpr int WindowSeconds          = 15 * 60;
    static constexpr int MaxFailuresPerUser     = 5;
    static constexpr int MaxFailuresPerAddress  = 20;

    /**
     * @brief Upper bound on tracked usernames.
     *
     * An attacker who invents a new username per attempt would otherwise grow
     * these tables without limit. Once the cap is reached, new usernames are not
     * tracked — the per-address bucket still stops them, and the addresses
     * actually attacking are few.
     */
    static constexpr int MaxTrackedUsernames = 10'000;

    /**
     * @brief Whether this attempt may proceed. Does not record anything.
     *
     * Checked before the password is hashed, so a blocked attempt costs no CPU.
     */
    RateLimitDecision check(const QString& username, const QString& address) const;

    /** Counts one failed attempt against both buckets. */
    void recordFailure(const QString& username, const QString& address);

    /**
     * @brief Clears the username's budget after a correct password.
     *
     * The address bucket is deliberately left alone. One success does not make the
     * preceding nineteen failures from that address innocent.
     */
    void recordSuccess(const QString& username);

    /** Replaces the time source. For tests, which cannot wait fifteen minutes. */
    void setClock(std::function<qint64()> secondsSinceEpoch);

    void clear();

    /** Tracked failure counts, for tests and diagnostics. */
    int failuresForUsername(const QString& username) const;
    int failuresForAddress(const QString& address) const;

private:
    using Attempts = QList<qint64>;   ///< Failure timestamps, oldest first.

    qint64 now() const;

    /** Drops timestamps outside the window and returns what remains. */
    static int prune(Attempts& attempts, qint64 cutoff);

    /** Drops every expired entry. Caller must hold the mutex. */
    void sweepExpiredLocked(qint64 cutoff);

    /** Seconds until @p attempts falls back under @p limit. */
    static int retryAfter(const Attempts& attempts, int limit, qint64 now);

    mutable QMutex m_mutex;

    QHash<QString, Attempts> m_usernameFailures;
    QHash<QString, Attempts> m_addressFailures;

    std::function<qint64()> m_clock;
};

} // namespace auth
