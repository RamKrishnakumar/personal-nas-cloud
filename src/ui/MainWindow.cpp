#include "MainWindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QHeaderView>
#include <QDir>
#include <QDebug>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_fileServer(new FileServer(this))
{
    setWindowTitle("Nestdrive v0.2");
    resize(900, 650);

    connect(m_fileServer, &FileServer::serverStarted,
            this,          &MainWindow::onServerStarted);
    connect(m_fileServer, &FileServer::serverStopped,
            this,          &MainWindow::onServerStopped);

    setupUI();
}

void MainWindow::setupUI()
{
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // --- Server control bar ---
    QHBoxLayout* barLayout = new QHBoxLayout();

    m_serverBtn = new QPushButton("▶  Start Server", this);
    m_serverBtn->setFixedHeight(32);
    connect(m_serverBtn, &QPushButton::clicked,
            this,         &MainWindow::onToggleServer);

    m_serverStatus = new QLabel("Server stopped", this);
    m_serverStatus->setStyleSheet("color: #888;");

    barLayout->addWidget(m_serverBtn);
    barLayout->addWidget(m_serverStatus);
    barLayout->addStretch();
    mainLayout->addLayout(barLayout);

    // --- File browser ---
    QString nasRoot = "E:/";
    QDir dir(nasRoot);
    if (!dir.exists())
        nasRoot = QDir::homePath();

    m_fileModel = new QFileSystemModel(this);
    m_fileModel->setRootPath(nasRoot);

    m_fileTree = new QTreeView(this);
    m_fileTree->setModel(m_fileModel);
    m_fileTree->setRootIndex(m_fileModel->index(nasRoot));
    m_fileTree->setColumnWidth(0, 300);
    m_fileTree->setSortingEnabled(true);
    m_fileTree->header()->setSectionResizeMode(QHeaderView::Interactive);

    mainLayout->addWidget(m_fileTree);
}

void MainWindow::onToggleServer()
{
    if (m_fileServer->isRunning()) {
        m_fileServer->stop();
    } else {
        m_fileServer->start(8080);
    }
}

void MainWindow::onServerStarted(QString ip, quint16 port)
{
    QString url = QString("http://%1:%2").arg(ip).arg(port);
    m_serverStatus->setStyleSheet("color: #00cc44; font-weight: bold;");
    m_serverStatus->setText("● Running at " + url);
    m_serverBtn->setText("■  Stop Server");
}

void MainWindow::onServerStopped()
{
    m_serverStatus->setStyleSheet("color: #888;");
    m_serverStatus->setText("Server stopped");
    m_serverBtn->setText("▶  Start Server");
}