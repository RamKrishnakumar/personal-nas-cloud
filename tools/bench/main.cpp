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
#include "media/VideoTranscoder.h"

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <psapi.h>
#elif defined(Q_OS_LINUX)
#  include <QFile>
// sysconf() and _SC_PAGESIZE, used below to turn /proc/self/statm's page count into
// bytes. Easy to omit, because the Qt include above is the one this file obviously
// needs and GCC's suggestion for the resulting error is "did you mean swscanf?".
#  include <unistd.h>
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
    QByteArray cacheControl;
    QByteArray acceptRanges;
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
        QByteArray extra;
        if (allowGzip)
            extra += "Accept-Encoding: gzip\r\n";
        if (!ifNoneMatch.isEmpty())
            extra += "If-None-Match: " + ifNoneMatch + "\r\n";

        return request("GET", target, extra);
    }

    /** @param extraHeaders Already CRLF-terminated, one per line. */
    Reply request(const QByteArray& method, const QByteArray& target,
                  const QByteArray& extraHeaders = {})
    {
        Reply reply;

        if (!ensureConnected())
            return reply;

        QByteArray request = method + " " + target + " HTTP/1.1\r\n"
                             "Host: bench\r\n"
                             "Cookie: nd_session=" + m_token + "\r\n"
                           + extraHeaders + "\r\n";

        m_socket->write(request);
        m_socket->flush();

        // HEAD declares a Content-Length that describes what a GET would return, and
        // sends no body. Reading one would block until the stall timeout.
        const bool bodyExpected = (method != "HEAD");

        if (!readReply(&reply, bodyExpected)) {
            // A closed connection mid-exchange: reopen once and retry, so one
            // dropped keep-alive does not abort the whole run.
            disconnect();
            if (!ensureConnected())
                return reply;
            m_socket->write(request);
            m_socket->flush();
            readReply(&reply, bodyExpected);
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

    bool readReply(Reply* reply, bool bodyExpected = true)
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
            else if (lower.startsWith("cache-control:"))
                reply->cacheControl = line.mid(line.indexOf(':') + 1).trimmed();
            else if (lower.startsWith("accept-ranges:"))
                reply->acceptRanges = line.mid(line.indexOf(':') + 1).trimmed();
            else if (lower.startsWith("connection:") && lower.contains("close"))
                closeAfter = true;
        }

        // 304 carries no body however it declares its length.
        if (reply->status == 304 || !bodyExpected)
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

/**
 * @brief Measures the request sequence a video player actually makes.
 *
 * §5.9 of the plan says this is to be measured and not guessed at, and the three
 * candidate causes it lists need different evidence: response headers, a slot that
 * was not released, and a `moov` atom at the end of the file. Only the last two show
 * up as timing, and they look nothing alike.
 */
struct VideoMeasurement
{
    QString name;
    qint64  fileSize = 0;

    qint64 headMs = 0;

    qint64 firstChunkMs    = 0;   ///< What a player fetches to start parsing.
    qint64 firstChunkBytes = 0;

    qint64 tailMs    = 0;         ///< The `moov` hunt on a non-faststart file.
    qint64 tailBytes = 0;

    qint64 sustainedMs    = 0;    ///< Throughput ceiling, disk plus loopback.
    qint64 sustainedBytes = 0;

    int    revalidateStatus = 0;  ///< 304 means the client can reuse what it holds.
    qint64 revalidateMs     = 0;

    bool   acceptsRanges = false;
    QByteArray cacheControl;
    QByteArray etag;
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

int  runMeasurements(const Context& context);
void measureVideoPlayback(const Context& context, const QJsonArray& entries,
                          const QString& folder);
int  runTranscodeCheck(const QString& sourcePath);

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
    const QCommandLineOption transcodeOption(
        QStringLiteral("transcode"),
        QStringLiteral("Transcode this video with the real ffmpeg and report, "
                       "instead of running the serving measurements."),
        QStringLiteral("file"));

    parser.addOption(rootOption);
    parser.addOption(countOption);
    parser.addOption(keepCacheOption);
    parser.addOption(transcodeOption);
    parser.process(application);

    // ── Transcode check ───────────────────────────────────────────────────
    // Exercises media::VideoTranscoder against the real binary and a real file. The
    // unit suite drives a fixture, which by construction cannot catch anything ffmpeg
    // itself decides — the `-vf` rejection on HEIF tiles in §5.7.2 was exactly that,
    // and it had passed every fixture test.
    if (parser.isSet(transcodeOption))
        return runTranscodeCheck(parser.value(transcodeOption));

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

    measureVideoPlayback(context, entries, targetFolder);

    return 0;
}

/**
 * @brief Reproduces what a player does on open, and times each part.
 *
 * Picks the largest video in the listing. Chosen over an average because the
 * complaint is about large 4K clips, and a 2 MB one would behave fine whatever is
 * wrong.
 */
void measureVideoPlayback(const Context& context, const QJsonArray& entries,
                          const QString& folder)
{
    Q_UNUSED(folder)

    QByteArray target;
    VideoMeasurement video;

    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("k")).toString() != QLatin1String("video"))
            continue;

        const qint64 bytes = qint64(entry.value(QStringLiteral("s")).toDouble());
        if (bytes > video.fileSize) {
            video.fileSize = bytes;
            video.name     = entry.value(QStringLiteral("n")).toString();
            target         = entry.value(QStringLiteral("u")).toString().toUtf8();
        }
    }

    if (target.isEmpty()) {
        out() << "\nNo video in this folder; playback not measured.\n";
        out().flush();
        return;
    }

    Client client(context.port, context.token);
    QElapsedTimer timer;

    // ── 1. The probe app.js makes before every play ───────────────────────
    timer.start();
    const Reply head = client.request("HEAD", target);
    video.headMs       = timer.elapsed();
    video.acceptsRanges = !head.acceptRanges.isEmpty();
    video.cacheControl  = head.cacheControl;
    video.etag          = head.etag;

    // ── 2. The opening chunk a player parses ──────────────────────────────
    timer.restart();
    const Reply first = client.get(target, {}, false);   // Server ignores gzip here.
    video.firstChunkMs    = timer.elapsed();
    video.firstChunkBytes = first.bodyBytes;

    // Re-requested as a bounded range, which is what a player really sends.
    {
        Client ranged(context.port, context.token);
        timer.restart();
        const Reply chunk = ranged.request(
            "GET", target, "Range: bytes=0-1048575\r\n");
        video.firstChunkMs    = timer.elapsed();
        video.firstChunkBytes = chunk.bodyBytes;
    }

    // ── 3. The moov hunt ──────────────────────────────────────────────────
    // A file whose index sits at the end forces this before a single frame can be
    // decoded: fetch the tail, then seek back to the start. On a spinning disk that
    // is a full-stroke seek, and it is §5.9's third candidate.
    {
        Client tail(context.port, context.token);
        const qint64 tailSize = qMin<qint64>(256 * 1024, video.fileSize);
        const QByteArray range = "Range: bytes="
                                 + QByteArray::number(video.fileSize - tailSize) + '-'
                                 + QByteArray::number(video.fileSize - 1) + "\r\n";
        timer.restart();
        const Reply reply = tail.request("GET", target, range);
        video.tailMs    = timer.elapsed();
        video.tailBytes = reply.bodyBytes;
    }

    // ── 4. Throughput ceiling ─────────────────────────────────────────────
    // What the disk and the server can actually sustain, which bounds everything
    // else. If this is below the file's bitrate, no amount of caching helps.
    {
        Client bulk(context.port, context.token);
        const qint64 window = qMin<qint64>(64LL * 1024 * 1024, video.fileSize);
        const QByteArray range = "Range: bytes=0-"
                                 + QByteArray::number(window - 1) + "\r\n";
        timer.restart();
        const Reply reply = bulk.request("GET", target, range);
        video.sustainedMs    = timer.elapsed();
        video.sustainedBytes = reply.bodyBytes;
    }

    // ── 5. Can a client reuse what it already holds? ──────────────────────
    {
        Client again(context.port, context.token);
        timer.restart();
        const Reply reply = again.request(
            "GET", target, "If-None-Match: " + video.etag + "\r\n");
        video.revalidateMs     = timer.elapsed();
        video.revalidateStatus = reply.status;
    }

    // ── Report ────────────────────────────────────────────────────────────
    const double seconds = video.sustainedMs / 1000.0;
    const double mbps = seconds > 0
                            ? (video.sustainedBytes * 8.0) / (seconds * 1000000.0)
                            : 0.0;

    out() << "\n### Video playback, §5.9 — measured\n\n";
    out() << "File: `" << video.name << "`, " << humanBytes(video.fileSize) << "\n\n";

    out() << "| Step | Measured |\n|---|---|\n";
    out() << "| HEAD probe (app.js sends one before every play) | " << video.headMs
          << " ms |\n";
    out() << "| First 1 MB, ranged | " << video.firstChunkMs << " ms |\n";
    out() << "| Tail " << humanBytes(video.tailBytes)
          << " — the `moov` hunt | " << video.tailMs << " ms |\n";
    out() << "| Sustained read of " << humanBytes(video.sustainedBytes) << " | "
          << video.sustainedMs << " ms = **"
          << QString::number(mbps, 'f', 0) << " Mbps** |\n";
    out() << "| Revalidate with If-None-Match | " << video.revalidateStatus
          << " in " << video.revalidateMs << " ms |\n";
    out() << "| `Accept-Ranges` | " << (video.acceptsRanges ? "yes" : "**missing**")
          << " |\n";
    out() << "| `Cache-Control` | `" << video.cacheControl << "` |\n";
    out() << "| `ETag` | " << (video.etag.isEmpty() ? "**missing**" : "present")
          << " |\n";

    if (video.fileSize > 0 && seconds > 0) {
        out() << "\nServer can deliver this file at roughly "
              << QString::number(mbps, 'f', 0)
              << " Mbps over loopback, which is the ceiling before any network.\n";
    }

    out().flush();
}

int runTranscodeCheck(const QString& sourcePath)
{
    const media::FrameExtractor frames;
    if (!frames.isAvailable()) {
        out() << "No ffmpeg found, so there is nothing to check.\n";
        return 1;
    }

    const media::VideoTranscoder transcoder(frames.executable());
    if (!transcoder.isAvailable()) {
        out() << "ffmpeg is present but ffprobe is not beside it.\n";
        return 1;
    }

    out() << "ffmpeg:  " << transcoder.ffmpegPath() << "\n";
    out() << "ffprobe: " << transcoder.ffprobePath() << "\n\n";

    const auto profile = transcoder.probe(sourcePath);
    if (!profile) {
        out() << "Could not probe " << sourcePath << "\n";
        return 1;
    }

    QString reason;
    const media::TranscodeNeed need = media::VideoTranscoder::assess(*profile, &reason);

    const QFileInfo sourceInfo(sourcePath);

    out() << "Source: " << sourceInfo.fileName() << "\n";
    out() << "  " << profile->width << "x" << profile->height << " "
          << profile->videoCodec << " " << profile->pixelFormat
          << (profile->isHdr() ? " HDR" : "") << "\n";
    out() << "  " << QString::number(profile->bitrateBps / 1'000'000.0, 'f', 1)
          << " Mbps, " << QString::number(profile->durationMs / 1000.0, 'f', 1)
          << " s, " << humanBytes(sourceInfo.size()) << "\n";
    out() << "  verdict: "
          << (need == media::TranscodeNeed::Recommended ? "transcode"
              : need == media::TranscodeNeed::NotNeeded ? "serve as-is"
                                                        : "unknown")
          << " — " << reason << "\n\n";

    if (need != media::TranscodeNeed::Recommended) {
        out() << "Nothing to do.\n";
        return 0;
    }

    out() << "Filter chain:\n  "
          << media::VideoTranscoder::filterChain(*profile) << "\n\n";

    QTemporaryDir workspace;
    const QString destination = workspace.filePath(QStringLiteral("out.mp4"));

    out() << "Transcoding";
    out().flush();

    QElapsedTimer timer;
    timer.start();

    QString error;
    int lastShown = -1;
    const bool ok = transcoder.transcode(
        sourcePath, destination, *profile,
        [&lastShown](int percent) {
            if (percent / 10 != lastShown / 10) {
                lastShown = percent;
                out() << " " << percent << "%";
                out().flush();
            }
            return true;
        },
        &error);

    const qint64 elapsed = timer.elapsed();
    out() << "\n\n";

    if (!ok) {
        out() << "FAILED after " << elapsed << " ms: " << error << "\n";
        return 1;
    }

    const QFileInfo produced(destination);
    const double ratePerSecond =
        elapsed > 0 ? double(profile->durationMs) / double(elapsed) : 0.0;
    const double outputMbps =
        profile->durationMs > 0
            ? (produced.size() * 8.0) / (profile->durationMs / 1000.0) / 1'000'000.0
            : 0.0;

    out() << "| Result | Value |\n|---|---|\n";
    out() << "| Elapsed | " << elapsed << " ms for "
          << QString::number(profile->durationMs / 1000.0, 'f', 1)
          << " s of video |\n";
    out() << "| Speed | **" << QString::number(ratePerSecond, 'f', 2)
          << "x realtime** |\n";
    out() << "| Output size | " << humanBytes(produced.size()) << " from "
          << humanBytes(sourceInfo.size()) << " ("
          << QString::number(double(sourceInfo.size()) / double(produced.size()),
                             'f', 0)
          << "x smaller) |\n";
    out() << "| Output bitrate | **" << QString::number(outputMbps, 'f', 2)
          << " Mbps** from "
          << QString::number(profile->bitrateBps / 1'000'000.0, 'f', 1) << " Mbps |\n";

    const auto result = transcoder.probe(destination);
    if (result) {
        out() << "| Output stream | " << result->width << "x" << result->height
              << " " << result->videoCodec << " " << result->pixelFormat
              << (result->isHdr() ? " (still HDR!)" : " SDR") << " |\n";
        out() << "| Re-assessed | "
              << (media::VideoTranscoder::assess(*result)
                      == media::TranscodeNeed::NotNeeded
                      ? "serve as-is ✅"
                      : "**still wants transcoding**")
              << " |\n";
    }

    out().flush();
    return 0;
}

} // namespace
