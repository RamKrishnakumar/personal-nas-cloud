#include "handlers/FileRouter.h"

#include "http/Connection.h"
#include "http/MimeType.h"
#include "http/PathResolver.h"
#include "http/RequestParser.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace {

/** Escapes text for inclusion in HTML element content or a quoted attribute. */
QString escapeHtml(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&':  out += QLatin1String("&amp;");  break;
        case '<':  out += QLatin1String("&lt;");   break;
        case '>':  out += QLatin1String("&gt;");   break;
        case '"':  out += QLatin1String("&quot;"); break;
        case '\'': out += QLatin1String("&#39;");  break;
        default:   out += c;                       break;
        }
    }
    return out;
}

/** Percent-encodes one path segment for use in a URL. */
QString encodeSegment(const QString& segment)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(segment));
}

/** True when the query string contains download=1. */
bool wantsDownload(QByteArrayView query)
{
    qsizetype start = 0;
    while (start <= query.size()) {
        qsizetype amp = query.indexOf('&', start);
        if (amp < 0)
            amp = query.size();
        if (query.sliced(start, amp - start) == QByteArrayView("download=1"))
            return true;
        start = amp + 1;
    }
    return false;
}

QString humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
    return QStringLiteral("%1.%2 GB")
        .arg(bytes / (1024LL * 1024 * 1024))
        .arg((bytes % (1024LL * 1024 * 1024)) / (1024LL * 1024 * 102));
}

} // namespace

namespace handlers {

FileRouter::FileRouter(QString nasRoot)
    : m_nasRoot(std::move(nasRoot))
{
}

void FileRouter::setNasRoot(const QString& nasRoot)
{
    m_nasRoot = nasRoot;
}

void FileRouter::handle(http::Connection& connection, const http::Request& request)
{
    // No other method has a defined meaning here yet. Upload and delete arrive in
    // v0.8, behind the permission model.
    if (request.method != "GET" && request.method != "HEAD") {
        connection.sendError(405);
        return;
    }

    const QString urlPath = QUrl::fromPercentEncoding(request.path);

    const auto resolved = http::resolveSafe(m_nasRoot, QString::fromUtf8(request.path));
    if (!resolved) {
        connection.sendError(403);
        return;
    }

    const QFileInfo info(*resolved);
    if (!info.exists()) {
        // Distinct from 403 on purpose: collapsing the two would tell a prober
        // which paths exist outside the root.
        connection.sendError(404);
        return;
    }

    if (info.isDir()) {
        connection.sendHtml(buildListingHtml(*resolved, urlPath));
        return;
    }

    connection.sendFile(*resolved, wantsDownload(request.query));
}

QString FileRouter::buildListingHtml(const QString& directoryPath,
                                     const QString& urlPath) const
{
    QDir directory(directoryPath);
    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);

    const QString base = urlPath.endsWith(QLatin1Char('/')) ? urlPath
                                                            : urlPath + QLatin1Char('/');

    // ── Media index, as real JSON ─────────────────────────────────────────
    // Built through QJsonDocument rather than string concatenation. A file named
    // with a double quote previously broke the generated array outright, and a
    // name containing a script tag was injected straight into the page.
    QJsonArray mediaIndex;
    for (const QFileInfo& entry : entries) {
        if (entry.isDir())
            continue;

        const QByteArray mime = http::mimeTypeForPath(entry.fileName());
        if (!http::isDisplayableMedia(mime))
            continue;

        QJsonObject item;
        item[QStringLiteral("n")] = entry.fileName();
        item[QStringLiteral("u")] = base + encodeSegment(entry.fileName());
        item[QStringLiteral("t")] = mime.startsWith("video/") ? QStringLiteral("v")
                                                             : QStringLiteral("i");
        mediaIndex.append(item);
    }

    const QString mediaJson = QString::fromUtf8(
        QJsonDocument(mediaIndex).toJson(QJsonDocument::Compact));

    // ── Styles ────────────────────────────────────────────────────────────
    static const QString css = QStringLiteral(R"(
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
background:#0a0a14;color:#ccc;min-height:100vh}
header{background:#111827;padding:14px 20px;border-bottom:1px solid #1f2937;
display:flex;align-items:center;gap:10px;position:sticky;top:0;z-index:10}
header h1{font-size:1rem;font-weight:600;color:#38bdf8}
header span{color:#4b5563;font-size:.75rem;overflow:hidden;text-overflow:ellipsis}
.back{color:#38bdf8;text-decoration:none;font-size:.85rem;padding:5px 10px;
border:1px solid #1e3a5f;border-radius:6px;margin-left:auto;white-space:nowrap}
.back:hover{background:#1e3a5f}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));
gap:12px;padding:16px}
.card{background:#111827;border-radius:8px;overflow:hidden;border:1px solid #1f2937;
cursor:pointer;text-decoration:none;color:inherit;display:block}
.card:hover{border-color:#38bdf8}
.thumb{height:110px;display:flex;align-items:center;justify-content:center;
font-size:2.5rem;background:#0d1117;overflow:hidden}
.thumb img{width:100%;height:100%;object-fit:cover}
.info{padding:8px 10px}
.info b{display:block;font-size:.78rem;white-space:nowrap;overflow:hidden;
text-overflow:ellipsis;color:#e5e7eb}
.info small{font-size:.68rem;color:#4b5563}
.empty{padding:40px;text-align:center;color:#4b5563;font-size:.9rem}
#ov{display:none;position:fixed;inset:0;z-index:1000;background:rgba(0,0,0,.92);
flex-direction:column;align-items:center;justify-content:center;padding:20px}
#ov.open{display:flex}
#ov-img,#ov-vid{max-width:94vw;max-height:74vh;border-radius:6px;
object-fit:contain;background:#000}
#ov-title{color:#e5e7eb;font-size:.9rem;margin-bottom:12px;max-width:80vw;
text-align:center;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#ov-bar{display:flex;gap:10px;margin-top:16px;align-items:center}
.ov-btn{padding:8px 18px;border-radius:20px;border:none;font-size:.82rem;
font-weight:600;cursor:pointer;text-decoration:none}
#ov-prev,#ov-next{background:#1f2937;color:#9ca3af;padding:8px 14px}
#ov-dl{background:#0ea5e9;color:#fff}
#ov-close{background:#374151;color:#9ca3af}
#ov-counter{color:#4b5563;font-size:.75rem;min-width:50px;text-align:center}
@media(max-width:500px){.grid{grid-template-columns:repeat(auto-fill,minmax(130px,1fr))}
#ov-img,#ov-vid{max-height:60vh}}
)");

    // ── Page ──────────────────────────────────────────────────────────────
    QString html;
    html.reserve(4096 + entries.size() * 256);

    html += QStringLiteral("<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                           "<meta name='theme-color' content='#0a0a14'>"
                           "<title>NestDrive</title><style>") + css
          + QStringLiteral("</style></head><body><header><h1>📁 NestDrive</h1><span>")
          + escapeHtml(urlPath) + QStringLiteral("</span>");

    if (urlPath != QLatin1String("/")) {
        QString parent = urlPath.left(urlPath.lastIndexOf(QLatin1Char('/')));
        if (parent.isEmpty())
            parent = QStringLiteral("/");
        html += QStringLiteral("<a class='back' href='") + escapeHtml(parent)
              + QStringLiteral("'>← Back</a>");
    }

    html += QStringLiteral("</header>");

    if (entries.isEmpty())
        html += QStringLiteral("<div class='empty'>This folder is empty.</div>");

    html += QStringLiteral("<div class='grid'>");

    int mediaIndexPosition = 0;
    for (const QFileInfo& entry : entries) {
        const QString  name     = entry.fileName();
        const QString  safeName = escapeHtml(name);
        const QString  link     = escapeHtml(base + encodeSegment(name));
        const QByteArray mime   = entry.isDir() ? QByteArray()
                                                : http::mimeTypeForPath(name);
        const bool isImage = mime.startsWith("image/");
        const bool isVideo = mime.startsWith("video/");

        if (isImage || isVideo) {
            html += QStringLiteral("<div class='card' data-midx='%1'><div class='thumb'>")
                        .arg(mediaIndexPosition++);
            if (isImage) {
                // Still the original file: the thumbnail cache lands in step 10,
                // which is what makes this grid cheap. loading=lazy only defers
                // the cost.
                html += QStringLiteral("<img src='%1' alt='%2' loading='lazy' "
                                       "decoding='async' width='160' height='110'>")
                            .arg(link, safeName);
            } else {
                html += QStringLiteral("🎬");
            }
            html += QStringLiteral("</div>");
        } else if (entry.isDir()) {
            html += QStringLiteral("<a class='card' href='%1'><div class='thumb'>📁</div>")
                        .arg(link);
        } else {
            html += QStringLiteral("<a class='card' href='%1?download=1'>"
                                   "<div class='thumb'>📄</div>").arg(link);
        }

        html += QStringLiteral("<div class='info'><b>%1</b><small>%2</small></div>")
                    .arg(safeName,
                         entry.isDir() ? QStringLiteral("Folder") : humanSize(entry.size()));

        html += (isImage || isVideo) ? QStringLiteral("</div>") : QStringLiteral("</a>");
    }

    html += QStringLiteral("</div>");

    // ── Overlay ───────────────────────────────────────────────────────────
    html += QStringLiteral(
        "<div id='ov'><div id='ov-title'></div>"
        "<img id='ov-img' style='display:none' alt='preview'>"
        "<video id='ov-vid' style='display:none' controls preload='none'></video>"
        "<div id='ov-bar'>"
        "<button class='ov-btn' id='ov-prev'>&#8592;</button>"
        "<span id='ov-counter'></span>"
        "<button class='ov-btn' id='ov-next'>&#8594;</button>"
        "<a class='ov-btn' id='ov-dl' download>&#8595; Download</a>"
        "<button class='ov-btn' id='ov-close'>&#10005;</button>"
        "</div></div>");

    html += QStringLiteral("<script>const media=") + mediaJson
          + QStringLiteral(R"(;
let cur=-1;
const ov=document.getElementById('ov'),
      ovTitle=document.getElementById('ov-title'),
      ovImg=document.getElementById('ov-img'),
      ovVid=document.getElementById('ov-vid'),
      ovCounter=document.getElementById('ov-counter'),
      ovDl=document.getElementById('ov-dl');
function show(i){
  if(i<0||i>=media.length)return;
  cur=i;
  const m=media[i];
  ovTitle.textContent=m.n;
  ovDl.href=m.u+'?download=1';
  ovDl.download=m.n;
  ovCounter.textContent=(i+1)+'/'+media.length;
  ovImg.style.display='none';
  ovVid.style.display='none';
  if(m.t==='i'){ovImg.src=m.u;ovImg.style.display='block';}
  else{ovVid.pause();ovVid.src=m.u;ovVid.style.display='block';}
  ov.classList.add('open');
  document.body.style.overflow='hidden';
}
function close_(){
  ov.classList.remove('open');
  ovVid.pause();ovVid.removeAttribute('src');ovVid.load();
  ovImg.removeAttribute('src');
  document.body.style.overflow='';
}
document.getElementById('ov-close').onclick=close_;
document.getElementById('ov-prev').onclick=()=>show(cur-1);
document.getElementById('ov-next').onclick=()=>show(cur+1);
ov.addEventListener('click',e=>{if(e.target===ov)close_();});
document.addEventListener('keydown',e=>{
  if(!ov.classList.contains('open'))return;
  if(e.key==='Escape')close_();
  else if(e.key==='ArrowLeft')show(cur-1);
  else if(e.key==='ArrowRight')show(cur+1);
});
document.querySelectorAll('[data-midx]').forEach(c=>{
  c.addEventListener('click',()=>show(parseInt(c.dataset.midx,10)));
});
</script></body></html>)");

    return html;
}

} // namespace handlers
