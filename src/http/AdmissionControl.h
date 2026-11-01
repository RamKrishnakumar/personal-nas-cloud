#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>

namespace http {

/**
 * @brief Opaque identity a stream slot is charged to.
 *
 * A session identity, supplied by the auth gate; the peer address only as a
 * fallback for a request that reached a transfer without one. Nothing here
 * interprets the value, it only compares them, which is what made the move off IP
 * keying in step 15 a change to the callers alone.
 */
using ClientId = QByteArray;

class AdmissionControl;

/**
 * @class StreamSlot
 * @brief RAII hold on one transfer slot.
 *
 * Releasing on destruction is the whole point. A slot that outlives its transfer
 * does not look like a bug from the outside — the server simply stops serving
 * large files after a few interrupted downloads and appears to hang. Moving is
 * supported so a slot can be handed to the object that owns the transfer;
 * copying is not, because two owners would release twice.
 */
class StreamSlot
{
public:
    /** A slot that holds nothing. Also the state after a bypass. */
    StreamSlot() = default;
    ~StreamSlot();

    StreamSlot(StreamSlot&& other) noexcept;
    StreamSlot& operator=(StreamSlot&& other) noexcept;

    StreamSlot(const StreamSlot&)            = delete;
    StreamSlot& operator=(const StreamSlot&) = delete;

    bool isHeld() const { return m_control != nullptr; }

    /** Releases early. Safe to call more than once. */
    void release();

private:
    friend class AdmissionControl;
    StreamSlot(AdmissionControl* control, ClientId client, QString path);

    AdmissionControl* m_control = nullptr;
    ClientId          m_client;
    QString           m_path;
};

/** Outcome of an admission request. */
enum class Admission
{
    Granted,     ///< A slot is held; proceed.
    Bypassed,    ///< Below the size threshold; no slot needed, proceed.
    AtCapacity,  ///< Wait and retry, then answer 503.
};

struct AdmissionResult
{
    Admission  outcome = Admission::AtCapacity;
    StreamSlot slot;     ///< Held only when outcome is Granted.
};

/**
 * @class AdmissionControl
 * @brief Bounds concurrent transfers, so a slow machine stays usable.
 *
 * ### What is limited, and what deliberately is not
 *
 * Only sustained transfers. Logging in, listing a directory, reading a thumbnail
 * and conditional requests are never queued or refused — they cost microseconds
 * and no sustained disk bandwidth. Limiting connections instead would break a
 * single browser tab, which opens about six of them for parallel thumbnails;
 * capping how many sessions may exist would evict a phone when a laptop logs in.
 * Neither is the resource that makes the machine lag.
 *
 * ### Slots are charged per session and path
 *
 * The identity is a session, not an account and not an address. An account would
 * make a phone and a laptop belonging to the same person compete for one
 * allowance; an address puts a whole household behind one NAT into a single
 * bucket, so one person's download stalls everyone else's.
 *
 * A slot is keyed on (client, path) and reference counted. That makes the video
 * seek case work: a player closes its connection and immediately reopens with a
 * Range header on the same file, and without reference counting that second
 * request would queue behind the slot its own predecessor still holds — a
 * deadlock against itself, appearing as a stall that is very hard to attribute.
 *
 * Thread-safe: one instance is shared by every worker thread.
 */
class AdmissionControl
{
public:
    /** Transfers smaller than this need no slot. */
    static constexpr qint64 DefaultSlotThresholdBytes = 2 * 1024 * 1024;

    /**
     * @param totalSlots     Concurrent transfers allowed, from the hardware tier.
     * @param perClientLimit Concurrent transfers one session may hold. Keeps a
     *                       browser downloading six files at once from occupying
     *                       every slot; the total time is bandwidth-bound anyway,
     *                       and fewer parallel reads suit a spinning disk better.
     * @param adminReserve   Slots **above** the shared pool, reachable only by an
     *                       administrator, so the owner of the machine is never
     *                       locked out of their own storage by guests. Above rather
     *                       than carved out of the pool because the pool is already
     *                       sized to what the disk sustains, and holding one back
     *                       would make the common case — nobody but the owner
     *                       streaming — slower than the hardware allows.
     */
    AdmissionControl(int totalSlots, int perClientLimit = 2, int adminReserve = 1);

    AdmissionControl(const AdmissionControl&)            = delete;
    AdmissionControl& operator=(const AdmissionControl&) = delete;

    /**
     * @brief Asks for permission to stream @p fileSize bytes of @p path.
     *
     * Never blocks. On AtCapacity the caller should retry shortly rather than
     * fail immediately — see Connection, which retries on a timer before
     * answering 503.
     */
    AdmissionResult request(const ClientId& client,
                            const QString&  path,
                            qint64          fileSize,
                            bool            isAdmin = false);

    /** Distinct transfers currently holding a slot. */
    int activeStreams() const;

    /** Transfers @p client currently holds. */
    int streamsFor(const ClientId& client) const;

    int totalSlots() const { return m_totalSlots; }
    int perClientLimit() const { return m_perClientLimit; }

    qint64 slotThresholdBytes() const;
    void   setSlotThresholdBytes(qint64 bytes);

private:
    friend class StreamSlot;

    /** Called by StreamSlot's destructor. */
    void releaseSlot(const ClientId& client, const QString& path);

    mutable QMutex m_mutex;

    /// client -> (path -> reference count)
    QHash<ClientId, QHash<QString, int>> m_held;

    const int m_totalSlots;
    const int m_perClientLimit;
    const int m_adminReserve;

    qint64 m_slotThresholdBytes = DefaultSlotThresholdBytes;
};

} // namespace http
