#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QString>
#include <QStringList>

/**
 * @class FileServer
 * @brief An asynchronous HTTP file server for local network file sharing.
 */
class FileServer : public QObject {
    Q_OBJECT

public:
    explicit FileServer(QObject* parent = nullptr);

    /**
     * @brief Starts the TCP server on the specified port.
     * @return true if successful, false if the port is already in use.
     */
    bool    start(quint16 port = 8080);

    /**
     * @brief Stops the server and closes all active connections.
     */
    void    stop();

    /** @return true if the server is currently listening for connections. */
    bool    isRunning() const;

    /** @return The current port the server is bound to. */
    quint16 port() const;

    /** @return The local IPv4 address of the machine (e.g., 192.168.x.x). */
    QString localIP() const;

    /** @brief Sets the root directory to be served. Ensure this ends with a slash. */
    void    setNasRoot(const QString& path) { nasRoot = path; }

signals:
    /** @brief Emitted when the server successfully starts listening. */
    void serverStarted(QString ip, quint16 port);
    
    /** @brief Emitted when the server is manually stopped. */
    void serverStopped();
    
    /** @brief Emitted every time a browser requests a specific URL path. */
    void requestReceived(QString path);

private slots:
    /** @brief Internal handler for incoming TCP connections. */
    void onNewConnection();

private:
    /** @brief Main router: parses HTTP headers and decides whether to send HTML or a File. */
    void    handleRequest(QTcpSocket* socket);

    /** @brief Generates the Grid-view HTML for a folder's contents. */
    QString buildFileListHTML(const QString& dirPath, const QString& urlPath);

    /** @brief Generates the Preview page for Images and Videos. */
    QString buildPreviewHTML(const QString& urlPath, 
                             const QString& fileName, 
                             const QString& type);

    // Configuration
    QString nasRoot = "E:/"; 

    // Server Core
    QTcpServer* m_server;
    quint16     m_port;
};