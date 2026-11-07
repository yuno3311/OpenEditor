// stdio <-> HTTP bridge for MCP clients that launch a local command. Reads newline-delimited
// JSON-RPC from stdin and forwards it to the running editor. Connection details come from the
// discovery file the editor writes when its MCP server is enabled.
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QThread>
#include <cstdio>
#include <iostream>
#include <string>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

class StdinReader : public QThread {
    Q_OBJECT
signals:
    void line(const QByteArray &l);
protected:
    void run() override
    {
        std::string s;
        while (std::getline(std::cin, s)) emit line(QByteArray::fromStdString(s));
    }
};

static void writeLine(const QByteArray &b)
{
    fwrite(b.constData(), 1, size_t(b.size()), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}
static void rpcError(const QJsonValue &id, const QString &msg)
{
    QJsonObject e{{"code", -32000}, {"message", msg}};
    writeLine(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"error", e}}).toJson(QJsonDocument::Compact));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("Open Editor");
    QCoreApplication::setOrganizationName("Open Editor");
#ifdef Q_OS_WIN
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    QNetworkAccessManager net;
    StdinReader in;
    int pending = 0;
    bool eof = false;
    auto maybeQuit = [&] { if (eof && pending == 0) QCoreApplication::quit(); };
    QObject::connect(&in, &StdinReader::line, &app, [&](QByteArray l) {
        l = l.trimmed();
        if (l.isEmpty()) return;
        const QJsonValue id = QJsonDocument::fromJson(l).object().value("id");
        QFile f(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/mcp.json");
        if (!f.open(QIODevice::ReadOnly)) {
            if (!id.isUndefined()) rpcError(id, "Open Editor is not running with its MCP server enabled. Turn it on in AI > MCP Server.");
            return;
        }
        const QJsonObject cfg = QJsonDocument::fromJson(f.readAll()).object();
        QNetworkRequest rq(QUrl(cfg["url"].toString()));
        rq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        rq.setRawHeader("Accept", "application/json");
        rq.setRawHeader("Authorization", "Bearer " + cfg["token"].toString().toUtf8());
        rq.setTransferTimeout(0);
        auto *r = net.post(rq, l);
        ++pending;
        QObject::connect(r, &QNetworkReply::finished, &app, [&, r, id] {
            QByteArray b = r->readAll();
            if (r->error() != QNetworkReply::NoError && b.isEmpty()) { if (!id.isUndefined()) rpcError(id, r->errorString()); }
            else if (!b.trimmed().isEmpty()) writeLine(b.trimmed());
            r->deleteLater();
            --pending;
            maybeQuit();
        });
    });
    QObject::connect(&in, &QThread::finished, &app, [&] { eof = true; maybeQuit(); });
    in.start();
    int rc = QCoreApplication::exec();
    in.terminate();
    in.wait(200);
    return rc;
}
#include "oe_mcp_bridge.moc"
