/**
 * @file
 * @brief Turns the application SVG into the icon formats operating systems read.
 *
 * `QApplication::setWindowIcon()` covers the running application and takes SVG
 * directly. The icon Explorer, the Start Menu, Finder and the Dock show is a
 * different thing entirely: it is read out of the file on disk, and neither Windows
 * nor macOS reads SVG. Windows wants a `.ico` compiled into the executable as a
 * resource; macOS wants a `.icns` inside the bundle.
 *
 * ### Why this exists rather than a line of ImageMagick
 *
 * Because every external tool this project has added to packaging has broken it.
 * `libqtiff` wanted a library the runner did not have; `libqsqlmysql` wanted another;
 * both failed the whole release. ImageMagick would need installing on two runners and
 * needs a delegate library to rasterise SVG at all, which is one more thing that is
 * present until it is not.
 *
 * Qt can already do it. `QImageWriter::supportedImageFormats()` lists both `ico` and
 * `icns` — the handlers are plugins, so they work on any host, and a `.icns` can be
 * produced on Windows. The SVG is rasterised by the same Qt plugin the application
 * uses for its own window icon, so what ships cannot look different from what the
 * window shows.
 *
 * ### Usage
 *
 *     mkicon <input.svg> <output.ico|output.icns> <pixels>
 *
 * Invoked from CMake at build time, so the icons are never committed and cannot drift
 * from the artwork.
 */

#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QTextStream>

namespace {

QTextStream& err()
{
    static QTextStream stream(stderr);
    return stream;
}

/**
 * @brief True when the rendered image is not simply a blank rectangle.
 *
 * A missing SVG plugin does not fail loudly — the reader returns a null image, and a
 * malformed document can return a transparent one. Either would produce a technically
 * valid icon file containing nothing, which is worse than no icon at all because the
 * build passes and the emptiness is only visible to whoever installs it.
 *
 * Sampled on a grid rather than per pixel: this runs on every build, and a sixty-four
 * point sample is enough to tell "something was drawn" from "nothing was".
 */
bool looksDrawn(const QImage& image)
{
    const int step = qMax(1, image.width() / 8);
    int opaque = 0;

    for (int y = 0; y < image.height(); y += step)
        for (int x = 0; x < image.width(); x += step)
            if (qAlpha(image.pixel(x, y)) > 128)
                ++opaque;

    return opaque > 4;
}

} // namespace

int main(int argc, char** argv)
{
    // QGuiApplication, not QCoreApplication: the image format plugins are loaded
    // through the GUI integration, and without it the SVG handler is not found.
    // Harmless on a headless machine — nothing here opens a window.
    QGuiApplication app(argc, argv);

    if (argc != 4) {
        err() << "usage: mkicon <input.svg> <output> <pixels>\n";
        return 2;
    }

    const QString source      = QString::fromLocal8Bit(argv[1]);
    const QString destination = QString::fromLocal8Bit(argv[2]);
    const int     size        = QString::fromLocal8Bit(argv[3]).toInt();

    if (size <= 0) {
        err() << "mkicon: size must be a positive number of pixels\n";
        return 2;
    }

    QImageReader reader(source);

    // Vector input has no natural size, so the target is set before reading rather
    // than scaling afterwards. That way the shapes are rasterised at the final
    // resolution instead of being resampled from whatever default the plugin picked,
    // which is the difference between crisp edges and soft ones.
    reader.setScaledSize(QSize(size, size));

    const QImage image = reader.read();
    if (image.isNull()) {
        err() << "mkicon: could not render " << source << ": "
              << reader.errorString() << "\n";
        return 1;
    }

    if (!looksDrawn(image)) {
        err() << "mkicon: " << source << " rendered as a blank image at " << size
              << "px. The SVG image plugin is probably missing.\n";
        return 1;
    }

    QImageWriter writer(destination);
    if (!writer.canWrite()) {
        err() << "mkicon: cannot write " << QFileInfo(destination).suffix()
              << " — this Qt has no handler for it. Available: ";
        for (const QByteArray& format : QImageWriter::supportedImageFormats())
            err() << format << ' ';
        err() << "\n";
        return 1;
    }

    if (!writer.write(image)) {
        err() << "mkicon: writing " << destination << " failed: "
              << writer.errorString() << "\n";
        return 1;
    }

    err() << "mkicon: wrote " << destination << " at " << size << "px\n";
    return 0;
}
