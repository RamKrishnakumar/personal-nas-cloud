#include "auth/LoginRateLimiter.h"

#include <QDateTime>

#include <algorithm>

namespace {

/** Bucket key for a username. Folded so case variants share one budget. */
QString usernameKey(const QString& username)
{
    return username.trimmed().toCaseFolded();
}

} // namespace

namespace auth {

qint64 LoginRateLimiter::now() const
{
    // Takes the lock because m_clock is replaceable. Never called while the lock is
    // already held — QMutex is not recursive, and the public methods read the clock
    // before locking for exactly that reason.
    std::function<qint64()> clock;
    {
        QMutexLocker locker(&m_mutex);
        clock = m_clock;
    }

    return clock ? clock() : QDateTime::currentSecsSinceEpoch();
}

void LoginRateLimiter::setClock(std::function<qint64()> secondsSinceEpoch)
{
    QMutexLocker locker(&m_mutex);
    m_clock = std::move(secondsSinceEpoch);
}

void LoginRateLimiter::clear()
{
    QMutexLocker locker(&m_mutex);
    m_usernameFailures.clear();
    m_addressFailures.clear();
}

int LoginRateLimiter::prune(Attempts& attempts, qint64 cutoff)
{
    // Timestamps are appended in order, so everything expired is at the front.
    auto firstLive = std::lower_bound(attempts.begin(), attempts.end(), cutoff);
    attempts.erase(attempts.begin(), firstLive);
    return int(attempts.size());
}

int LoginRateLimiter::retryAfter(const Attempts& attempts, int limit, qint64 now)
{
    if (attempts.size() < qsizetype(limit))
        return 0;

    // Freed when the attempt that pushed the count to the limit leaves the window.
    const qint64 blocking = attempts.at(attempts.size() - limit);
    const qint64 freeAt   = blocking + WindowSeconds;

    return int(std::max<qint64>(1, freeAt - now));
}

RateLimitDecision LoginRateLimiter::check(const QString& username,
                                          const QString& address) const
{
    const QString key    = usernameKey(username);
    const qint64  moment = now();
    const qint64  cutoff = moment - WindowSeconds;

    QMutexLocker locker(&m_mutex);

    // Copied and pruned locally rather than mutating: a check must not change
    // state, so that calling it twice cannot produce two different answers.
    const auto liveAttempts = [cutoff](const Attempts& stored) {
        Attempts live;
        for (qint64 timestamp : stored) {
            if (timestamp >= cutoff)
                live.append(timestamp);
        }
        return live;
    };

    const Attempts userAttempts =
        liveAttempts(m_usernameFailures.value(key));
    if (userAttempts.size() >= qsizetype(MaxFailuresPerUser)) {
        return { false, retryAfter(userAttempts, MaxFailuresPerUser, moment) };
    }

    const Attempts addressAttempts =
        liveAttempts(m_addressFailures.value(address));
    if (addressAttempts.size() >= qsizetype(MaxFailuresPerAddress)) {
        return { false, retryAfter(addressAttempts, MaxFailuresPerAddress, moment) };
    }

    return { true, 0 };
}

void LoginRateLimiter::recordFailure(const QString& username, const QString& address)
{
    const QString key    = usernameKey(username);
    const qint64  moment = now();
    const qint64  cutoff = moment - WindowSeconds;

    QMutexLocker locker(&m_mutex);

    // Only the two buckets this attempt touches are pruned. Sweeping the whole
    // table on every failed login would make each attempt cost as much as the
    // number of names being tracked — which is precisely the quantity an attacker
    // controls.
    auto existing = m_usernameFailures.find(key);
    if (existing != m_usernameFailures.end()) {
        prune(existing.value(), cutoff);
        existing.value().append(moment);
    } else {
        // A table at its cap is swept once, in the hope of making room, rather than
        // on every attempt.
        if (m_usernameFailures.size() >= MaxTrackedUsernames)
            sweepExpiredLocked(cutoff);

        // A new username is only tracked while there is room. Past the cap the
        // per-username budget degrades, and the per-address bucket below is what
        // makes that safe: it is never capped, because addresses are few.
        if (m_usernameFailures.size() < MaxTrackedUsernames)
            m_usernameFailures[key].append(moment);
    }

    if (!address.isEmpty()) {
        auto& attempts = m_addressFailures[address];
        prune(attempts, cutoff);
        attempts.append(moment);
    }
}

void LoginRateLimiter::sweepExpiredLocked(qint64 cutoff)
{
    for (auto it = m_usernameFailures.begin(); it != m_usernameFailures.end();) {
        if (prune(it.value(), cutoff) == 0)
            it = m_usernameFailures.erase(it);
        else
            ++it;
    }

    for (auto it = m_addressFailures.begin(); it != m_addressFailures.end();) {
        if (prune(it.value(), cutoff) == 0)
            it = m_addressFailures.erase(it);
        else
            ++it;
    }
}

void LoginRateLimiter::recordSuccess(const QString& username)
{
    QMutexLocker locker(&m_mutex);
    m_usernameFailures.remove(usernameKey(username));
}

int LoginRateLimiter::failuresForUsername(const QString& username) const
{
    const qint64 cutoff = now() - WindowSeconds;

    QMutexLocker locker(&m_mutex);
    int live = 0;
    for (qint64 timestamp : m_usernameFailures.value(usernameKey(username))) {
        if (timestamp >= cutoff)
            ++live;
    }
    return live;
}

int LoginRateLimiter::failuresForAddress(const QString& address) const
{
    const qint64 cutoff = now() - WindowSeconds;

    QMutexLocker locker(&m_mutex);
    int live = 0;
    for (qint64 timestamp : m_addressFailures.value(address)) {
        if (timestamp >= cutoff)
            ++live;
    }
    return live;
}

} // namespace auth
