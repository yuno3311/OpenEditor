// Built-in AI assistant: a chat panel that drives the editor through the shared tool table, using
// the Claude Messages API over HTTPS. C++ has no official SDK, so this is the raw HTTP surface.
#pragma once
#include "panels.h"
#include "widgets.h"
#include <QJsonArray>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBrowser>

class AssistantPanel : public Panel {
    Q_OBJECT
public:
    explicit AssistantPanel(QWidget *p = nullptr);
    static bool hasKey();
    static void storeKey(const QString &key);
    void keyDialog();
    void send(const QString &text);
    bool busy() const { return running; }
    int turnsThisRequest() const { return turns; }
signals:
    void idle();
protected:
    bool eventFilter(QObject *o, QEvent *e) override;
private:
    static QString loadKey();
    void request();
    void handleReply(const QByteArray &body);
    void append(const QString &who, const QString &html);
    void setRunning(bool on);
    void finishTurn(const QString &note = {});
    QTextBrowser *log;
    QPlainTextEdit *input;
    QPushButton *sendBtn, *stopBtn;
    QLabel *usage;
    QNetworkAccessManager net;
    QJsonArray messages, toolDefs;
    bool running = false, cancelled = false;
    int turns = 0, retries = 0;
    qint64 tokensIn = 0, tokensOut = 0;
    class QNetworkReply *pending = nullptr;
};
