#include "TrayManager.h"
#include "MainWindow.h"
#include <QApplication>

TrayManager::TrayManager(MainWindow* window, QObject* parent) : QObject(parent), m_window(window) {
    m_tray = new QSystemTrayIcon(QIcon(":/resources/app.ico"), this);
    m_tray->setToolTip("MyNAS — Running");

    m_menu = new QMenu();
    QAction* showAction = m_menu->addAction("Open MyNAS");
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