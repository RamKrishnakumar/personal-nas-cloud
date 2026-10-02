// A stand-in for the ffmpeg CLI, used by tst_frameextractor.
//
// The real binary is ~80 MB, may not be installed, and its behaviour on a given file
// depends on how it was built — none of which makes for a test that says the same
// thing twice. What media::FrameExtractor actually has to get right is the plumbing:
// the arguments it builds, the output it checks for, and what it does when the
// process misbehaves. All of that is testable against a program that misbehaves on
// request, and only on request.
//
// Behaviour is chosen by the FAKE_FFMPEG_MODE environment variable:
//
//   ok              Write a valid PNG at the requested size. The default.
//   bad_version     Fail the -version probe, like a wrong-architecture binary.
//   seek_fails      Fail when -ss is present, succeed without it. A short clip.
//   silent_success  Exit 0 having written nothing. ffmpeg really does this.
//   empty_file      Exit 0 having written a zero-byte file.
//   garbage         Exit 0 having written bytes that are not an image.
//   hang            Never finish, so the timeout and kill can be exercised.
//
// Every invocation appends its arguments to FAKE_FFMPEG_LOG when that is set, which
// is how the tests assert on what was passed rather than only on what came back.

#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include <cstdio>

namespace {

QByteArray mode()
{
    const QByteArray value = qgetenv("FAKE_FFMPEG_MODE");
    return value.isEmpty() ? QByteArrayLiteral("ok") : value;
}

void logArguments(const QStringList& arguments)
{
    const QString logPath = QString::fromLocal8Bit(qgetenv("FAKE_FFMPEG_LOG"));
    if (logPath.isEmpty())
        return;

    QFile log(logPath);
    if (!log.open(QIODevice::Append | QIODevice::Text))
        return;

    QTextStream(&log) << arguments.join(QLatin1Char(' ')) << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = QCoreApplication::arguments().mid(1);

    logArguments(arguments);

    const QByteArray selected = mode();

    // ── The version probe ─────────────────────────────────────────────────
    if (arguments.contains(QStringLiteral("-version"))) {
        if (selected == "bad_version")
            return 1;

        std::printf("ffmpeg version 0.0-fake Copyright (c) nobody\n");
        return 0;
    }

    if (selected == "hang") {
        // Longer than any timeout under test. The point is to be killed.
        QThread::sleep(600);
        return 0;
    }

    // The output path is the last argument, as it is for the real thing.
    if (arguments.isEmpty())
        return 1;
    const QString outputPath = arguments.last();

    if (selected == "silent_success")
        return 0;                      // Claims success, writes nothing.

    if (selected == "seek_fails" && arguments.contains(QStringLiteral("-ss")))
        return 1;                      // The clip is shorter than the seek offset.

    if (selected == "empty_file") {
        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly))
            return 1;
        return 0;                      // Zero bytes.
    }

    if (selected == "garbage") {
        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly))
            return 1;
        file.write("this is not a picture");
        return 0;
    }

    // ── ok ────────────────────────────────────────────────────────────────
    // A recognisable image rather than a blank one, so a test can tell a real
    // decode from an accidentally-empty buffer.
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);

    return image.save(outputPath, "PNG") ? 0 : 1;
}
