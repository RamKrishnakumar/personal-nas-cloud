#pragma once
#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>

class MainWindow;

class TrayManager : public QObject {
    Q_OBJECT
public:
    explicit TrayManager(MainWindow* window, QObject* parent = nullptr);
private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onQuitClicked();
private:
    QSystemTrayIcon*    m_tray;
    QMenu*              m_menu;
    MainWindow*          m_window;

};