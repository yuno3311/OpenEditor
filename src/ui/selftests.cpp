// End-to-end checks that drive the real window: `OpenEditor --text-test <outdir> <model> <media> <expect...>`
// and `OpenEditor --ai-test <outdir>`.
#include "aitools.h"
#include "assistant.h"
#include "mainwindow.h"
#include "mcpserver.h"
#include "../core/mediacache.h"
#include "../core/ops.h"
#include "../core/project.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <cstdio>

static int fails = 0;
static void expect(bool ok, const QString &what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what.toUtf8().constData());
    fflush(stdout);
    fails += !ok;
}
static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(3); }
}

int textSelfTest(MainWindow &w, const QStringList &args)
{
    const QString out = args[0], model = args[1], media = args[2];
    QStringList want = args.mid(3);
    QDir().mkpath(out);
    PRJ->reset();
    PRJ->loaded = true;
    w.showPage(MainWindow::PageEdit);
    w.setWorkspace("Text");
    w.project->importFiles({media});
    const MediaItem m = PRJ->s.media.first();
    PRJ->createSequence("Speech", m.w > 0 ? m.w : 1280, m.h > 0 ? m.h : 720, 30, 1);
    PRJ->edit("Add", [&](ProjectState &st) {
        Sequence &q = *st.seqById(st.activeSeq);
        Ops::overwriteMedia(st, q, m, 0, m.duration, 0, Ops::Patch{m.kind != MediaKind::Audio ? 0 : -1, 0});
    });
    w.timeline->view->zoomFit();

    // Deliberately no wait for the audio conform: this is the moment users press the button.
    QElapsedTimer tm;
    tm.start();
    w.captions->beginTranscribeForTest(model);
    while (w.captions->transcribing() && tm.elapsed() < 280000) pump(40);
    auto *q = PRJ->active();
    printf("transcribed %.1f s of media in %.1f s, %lld captions\n", m.duration, tm.elapsed() / 1000.0, (long long)q->captions.size());
    QString all;
    int words = 0;
    for (auto &c : q->captions) {
        printf("  %6.2f-%6.2f  %s\n", c.start / 30.0, c.end / 30.0, c.text.toUtf8().constData());
        all += c.text + " ";
        words += c.words.size();
    }
    expect(!q->captions.isEmpty(), "transcribing right after import still produces text");
    for (auto &s : std::as_const(want)) expect(all.contains(s, Qt::CaseInsensitive), "transcript contains: " + s);
    expect(words >= q->captions.size() * 2, "captions carry word timing");
    Frame lastEnd = 0;
    bool ordered = true;
    for (auto &c : q->captions) for (auto &x : c.words) { ordered &= x.start >= lastEnd - 1 && x.end > x.start; lastEnd = x.end; }
    expect(ordered, "word times run forward");
    expect(lastEnd <= q->duration() + 2, "word times stay inside the sequence");

    auto *view = w.captions->transcriptView();
    pump(100);
    expect(view->wordCount() == words, "transcript view shows every word");
    w.grab().save(out + "/transcript.png");

    // Text-based edit: select two words in the middle, press the real Delete key.
    const int mid = words / 2;
    const QString before = all;
    const Frame dur0 = q->duration();
    const auto cap0 = q->captions;
    Word w1, w2;
    int k = 0;
    for (auto &c : cap0) for (auto &x : c.words) { if (k == mid) w1 = x; if (k == mid + 1) w2 = x; ++k; }
    expect(view->selectWords(mid, mid + 1), "selecting words works");
    view->setFocus();
    QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(view, &over);
    expect(over.isAccepted(), "Delete goes to the transcript, not the window shortcut");
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(view, &press);
    pump(200);
    q = PRJ->active();
    QString after;
    for (auto &c : q->captions) after += c.text + " ";
    const Frame cut = dur0 - q->duration();
    printf("removed \"%s %s\", %lld frames\n", w1.text.toUtf8().constData(), w2.text.toUtf8().constData(), (long long)cut);
    expect(w2.end - w1.start >= 3 && cut >= w2.end - w1.start - 1 && cut < (w2.end - w1.start) + 40, "delete removes those words from the timeline, and only those");
    expect(before.count(w1.text + " " + w2.text) == after.count(w1.text + " " + w2.text) + 1, "deleted words are gone from the text");
    int clipsWithSameLink = 0;
    for (auto &t : q->audio) for (auto &c : t.clips) if (c.link) for (auto &o : t.clips) clipsWithSameLink += o.id != c.id && o.link == c.link;
    expect(clipsWithSameLink == 0, "pieces on either side of the cut are not linked to each other");
    bool after2 = true;
    int k2 = 0;
    for (auto &c : q->captions) for (auto &x : c.words) { if (k2 >= mid) after2 &= x.start >= w1.start - 2; ++k2; }
    expect(after2, "words after the cut moved earlier with the picture");
    w.grab().save(out + "/transcript_after.png");
    if (view->wordCount()) PRJ->undo.undo();
    pump(100);
    q = PRJ->active();
    expect(q->duration() == dur0 && q->captions.size() == cap0.size(), "undo restores footage and text");
    expect(view->wordCount() == words, "undo restores the transcript view");

    // Retyping a caption must not lose it, and burned-in text must render for this language.
    if (q->captions.isEmpty()) { printf("\nno captions\n"); return 1; }
    PRJ->setPlayhead(q->captions.first().start + 2);
    pump(500);
    w.setWorkspace("Editing");
    pump(300);
    w.grab().save(out + "/captions_burned.png");
    PRJ->undo.setClean();
    printf(fails ? "\n%d FAILURES\n" : "\nTEXT TEST PASSED\n", fails);
    fflush(stdout);
    return fails ? 1 : 0;
}

// ------------------------------------------------------------------ MCP and assistant
static QJsonObject httpRpc(QNetworkAccessManager &net, const QString &url, const QByteArray &token, const QJsonObject &rpc, int *status, const QByteArray &origin = {})
{
    QNetworkRequest rq{QUrl(url)};
    rq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!token.isEmpty()) rq.setRawHeader("Authorization", "Bearer " + token);
    if (!origin.isEmpty()) rq.setRawHeader("Origin", origin);
    auto *r = net.post(rq, QJsonDocument(rpc).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    *status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    auto o = QJsonDocument::fromJson(r->readAll()).object();
    r->deleteLater();
    return o;
}

// Stands in for the Claude API so the assistant loop can be tested without a key or spend.
class MockClaude : public QTcpServer {
public:
    QVector<QByteArray> requests, heads;
    int round = 0;
    void incomingConnection(qintptr h) override
    {
        auto *s = new QTcpSocket(this);
        s->setSocketDescriptor(h);
        auto buf = std::make_shared<QByteArray>();
        connect(s, &QTcpSocket::readyRead, this, [this, s, buf] {
            *buf += s->readAll();
            int he = buf->indexOf("\r\n\r\n");
            if (he < 0) return;
            QByteArray head = buf->left(he).toLower();
            int cl = head.indexOf("content-length:");
            int len = head.mid(cl + 15, head.indexOf("\r\n", cl) - cl - 15).trimmed().toInt();
            if (buf->size() < he + 4 + len) return;
            heads << head;
            requests << buf->mid(he + 4, len);
            QJsonObject reply;
            if (round++ == 0)
                reply = {{"stop_reason", "tool_use"}, {"usage", QJsonObject{{"input_tokens", 10}, {"output_tokens", 5}}},
                         {"content", QJsonArray{QJsonObject{{"type", "thinking"}, {"thinking", ""}, {"signature", "sig"}},
                                                QJsonObject{{"type", "text"}, {"text", "Checking."}},
                                                QJsonObject{{"type", "tool_use"}, {"id", "t1"}, {"name", "add_marker"}, {"input", QJsonObject{{"frame", 12}, {"name", "from claude"}}}},
                                                QJsonObject{{"type", "tool_use"}, {"id", "t2"}, {"name", "no_such_tool"}, {"input", QJsonObject{}}}}}};
            else reply = {{"stop_reason", "end_turn"}, {"usage", QJsonObject{{"input_tokens", 20}, {"output_tokens", 6}}}, {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "Added the marker."}}}}};
            QByteArray body = QJsonDocument(reply).toJson(QJsonDocument::Compact);
            s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            s->flush();
            s->disconnectFromHost();
        });
    }
};

int aiSelfTest(MainWindow &w, const QStringList &args)
{
    PRJ->reset();
    PRJ->loaded = true;
    w.showPage(MainWindow::PageEdit);
    PRJ->createSequence("AI", 640, 360, 30, 1);
    auto *mcp = McpServer::instance();
    expect(!mcp->running(), "MCP is off until the user enables it");
    expect(mcp->start(0), "server starts");
    const QString url = mcp->url();
    const QByteArray tok = mcp->token().toUtf8();
    QNetworkAccessManager net;
    int st = 0;
    QJsonObject init{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}}}};
    httpRpc(net, url, "wrong", init, &st);
    expect(st == 401, "wrong token is rejected");
    httpRpc(net, url, {}, init, &st);
    expect(st == 401, "missing token is rejected");
    httpRpc(net, url, tok, init, &st, "https://evil.example");
    expect(st == 403, "a website origin is rejected");
    auto r = httpRpc(net, url, tok, init, &st);
    expect(st == 200 && r["result"].toObject()["protocolVersion"].toString() == "2025-06-18", "initialize negotiates the protocol version");
    expect(r["result"].toObject()["serverInfo"].toObject()["name"].toString() == "open-editor", "server identifies itself");
    httpRpc(net, url, tok, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, &st);
    expect(st == 202, "notifications get 202");
    r = httpRpc(net, url, tok, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}, &st);
    const int ntools = r["result"].toObject()["tools"].toArray().size();
    printf("tools advertised: %d\n", ntools);
    expect(ntools >= 40, "tools are listed");
    auto call = [&](const QString &name, const QJsonObject &a) {
        static int id = 10;
        auto x = httpRpc(net, url, tok, {{"jsonrpc", "2.0"}, {"id", ++id}, {"method", "tools/call"}, {"params", QJsonObject{{"name", name}, {"arguments", a}}}}, &st);
        return x["result"].toObject();
    };
    r = call("add_title", {{"text", "From MCP"}, {"frame", 0}, {"duration_frames", 30}, {"size", 60}});
    expect(!r["isError"].toBool() && PRJ->active()->video[0].clips.size() == 1, "tools/call edits the timeline");
    r = call("get_frame", {{"frame", 5}, {"width", 320}});
    expect(r["content"].toArray()[0].toObject()["type"].toString() == "image" && r["content"].toArray()[0].toObject()["data"].toString().size() > 500, "get_frame returns an image");
    r = call("undo", {});
    expect(PRJ->active()->video[0].clips.isEmpty(), "AI edits are undoable");
    r = call("add_clip", {{"media_id", 99999}});
    expect(r["isError"].toBool(), "bad arguments come back as tool errors, not crashes");
    r = call("export_sequence", {{"path", "relative.mp4"}});
    expect(r["isError"].toBool(), "export refuses a relative path");
    httpRpc(net, url, tok, {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "bogus"}}, &st);
    expect(st == 200, "unknown method returns a JSON-RPC error");

    // stdio bridge, through the real executable and the discovery file
    QProcess br;
    br.setProgram(QCoreApplication::applicationDirPath() + "/oe-mcp.exe");
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    br.setProcessEnvironment(env);
    br.start();
    br.waitForStarted(5000);
    // The bridge reads the regular app data folder; point its discovery at this test server.
    QString disc = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Open Editor/Open Editor";
    QDir().mkpath(disc);
    QFile::copy(mcp->discoveryFile(), disc + "/mcp.json");
    br.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}}).toJson(QJsonDocument::Compact) + "\n");
    QElapsedTimer t;
    t.start();
    QByteArray got;
    while (t.elapsed() < 6000 && !got.contains('\n')) { pump(20); got += br.readAllStandardOutput(); }
    expect(QJsonDocument::fromJson(got.trimmed()).object()["id"].toInt() == 7, "oe-mcp stdio bridge answers");
    br.closeWriteChannel();
    br.waitForFinished(2000);
    if (br.state() != QProcess::NotRunning) br.kill();
    QFile::remove(disc + "/mcp.json");

    mcp->stop();
    expect(!mcp->running() && !QFile::exists(mcp->discoveryFile()), "stopping removes the discovery file");

    // Assistant loop against the mock API
    MockClaude mock;
    mock.listen(QHostAddress::LocalHost, 0);
    qputenv("ANTHROPIC_BASE_URL", QString("http://127.0.0.1:%1").arg(mock.serverPort()).toUtf8());
    qputenv("ANTHROPIC_API_KEY", "test-key");
    bool idle = false;
    QObject::connect(w.assistant, &AssistantPanel::idle, &w, [&] { idle = true; });
    w.assistant->send("Add a marker at frame 12");
    t.restart();
    while (!idle && t.elapsed() < 8000) pump(20);
    expect(idle, "assistant finishes its loop");
    expect(mock.requests.size() == 2, "assistant sends the tool results back");
    auto first = QJsonDocument::fromJson(mock.requests.value(0)).object();
    auto second = QJsonDocument::fromJson(mock.requests.value(1)).object();
    expect(first["model"].toString() == "claude-opus-5-5" && first["tools"].toArray().size() == ntools, "request uses the default model and every tool");
    expect(mock.heads.value(0).contains("anthropic-version: 2026-10-21") && mock.heads.value(0).contains("x-api-key: test-key"), "request headers are right");
    auto msgs = second["messages"].toArray();
    auto asst = msgs[1].toObject()["content"].toArray();
    expect(asst[0].toObject()["type"].toString() == "thinking" && asst[0].toObject()["signature"].toString() == "sig", "thinking blocks go back unchanged");
    auto res = msgs[2].toObject()["content"].toArray();
    expect(res.size() == 2 && res[1].toObject()["is_error"].toBool(), "both tool results return in one message, the failure flagged");
    expect(PRJ->active()->markers.size() == 1 && PRJ->active()->markers[0].name == "from claude", "assistant really edited the sequence");
    PRJ->undo.setClean();
    printf(fails ? "\n%d FAILURES\n" : "\nAI TEST PASSED\n", fails);
    fflush(stdout);
    return fails ? 1 : 0;
}
