#include <QTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include "handlers/FileRouter.h"
#include "http/HttpServer.h"

namespace {

struct Response
{
    int        status = 0;
    QByteArray body;
    QList<std::pair<QByteArray, QByteArray>> headers;

    QByteArray header(const QByteArray& lowercaseName) const
    {
        for (const auto& [name, value] : headers)
            if (name == lowercaseName)
                return value;
        return {};
    }
};

template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs = 10'000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return true;
}

bool readResponse(QTcpSocket* socket, Response* out, int timeoutMs = 10'000)
{
    QByteArray buffer;
    const auto drain = [&] {
        if (socket->bytesAvailable() > 0)
            buffer += socket->readAll();
    };

    if (!spinUntil([&] { drain(); return buffer.contains("\r\n\r\n"); }, timeoutMs))
        return false;

    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');

    const QList<QByteArray> statusParts = lines.first().simplified().split(' ');
    if (statusParts.size() < 2)
        return false;
    out->status = statusParts.at(1).toInt();

    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon > 0)
            out->headers.append({ line.left(colon).toLower(), line.mid(colon + 1).trimmed() });
    }

    const qint64 expected = out->header("content-length").toLongLong();
    if (!spinUntil([&] { drain(); return buffer.size() - (headerEnd + 4) >= expected; },
                   timeoutMs)) {
        return false;
    }

    out->body = buffer.mid(headerEnd + 4, expected);
    return true;
}

} // namespace

/**
 * Tests for the /api/list endpoint and the embedded web assets.
 *
 * Replaces the server-rendered HTML listing. What matters here is that paging is
 * honest about totals, that names are carried as JSON data rather than markup, and
 * that the resource-embedded assets actually linked into the binary — a resource
 * that silently failed to register presents as a blank page.
 */
class TestListApi : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void listsRootWithTotals();
    void pagesThroughALargeFolder();
    void limitIsClamped();
    void offsetBeyondEndYieldsNoEntries();
    void directoriesComeFirst();
    void entriesCarryTypeAndSize();
    void awkwardNamesSurviveAsJsonData();
    void sortBySizeDescending();
    void missingFolderIsNotFound();
    void traversalIsRefused();
    void applicationShellIsServedForDirectories();
    void staticAssetsAreEmbedded();
    void staticAssetTraversalIsRefused();
    void jsonIsGzippedForLargeFolders();

private:
    Response request(const QByteArray& raw);
    QJsonObject listJson(const QByteArray& query);

    QTemporaryDir m_tmp;
    QString       m_root;

    std::unique_ptr<handlers::FileRouter> m_router;
    std::unique_ptr<http::HttpServer>     m_server;
    quint16                               m_port = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestListApi::initTestCase()
{
    QVERIFY(m_tmp.isValid());
    m_root = m_tmp.path();

    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Album")));
    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Zebra")));

    // Enough to need more than one page.
    QVERIFY(QDir().mkpath(m_root + QStringLiteral("/Big")));
    for (int i = 0; i < 450; ++i) {
        QFile file(m_root + QStringLiteral("/Big/file-%1.txt").arg(i, 4, 10, QLatin1Char('0')));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(i + 1, 'x'));
        file.close();
    }

    const QStringList rootFiles = {
        QStringLiteral("photo.jpg"),
        QStringLiteral("clip.mp4"),
        QStringLiteral("notes.txt"),
        QStringLiteral("A&B 'quoted' \x7B name\x7D.jpg"),
    };
    for (const QString& name : rootFiles) {
        QFile file(m_root + QLatin1Char('/') + name);
        QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(name));
        file.write(QByteArray(2048, 'd'));
        file.close();
    }

    m_router = std::make_unique<handlers::FileRouter>(m_root);
    m_server = std::make_unique<http::HttpServer>(m_router.get());
    QVERIFY(m_server->start(0, QHostAddress::LocalHost));
    m_port = m_server->serverPort();
}

void TestListApi::cleanupTestCase()
{
    if (m_server)
        m_server->stop();
    m_server.reset();
    m_router.reset();
}

Response TestListApi::request(const QByteArray& raw)
{
    Response response;
    auto* socket = new QTcpSocket(this);
    socket->connectToHost(QHostAddress::LocalHost, m_port);
    if (!spinUntil([socket] { return socket->state() == QAbstractSocket::ConnectedState; },
                   5000)) {
        delete socket;
        return response;
    }

    socket->write(raw);
    socket->flush();
    readResponse(socket, &response);
    socket->abort();
    socket->deleteLater();
    return response;
}

QJsonObject TestListApi::listJson(const QByteArray& query)
{
    const Response r = request("GET /api/list?" + query + " HTTP/1.1\r\n"
                               "Host: h\r\nConnection: close\r\n\r\n");
    if (r.status != 200)
        return {};
    return QJsonDocument::fromJson(r.body).object();
}

// ─────────────────────────────────────────────────────────────────────────────

void TestListApi::listsRootWithTotals()
{
    const Response r = request("GET /api/list?path=/ HTTP/1.1\r\n"
                               "Host: h\r\nConnection: close\r\n\r\n");

    QCOMPARE(r.status, 200);
    QCOMPARE(r.header("content-type"), QByteArray("application/json"));
    QCOMPARE(r.header("cache-control"), QByteArray("no-store"));

    const QJsonObject body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("path")).toString(), QStringLiteral("/"));
    QCOMPARE(body.value(QStringLiteral("offset")).toInt(), 0);

    // 3 directories + 4 files.
    QCOMPARE(body.value(QStringLiteral("total")).toInt(), 7);
    QCOMPARE(body.value(QStringLiteral("entries")).toArray().size(), 7);

    // Advisory only — the server re-checks every mutating request — but it must be
    // present so the client can hide actions it cannot perform.
    const QJsonObject perm = body.value(QStringLiteral("perm")).toObject();
    QVERIFY(perm.contains(QStringLiteral("read")));
    QVERIFY(perm.contains(QStringLiteral("write")));
    QVERIFY(perm.contains(QStringLiteral("delete")));
}

void TestListApi::pagesThroughALargeFolder()
{
    // The whole point of paging: total reports the folder, entries report the page.
    const QJsonObject first = listJson("path=/Big&offset=0&limit=200");
    QCOMPARE(first.value(QStringLiteral("total")).toInt(), 450);
    QCOMPARE(first.value(QStringLiteral("entries")).toArray().size(), 200);

    const QJsonObject second = listJson("path=/Big&offset=200&limit=200");
    QCOMPARE(second.value(QStringLiteral("total")).toInt(), 450);
    QCOMPARE(second.value(QStringLiteral("entries")).toArray().size(), 200);

    const QJsonObject last = listJson("path=/Big&offset=400&limit=200");
    QCOMPARE(last.value(QStringLiteral("entries")).toArray().size(), 50);

    // Pages must not overlap, or the client would render duplicates.
    const QString firstName =
        first.value(QStringLiteral("entries")).toArray().at(0)
             .toObject().value(QStringLiteral("n")).toString();
    const QString secondName =
        second.value(QStringLiteral("entries")).toArray().at(0)
              .toObject().value(QStringLiteral("n")).toString();
    QVERIFY(firstName != secondName);
    QCOMPARE(firstName, QStringLiteral("file-0000.txt"));
    QCOMPARE(secondName, QStringLiteral("file-0200.txt"));
}

void TestListApi::limitIsClamped()
{
    // An unbounded limit would let one request serialise an entire huge directory,
    // undoing the reason paging exists.
    const QJsonObject huge = listJson("path=/Big&offset=0&limit=99999");
    QCOMPARE(huge.value(QStringLiteral("limit")).toInt(), 1000);

    const QJsonObject zero = listJson("path=/Big&offset=0&limit=0");
    QCOMPARE(zero.value(QStringLiteral("limit")).toInt(), 1);

    const QJsonObject negative = listJson("path=/Big&offset=-5&limit=-5");
    QCOMPARE(negative.value(QStringLiteral("offset")).toInt(), 0);
    QCOMPARE(negative.value(QStringLiteral("limit")).toInt(), 1);
}

void TestListApi::offsetBeyondEndYieldsNoEntries()
{
    const QJsonObject body = listJson("path=/Big&offset=100000&limit=200");
    QCOMPARE(body.value(QStringLiteral("total")).toInt(), 450);
    QCOMPARE(body.value(QStringLiteral("entries")).toArray().size(), 0);
}

void TestListApi::directoriesComeFirst()
{
    const QJsonArray entries =
        listJson("path=/").value(QStringLiteral("entries")).toArray();
    QVERIFY(entries.size() >= 4);

    bool seenFile = false;
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        const bool isDirectory = entry.value(QStringLiteral("d")).toBool();
        if (!isDirectory)
            seenFile = true;
        else
            QVERIFY2(!seenFile, "A directory appeared after a file");
    }
}

void TestListApi::entriesCarryTypeAndSize()
{
    const QJsonArray entries =
        listJson("path=/").value(QStringLiteral("entries")).toArray();

    QHash<QString, QJsonObject> byName;
    for (const QJsonValue& value : entries)
        byName.insert(value.toObject().value(QStringLiteral("n")).toString(),
                      value.toObject());

    const QJsonObject photo = byName.value(QStringLiteral("photo.jpg"));
    QCOMPARE(photo.value(QStringLiteral("k")).toString(), QStringLiteral("image"));
    QCOMPARE(photo.value(QStringLiteral("s")).toInteger(), 2048);
    QVERIFY(photo.value(QStringLiteral("m")).toInteger() > 0);

    QCOMPARE(byName.value(QStringLiteral("clip.mp4"))
                 .value(QStringLiteral("k")).toString(), QStringLiteral("video"));
    QCOMPARE(byName.value(QStringLiteral("notes.txt"))
                 .value(QStringLiteral("k")).toString(), QStringLiteral("file"));

    // Directories carry no size: it would be meaningless and costs bytes per entry.
    const QJsonObject album = byName.value(QStringLiteral("Album"));
    QVERIFY(album.value(QStringLiteral("d")).toBool());
    QVERIFY(!album.contains(QStringLiteral("s")));
}

void TestListApi::awkwardNamesSurviveAsJsonData()
{
    // Previously the name went into generated markup and a JS array literal, where
    // a quote broke the page. As JSON it is data, and the client sets textContent.
    const QJsonArray entries =
        listJson("path=/").value(QStringLiteral("entries")).toArray();

    bool found = false;
    for (const QJsonValue& value : entries) {
        const QString name = value.toObject().value(QStringLiteral("n")).toString();
        if (name.contains(QLatin1Char('&')) && name.contains(QLatin1Char('\''))) {
            found = true;
            QVERIFY2(name.contains(QStringLiteral("A&B")),
                     qPrintable(QStringLiteral("name came back mangled: %1").arg(name)));
        }
    }
    QVERIFY2(found, "The awkwardly named file was not listed");
}

void TestListApi::sortBySizeDescending()
{
    // Both directions are checked, because QDir's natural order differs per key:
    // Name is ascending but Size and Time are descending, so getting Reversed
    // right for one direction says nothing about the other.
    const QJsonArray descending =
        listJson("path=/Big&sort=size&order=desc&limit=5")
            .value(QStringLiteral("entries")).toArray();
    QCOMPARE(descending.size(), 5);

    qint64 previous = std::numeric_limits<qint64>::max();
    for (const QJsonValue& value : descending) {
        const qint64 size = value.toObject().value(QStringLiteral("s")).toInteger();
        QVERIFY2(size <= previous, "Sizes were not in descending order");
        previous = size;
    }

    const QJsonArray ascending =
        listJson("path=/Big&sort=size&order=asc&limit=5")
            .value(QStringLiteral("entries")).toArray();
    QCOMPARE(ascending.size(), 5);

    previous = -1;
    for (const QJsonValue& value : ascending) {
        const qint64 size = value.toObject().value(QStringLiteral("s")).toInteger();
        QVERIFY2(size >= previous, "Sizes were not in ascending order");
        previous = size;
    }

    // The two directions must actually differ, or both loops would pass on a
    // single-valued list and prove nothing.
    QVERIFY(descending.at(0).toObject().value(QStringLiteral("s")).toInteger()
            != ascending.at(0).toObject().value(QStringLiteral("s")).toInteger());
}

void TestListApi::missingFolderIsNotFound()
{
    const Response r = request("GET /api/list?path=/NoSuchFolder HTTP/1.1\r\n"
                               "Host: h\r\nConnection: close\r\n\r\n");
    QCOMPARE(r.status, 404);
}

void TestListApi::traversalIsRefused()
{
    // The API takes a path parameter, so it needs the same guard as the file route.
    for (const QByteArray& query : { QByteArray("path=/.."),
                                     QByteArray("path=/../.."),
                                     QByteArray("path=%2F..%2F..") }) {
        const Response r = request("GET /api/list?" + query + " HTTP/1.1\r\n"
                                   "Host: h\r\nConnection: close\r\n\r\n");
        QVERIFY2(r.status == 403,
                 qPrintable(QStringLiteral("%1 returned %2")
                                .arg(QString::fromUtf8(query)).arg(r.status)));
    }
}

void TestListApi::applicationShellIsServedForDirectories()
{
    // A directory URL returns the application, which then calls the API. That is
    // what keeps deep links working without rendering any listing markup.
    const Response r = request("GET /Album HTTP/1.1\r\n"
                               "Host: h\r\nConnection: close\r\n\r\n");

    QCOMPARE(r.status, 200);
    QVERIFY(r.header("content-type").startsWith("text/html"));
    QVERIFY(r.body.contains("/_static/app.js"));

    // No listing content: that is now the client's job.
    QVERIFY2(!r.body.contains("photo.jpg"),
             "The shell should not embed directory contents");
}

void TestListApi::staticAssetsAreEmbedded()
{
    // Guards against the resources failing to link out of the static library, which
    // would otherwise present as a blank page with no server-side error.
    const Response script = request("GET /_static/app.js HTTP/1.1\r\n"
                                    "Host: h\r\nConnection: close\r\n\r\n");
    QCOMPARE(script.status, 200);
    QVERIFY(script.header("content-type").startsWith("text/javascript"));
    QVERIFY(script.body.contains("api/list"));

    const Response style = request("GET /_static/app.css HTTP/1.1\r\n"
                                   "Host: h\r\nConnection: close\r\n\r\n");
    QCOMPARE(style.status, 200);
    QVERIFY(style.header("content-type").startsWith("text/css"));

    const Response missing = request("GET /_static/nope.js HTTP/1.1\r\n"
                                     "Host: h\r\nConnection: close\r\n\r\n");
    QCOMPARE(missing.status, 404);
}

void TestListApi::staticAssetTraversalIsRefused()
{
    // The asset name is used to build a resource path, so it must not accept
    // separators or parent references.
    for (const QByteArray& target : { QByteArray("/_static/../web/app.js"),
                                      QByteArray("/_static/sub/app.js"),
                                      QByteArray("/_static/..%2Fapp.js") }) {
        const Response r = request("GET " + target + " HTTP/1.1\r\n"
                                   "Host: h\r\nConnection: close\r\n\r\n");
        QVERIFY2(r.status == 404 || r.status == 403,
                 qPrintable(QStringLiteral("%1 returned %2")
                                .arg(QString::fromUtf8(target)).arg(r.status)));
    }
}

void TestListApi::jsonIsGzippedForLargeFolders()
{
    // The payoff from pairing step 9 with step 11: a few hundred kilobytes of JSON
    // compresses to a fraction of that, and this is the response a slow link waits
    // on before anything appears.
    const Response plain = request("GET /api/list?path=/Big&limit=1000 HTTP/1.1\r\n"
                                   "Host: h\r\nConnection: close\r\n\r\n");
    const Response gzipped = request("GET /api/list?path=/Big&limit=1000 HTTP/1.1\r\n"
                                     "Host: h\r\nAccept-Encoding: gzip\r\n"
                                     "Connection: close\r\n\r\n");

    QCOMPARE(plain.status, 200);
    QCOMPARE(gzipped.status, 200);
    QCOMPARE(gzipped.header("content-encoding"), QByteArray("gzip"));
    QVERIFY2(gzipped.body.size() * 3 < plain.body.size(),
             qPrintable(QStringLiteral("gzipped %1 of %2 bytes")
                            .arg(gzipped.body.size()).arg(plain.body.size())));
}

QTEST_GUILESS_MAIN(TestListApi)
#include "tst_listapi.moc"
