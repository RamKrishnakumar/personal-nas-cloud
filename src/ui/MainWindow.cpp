/**
 * @file MainWindow.cpp
 * @brief Main application window for NestDrive.
 *
 * This file is responsible for:
 *  1. Building the entire UI layout (server control bar + file browser)
 *  2. Owning and managing the FileServer instance (LAN/remote file access)
 *  3. Owning and managing the DdnsUpdater instance (keeps DDNS hostname current)
 *  4. Reacting to server start/stop events and updating UI labels accordingly
 */

// ── Includes ──────────────────────────────────────────────────────────────────
#include "MainWindow.h"                 // This file's own header — declares the class

#include <QVBoxLayout>                  // Vertical box layout — stacks widgets top to bottom
#include <QHBoxLayout>                  // Horizontal box layout — places widgets side by side
#include <QWidget>                      // Base class for all UI elements
#include <QHeaderView>                  // Controls the column header behaviour in QTreeView
#include <QDir>                         // Filesystem directory operations (checking if E:/ exists)
#include <QDebug>                       // qDebug() for logging to Qt Creator's output panel
#include <QDateTime>                    // Certificate expiry arithmetic
#include <QMessageBox>                  // Confirmation after an account is created

#include "app/Paths.h"
#include "auth/UserStore.h"             // Account lookup and creation
#include "db/AuditLog.h"                // Records account creation
#include "ui/AccountDialog.h"           // Collects a new account's details

// ── Constructor ───────────────────────────────────────────────────────────────

/**
 * @brief Constructs the MainWindow.
 *
 * Order of operations:
 *  1. Initialize member objects (FileServer, DdnsUpdater) via initializer list
 *  2. Set window title and size
 *  3. Connect FileServer signals to our UI update slots
 *  4. Initialize and start the DdnsUpdater
 *  5. Build the UI layout (setupUI)
 *
 * @param parent  Qt parent widget — passing nullptr makes this a top-level window
 */
MainWindow::MainWindow(db::Database* database, QWidget* parent)
    : QMainWindow(parent)                    // Initialize Qt's base window class
    , m_database(database)                   // Not owned — created in main(), outlives this window
    , m_fileServer(new FileServer(this))     // Create file server — 'this' = Qt parent (auto memory managed)
    , m_ddnsUpdater(new DdnsUpdater(this))   // Create DDNS updater — 'this' = Qt parent (auto memory managed)
{
    // ── Window Setup ──────────────────────────────────────────────────────────
    setWindowTitle("NestDrive v0.5");  // Title shown in the window title bar
    resize(900, 650);                  // Default window size in pixels (width x height)

    // ── FileServer Signal Connections ─────────────────────────────────────────
    // Qt signals & slots: when FileServer emits a signal, our slot function
    // is called automatically. This keeps server logic decoupled from UI logic.

    // When server successfully starts → call onServerStarted() to update the UI label
    connect(m_fileServer, &FileServer::serverStarted,
            this,          &MainWindow::onServerStarted);

    // When server stops → call onServerStopped() to reset the UI label
    connect(m_fileServer, &FileServer::serverStopped,
            this,          &MainWindow::onServerStopped);

    // ── DDNS Updater Setup ────────────────────────────────────────────────────
    // Only the signal wiring here. Whether the updater actually runs is decided in
    // startDdnsIfUseful(), once the connection has been classified — publishing an
    // address is the wrong thing to do on a connection behind the provider's NAT.
    connect(m_ddnsUpdater, &DdnsUpdater::updateSucceeded,
            this, [](const QString& ip, const QString& response) {
        qDebug() << "[DDNS] ✅ Updated. IP:" << ip << "Response:" << response;
    });

    connect(m_ddnsUpdater, &DdnsUpdater::updateFailed,
            this, [](const QString& error) {
        qDebug() << "[DDNS] ❌ Failed:" << error;
    });

    // ── Thumbnails ────────────────────────────────────────────────────────────
    // Before the store, which is what passes the extractor on to the cache. Looks
    // beside the application and then on PATH when the config key is empty, which it
    // is by default.
    m_fileServer->setFrameExtractor(m_config.ffmpegPath());

    // The grid points at 320 px copies instead of originals, which is what makes a
    // folder of photos usable over a slow link.
    m_fileServer->setThumbnailStore(m_database, app::paths::thumbnailDirectory());

    // ── Optimised video ───────────────────────────────────────────────────────
    // Nothing is transcoded until someone tries to play a file: it costs roughly
    // four minutes of work per minute of 4K video on this class of machine, which is
    // not something to start uninvited.
    m_fileServer->setVideoStore(m_database, app::paths::videoDirectory());

    // ── Accounts ──────────────────────────────────────────────────────────────
    // Installs the gate, so from here nothing is served without a session. Also
    // builds the audit log, which is why this has to come before the first-run
    // prompt below: creating the administrator is itself an audited event.
    m_fileServer->setAccountStore(m_database);

    // ── Build UI ──────────────────────────────────────────────────────────────
    // Called last — all member objects must be initialized before UI is built
    // because setupUI() references m_fileServer and other members.
    setupUI();

    // ── Remote access ─────────────────────────────────────────────────────────
    // After setupUI() so the warning banner exists to report into, and before the
    // server can be started — which only happens on a button press, so this is in
    // time. Getting the order wrong the other way round means the very first
    // certificate request, the one most likely to fail because HTTPS has not been
    // enabled for the tailnet yet, reports into a label that does not exist and is
    // silently dropped.
    setUpRemoteAccess();

    // After the UI exists, so the dialog has a parent to be modal against.
    promptForFirstAdminIfNeeded();
}

// ── Accounts ──────────────────────────────────────────────────────────────────

void MainWindow::promptForFirstAdminIfNeeded()
{
    auth::UserStore* users = m_fileServer->accountStore();

    // Refreshes the warning banner as well, so it disappears the moment an account
    // exists rather than at the next launch.
    //
    // The wording changed in step 16 because step 14 inverted what zero accounts
    // means. It used to mean the library was open to anyone who could reach the port;
    // now the gate refuses every request, so it means nobody can get in at all —
    // including the owner. Leaving the old text up would have sent people looking for
    // an exposure that no longer exists while the real problem went unexplained.
    const auto refreshWarning = [this](int accounts) {
        const bool unusable = accounts == 0;
        m_accountWarning->setVisible(unusable);
        if (unusable) {
            m_accountWarning->setText(
                tr("⚠  No account exists yet, so nobody can sign in and no file can "
                   "be reached from another device — every request is sent to the "
                   "login page. Create an administrator account to start using "
                   "NestDrive."));
        }
    };

    if (!users) {
        m_accountsBtn->setEnabled(false);
        m_accountsBtn->setToolTip(tr("Unavailable: the database did not open."));
        refreshWarning(0);
        return;
    }

    if (users->count() > 0) {
        refreshWarning(users->count());
        return;
    }

    // Shown once, at first launch. Leaving without creating one is allowed but has to
    // be confirmed — the dialog explains that nothing is reachable until an account
    // exists — and the banner then stays up.
    AccountDialog dialog(users, /*firstAdmin=*/true, this);
    dialog.exec();

    refreshWarning(users->count());
    recordAccountCreated(dialog.createdUserId(), dialog.createdUsername());
}

void MainWindow::recordAccountCreated(int userId, const QString& username)
{
    if (username.isEmpty())
        return;                    // Nothing was created.

    qInfo() << "[Auth] Account created:" << username;

    // The one audited event that happens in this window rather than over HTTP. There
    // is no peer address because there is no request: it was done at the machine.
    if (db::AuditLog* audit = m_fileServer->auditLog())
        audit->recordUserCreate(userId, QStringLiteral("local"));
}

void MainWindow::onManageAccounts()
{
    auth::UserStore* users = m_fileServer->accountStore();
    if (!users)
        return;

    const bool firstAdmin = users->count() == 0;

    AccountDialog dialog(users, firstAdmin, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_accountWarning->setVisible(users->count() == 0);
    recordAccountCreated(dialog.createdUserId(), dialog.createdUsername());

    QMessageBox::information(
        this, tr("Account created"),
        tr("%1 can now sign in at %2/login.")
            .arg(dialog.createdUsername(),
                 m_fileServer->isRunning()
                     // scheme() rather than a literal "http": with HTTPS on, the
                     // address printed here was one the server would not answer on.
                     ? QStringLiteral("%1://%2:%3")
                           .arg(scheme(),
                                m_fileServer->localIP(),
                                QString::number(m_fileServer->port()))
                     : tr("the server address once it is running")));
}

// ── Remote access ─────────────────────────────────────────────────────────────
//
// Two paths out of the house, and they are not interchangeable.
//
// Tailscale works on any connection, including behind the provider's NAT, and needs
// no router configuration — but every viewing device has to have it installed. A
// forwarded port and the DDNS hostname give an ordinary link anyone can open with
// nothing installed, but need a connection whose router actually holds the address
// the internet sees.
//
// Which of those is true is measured rather than assumed. The owner here has one
// connection of each kind, and the usual shortcut of checking whether the public
// address falls in a reserved range gets one of them wrong.
//
// It has to be one or the other, not both: the two need certificates for different
// names, and Qt exposes no server-side SNI hook, so one listener can present exactly
// one certificate.

void MainWindow::setUpRemoteAccess()
{
    // Torn down first, so this can be re-run when the method is changed in the
    // control bar without restarting the application.
    m_certRenewer.reset();
    m_connectionCheck.reset();
    m_tailscale.reset();
    m_upnp.reset();
    m_remoteName.clear();
    m_methodNote.clear();
    m_method = core::RemoteMethod::LanOnly;

    if (m_certWarning)
        m_certWarning->setVisible(false);

    // Tailscale first, because probing it is local and immediate — one subprocess
    // that answers in milliseconds.
    if (m_config.useTailscaleCert()) {
        m_tailscale = std::make_unique<core::Tailscale>(m_config.tailscalePath());

        if (!m_tailscale->isAvailable()) {
            // Ordinary on a machine without it, and it has to stay ordinary: the LAN
            // keeps working. Debug rather than warning, for the same reason the DDNS
            // gate above is.
            qDebug() << "[Remote] ℹ️ Tailscale not found. See docs/REMOTE-ACCESS.md.";
            m_tailscale.reset();
        } else {
            qInfo() << "[Remote] Tailscale" << m_tailscale->versionLine();

            const core::TailscaleStatus status = m_tailscale->status();
            m_remoteName = status.dnsName;

            if (status.isOnline) {
                qInfo() << "[Remote] This machine is" << m_remoteName
                        << "at" << status.address;
            } else {
                qWarning() << "[Remote] ⚠️ Tailscale is installed but signed out.";
            }
        }
    }

    // Classifying the connection is two network round trips, so it finishes in
    // applyRemoteMethod(). Blocking here would stall the window for the slower of
    // them at every launch.
    m_upnp = std::make_unique<core::UpnpClient>();

    // No Qt parent, deliberately. These are held in unique_ptr, and handing them
    // `this` as well would have both the parent and the smart pointer delete them.
    m_connectionCheck =
        std::make_unique<core::ConnectionCheck>(m_upnp.get());

    connect(m_connectionCheck.get(), &core::ConnectionCheck::finished,
            this, &MainWindow::applyRemoteMethod);

    m_connectionCheck->start();
}

void MainWindow::applyRemoteMethod(core::ConnectionType connection)
{
    m_connection = connection;

    // Detection runs every launch and is trusted over the stored answer, because the
    // answer belongs to the connection rather than to the machine — this laptop moves
    // between one provider behind carrier-grade NAT and one not. The stored value is
    // only a fallback for when the router will not say and the address alone does not
    // settle it.
    if (m_connection == core::ConnectionType::Unknown) {
        const core::ConnectionType remembered =
            core::connectionTypeFromString(m_config.connectionType());

        if (remembered != core::ConnectionType::Unknown) {
            m_connection = remembered;
            qInfo() << "[Remote] Detection was inconclusive; using the remembered "
                       "answer from config.ini.";
        }
    } else {
        // Worth storing for the next launch on a connection that cannot be
        // classified automatically.
        m_config.setConnectionType(core::connectionTypeToString(m_connection));
        m_config.save();
    }

    const core::RemoteMethod configured =
        core::remoteMethodFromString(m_config.remoteMethod());

    const bool tailscaleOnline = m_tailscale && !m_remoteName.isEmpty();

    m_method = core::resolveRemoteMethod(configured,
                                         m_connection,
                                         tailscaleOnline,
                                         !m_config.ddnsHostname().isEmpty());

    // Recorded when the result is not what was asked for. Falling back silently would
    // leave somebody who chose "Public link" with no idea why their link does not
    // work, which is the worst of both outcomes.
    if (configured != core::RemoteMethod::Auto && configured != m_method) {
        m_methodNote = tr("%1 was selected but is not available on this connection, "
                          "so %2 is in use.")
                           .arg(core::describeRemoteMethod(configured),
                                core::describeRemoteMethod(m_method));
        qWarning().noquote() << "[Remote] ⚠️" << m_methodNote;
    }

    qInfo().noquote() << "[Remote] Method:" << core::describeRemoteMethod(m_method);

    startDdnsIfUseful();

    switch (m_method) {
    case core::RemoteMethod::Tailscale:
        startTailscaleCertificates();
        break;

    case core::RemoteMethod::Public:
        checkPublicCertificate();
        break;

    case core::RemoteMethod::Auto:
    case core::RemoteMethod::LanOnly:
        break;
    }

    // The status line was written before any of this was known.
    if (m_fileServer->isRunning())
        onServerStarted(m_fileServer->localIP(), m_fileServer->port());
}

void MainWindow::startDdnsIfUseful()
{
    // Gate on the credentials themselves rather than on isLoaded(). A config.ini
    // written on first launch exists but has blank DDNS fields, and starting the
    // updater then would send a failing authentication request to No-IP every five
    // minutes — repeated failures can get an account blocked.
    const QString user = m_config.ddnsUsername();
    const QString pass = m_config.ddnsPassword();
    const QString host = m_config.ddnsHostname();

    if (user.isEmpty() || pass.isEmpty() || host.isEmpty()) {
        qDebug() << "[Main] ℹ️ DDNS credentials incomplete in"
                 << AppConfig::configPath()
                 << "— DDNS disabled. LAN access still works.";
        return;
    }

    if (m_connection == core::ConnectionType::CarrierNat) {
        // Deliberately refused, not merely skipped as pointless.
        //
        // Behind the provider's NAT the address an external service reports is the
        // provider's, shared with other subscribers. Publishing it would point this
        // household's hostname at somebody else's connection — so this is not a
        // wasted update, it is a wrong one.
        qWarning() << "[DDNS] ⚠️ Not publishing: this connection is behind the "
                      "provider's NAT, so the visible address is not this "
                      "household's. See docs/REMOTE-ACCESS.md.";
        m_ddnsUpdater->stop();
        return;
    }

    m_ddnsUpdater->setCredentials(user, pass, host);
    m_ddnsUpdater->start(5 * 60 * 1000);
}

void MainWindow::startTailscaleCertificates()
{
    if (!m_config.useHttps()) {
        // Reachable, but over plain HTTP — which inside the overlay is still
        // encrypted end to end by WireGuard, so this is not the hole it looks like.
        // What it costs is browser trust and the secure-context features that depend
        // on it, not confidentiality.
        //
        // No certificate is requested, deliberately: certPath and keyPath are the
        // owner's files until they ask for HTTPS, and silently overwriting a
        // certificate they are managing themselves would be a poor way to repay
        // leaving this setting at its default.
        qInfo() << "[Remote] Reachable over http:// at" << m_remoteName
                << "— set useHttps=true in config.ini for a browser-trusted "
                   "certificate.";
        return;
    }

    m_certRenewer = std::make_unique<core::CertRenewer>(
        m_tailscale.get(), m_config.certPath(), m_config.keyPath());

    m_certRenewer->setDnsName(m_remoteName);
    m_certRenewer->setThresholdDays(m_config.certRenewDays());

    // Straight into the running server. This is the whole reason the renewer exists
    // as a signal rather than a function the start path calls once: a 90-day
    // certificate will come due while the server is up, and stopping it to install a
    // new one would interrupt whatever is being watched at the time.
    connect(m_certRenewer.get(), &core::CertRenewer::certificateRenewed,
            this, [this](const QSslConfiguration& configuration) {
        m_fileServer->reloadTls(configuration);

        if (m_certWarning)
            m_certWarning->setVisible(false);

        qInfo() << "[Remote] ✅ Certificate installed without a restart.";
    });

    connect(m_certRenewer.get(), &core::CertRenewer::renewalFailed,
            this, [this](const QString& reason) {
        if (!m_certWarning)
            return;

        // The CLI's own words. The common cause is HTTPS certificates not being
        // enabled for the tailnet, and it names the admin page to fix it on.
        //
        // PlainText explicitly, because this string is another program's stderr:
        // under the default AutoText a stray angle bracket in it would be guessed at
        // as markup and swallow part of the message.
        m_certWarning->setTextFormat(Qt::PlainText);
        m_certWarning->setText(QStringLiteral("⚠️ Certificate renewal failed: %1")
                                   .arg(reason));
        m_certWarning->setVisible(true);
    });

    // Checks once now, then daily. The first call is what obtains a certificate on a
    // machine that has never had one — and the most likely one to fail, because
    // HTTPS certificates have to be enabled for the tailnet once before any request
    // can succeed. The renewalFailed handler above puts that in the banner.
    m_certRenewer->start();
}

void MainWindow::checkPublicCertificate()
{
    if (!m_config.useHttps())
        return;

    const QString hostname = m_config.ddnsHostname();

    // Read, never written. On this path the certificate belongs to the owner: they
    // obtained it with win-acme or a similar tool, and that tool renews it on its own
    // schedule — which keeps working while NestDrive is closed, unlike anything this
    // process could arrange. Replacing it would quietly undo that.
    const QList<QSslCertificate> chain =
        QSslCertificate::fromPath(core::resolveTlsPath(m_config.certPath()),
                                  QSsl::Pem);

    const QSslCertificate leaf = chain.isEmpty() ? QSslCertificate() : chain.first();

    if (!m_certWarning)
        return;

    m_certWarning->setTextFormat(Qt::RichText);

    if (!core::CertRenewer::covers(leaf, hostname)) {
        // Exactly the failure a phone found on the other path: a certificate that is
        // perfectly valid but for the wrong name. Said here, in the window, rather
        // than left for somebody to discover on a device they have already handed to
        // a relative.
        m_certWarning->setText(
            tr("⚠️ The certificate does not cover <b>%1</b>, so browsers will warn "
               "about it. Obtain one for that name — see Remote access for the "
               "command — and point certPath and keyPath at it.")
                .arg(hostname.toHtmlEscaped()));
        m_certWarning->setVisible(true);
        return;
    }

    if (core::CertRenewer::shouldRenew(leaf.expiryDate(),
                                       QDateTime::currentDateTimeUtc(),
                                       m_config.certRenewDays())) {
        m_certWarning->setText(
            tr("⚠️ The certificate for <b>%1</b> expires on %2, and NestDrive does "
               "not manage this one. Renew it with the tool that issued it.")
                .arg(hostname.toHtmlEscaped(),
                     leaf.expiryDate().toString(QStringLiteral("d MMMM yyyy"))));
        m_certWarning->setVisible(true);
    }
}

QString MainWindow::scheme() const
{
    return m_fileServer->useHttps() ? QStringLiteral("https") : QStringLiteral("http");
}

QString MainWindow::remoteUrl() const
{
    QString host;

    switch (m_method) {
    case core::RemoteMethod::Public:
        host = m_config.ddnsHostname();
        break;

    case core::RemoteMethod::Tailscale:
        // The name ahead of the address. The certificate is issued for the name, so
        // reaching the server by address produces exactly the warning a real
        // certificate exists to remove.
        host = !m_remoteName.isEmpty() ? m_remoteName : m_fileServer->overlayIP();
        break;

    case core::RemoteMethod::Auto:
    case core::RemoteMethod::LanOnly:
        break;
    }

    if (host.isEmpty())
        return {};

    return QStringLiteral("%1://%2:%3")
        .arg(scheme(), host, QString::number(m_fileServer->port()));
}

void MainWindow::onRemoteMethodChanged(int index)
{
    const auto chosen = core::RemoteMethod(m_remoteMethodBox->itemData(index).toInt());

    m_config.setRemoteMethod(core::remoteMethodToString(chosen));
    m_config.save();

    // Re-run rather than prompt for a restart. Swapping the certificate in a running
    // server is already supported — it is what renewal does — so there is nothing
    // here that needs the process restarted.
    setUpRemoteAccess();
}

void MainWindow::onForwardPort()
{
    if (!m_upnp || !m_upnp->isAvailable()) {
        QMessageBox::information(
            this, tr("Forward the port"),
            tr("The router did not answer a UPnP search, so NestDrive cannot ask it "
               "to forward anything. UPnP is usually switched off in the router's "
               "own settings.\n\nThe rule can still be added by hand — see "
               "docs/REMOTE-ACCESS.md."));
        return;
    }

    const QString client = m_fileServer->localIP();
    const quint16 port   = m_config.serverPort();

    if (QMessageBox::question(
            this, tr("Forward the port"),
            tr("Ask the router to forward port %1 to this machine at %2?\n\n"
               "This adds a rule to the router, which anyone on the internet can "
               "then use to reach NestDrive. Accounts still apply.")
                .arg(QString::number(port), client))
        != QMessageBox::Yes) {
        return;
    }

    // One-shot connections: this button can be pressed again, and leaving the old
    // ones attached would show one message box per previous press.
    auto* context = new QObject(this);

    connect(m_upnp.get(), &core::UpnpClient::portMappingFinished, context,
            [this, context, port](bool succeeded) {
        context->deleteLater();

        if (succeeded) {
            QMessageBox::information(
                this, tr("Forward the port"),
                tr("Port %1 is now forwarded. Test it from a phone on mobile data "
                   "with Wi-Fi switched off — that is the only test that proves it.")
                    .arg(QString::number(port)));
        }
    });

    connect(m_upnp.get(), &core::UpnpClient::failed, context,
            [this, context](const QString& reason) {
        context->deleteLater();

        QMessageBox::warning(this, tr("Forward the port"),
                             tr("The router refused: %1").arg(reason));
    });

    m_upnp->requestPortMapping(port, port, client, QStringLiteral("NestDrive"));
}

void MainWindow::onShowRemoteAccess()
{
    QString body = tr("<b>Connection:</b> %1<br>%2")
                       .arg(core::describeConnectionType(m_connection),
                            m_connectionCheck ? m_connectionCheck->explanation()
                                              : tr("Not checked yet."));

    body += tr("<hr><b>Method:</b> %1").arg(core::describeRemoteMethod(m_method));

    if (!m_methodNote.isEmpty())
        body += tr("<br>%1").arg(m_methodNote);

    switch (m_method) {
    case core::RemoteMethod::Public: {
        body += tr("<p>Reachable by anyone at:</p><p><tt>%1</tt></p>"
                   "<p>Nothing needs installing on the viewing device. The port must "
                   "be forwarded on the router — there is a button for that if the "
                   "router allows it.</p>")
                    .arg(remoteUrl());

        const QList<QSslCertificate> chain =
            QSslCertificate::fromPath(core::resolveTlsPath(m_config.certPath()),
                                  QSsl::Pem);
        const QSslCertificate leaf =
            chain.isEmpty() ? QSslCertificate() : chain.first();

        if (core::CertRenewer::covers(leaf, m_config.ddnsHostname())) {
            body += tr("<p>Certificate valid until %1. NestDrive does not manage "
                       "this one — the tool that issued it renews it.</p>")
                        .arg(leaf.expiryDate()
                                 .toString(QStringLiteral("d MMMM yyyy")));
        } else {
            body += tr("<p><b>No certificate for this name yet,</b> so browsers will "
                       "warn. Get one once with win-acme:</p>"
                       "<p><tt>wacs.exe --target manual --host %1 "
                       "--store pemfiles --pemfilespath .</tt></p>"
                       "<p>That needs port 80 forwarded as well, for the challenge. "
                       "It installs its own renewal task, so this is a one-off.</p>")
                        .arg(m_config.ddnsHostname().toHtmlEscaped());
        }
        break;
    }

    case core::RemoteMethod::Tailscale: {
        body += tr("<p>Reachable at:</p><p><tt>%1</tt></p>"
                   "<p>Open that on any device signed into the same Tailscale "
                   "account. Use the name rather than the address — the certificate "
                   "is issued for the name.</p>")
                    .arg(remoteUrl());

        const QDateTime expiry =
            m_certRenewer ? m_certRenewer->currentExpiry() : QDateTime();

        if (!m_config.useHttps()) {
            body += tr("<p>Served over plain HTTP. Traffic is still encrypted end to "
                       "end by Tailscale itself, so this is not an exposure — but the "
                       "browser will not show a padlock. Set <tt>useHttps=true</tt> "
                       "in <tt>config.ini</tt> for a browser-trusted certificate, "
                       "obtained and renewed automatically.</p>");
        } else if (expiry.isValid()) {
            body += tr("<p>Certificate valid until %1, renewed automatically while "
                       "NestDrive is running.</p>")
                        .arg(expiry.toString(QStringLiteral("d MMMM yyyy")));
        } else {
            body += tr("<p><b>No certificate yet.</b> Enable HTTPS certificates for "
                       "your tailnet at <tt>login.tailscale.com/admin/dns</tt>, then "
                       "restart NestDrive.</p>");
        }

        if (m_certRenewer && !m_certRenewer->lastError().isEmpty()) {
            // Escaped: another program's stderr being placed in rich text, where an
            // angle bracket would be read as markup and hide the rest.
            body += tr("<p>Last problem: %1</p>")
                        .arg(m_certRenewer->lastError().toHtmlEscaped());
        }
        break;
    }

    case core::RemoteMethod::Auto:
    case core::RemoteMethod::LanOnly:
        if (m_connection == core::ConnectionType::CarrierNat) {
            body += tr("<p>This connection is behind the provider's NAT, so "
                       "forwarding a port cannot work. Install Tailscale on this "
                       "machine and on each device you want to view from, sign them "
                       "into the same account, and the library becomes reachable from "
                       "anywhere.</p>");
        } else {
            body += tr("<p>Not reachable from outside this network. Either install "
                       "Tailscale, or — with a public address — set a DDNS hostname "
                       "and forward the port.</p>");
        }

        body += tr("<p>See <tt>docs/REMOTE-ACCESS.md</tt>.</p>");
        break;
    }

    // The local address as well. Both are true at once and they are not
    // interchangeable: this one is faster in the house and works with no remote
    // access configured at all.
    if (m_fileServer->isRunning()) {
        body += tr("<hr><p>On this network: <tt>%1://%2:%3</tt></p>")
                    .arg(scheme(),
                         m_fileServer->localIP(),
                         QString::number(m_fileServer->port()));
    }

    QMessageBox dialog(this);
    dialog.setWindowTitle(tr("Remote access"));
    dialog.setTextFormat(Qt::RichText);
    dialog.setText(body);

    QPushButton* forward = nullptr;
    if (m_method == core::RemoteMethod::Public)
        forward = dialog.addButton(tr("Forward the port"), QMessageBox::ActionRole);

    dialog.addButton(QMessageBox::Close);
    dialog.exec();

    // clickedButton() rather than the exec() result. For a custom button exec()
    // returns nothing meaningful, and dismissing the dialog with the window close
    // button would have compared unequal to Close — which would have added a rule to
    // the router on the strength of somebody closing a dialog.
    if (forward && dialog.clickedButton() == forward)
        onForwardPort();
}

// ── UI Builder ────────────────────────────────────────────────────────────────

/**
 * @brief Constructs and arranges all UI widgets inside the main window.
 *
 * Layout structure:
 *  QMainWindow
 *  └── central QWidget
 *      └── QVBoxLayout (mainLayout) — vertical stack
 *          ├── QHBoxLayout (barLayout) — top control bar
 *          │   ├── QPushButton (m_serverBtn) — start/stop server
 *          │   └── QLabel (m_serverStatus) — shows current server URL
 *          └── QTreeView (m_fileTree) — file browser for E:/ drive
 */
void MainWindow::setupUI()
{
    // Every QMainWindow needs a central widget — it fills the window area.
    // All other widgets are placed inside this central widget.
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    // Main vertical layout — stacks the control bar on top, file tree below.
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(8, 8, 8, 8); // 8px padding on all four sides
    mainLayout->setSpacing(6);                   // 6px gap between child widgets

    // ── Server Control Bar ────────────────────────────────────────────────────
    // A horizontal row at the top containing the Start/Stop button and status label.
    auto* barLayout = new QHBoxLayout();

    // Start/Stop button — clicking toggles the HTTP file server on/off.
    m_serverBtn = new QPushButton("▶  Start Server", this);
    m_serverBtn->setFixedHeight(32); // Fixed height keeps the bar compact
    connect(m_serverBtn, &QPushButton::clicked,
            this,         &MainWindow::onToggleServer);

    // Status label — shows "Server stopped" or the live URL when running.
    // Styled grey when stopped, bright green when running.
    m_serverStatus = new QLabel("Server stopped", this);
    m_serverStatus->setStyleSheet("color: #888;"); // Grey = inactive

    // Accounts button — creating the first one is what makes a login possible.
    m_accountsBtn = new QPushButton("Accounts…", this);
    m_accountsBtn->setFixedHeight(32);
    connect(m_accountsBtn, &QPushButton::clicked,
            this,          &MainWindow::onManageAccounts);

    // Which way out of the house. Automatic is right for almost everybody — the two
    // paths suit different connections and NestDrive can tell which this is — but the
    // choice is exposed because the preference is sometimes not about capability: on
    // a connection that can carry a public link, somebody may still want the overlay
    // so that nothing is exposed publicly at all.
    m_remoteMethodBox = new QComboBox(this);
    m_remoteMethodBox->setFixedHeight(32);
    m_remoteMethodBox->setToolTip(
        tr("How the library is reached from outside this network"));

    for (const auto method : { core::RemoteMethod::Auto,
                               core::RemoteMethod::Tailscale,
                               core::RemoteMethod::Public,
                               core::RemoteMethod::LanOnly }) {
        m_remoteMethodBox->addItem(core::describeRemoteMethod(method), int(method));
    }

    {
        // Set before the signal is connected. Doing it after would fire the handler
        // during construction and re-run setup for a value that had not changed.
        const int index = m_remoteMethodBox->findData(
            int(core::remoteMethodFromString(m_config.remoteMethod())));

        if (index >= 0)
            m_remoteMethodBox->setCurrentIndex(index);
    }

    connect(m_remoteMethodBox, &QComboBox::currentIndexChanged,
            this,              &MainWindow::onRemoteMethodChanged);

    // Remote access — what to install and which address to use from outside.
    m_remoteBtn = new QPushButton("Remote access…", this);
    m_remoteBtn->setFixedHeight(32);
    connect(m_remoteBtn, &QPushButton::clicked,
            this,        &MainWindow::onShowRemoteAccess);

    // Add button and label to the horizontal bar
    barLayout->addWidget(m_serverBtn);
    barLayout->addWidget(m_serverStatus);
    barLayout->addStretch(); // Pushes button and label to the left, leaves right side empty
    barLayout->addWidget(m_remoteMethodBox);
    barLayout->addWidget(m_remoteBtn);
    barLayout->addWidget(m_accountsBtn);

    // Add the horizontal bar to the top of the main vertical layout
    mainLayout->addLayout(barLayout);

    // Standing warning while the server is open to anyone who can reach the port.
    // Visible in the window rather than only in the log, because the log is not
    // where someone decides whether to forward a port.
    m_accountWarning = new QLabel(this);
    m_accountWarning->setWordWrap(true);
    m_accountWarning->setStyleSheet(
        "background: #4a2000; color: #ffd9a0; padding: 6px; border-radius: 4px;");
    m_accountWarning->setVisible(false);
    mainLayout->addWidget(m_accountWarning);

    // Same treatment for a certificate about to lapse. Worth its own banner rather
    // than sharing the one above: an expired certificate does not expose anything,
    // it just stops every device trusting the server, and the two need different
    // actions from the owner.
    m_certWarning = new QLabel(this);
    m_certWarning->setWordWrap(true);
    m_certWarning->setStyleSheet(
        "background: #4a2000; color: #ffd9a0; padding: 6px; border-radius: 4px;");
    m_certWarning->setVisible(false);
    mainLayout->addWidget(m_certWarning);

    // ── File Browser ──────────────────────────────────────────────────────────
    // Shows the contents of the NAS storage drive (E:/) in a tree view.
    // Falls back to the user's home folder if E:/ doesn't exist on this machine.

    QString nasRoot = m_config.nasRoot(); // Read NAS root path from config.ini (e.g. "E:/");
    QDir dir(nasRoot);
    if (!dir.exists()) {
        // E:/ not found — fall back gracefully instead of crashing
        nasRoot = QDir::homePath();
        qDebug() << "[NAS] " << nasRoot << " not found, falling back to:" << nasRoot;
    }

    // QFileSystemModel provides live filesystem data to the tree view.
    // It watches the directory for changes (new files, deletions etc.) automatically.
    m_fileModel = new QFileSystemModel(this);
    m_fileModel->setRootPath(nasRoot); // Sets which directory to start scanning from

    // QTreeView renders the filesystem model as an expandable tree.
    m_fileTree = new QTreeView(this);
    m_fileTree->setModel(m_fileModel);

    // setRootIndex tells the tree to show nasRoot as the top-level folder,
    // hiding all parent directories above it (e.g. C:/, D:/ won't appear)
    m_fileTree->setRootIndex(m_fileModel->index(nasRoot));
    m_fileTree->setColumnWidth(0, 300);                              // Name column width
    m_fileTree->setSortingEnabled(true);                             // Allow column sorting
    m_fileTree->header()->setSectionResizeMode(QHeaderView::Interactive); // User can resize columns

    // Add the file browser below the control bar — takes up remaining space
    mainLayout->addWidget(m_fileTree);
}

// ── Server Toggle Slot ────────────────────────────────────────────────────────

/**
 * @brief Called when the Start/Stop button is clicked.
 *
 * Acts as a toggle — checks current state and either starts or stops the server.
 * The UI update (button text, status label) happens via the serverStarted and
 * serverStopped signals emitted by FileServer, keeping this function simple.
 *
 * What this does:
 *   When the user clicks "▶ Start Server" we now:
 *     1. Read useHttps / certPath / keyPath from config.ini via AppConfig.
 *     2. If useHttps=true, call loadTlsFiles() and abort with a message if
 *        the cert/key files cannot be loaded.
 *     3. Call setUseHttps() to arm HTTPS mode in FileServer.
 *     4. Call start() as before — the server now accepts TLS connections.
 *   onServerStarted now shows https:// in the status label.
 */
void MainWindow::onToggleServer()
{
    if (m_fileServer->isRunning()) {
        m_fileServer->stop();
        return;
    }

    // ── Read config ───────────────────────────────────────────────────────
    m_fileServer->setNasRoot(m_config.nasRoot());

    // ── Step 1: wire TLS if useHttps = true in config.ini ────────────────
    const bool wantHttps = m_config.useHttps();
    m_fileServer->setUseHttps(wantHttps);

    if (wantHttps) {
        const bool tlsOk = m_fileServer->loadTlsFiles(
            m_config.certPath(),   // e.g. "cert.pem" next to exe
            m_config.keyPath()     // e.g. "key.pem"  next to exe
        );

        if (!tlsOk) {
            // Surface the error in the status label so the user
            // can fix the paths in config.ini without digging in logs
            m_serverStatus->setStyleSheet("color: #ff4444; font-weight: bold;");
            m_serverStatus->setText("⚠️ TLS error: " + m_fileServer->tlsErrorString());
            return;
        }
    }

    // ── Start server (HTTP or HTTPS depending on setUseHttps above) ───────
    if (!m_fileServer->start(m_config.serverPort())) {
        m_serverStatus->setStyleSheet("color: #ff4444; font-weight: bold;");
        m_serverStatus->setText("⚠️ Port " +
            QString::number(m_config.serverPort()) + " already in use.");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onServerStarted(QString ip, quint16 port)
{
    QString text = QStringLiteral("● Running at %1://%2:%3")
                       .arg(scheme(), ip, QString::number(port));

    // Both addresses when there are both. They are not alternatives: this one is
    // faster inside the house and works with Tailscale switched off, and the remote
    // one is the only one that works from a phone on mobile data.
    const QString remote = remoteUrl();
    if (!remote.isEmpty())
        text += QStringLiteral("   •   from anywhere at %1").arg(remote);

    m_serverStatus->setStyleSheet("color: #00cc44; font-weight: bold;");
    m_serverStatus->setText(text);
    m_serverBtn->setText("■ Stop Server");

    // Checked on start rather than only when the renewer's daily timer fires, so an
    // application that has been closed for two months says something immediately
    // instead of looking fine until the first tick.
    if (m_certRenewer && m_certWarning) {
        const QDateTime expiry = m_certRenewer->currentExpiry();

        if (expiry.isValid()
            && core::CertRenewer::shouldRenew(expiry,
                                              QDateTime::currentDateTimeUtc(),
                                              m_config.certRenewDays())) {
            m_certWarning->setText(
                tr("⚠️ The HTTPS certificate expires on %1. Renewal will be "
                   "attempted automatically; see Remote access for details.")
                    .arg(expiry.toString(QStringLiteral("d MMMM yyyy"))));
            m_certWarning->setVisible(true);
        }
    }
}

/**
 * @brief Called automatically when FileServer emits serverStopped().
 *
 * Resets the status label and button back to their default stopped state.
 */
void MainWindow::onServerStopped()
{
    // Grey = server is inactive
    m_serverStatus->setStyleSheet("color: #888;");
    m_serverStatus->setText("Server stopped");

    // Change button label back to show start action
    m_serverBtn->setText("▶  Start Server");
}
