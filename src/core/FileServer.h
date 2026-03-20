#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QString>

class FileServer : public QObject {
    Q_OBJECT

public:
    explicit FileServer(QObject* parent = nullptr);

    bool    start(quint16 port = 8080);
    void    stop();
    bool    isRunning() const;
    quint16 port() const;
    QString localIP() const;

signals:
    void serverStarted(QString ip, quint16 port);
    void serverStopped();
    void requestReceived(QString path);

private slots:
    void onNewConnection();

private:
    void handleRequest(QTcpSocket* socket);
    QString buildFileListHTML(const QString& dirPath, const QString& urlPath);
    QString nasRoot = "E:/";

    QTcpServer* m_server;
    quint16     m_port;
};
