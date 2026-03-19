#pragma once
#include <QMainWindow>
#include <QTreeView>
#include <QFileSystemModel>

class MainWindow : public QMainWindow {
    Q_OBJECT;
public:
    explicit MainWindow(QWidget* parent = nullptr);
private:
    QTreeView*          m_fileTree;
    QFileSystemModel*   m_fileModel;
    void setupUI();
};