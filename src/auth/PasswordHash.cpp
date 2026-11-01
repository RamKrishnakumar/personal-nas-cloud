#include "auth/PasswordHash.h"

#include <QCryptographicHash>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QStringList>

namespace {

/** Algorithm tag written into, and required by, every hash string. */
const QString AlgorithmTag = QStringLiteral("pbkdf2-sha256");

/** URL-safe and unpadded, so the encoded hash contains no '$' or '='. */
constexpr QByteArray::Base64Options Base64Format =
    QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

/**
 * @brief Random bytes from the operating system's CSPRNG.
 *
 * QRandomGenerator::system() is backed by BCryptGenRandom on Windows and
 * getrandom() on Linux. The default-constructed QRandomGenerator is *not* a
 * CSPRNG, which is why system() is named explicitly here.
 */
QByteArray randomBytes(int count)
{
    // Generated as 32-bit words because that is the only width the generator
    // fills, then trimmed. Rounding up avoids a short read when count is not a
    // multiple of four.
    const int words = (count + 3) / 4;
    QByteArray bytes(words * 4, Qt::Uninitialized);

    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32*>(bytes.data()), words);

    bytes.truncate(count);
    return bytes;
}

QByteArray derive(const QByteArray& password, const QByteArray& salt, int iterations)
{
    // Qt's own implementation rather than a hand-rolled PBKDF2 loop. Writing
    // crypto primitives by hand is how subtle, silent breakage gets introduced.
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256,
                                              password, salt, iterations,
                                              auth::DerivedKeyBytes);
}

} // namespace

namespace auth {

bool constantTimeEquals(const QByteArray& left, const QByteArray& right)
{
    if (left.size() != right.size())
        return false;

    // Accumulate every difference instead of returning early, so the time taken
    // does not reveal the length of the matching prefix.
    unsigned char difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);

    return difference == 0;
}

QString hashPassword(const QString& password)
{
    return hashPassword(password, Pbkdf2Iterations);
}

QString hashPassword(const QString& password, int iterations)
{
    const QByteArray utf8 = password.toUtf8();
    if (utf8.isEmpty() || utf8.size() > MaxPasswordBytes || iterations < 1)
        return QString();

    const QByteArray salt = randomBytes(SaltBytes);
    const QByteArray key  = derive(utf8, salt, iterations);
    if (key.isEmpty())
        return QString();

    return QStringLiteral("$%1$i=%2$%3$%4")
        .arg(AlgorithmTag)
        .arg(iterations)
        .arg(QString::fromLatin1(salt.toBase64(Base64Format)),
             QString::fromLatin1(key.toBase64(Base64Format)));
}

VerifyResult verifyPassword(const QString& password, const QString& encoded)
{
    VerifyResult result;

    // Shape: ["", algorithm, "i=<n>", salt, key]. A leading empty field is
    // expected, because the string starts with the separator.
    const QStringList fields = encoded.split(QLatin1Char('$'));
    if (fields.size() != 5 || !fields.at(0).isEmpty())
        return result;

    if (fields.at(1) != AlgorithmTag)
        return result;          // Possibly a future Argon2id hash; not ours to read.

    const QString iterationField = fields.at(2);
    if (!iterationField.startsWith(QLatin1String("i=")))
        return result;

    bool      parsed     = false;
    const int iterations = iterationField.mid(2).toInt(&parsed);
    if (!parsed || iterations < 1)
        return result;

    const QByteArray salt =
        QByteArray::fromBase64(fields.at(3).toLatin1(), Base64Format);
    const QByteArray expected =
        QByteArray::fromBase64(fields.at(4).toLatin1(), Base64Format);

    if (salt.isEmpty() || expected.size() != DerivedKeyBytes)
        return result;

    const QByteArray utf8 = password.toUtf8();

    // An over-long candidate is rejected without hashing. It cannot be correct —
    // hashPassword() refuses to store one — so there is no account whose timing
    // this could be compared against.
    if (utf8.size() > MaxPasswordBytes) {
        result.outcome = VerifyOutcome::Mismatch;
        return result;
    }

    const QByteArray actual = derive(utf8, salt, iterations);

    result.outcome = constantTimeEquals(actual, expected) ? VerifyOutcome::Ok
                                                         : VerifyOutcome::Mismatch;
    result.needsUpgrade = iterations < Pbkdf2Iterations;
    return result;
}

const QString& decoyHash()
{
    // Generated once with the real parameters and pasted in. The salt and key are
    // random bytes; no password derives this key, so verification against it always
    // fails while costing exactly what a real verification costs.
    static const QString decoy = QStringLiteral(
        "$pbkdf2-sha256$i=600000"
        "$3QqkN7xVYwJmLpcH2sTfuA"
        "$Yk9vR2hXc1pGbnE4dDZLbVB3eTNBaGxVc0owZE5mUXc");
    return decoy;
}

} // namespace auth
