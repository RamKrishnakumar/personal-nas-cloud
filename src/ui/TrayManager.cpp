#include "TrayManager.h"
#include "MainWindow.h"
#include <QApplication>
#include <QIcon>

namespace {

/**
 * @brief The icon for the tray, which is not the same choice on every platform.
 *
 * macOS draws a menu-bar icon as a *template*: it keeps only the shape and recolours
 * it itself, dark on a light menu bar and light on a dark one, inverting again while
 * the menu is open. QIcon::setIsMask(true) is what hands it over on those terms. A
 * colour icon there would stay the wrong colour in one of the two themes, so the
 * single-colour silhouette is used instead — and it is also the only version that
 * survives being drawn at menu-bar size, where the full icon's concentric arcs merge
 * into a smear.
 *
 * Windows and Linux draw the icon as given, with no recolouring, so the full icon is
 * the better choice: its own dark background is what keeps it legible whether the
 * taskbar is light or dark.
 */
QIcon trayIcon()
{
#ifdef Q_OS_MACOS
    QIcon icon(QStringLiteral(":/web/tray.svg"));
    icon.setIsMask(true);
    return icon;
#else
    return QIcon(QStringLiteral(":/web/icon.svg"));
#endif
}

} // namespace

TrayManager::TrayManager(MainWindow* window, QObject* parent) : QObject(parent), m_window(window) {
    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(trayIcon());
    m_tray->setToolTip("NestDrive — Running");

    m_menu = new QMenu();
    QAction* showAction = m_menu->addAction("Open NestDrive");
    m_menu->addSeparator();
    QAction* quitAction = m_menu->addAction("Quit");

    m_tray->setContextMenu(m_menu);
    m_tray->show();

    connect(showAction, &QAction::triggered, m_window, &MainWindow::show);
    connect(quitAction, &QAction::triggered, this, &TrayManager::onQuitClicked);
    connect(m_tray, &QSystemTrayIcon::activated, this, &TrayManager::onTrayActivated);
}

void TrayManager::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::DoubleClick) {
        m_window->show();
        m_window->raise();
    }
}

void TrayManager::onQuitClicked() {
    QApplication::quit();
}