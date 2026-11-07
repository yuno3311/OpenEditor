// Model Context Protocol server (Streamable HTTP). Off by default. Bound to 127.0.0.1, requires a
// bearer token, and rejects cross-origin browser requests.
#pragma once
#include <QJsonObject>
#include <QObject>
#include <memory>

class QHttpServer;
class QTcpServer;
class QHttpServerRequest;

class McpServer : public QObject {
    Q_OBJECT
public:
    static McpServer *instance();
    ~McpServer() override;
    bool start(quint16 port);
    void stop();
    bool running() const;
    quint16 port() const { return boundPort; }
    QString url() const;
    QString token();
    void regenerateToken();
    QString discoveryFile() const;
    QJsonObject handle(const QJsonObject &rpc, bool *isNotification); // transport independent
signals:
    void stateChanged(bool running);
    void called(const QString &tool);
private:
    explicit McpServer(QObject *p = nullptr);
    std::unique_ptr<QHttpServer> http;
    QTcpServer *tcp = nullptr;
    quint16 boundPort = 0;
};
