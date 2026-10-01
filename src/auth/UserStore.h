#pragma once

#include <QString>

#include <optional>

namespace db { class Database; }

namespace auth {

/** The three roles from §9.1 of the v0.6 plan. */
enum class Role
{
    Admin,
    Family,
    Guest,
};

QString roleToString(Role role);
std::optional<Role> roleFromString(const QString& text);

struct UserRecord
{
    int     id           = 0;
    QString username;                  ///< As stored: normalised, lower case.
    QString passwordHash;
    Role    role         = Role::Guest;
    bool    enabled      = true;

    bool isValid() const { return id > 0; }
};

/**
 * @class UserStore
 * @brief Accounts, as rows in the `users` table.
 *
 * ### Usernames are normalised, not merely unique
 *
 * SQLite's UNIQUE uses binary collation, so `admin` and `Admin` would be two
 * separate accounts for what any person would read as one name. Every username is
 * therefore trimmed and case-folded before it is stored or looked up. Without
 * this, an attacker could register `Admin` alongside the real `admin` and rely on
 * people not noticing.
 *
 * Thread-safe through db::Database, which serialises on its own mutex.
 */
class UserStore
{
public:
    explicit UserStore(db::Database* database);

    /** Normalised form used for storage and lookup. Exposed because callers compare. */
    static QString normaliseUsername(const QString& username);

    std::optional<UserRecord> findByUsername(const QString& username) const;
    std::optional<UserRecord> findById(int userId) const;

    /**
     * @brief Creates an account, hashing @p password.
     *
     * @param error Set to a reason on failure: a name already taken, a password
     *              under MinPasswordLength, an unusable role.
     * @return The new user id, or 0.
     *
     * @warning Hashes the password, so this takes over a second. Call it off the
     *          GUI thread.
     */
    int create(const QString& username,
               const QString& password,
               Role           role,
               QString*       error);

    /** Replaces a stored hash, for the upgrade-on-login path. */
    bool updatePasswordHash(int userId, const QString& encodedHash);

    void setEnabled(int userId, bool enabled);

    /** Stamps `last_login_at`. Called after a successful login. */
    void recordLogin(int userId);

    /** Total accounts. Step 16's first-run wizard uses this to decide bootstrap. */
    int count() const;

    /**
     * @brief Lowers the hashing work factor. Tests only.
     *
     * A suite that creates twenty accounts at the production work factor takes most
     * of a minute, which is long enough that people stop running it. Named to be
     * obvious in review if it ever appears outside a test.
     */
    void setIterationsForTesting(int iterations) { m_iterations = iterations; }

private:
    db::Database* m_database  = nullptr;
    int           m_iterations = 0;    ///< 0 means the production work factor.
};

} // namespace auth
