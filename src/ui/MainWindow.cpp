#include "MainWindow.h"
#include <QVBoxLayout>
#include <QWidget>
#include <QHeaderView>
#include <QDir>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("MyNAS");
    resize(900,600);
    setupUI();
}

void MainWindow::setupUI() {
    //  Central widget
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* layout = new QVBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 8);

    // File system model
    m_fileModel = new QFileSystemModel(this);
    m_fileModel->setRootPath(QDir::homePath());

    // Tree View
    m_fileTree = new QTreeView(this);
    m_fileTree->setModel(m_fileModel);
    m_fileTree->setRootIndex(m_fileModel->index(QDir::homePath()));
    m_fileTree->setColumnWidth(0, 300);
    m_fileTree->setSortingEnabled(true);
    m_fileTree->header()->setSectionResizeMode(QHeaderView::Interactive);

    layout->addWidget(m_fileTree);
}