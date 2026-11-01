#include "http/AdmissionControl.h"

#include <algorithm>
#include <utility>

namespace http {

// ─────────────────────────────────────────────────────────────────────────────
//  StreamSlot
// ─────────────────────────────────────────────────────────────────────────────

StreamSlot::StreamSlot(AdmissionControl* control, ClientId client, QString path)
    : m_control(control)
    , m_client(std::move(client))
    , m_path(std::move(path))
{
}

StreamSlot::StreamSlot(StreamSlot&& other) noexcept
    : m_control(other.m_control)
    , m_client(std::move(other.m_client))
    , m_path(std::move(other.m_path))
{
    // Clearing the source is what stops the moved-from object releasing the slot
    // when it is destroyed.
    other.m_control = nullptr;
}

StreamSlot& StreamSlot::operator=(StreamSlot&& other) noexcept
{
    if (this == &other)
        return *this;

    release();                      // Give up whatever this held first.

    m_control = other.m_control;
    m_client  = std::move(other.m_client);
    m_path    = std::move(other.m_path);

    other.m_control = nullptr;
    return *this;
}

StreamSlot::~StreamSlot()
{
    release();
}

void StreamSlot::release()
{
    if (!m_control)
        return;

    // Cleared before the call so a re-entrant release cannot decrement twice.
    AdmissionControl* control = m_control;
    m_control = nullptr;

    control->releaseSlot(m_client, m_path);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AdmissionControl
// ─────────────────────────────────────────────────────────────────────────────

AdmissionControl::AdmissionControl(int totalSlots, int perClientLimit, int adminReserve)
    : m_totalSlots(std::max(1, totalSlots))
    , m_perClientLimit(std::max(1, perClientLimit))
    , m_adminReserve(std::max(0, adminReserve))
{
}

qint64 AdmissionControl::slotThresholdBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_slotThresholdBytes;
}

void AdmissionControl::setSlotThresholdBytes(qint64 bytes)
{
    QMutexLocker locker(&m_mutex);
    m_slotThresholdBytes = std::max<qint64>(0, bytes);
}

AdmissionResult AdmissionControl::request(const ClientId& client,
                                         const QString&  path,
                                         qint64          fileSize,
                                         bool            isAdmin)
{
    AdmissionResult result;

    QMutexLocker locker(&m_mutex);

    // ── Too small to be worth controlling ─────────────────────────────────
    // Queueing a 200 KB file adds latency and saves nothing: it completes in a
    // blink and does not hold the disk. Browsing a folder of thumbnails
    // therefore consumes no slots at all.
    if (fileSize < m_slotThresholdBytes) {
        result.outcome = Admission::Bypassed;
        return result;
    }

    QHash<QString, int>& forClient = m_held[client];

    // ── Already streaming this exact file ─────────────────────────────────
    // The seek case. Granting without taking another slot is what stops a player
    // queueing behind its own previous connection.
    const auto existing = forClient.find(path);
    if (existing != forClient.end()) {
        ++existing.value();
        result.outcome = Admission::Granted;
        result.slot    = StreamSlot(this, client, path);
        return result;
    }

    // ── Per-client ceiling ────────────────────────────────────────────────
    if (forClient.size() >= m_perClientLimit) {
        if (forClient.isEmpty())
            m_held.remove(client);          // Do not leave an empty bucket behind.
        result.outcome = Admission::AtCapacity;
        return result;
    }

    // ── Global ceiling, with the administrator's reserve ──────────────────
    int active = 0;
    for (auto it = m_held.cbegin(); it != m_held.cend(); ++it)
        active += it.value().size();

    const int ceiling = isAdmin ? m_totalSlots + m_adminReserve : m_totalSlots;
    if (active >= ceiling) {
        if (forClient.isEmpty())
            m_held.remove(client);
        result.outcome = Admission::AtCapacity;
        return result;
    }

    forClient.insert(path, 1);
    result.outcome = Admission::Granted;
    result.slot    = StreamSlot(this, client, path);
    return result;
}

void AdmissionControl::releaseSlot(const ClientId& client, const QString& path)
{
    QMutexLocker locker(&m_mutex);

    const auto clientEntry = m_held.find(client);
    if (clientEntry == m_held.end())
        return;

    const auto pathEntry = clientEntry.value().find(path);
    if (pathEntry == clientEntry.value().end())
        return;

    if (--pathEntry.value() <= 0)
        clientEntry.value().erase(pathEntry);

    // Buckets are pruned so streamsFor() and activeStreams() cannot drift upward
    // over a long uptime.
    if (clientEntry.value().isEmpty())
        m_held.erase(clientEntry);
}

int AdmissionControl::activeStreams() const
{
    QMutexLocker locker(&m_mutex);

    int active = 0;
    for (auto it = m_held.cbegin(); it != m_held.cend(); ++it)
        active += it.value().size();
    return active;
}

int AdmissionControl::streamsFor(const ClientId& client) const
{
    QMutexLocker locker(&m_mutex);
    return m_held.value(client).size();
}

} // namespace http
