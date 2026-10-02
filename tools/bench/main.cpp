// NestDrive measurement harness — step 18 of the v0.6 plan.
//
// §11 of the plan is a table of targets with a note on it: "Measure the baseline
// before starting — these are targets, not measurements." This produces the
// measurements, as a markdown table ready to paste back into that document.
//
// It drives the real stack: HttpServer with its worker pool, AuthGate, FileRouter,
// ThumbnailCache, FrameExtractor. Numbers from a stub would be worthless, and the
// point of the exercise is to find out whether the claims in §11 are true of the
// thing that actually ships.
//
//   nestdrive_bench                      Generate a synthetic 60-photo folder.
//   nestdrive_bench --root <path>         Measure a real folder instead.
//   nestdrive_bench --count 200           Size of the synthetic folder.
//   nestdrive_bench --keep-cache          Reuse any existing thumbnail cache.
//
// Measuring a real folder is the more useful mode: synthetic noise images do not
// compress like photographs, so a synthetic run overstates thumbnail sizes.

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include "auth/LoginRateLimiter.h"
#include "auth/PermissionStore.h"
#include "auth/SessionStore.h"
#include "auth/UserStore.h"
#include "db/AuditLog.h"
#include "db/Database.h"
#include "handlers/AuthGate.h"
#include "handlers/AuthHandler.h"
#include "handlers/FileRouter.h"
#include "http/HardwareTier.h"
#include "http/HttpServer.h"
#include "media/FrameExtractor.h"
#include "media/ThumbnailCache.h"

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <psapi.h>
#elif defined(Q_OS_LINUX)
#  include <QFile>
#endif

namespace {

QTextStream& out()
{
    static QTextStream stream(stdout);
    return stream;
}

/** Resident set size in bytes, or 0 where it cannot be read. */
qint64 residentBytes()
{
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return qint64(counters.WorkingSetSize);
    return 0;
#elif defined(Q_OS_LINUX)
    QFile status(QStringLiteral("/proc/self/statm"));
    if (!status.open(QIODevice::ReadOnly))
        return 0;
    const QList<QByteArray> fields = status.readAll().simplified().split(' ');
    if (fields.size() < 2)
        return 0;
    return fields.at(1).toLongLong() * qint64(sysconf(_SC_PAGESIZE));
#else
    return 0;      // Reported as unavailable rather than guessed at.
#endif
}

QString humanBytes(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("n/a");
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
}

// ─────────────────────────────────────────────────────────────────────────────
//  A minimal HTTP/1.1 client
//
//  Deliberately not QNetworkAccessManager: that keeps its own connection pool and
//  its own cache, both of which are exactly what is being measured here. Counting
//  wire bytes and connections requires owning the socket.
// ─────────────────────────────────────────────────────────────────────────────

struct Reply
{
    int        status = 0;
    qint64     wireBytes = 0;    ///< Headers and body, as sent down the socket.
    qint64     bodyBytes = 0;
    QByteArray body;
    QByteArray etag;
    bool       ok() const { return status >= 200 && status < 400; }
};

class Client
{
public:
    Client(quint16 port, QByteArray sessionToken)
        : m_port(port), m_token(std::move(sessionToken)) {}

    ~Client() { disconnect(); }

    /** Connections opened over this client's lifetime. The keep-alive measure. */
    int connectionsOpened() const { return m_connectionsOpened; }

    void disconnect()
    {
        if (m_socket) {
            m_socket->abort();
            delete m_socket;
            m_socket = nullptr;
        }
    }

    /**
     * @param allowGzip Offer gzip. Off when the caller needs to read the body: this
     *                  client does not decompress, and the first run of this harness
     *                  silently parsed gzip bytes as JSON and reported a folder of
     *                  zero files. Measuring the compressed size is a separate call.
     */
    Reply get(const QByteArray& target, const QByteArray& ifNoneMatch = {},
              bool allowGzip = true)
    {
        Reply reply;

        if (!ensureConnected())
            return reply;

        QByteArray request = "GET " + target + " HTTP/1.1\r\n"
                             "Host: bench\r\n"
                             "Cookie: nd_session=" + m_token + "\r\n";
        if (allowGzip)
            request += "Accept-Encoding: gzip\r\n";
        if (!ifNoneMatch.isEmpty())
            request += "If-None-Match: " + ifNoneMatch + "\r\n";
        request += "\r\n";

        m_socket->write(request);
        m_socket->flush();

        if (!readReply(&reply)) {
            // A closed connection mid-exchange: reopen once and retry, so one
            // dropped keep-alive does not abort the whole run.
            disconnect();
            if (!ensureConnected())
                return reply;
            m_socket->write(request);
            m_socket->flush();
            readReply(&reply);
        }

        return reply;
    }

private:
    bool ensureConnected()
    {
        if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState)
            return true;

        disconnect();

        m_socket = new QTcpSocket;
        m_socket->connectToHost(QHostAddress::LocalHost, m_port);
        if (!m_socket->waitForConnected(5000)) {
            disconnect();
            return false;
        }

        ++m_connectionsOpened;
        return true;
    }

    bool readReply(Reply* reply)
    {
        QByteArray buffer;

        while (!buffer.contains("\r\n\r\n")) {
            if (!m_socket->waitForReadyRead(30'000))
                return false;
            buffer += m_socket->readAll();
        }

        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        const QByteArray head = buffer.left(headerEnd);

        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> statusParts = lines.first().simplified().split(' ');
        if (statusParts.size() < 2)
            return false;
        reply->status = statusParts.at(1).toInt();

        qint64 contentLength = 0;
        bool   closeAfter    = false;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line  = lines.at(i).trimmed();
            const QByteArray lower = line.toLower();
            if (lower.startsWith("content-length:"))
                contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
            else if (lower.startsWith("etag:"))
                reply->etag = line.mid(line.indexOf(':') + 1).trimmed();
            else if (lower.startsWith("connection:") && lower.contains("close"))
                closeAfter = true;
        }

        // 304 carries no body however it declares its length.
        if (reply->status == 304)
            contentLength = 0;

        while (buffer.size() - (headerEnd + 4) < contentLength) {
            if (!m_socket->waitForReadyRead(30'000))
                return false;
            buffer += m_socket->readAll();
        }

        reply->body      = buffer.mid(headerEnd + 4, contentLength);
        reply->bodyBytes = contentLength;

        // Header block plus body: what actually crossed the network, which is the
        // honest figure for a page made of many small requests.
        reply->wireBytes = (headerEnd + 4) + contentLength;

        if (closeAfter)
            disconnect();

        return true;
    }

    quint16     m_port = 0;
    QByteArray  m_token;
    QTcpSocket* m_socket = nullptr;
    int         m_connectionsOpened = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

/** Writes @p count photo-sized JPEGs into @p directory. */
bool generatePhotos(const QString& directory, int count)
{
    if (!QDir().mkpath(directory))
        return false;

    out() << "Generating " << count << " synthetic photos (this takes a moment)...\n";
    out().flush();

    for (int i = 0; i < count; ++i) {
        // 12 megapixels, which is what a phone camera produces. Smooth gradients
        // plus noise so the JPEG does not compress to nothing — a flat image would
        // make the baseline column meaninglessly small.
        QImage image(4032, 3024, QImage::Format_RGB32);

        auto* random = QRandomGenerator::global();
        for (int y = 0; y < image.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const int base = (x * 255) / image.width();
                const int tint = (y * 255) / image.height();
                line[x] = qRgb((base + int(random->bounded(24))) & 0xFF,
                               (tint + int(random->bounded(24))) & 0xFF,
                               ((base + tint) / 2 + int(random->bounded(24))) & 0xFF);
            }
        }

        const QString path = QStringLiteral("%1/photo%2.jpg")
                                 .arg(directory)
                                 .arg(i, 4, 10, QLatin1Char('0'));
        if (!image.save(path, "JPEG", 90))
            return false;
    }

    return true;
}

struct FolderMeasurement
{
    QString folder;
    int     photoCount    = 0;
    qint64  originalBytes = 0;   ///< Every file, images and videos alike.

    /**
     * Images only, which is the honest v0.5 comparison: the old grid put originals
     * in <img> tags, and a video never rendered in one. Counting a 645 MB clip in
     * "bytes to paint the grid" inflates the improvement by a factor of ten.
     */
    qint64 imageOriginalBytes = 0;
    int    imageCount         = 0;
    int    videoCount         = 0;

    /// Split by decoder, because a subprocess costs an order of magnitude more than
    /// Qt's reader and §11's budget was written before either existed.
    qint64 externalDecodeMs    = 0;
    int    externalDecodeCount = 0;
    qint64 qtDecodeMs          = 0;
    int    qtDecodeCount       = 0;

    qint64 listingWireBytes   = 0;   ///< Gzipped, as the network sees it.
    qint64 listingUncompressed = 0;
    qint64 listingMs          = 0;

    qint64 coldPaintBytes = 0;
    qint64 coldPaintMs    = 0;
    int    coldPaintConnections = 0;

    qint64 warmPaintBytes = 0;
    qint64 warmPaintMs    = 0;
    int    warmPaintConnections = 0;

    qint64 revisitBytes = 0;     ///< With If-None-Match: should be headers only.
    int    revisitNotModified = 0;

    qint64 onePreviewBytes  = 0;
    qint64 oneOriginalBytes = 0;

    int thumbnailsFetched = 0;
    int thumbnailsMissing = 0;   ///< Entries the listing offered no thumbnail for.
};

/** Everything the measurement run needs from the stack main() assembled. */
struct Context
{
    quint16    port = 0;
    QByteArray token;
    QString    nasRoot;
    bool       synthetic = false;
    qint64     rssBefore = 0;
    bool       ffmpegAvailable = false;
    http::HardwareProfile  profile;
    media::ThumbnailCache* thumbnails = nullptr;
};

int runMeasurements(const Context& context);

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nestdrive_bench"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Measures NestDrive against the v0.6 plan's §11 budget."));
    parser.addHelpOption();

    const QCommandLineOption rootOption(
        QStringLiteral("root"),
        QStringLiteral("Folder of real photos to measure. Generated if omitted."),
        QStringLiteral("path"));
    const QCommandLineOption countOption(
        QStringLiteral("count"),
        QStringLiteral("Synthetic photos to generate (default 60)."),
        QStringLiteral("n"), QStringLiteral("60"));
    const QCommandLineOption keepCacheOption(
        QStringLiteral("keep-cache"),
        QStringLiteral("Reuse an existing thumbnail cache instead of starting cold."));

    parser.addOption(rootOption);
    parser.addOption(countOption);
    parser.addOption(keepCacheOption);
    parser.process(application);

    // ── Workspace ─────────────────────────────────────────────────────────
    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        out() << "Could not create a temporary directory.\n";
        return 1;
    }

    QString nasRoot = parser.value(rootOption);
    const bool synthetic = nasRoot.isEmpty();

    if (synthetic) {
        nasRoot = workspace.filePath(QStringLiteral("nas"));
        if (!generatePhotos(nasRoot + QStringLiteral("/Album"),
                            parser.value(countOption).toInt())) {
            out() << "Could not generate the sample photos.\n";
            return 1;
        }
    } else if (!QFileInfo(nasRoot).isDir()) {
        out() << "Not a directory: " << nasRoot << "\n";
        return 1;
    }

    const qint64 rssBefore = residentBytes();

    // ── The real stack ────────────────────────────────────────────────────
    db::Database database;
    if (!database.open(workspace.filePath(QStringLiteral("index.sqlite")))) {
        out() << "Database: " << database.lastError() << "\n";
        return 1;
    }

    auth::UserStore       users(&database);
    auth::SessionStore    sessions(&database);
    auth::PermissionStore permissions(&database);
    auth::LoginRateLimiter limiter;
    db::AuditLog          audit(&database);

    // Cheap hashing: this measures serving, and 600,000 iterations would add
    // seconds of setup that tell us nothing about the thing under test.
    users.setIterationsForTesting(1000);

    QString error;
    const int adminId = users.create(QStringLiteral("bench"),
                                     QStringLiteral("bench-password"),
                                     auth::Role::Admin, &error);
    if (adminId <= 0) {
        out() << "Could not create the measurement account: " << error << "\n";
        return 1;
    }

    // Issued directly. The login endpoint is deliberately slow by design and is not
    // what §11 is about.
    const QByteArray token =
        sessions.create(adminId, auth::SessionStore::DefaultIdleSeconds,
                        QStringLiteral("127.0.0.1"), QStringLiteral("bench"));
    if (token.isEmpty()) {
        out() << "Could not create a session.\n";
        return 1;
    }

    const QString cacheDirectory = parser.isSet(keepCacheOption)
                                       ? QDir::homePath()
                                             + QStringLiteral("/.nestdrive-bench-cache")
                                       : workspace.filePath(QStringLiteral("thumbs"));

    media::FrameExtractor frames;
    const http::HardwareProfile profile = http::detectHardwareProfile();

    media::ThumbnailCache thumbnails(&database, cacheDirectory,
                                     profile.thumbnailThreads);
    thumbnails.setFrameExtractor(&frames);

    handlers::FileRouter router(nasRoot);
    router.setThumbnailCache(&thumbnails);
    router.setAuditLog(&audit);

    handlers::AuthHandler authentication(&users, &sessions, &limiter);
    authentication.setAuditLog(&audit);

    handlers::AuthGate gate(&router, &authentication, &sessions, &users, &permissions);

    http::HttpServer server(&gate);
    server.setHardwareProfile(profile);
    if (!server.start(0, QHostAddress::LocalHost)) {
        out() << "Could not start the server.\n";
        return 1;
    }

    const quint16 port = server.serverPort();

    // Everything below runs on a worker thread, with exec() left running on this one.
    //
    // Not a style choice. HttpServer is a QTcpServer, and it only accepts a connection
    // when its thread processes the accept notification — so a client that blocks the
    // main thread in waitForReadyRead() connects at the TCP level, where the listen
    // backlog takes it, and then waits forever for a server that never got told. The
    // first run of this harness did exactly that and reported "status 0".
    Context context;
    context.port            = server.serverPort();
    context.token           = token;
    context.nasRoot         = nasRoot;
    context.synthetic       = synthetic;
    context.rssBefore       = rssBefore;
    context.ffmpegAvailable = frames.isAvailable();
    context.profile         = profile;
    context.thumbnails      = &thumbnails;

    int exitCode = 0;
    QThread* worker = QThread::create([&] {
        exitCode = runMeasurements(context);
        QCoreApplication::quit();
    });

    worker->start();
    application.exec();
    worker->wait();
    delete worker;

    server.stop();
    return exitCode;
}

namespace {

int runMeasurements(const Context& context)
{
    const quint16     port      = context.port;
    const QByteArray& token     = context.token;
    const QString&    nasRoot   = context.nasRoot;
    const bool        synthetic = context.synthetic;
    media::ThumbnailCache& thumbnails = *context.thumbnails;

    out() << "\nServing " << nasRoot << " on port " << port << "\n";
    out() << "Hardware: " << context.profile.workerThreads << " workers, "
          << context.profile.thumbnailThreads << " thumbnail thread(s), "
          << context.profile.streamSlots << " stream slot(s)\n";
    out() << "ffmpeg: " << (context.ffmpegAvailable ? "yes" : "no") << "\n\n";
    out().flush();

    // ── Pick the folder with the most images in it ─────────────────────────
    QString targetFolder = QStringLiteral("/");
    {
        int best = -1;
        QDirIterator walker(nasRoot, QDir::Dirs | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
        QStringList candidates = { nasRoot };
        while (walker.hasNext())
            candidates.append(walker.next());

        for (const QString& candidate : candidates) {
            const int images = QDir(candidate)
                                   .entryInfoList(QDir::Files)
                                   .size();
            if (images > best) {
                best = images;
                targetFolder = QDir(nasRoot).relativeFilePath(candidate);
            }
        }

        targetFolder = (targetFolder == QStringLiteral("."))
                           ? QStringLiteral("/")
                           : QLatin1Char('/') + targetFolder;
    }

    FolderMeasurement measurement;
    measurement.folder = targetFolder;

    Client client(port, token);

    // ── Listing ───────────────────────────────────────────────────────────
    const QByteArray listTarget =
        "/api/list?path=" + QUrl::toPercentEncoding(targetFolder)
        + "&offset=0&limit=200";

    // Uncompressed, because the body has to be parsed. The compressed size — which is
    // what actually crosses the network — is measured separately below.
    QElapsedTimer timer;
    timer.start();
    const Reply listing = client.get(listTarget, {}, /*allowGzip=*/false);
    measurement.listingMs            = timer.elapsed();
    measurement.listingUncompressed  = listing.wireBytes;

    if (!listing.ok()) {
        out() << "Listing failed with status " << listing.status << "\n";
        return 1;
    }

    measurement.listingWireBytes = client.get(listTarget).wireBytes;

    const QJsonObject body    = QJsonDocument::fromJson(listing.body).object();
    const QJsonArray  entries = body.value(QStringLiteral("entries")).toArray();

    QList<QByteArray> thumbnailTargets;
    QList<bool>       needsExternalDecoder;    // Parallel to thumbnailTargets.
    QByteArray        largestPreviewTarget;

    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("d")).toBool())
            continue;

        const QString name  = entry.value(QStringLiteral("n")).toString();
        const qint64  bytes = qint64(entry.value(QStringLiteral("s")).toDouble());
        const bool    video = entry.value(QStringLiteral("k")).toString()
                              == QLatin1String("video");

        ++measurement.photoCount;
        measurement.originalBytes += bytes;

        if (video) {
            ++measurement.videoCount;
        } else {
            ++measurement.imageCount;
            measurement.imageOriginalBytes += bytes;
        }

        const QString thumbnail = entry.value(QStringLiteral("t")).toString();
        if (thumbnail.isEmpty()) {
            ++measurement.thumbnailsMissing;
            continue;
        }

        thumbnailTargets.append(thumbnail.toUtf8());
        needsExternalDecoder.append(media::FrameExtractor::handles(name));

        // The largest image, not the first one. §11's claim is about a phone photo —
        // several megabytes — and whichever file happens to sort first is as likely to
        // be a 30 KB screenshot, which would make the preview look like no saving.
        const QString preview = entry.value(QStringLiteral("p")).toString();
        if (!video && !preview.isEmpty() && bytes > measurement.oneOriginalBytes) {
            largestPreviewTarget         = preview.toUtf8();
            measurement.oneOriginalBytes = bytes;
        }
    }

    out() << "Folder " << targetFolder << " — " << measurement.photoCount
          << " files, " << thumbnailTargets.size() << " with thumbnails\n\n";
    out().flush();

    // ── Cold paint: thumbnails generated on demand ─────────────────────────
    QList<QByteArray> etags;
    {
        out() << "Cold paint (generating thumbnails)...\n";
        out().flush();

        timer.restart();
        for (int i = 0; i < thumbnailTargets.size(); ++i) {
            QElapsedTimer one;
            one.start();

            const Reply reply = client.get(thumbnailTargets.at(i));
            const qint64 elapsed = one.elapsed();

            measurement.coldPaintBytes += reply.wireBytes;
            etags.append(reply.etag);
            if (reply.ok())
                ++measurement.thumbnailsFetched;

            if (needsExternalDecoder.at(i)) {
                measurement.externalDecodeMs += elapsed;
                ++measurement.externalDecodeCount;
            } else {
                measurement.qtDecodeMs += elapsed;
                ++measurement.qtDecodeCount;
            }
        }
        measurement.coldPaintMs = timer.elapsed();
        measurement.coldPaintConnections = client.connectionsOpened();
    }

    // ── Warm paint: same request, cache populated, no validators ───────────
    {
        Client warm(port, token);
        timer.restart();
        for (const QByteArray& target : thumbnailTargets)
            measurement.warmPaintBytes += warm.get(target).wireBytes;
        measurement.warmPaintMs = timer.elapsed();
        measurement.warmPaintConnections = warm.connectionsOpened();
    }

    // ── Revisit: what a browser that already holds them sends ──────────────
    {
        Client revisit(port, token);
        for (int i = 0; i < thumbnailTargets.size(); ++i) {
            const Reply reply = revisit.get(thumbnailTargets.at(i), etags.value(i));
            measurement.revisitBytes += reply.wireBytes;
            if (reply.status == 304)
                ++measurement.revisitNotModified;
        }
    }

    // ── Opening one photo ─────────────────────────────────────────────────
    if (!largestPreviewTarget.isEmpty())
        measurement.onePreviewBytes = client.get(largestPreviewTarget).bodyBytes;

    const qint64 rssAfter  = residentBytes();
    const qint64 rssBefore = context.rssBefore;

    // ── Report ────────────────────────────────────────────────────────────
    const auto ratio = [](qint64 from, qint64 to) {
        if (to <= 0)
            return QStringLiteral("n/a");
        return QStringLiteral("%1x").arg(double(from) / double(to), 0, 'f', 0);
    };

    out() << "\n";
    out() << "### Measured on this machine, " << QDate::currentDate().toString(Qt::ISODate)
          << "\n\n";
    out() << "Source: " << (synthetic ? "synthetic photos" : "real photos")
          << ", folder `" << targetFolder << "`, " << measurement.imageCount
          << " images and " << measurement.videoCount << " video(s)\n\n";

    const qint64 paintBytes =
        measurement.warmPaintBytes + measurement.listingWireBytes;

    out() << "| Metric | v0.5 behaviour | v0.6 measured | Improvement |\n";
    out() << "|---|---|---|---|\n";

    // Images only. v0.5 put originals in <img> tags, and a video never rendered in
    // one — so counting a 645 MB clip here would overstate the win roughly tenfold.
    out() << "| Bytes to paint the grid | "
          << humanBytes(measurement.imageOriginalBytes)
          << " | " << humanBytes(paintBytes)
          << " | " << ratio(measurement.imageOriginalBytes, paintBytes) << " |\n";

    out() << "| Connections for the whole grid | " << thumbnailTargets.size() + 1
          << " | " << measurement.warmPaintConnections << " | "
          << ratio(thumbnailTargets.size() + 1, measurement.warmPaintConnections)
          << " |\n";

    out() << "| Listing response | full HTML of every row | "
          << humanBytes(measurement.listingWireBytes) << " gzipped ("
          << humanBytes(measurement.listingUncompressed) << " raw) in "
          << measurement.listingMs << " ms | — |\n";

    out() << "| Opening one photo | " << humanBytes(measurement.oneOriginalBytes)
          << " | " << humanBytes(measurement.onePreviewBytes) << " | "
          << ratio(measurement.oneOriginalBytes, measurement.onePreviewBytes)
          << " |\n";

    out() << "| Repeat visit, client holds them | full re-download | "
          << humanBytes(measurement.revisitBytes) << " ("
          << measurement.revisitNotModified << "/" << thumbnailTargets.size()
          << " answered 304) | "
          << ratio(measurement.warmPaintBytes, measurement.revisitBytes) << " |\n";

    out() << "| Cold paint, thumbnails generated | n/a | "
          << measurement.coldPaintMs << " ms for "
          << measurement.thumbnailsFetched << " files ("
          << (measurement.thumbnailsFetched > 0
                  ? measurement.coldPaintMs / measurement.thumbnailsFetched
                  : 0)
          << " ms each) | — |\n";

    // Split out, because §11's 30-150 ms budget was written for Qt's reader and the
    // two decoders are not remotely comparable.
    if (measurement.qtDecodeCount > 0) {
        out() << "| &nbsp;&nbsp;of which Qt decode | 30-150 ms budget | "
              << measurement.qtDecodeMs / measurement.qtDecodeCount << " ms each over "
              << measurement.qtDecodeCount << " files | — |\n";
    }
    if (measurement.externalDecodeCount > 0) {
        out() << "| &nbsp;&nbsp;of which ffmpeg subprocess | not budgeted | "
              << measurement.externalDecodeMs / measurement.externalDecodeCount
              << " ms each over " << measurement.externalDecodeCount
              << " files | — |\n";
    }

    out() << "| Warm paint, served from cache | n/a | "
          << measurement.warmPaintMs << " ms | — |\n";

    out() << "| Steady RSS | n/a | "
          << (rssAfter > 0 ? humanBytes(rssAfter) : QStringLiteral("unavailable"))
          << " (grew " << humanBytes(rssAfter - rssBefore) << " while serving) | — |\n";

    if (measurement.thumbnailsMissing > 0) {
        out() << "\n" << measurement.thumbnailsMissing
              << " file(s) were offered no thumbnail — unreadable format, or already "
                 "recorded as undecodable.\n";
    }

    out() << "\nThumbnail format: " << thumbnails.outputFormat()
          << " | cache on disk: " << humanBytes(thumbnails.totalBytes()) << "\n";
    out().flush();

    return 0;
}

} // namespace
