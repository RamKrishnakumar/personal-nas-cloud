#pragma once

#include <QByteArray>
#include <QString>

namespace auth {

/**
 * @brief Iterations used for new hashes.
 *
 * OWASP's floor for PBKDF2-HMAC-SHA256. Measured at roughly 1.4 s with Qt's
 * implementation on a modern laptop, and an estimated 3–4 s on the reference
 * i5 6th gen, which is why verification never runs on a worker thread — see
 * handlers::AuthHandler.
 *
 * The number is recorded in every hash, so raising it later leaves existing
 * accounts working and upgradeable on next login rather than locked out.
 */
constexpr int Pbkdf2Iterations = 600'000;

constexpr int SaltBytes       = 16;
constexpr int DerivedKeyBytes = 32;

/**
 * @brief Longest password accepted, in UTF-8 bytes.
 *
 * Present because the cost of hashing grows with input length: without a cap, a
 * single request carrying a megabyte "password" would occupy the hashing thread
 * far longer than a real login.
 */
constexpr qsizetype MaxPasswordBytes = 1024;

/** Shortest password accepted when creating or changing one. */
constexpr int MinPasswordLength = 8;

enum class VerifyOutcome
{
    Ok,         ///< Password matches.
    Mismatch,   ///< Password does not match.
    Malformed,  ///< The stored string is not a hash this build understands.
};

struct VerifyResult
{
    VerifyOutcome outcome = VerifyOutcome::Malformed;

    /**
     * True when the stored hash is weaker than what this build would write now.
     *
     * The caller holds the plaintext only during login, so that is the one moment
     * a hash can be upgraded. Ignoring this is safe; acting on it is better.
     */
    bool needsUpgrade = false;

    bool ok() const { return outcome == VerifyOutcome::Ok; }
};

/**
 * @brief Hashes @p password for storage.
 *
 * Returns `$pbkdf2-sha256$i=<iterations>$<salt>$<key>` with salt and key in
 * base64url. The algorithm name is part of the string so a future move to
 * Argon2id can verify both formats and re-hash on login, rather than invalidating
 * every account.
 *
 * Returns an empty string when @p password is empty or over MaxPasswordBytes.
 *
 * @warning Takes over a second by design. Never call this from the GUI thread or
 *          from an HTTP worker thread.
 */
QString hashPassword(const QString& password);

/** Overload for tests, which cannot afford the real work factor. */
QString hashPassword(const QString& password, int iterations);

/**
 * @brief Checks @p password against @p encoded.
 *
 * Costs whatever the iteration count recorded in @p encoded costs. A malformed
 * string returns immediately, which is the one case that is deliberately fast:
 * it cannot happen for a real account, so there is nothing to equalise against.
 */
VerifyResult verifyPassword(const QString& password, const QString& encoded);

/**
 * @brief A hash to verify against when the username does not exist.
 *
 * Login must take the same time whether the account is missing, disabled, or the
 * password is simply wrong. Anything else lets an attacker enumerate usernames
 * with a stopwatch. Verifying against this costs exactly what verifying a real
 * account costs.
 *
 * A fixed literal rather than something generated at startup: generating it would
 * cost a second and a half on every launch, and nothing here needs to be secret.
 * No password produces this key, so it can never match.
 */
const QString& decoyHash();

/**
 * @brief Compares two byte arrays without leaking where they first differ.
 *
 * Exposed for testing. Length is not secret here — both sides are fixed-size
 * digests — so only the content comparison is time-invariant.
 */
bool constantTimeEquals(const QByteArray& left, const QByteArray& right);

} // namespace auth
