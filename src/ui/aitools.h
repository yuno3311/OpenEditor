// The editor's command surface for AI agents. The MCP server and the built-in assistant both call
// this one table, so every action goes through the normal undo stack.
#pragma once
#include "../core/model.h"
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <functional>

class AiTools : public QObject {
    Q_OBJECT
public:
    static AiTools *instance();
    QJsonArray definitions() const;                                    // name, description, inputSchema
    QJsonObject call(const QString &name, const QJsonObject &args);    // MCP-shaped: content[], isError
    std::function<void(bool)> setProxiesEnabled;                        // wired by the main window
    std::function<void(Frame)> showFrame;
signals:
    void activity(const QString &text);
private:
    explicit AiTools(QObject *p = nullptr);
    using Fn = std::function<QJsonObject(const QJsonObject &)>;
    struct Tool { QString name, description; QJsonObject schema; Fn fn; };
    void add(const QString &name, const QString &desc, const QJsonArray &params, const Fn &fn);
    void build();
    int startJob(const QString &kind);
    void updateJob(int id, const QString &state, double progress, const QString &message);
    QVector<Tool> tools;
    struct Job { QString kind, state = "running", message; double progress = 0; };
    QHash<int, Job> jobs;
    int nextJob = 1;
};
#define AITOOLS (AiTools::instance())
