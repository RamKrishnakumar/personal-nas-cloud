#pragma once
#include <QMainWindow>
#include <QTreeView>
#include <QFileSystemModel>
#include <QLabel>
#include <QPushButton>

#include "FileServer.h"

class MainWindow : public QMainWindow {
    Q_OBJECT;
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onToggleServer();
    void onServerStarted(QString ip, quint16 port);
    void onServerStopped();

private:
    QTreeView*          m_fileTree;
    QFileSystemModel*   m_fileModel;
    QPushButton*        m_serverBtn;
    QLabel*             m_serverStatus;
    FileServer*         m_fileServer;


    void setupUI();
};