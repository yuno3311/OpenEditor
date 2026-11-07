#include "mcpserver.h"
#include "aitools.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QHttpServerResponder>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QUrl>

static const QStringList kProtocols{"2025-11-25", "2025-06-18", "2025-03-26"};

McpServer *McpServer::instance() { static McpServer s; return &s; }
McpServer::McpServer(QObject *p) : QObject(p) {}
McpServer::~McpServer() { stop(); }

QString McpServer::discoveryFile() const
{
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(d);
    return d + "/mcp.json";
}
QString McpServer::url() const { return QString("http://127.0.0.1:%1/mcp").arg(boundPort); }
QString McpServer::token()
{
    QSettings s;
    QString t = s.value("mcp/token").toString();
    if (t.isEmpty()) { regenerateToken(); t = s.value("mcp/token").toString(); }
    return t;
}
void McpServer::regenerateToken()
{
    QByteArray b(32, 0);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(b.data()), 8);
    QSettings().setValue("mcp/token", QString::fromLatin1(b.toHex()));
}
bool McpServer::running() const { return http != nullptr; }

static bool sameToken(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size()) return false;
    int diff = 0;
    for (int i = 0; i < a.size(); ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

static QHttpServerResponse jsonReply(const QJsonObject &o, QHttpServerResponse::StatusCode code = QHttpServerResponse::StatusCode::Ok)
{
    return QHttpServerResponse(QByteArray("application/json"), QJsonDocument(o).toJson(QJsonDocument::Compact), code);
}

bool McpServer::start(quint16 wantPort)
{
    stop();
    http = std::make_unique<QHttpServer>();
    const QByteArray want = "Bearer " + token().toUtf8();
    http->route("/mcp", QHttpServerRequest::Method::Post, [this, want](const QHttpServerRequest &r) {
        using SC = QHttpServerResponse::StatusCode;
        const QByteArray origin = r.value("Origin");
        if (!origin.isEmpty()) {
            const QString host = QUrl(QString::fromLatin1(origin)).host();
            if (host != "127.0.0.1" && host != "localhost") return jsonReply({{"error", "origin not allowed"}}, SC::Forbidden);
        }
        if (!sameToken(r.value("Authorization"), want)) return jsonReply({{"error", "unauthorized"}}, SC::Unauthorized);
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(r.body(), &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
            QJsonObject e{{"code", -32700}, {"message", "Parse error"}};
            return jsonReply({{"jsonrpc", "2.0"}, {"id", QJsonValue()}, {"error", e}}, SC::BadRequest);
        }
        bool notification = false;
        QJsonObject reply = handle(doc.object(), &notification);
        if (notification) return QHttpServerResponse(SC::Accepted);
        return jsonReply(reply);
    });
    // No server-initiated stream: the spec allows 405 here.
    http->route("/mcp", QHttpServerRequest::Method::Get, [] { return QHttpServerResponse(QHttpServerResponse::StatusCode::MethodNotAllowed); });
    http->route("/mcp", QHttpServerRequest::Method::Delete, [] { return QHttpServerResponse(QHttpServerResponse::StatusCode::MethodNotAllowed); });
    tcp = new QTcpServer(http.get());
    if (!tcp->listen(QHostAddress::LocalHost, wantPort) || !http->bind(tcp)) {
        http.reset();
        tcp = nullptr;
        return false;
    }
    boundPort = tcp->serverPort();
    QFile f(discoveryFile());
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(QJsonObject{{"url", url()}, {"token", token()}}).toJson());
    emit stateChanged(true);
    return true;
}

void McpServer::stop()
{
    if (!http) return;
    http.reset();
    tcp = nullptr;
    boundPort = 0;
    QFile::remove(discoveryFile());
    emit stateChanged(false);
}

QJsonObject McpServer::handle(const QJsonObject &rpc, bool *isNotification)
{
    const QString method = rpc["method"].toString();
    const QJsonValue id = rpc["id"];
    const QJsonObject params = rpc["params"].toObject();
    *isNotification = !rpc.contains("id");
    auto ok = [&](const QJsonObject &result) { return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}; };
    auto err = [&](int code, const QString &m) {
        return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", m}}}};
    };
    if (*isNotification) return {};
    if (method == "initialize") {
        QString ver = params["protocolVersion"].toString();
        if (!kProtocols.contains(ver)) ver = kProtocols.first();
        return ok({{"protocolVersion", ver},
                   {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                   {"serverInfo", QJsonObject{{"name", "open-editor"}, {"title", "Open Editor"}, {"version", QCoreApplication::applicationVersion()}}},
                   {"instructions", "Controls the Open Editor video editor. Start with get_project and get_timeline. Times are in sequence frames. "
                                    "Every edit is undoable with undo. Use get_frame to look at the result. Transcribe and export run in the background: poll job_status."}});
    }
    if (method == "ping") return ok({});
    if (method == "tools/list") return ok({{"tools", AITOOLS->definitions()}});
    if (method == "tools/call") {
        const QString name = params["name"].toString();
        emit called(name);
        return ok(AITOOLS->call(name, params["arguments"].toObject()));
    }
    return err(-32601, "Method not found: " + method);
}
