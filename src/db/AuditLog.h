#pragma once

#include <QString>

#include <functional>

namespace db {

class Database;

/**
 * @brief What was attempted. The vocabulary stored in `audit.action`.
 *
 * Fixed set rather than free text, so a query for every download cannot be defeated
 * by one call site spelling it differently.
 */
enum class Action
{
    Login,        ///< A session was issued.
    LoginFail,    ///< Credentials rejected, or the attempt was throttled.
    Logout,
    List,         ///< A folder was opened. See AuditLog for why not every page.
    Download,     ///< A file was served, or refused.
    UserCreate,   ///< An account was created.
    Upload,       ///< v0.8.
    Delete,       ///< v0.8.
    Mkdir,        ///< v0.8.
};

QString actionToString(Action action);

/**
 * @brief One thing that happened, as it will be stored.
 *
 * The defaults are the "not applicable" values, and each becomes SQL NULL rather
 * than a zero that a later query would have to know to exclude.
 */
struct AuditEntry
{
    Action  action  = Action::Login;
    int     userId  = 0;     ///< 0 when nobody was identified.
    QString path;            ///< NAS-relative, never a filesystem path.
    qint64  bytes   = -1;    ///< -1 when the action moves no data.
    int     status  = 0;     ///< HTTP status, 0 when there was no response.
    QString address;
};

/**
 * @class AuditLog
 * @brief Appends to the `audit` table.
 *
 * ### What is recorded, and what deliberately is not
 *
 * Sign-ins, sign-outs, failures, folder opens, downloads and account creation.
 * **Not** thumbnails: a single folder view fires dozens of them, so logging them
 * would bury every other entry and make the table mostly noise about one action
 * nobody audits.
 *
 * A folder open is recorded once per visit rather than once per page. The listing
 * API is paged, so scrolling a five-thousand-file folder issues twenty-five calls
 * that all mean the same thing — "they opened this folder" — and twenty-four of them
 * carry no information the first does not.
 *
 * ### A failed login records the account only when it exists
 *
 * There is no column for the username that was tried, and that is deliberate rather
 * than an omission. People type their password into the username box; storing
 * whatever was typed would put plaintext passwords in the log. So a failure against
 * a real account records that account's id, and a failure against a username that
 * does not exist records only the address. Probing is still visible as a run of
 * failures from one address, which is the signal that matters, and the one thing
 * that must never be stored cannot be.
 *
 * ### Never on the critical path of correctness
 *
 * Every method returns bool, and no caller is expected to act on false. A failed
 * audit write must not turn a working download into an error: losing one log row is
 * a smaller harm than refusing service, and the failure is reported to the
 * application log where it can be seen.
 *
 * ### Growth is bounded
 *
 * Browsing a photo library daily writes rows indefinitely, so sweepOlderThan() is
 * called on the same hourly timer as the session sweep. Without it the table is the
 * one part of the database that grows without limit.
 *
 * Thread-safe: one instance is shared by every worker, and db::Database serialises
 * the connection.
 */
class AuditLog
{
public:
    /** How long entries are kept. Long enough to investigate, not forever. */
    static constexpr int DefaultRetentionDays = 90;

    explicit AuditLog(Database* database);

    bool record(const AuditEntry& entry);

    // ── Convenience, so call sites read as what happened ──────────────────

    bool recordLogin(int userId, const QString& address);

    /** @param userId The targeted account, or 0 when the username does not exist. */
    bool recordLoginFailure(int userId, const QString& address, int status);

    bool recordLogout(int userId, const QString& address);

    bool recordList(int userId, const QString& path, const QString& address, int status);

    bool recordDownload(int userId, const QString& path, qint64 bytes,
                        const QString& address, int status);

    bool recordUserCreate(int createdUserId, const QString& address);

    /** Deletes entries older than @p days. Returns how many went. */
    int sweepOlderThan(int days);

    int count() const;

    /** Replaces the time source, for tests that need to reach the retention edge. */
    void setClock(std::function<qint64()> secondsSinceEpoch);

private:
    qint64 now() const;

    Database* m_database = nullptr;

    std::function<qint64()> m_clock;
};

} // namespace db
